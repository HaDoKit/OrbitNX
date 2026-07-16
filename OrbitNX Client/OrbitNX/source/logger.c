#include "logger.h"

#include <stdio.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <switch.h>

static FILE *s_log_file = NULL;
static u64 s_start_tick = 0;
// orbitnx_log is called from main/video/audio/input, each on its own thread - this mutex serializes
// fprintf/fflush calls against the same FILE* across all of them, since stdio's internal buffer
// state isn't guaranteed safe to mutate concurrently on this libc.
static Mutex s_log_mutex;
static bool s_log_mutex_initialized = false;
static u64 s_last_flush_tick = 0;

// Flushing on every call would serialize high-frequency logging (e.g. video.c's uncapped
// decode-failure logging under packet loss) through the mutex above, blocking the video decode
// thread on other threads' SD card writes during exactly the moments it can least afford to stall.
// Rate-limiting the actual flush (not the fprintf, which stays cheap/buffered) bounds how often
// any call touches the SD card, regardless of how bursty the callers are.
#define LOG_FLUSH_MIN_INTERVAL_NS 200000000ULL // 200ms

void orbitnx_log_init(void)
{
    mutexInit(&s_log_mutex);
    s_log_mutex_initialized = true;

    // mkdir on a path that already exists just fails harmlessly (checked via the file open below,
    // not this return value) - covers both "installed under sdmc:/switch/OrbitNX/" and "launched via
    // nxlink, that directory never existed" the same way audio.c's SDP handling had to.
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/OrbitNX", 0777);

    s_log_file = fopen("sdmc:/switch/OrbitNX/orbitnx.log", "w");
    s_start_tick = armGetSystemTick();

    if (s_log_file == NULL)
        printf("orbitnx_log_init: failed to open log file on sdmc:\n");
}

void orbitnx_log(const char *category, const char *fmt, ...)
{
    if (s_log_file == NULL) return;

    // See s_log_mutex's doc comment - serializes every call (from whichever thread) against every
    // other, so only one thread at a time is ever touching s_log_file or blocked on its SD card
    // I/O, instead of however many threads happened to call orbitnx_log at nearly the same moment.
    if (s_log_mutex_initialized) mutexLock(&s_log_mutex);

    double elapsed_s = (double)armTicksToNs(armGetSystemTick() - s_start_tick) / 1e9;
    fprintf(s_log_file, "[%8.3fs] [%s] ", elapsed_s, category);

    va_list args;
    va_start(args, fmt);
    vfprintf(s_log_file, fmt, args);
    va_end(args);

    fprintf(s_log_file, "\n");

    // See LOG_FLUSH_MIN_INTERVAL_NS's doc comment - only actually touches the SD card at most every
    // 200ms, however many log calls happen to land in between. Still flushes on every call in
    // practice for this app's normal heartbeat-paced logging (nothing calls orbitnx_log faster than
    // every 200ms outside of a burst), so crash survivability for the common case is unaffected.
    u64 now_tick = armGetSystemTick();
    if (armTicksToNs(now_tick - s_last_flush_tick) >= LOG_FLUSH_MIN_INTERVAL_NS)
    {
        fflush(s_log_file);
        s_last_flush_tick = now_tick;
    }

    if (s_log_mutex_initialized) mutexUnlock(&s_log_mutex);
}

void orbitnx_log_close(void)
{
    if (s_log_file != NULL)
    {
        fclose(s_log_file);
        s_log_file = NULL;
    }
}
