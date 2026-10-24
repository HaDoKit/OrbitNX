#include "logger.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

#include <libavutil/log.h>

#ifdef DEBUG
#include <unistd.h>
#endif

static Mutex s_logMutex;

#ifdef DEBUG
static int s_nxlinkFd = -1;
#endif

bool logInit(void)
{
    mutexInit(&s_logMutex);

#ifdef DEBUG
    // Requires the BSD socket service to already be up - see network.h's networkInit().
    int fd = nxlinkStdio();
    if (fd < 0)
        return false;

    s_nxlinkFd = fd;
    logWrite("logger", "nxlink debug console connected");
#endif

    return true;
}

void logWrite(const char *category, const char *fmt, ...)
{
    mutexLock(&s_logMutex);

    printf("[%s] ", category);

    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);

    printf("\n");

    mutexUnlock(&s_logMutex);
}

static void ffmpegLogCallback(void *avcl, int level, const char *fmt, va_list args)
{
    // Exception to the ERROR-only filter below: libavformat's UDP "Circular buffer overrun"
    // is logged at WARNING, and it's the one direct on-device proof that video datagrams were
    // dropped on this side (rather than lost over Wi-Fi). Checked on the raw format string, so
    // every other warning still costs nothing beyond this strstr.
    bool isUdpOverrun = level == AV_LOG_WARNING && strstr(fmt, "overrun") != NULL;
    if (level > av_log_get_level() && !isUdpOverrun)
        return;

    char message[256];
    vsnprintf(message, sizeof(message), fmt, args);

    // av_log messages conventionally end with their own '\n' - strip it, logWrite() adds one.
    size_t len = strlen(message);
    while (len > 0 && (message[len - 1] == '\n' || message[len - 1] == '\r'))
        message[--len] = '\0';

    (void)avcl;
    logWrite("ffmpeg", "%s", message);
}

void logInstallFfmpegCallback(void)
{
    av_log_set_callback(ffmpegLogCallback);

    // Confirmed live: at the default level (INFO), NVTEGRA's decode path logs an "alignment,
    // falling back to cpu transfer" line on nearly every frame - tens of times a second, each one
    // going through logWrite()'s mutex and (in a DEBUG build) the nxlink socket. That overhead was
    // itself observed contributing to audio glitches (audio.c's raw socket has no large buffer of
    // its own, so brief CPU starvation drops packets instead of just adding latency). ERROR keeps
    // genuine failures visible without the per-frame noise.
    av_log_set_level(AV_LOG_ERROR);
}

void logExit(void)
{
#ifdef DEBUG
    if (s_nxlinkFd >= 0)
    {
        close(s_nxlinkFd);
        s_nxlinkFd = -1;
    }
#endif
}
