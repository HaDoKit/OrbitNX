#include <libavutil/samplefmt.h>

#include <switch.h>
#include <SDL.h>

#include "context.h"
#include "input.h"
#include "video.h"
#include "network.h"
#include "renderer.h"
#include "audio.h"
#include "logger.h"
static const SocketInitConfig socketInitConf = {
    .tcp_tx_buf_size = 0x80000,
    .tcp_rx_buf_size = 0x100000,
    .tcp_tx_buf_max_size = 0x400000,
    .tcp_rx_buf_max_size = 0x400000,

    .udp_tx_buf_size = 0x1400,
    // Opus/RTP audio arrives continuously at ~16KB/s (128kbps) - the previous 0x3500 (13.5KB) gave
    // under a second of headroom before overflowing, which is exactly what was happening: a fatal
    // avio_read_partial I/O error ~0.8s into every session, from this buffer filling up faster than
    // the audio thread could drain it. 0x20000 gives ~8s of slack at that rate, well above any
    // realistic scheduling jitter.
    .udp_rx_buf_size = 0x20000,

    .sb_efficiency = 2,

    // video.c, audio.c, and input.c each own and read/write a socket on their own dedicated
    // thread - libnx's default of 3 bsd sessions leaves zero headroom for that. Raised above the
    // 3 known concurrent consumers to give the session pool spare capacity under load.
    .num_bsd_sessions = 6,
};

static Thread renderThread;
static Thread inputHandlerThread;
static Thread audioHandlerThread;
void startInput()
{
    input_init();
    threadCreate(&inputHandlerThread, inputHandlerLoop, NULL, NULL, 0x1000, 0x2b, 0);
    threadStart(&inputHandlerThread);
}

void startAudio()
{
    // On same thread as input and preemptive
    threadCreate(&audioHandlerThread, audioHandlerLoop, NULL, NULL, 0x10000, 0x20, 1);
    threadStart(&audioHandlerThread);
}

void startRender(VideoContext *videoContext)
{
    threadCreate(&renderThread, videoLoop, videoContext, NULL, 0x800000, 0x2b, 2);
    threadStart(&renderThread);
}

RenderContext *renderContext = NULL;
VideoContext *videoContext = NULL;
ClkrstSession cpuSession;
void init()
{
    orbitnx_log_init();
    orbitnx_log("main", "init starting");
    appletSetIdleTimeDetectionExtension(AppletIdleTimeDetectionExtension_None);
    appletSetWirelessPriorityMode(AppletWirelessPriorityMode_OptimizedForWlan);
    appletSetAutoSleepDisabled(true);

    printf("networkInit\n");
    network_init(&socketInitConf);

    printf("romfsInit\n");
    romfsInit();

    printf("audoutInitialize\n");
    audoutInitialize();

    printf("audoutStartAudioOut\n");
    audoutStartAudioOut();

    printf("clkrstInitialize\n");
    clkrstInitialize();
    clkrstOpenSession(&cpuSession, PcvModuleId_CpuBus, 3);
    clkrstSetClockRate(&cpuSession, 1785000000);

    printf("startAudio\n");
    startAudio();

    printf("startInput\n");
    startInput();

    printf("makeRenderer\n");
    renderContext = makeRenderer();
    printf("createVideoContext\n");
    videoContext = createVideoContext();
    videoContext->renderContext = renderContext;
    printf("startRender\n");
    startRender(videoContext);
    printf("Complete INIT!\n");
}

void unInit()
{
    orbitnx_log("main", "unInit starting");
    freeRenderer(renderContext);
    freeVideoContext(videoContext);
    clkrstCloseSession(&cpuSession); // end OC
    clkrstExit();
    input_unInit();
    audoutStopAudioOut();
    audoutExit();
    network_unInit();
    plExit();
    orbitnx_log("main", "unInit complete");
    orbitnx_log_close();
}
bool threadsSleeping = false;
int main(int argc, char **argv)
{
    init();

    // Separate from video/audio/input's own per-thread heartbeats - this one is the main thread's
    // own pulse. If the whole app hangs (not just one subsystem's thread), this is what stops
    // ticking, distinguishing "one stream died" (its own heartbeat goes silent, this one keeps
    // going) from "the whole app is stuck" (every heartbeat in the log, including this one, stops
    // at the same timestamp).
    u64 main_heartbeat_tick = armGetSystemTick();

    while (appletMainLoop())
    {
        if (appletGetFocusState() == AppletFocusState_InFocus)
        {
            if (threadsSleeping)
            {
                threadResume(&renderThread);
                threadResume(&inputHandlerThread);
                threadResume(&audioHandlerThread);
                threadsSleeping = false;
            }
            if (isVideoActive(renderContext))
            {
                displayFrame(renderContext);
            }
            else
            {
                drawSplash(renderContext);
            }
            svcSleepThread(14285714ULL); // Nano sleep to keep at 70fps to handle drop frames without stutter
        }
        else
        {
            printf("sleeping\n");
            if (!threadsSleeping)
            {
                threadPause(&renderThread);
                threadPause(&inputHandlerThread);
                threadPause(&audioHandlerThread);
                threadsSleeping = true;
            }
            svcSleepThread(1000000000ULL);
        }

        u64 now_tick = armGetSystemTick();
        if ((double)armTicksToNs(now_tick - main_heartbeat_tick) / 1e9 >= 5.0)
        {
            // init() requests an overclock to 1785MHz via clkrstSetClockRate, which is a request,
            // not a guarantee - the SoC can silently thermal-throttle under sustained decode load.
            // clkrstGetClockRate reads what's actually in effect right now, so a downward trend
            // here over the session distinguishes throttling from other causes of degradation.
            u32 actual_clock_hz = 0;
            clkrstGetClockRate(&cpuSession, &actual_clock_hz);

            // Tracks process memory usage to distinguish slow resource exhaustion (rising trend)
            // from a sudden event, as a possible cause of degradation over a long session.
            u64 used_memory = 0;
            svcGetInfo(&used_memory, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
            orbitnx_log("main", "heartbeat: main loop alive, videoActive=%d, threadsSleeping=%d, cpuClockMHz=%u, usedMemoryMB=%.1f",
                       isVideoActive(renderContext), threadsSleeping, actual_clock_hz / 1000000, used_memory / 1024.0 / 1024.0);
            main_heartbeat_tick = now_tick;
        }
    }
    /* Deinitialize all used systems */
    unInit();
    return 0;
}
