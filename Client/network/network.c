#include "network.h"

#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>

_Static_assert(sizeof(OrbitNxHidPacket) == ORBITNX_HID_PAYLOAD_SIZE,
    "OrbitNxHidPacket must stay exactly ORBITNX_HID_PAYLOAD_SIZE bytes - see network.h");
_Static_assert(sizeof(OrbitNxSessionConfig) == ORBITNX_SESSION_CONFIG_SIZE,
    "OrbitNxSessionConfig must stay exactly ORBITNX_SESSION_CONFIG_SIZE bytes - see network.h");

static const uint8_t s_hidMarker[ORBITNX_HID_MARKER_SIZE] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

static int s_listenSock = -1;
static int s_clientSock = -1;

// Guards the two fields below - written by networkAcceptHid() (network thread), read by
// networkWaitForSessionConfig() (called from video.c on the main thread, and potentially audio.c
// on its own thread).
static Mutex s_sessionConfigMutex;
static OrbitNxSessionConfig s_sessionConfig;
static bool s_sessionConfigReady = false;

static bool sendAll(int sock, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t sent = 0;
    while (sent < len)
    {
        ssize_t n = send(sock, p + sent, len - sent, 0);
        if (n <= 0)
            return false;
        sent += (size_t)n;
    }
    return true;
}

static bool recvAll(int sock, void *data, size_t len)
{
    uint8_t *p = (uint8_t *)data;
    size_t received = 0;
    while (received < len)
    {
        ssize_t n = recv(sock, p + received, len - received, 0);
        if (n <= 0)
            return false;
        received += (size_t)n;
    }
    return true;
}

bool networkInit(void)
{
    SocketInitConfig cfg = *socketGetDefaultInitConfig();
    // Concurrently open once HID/video/audio are all running: HID's listen socket + its accepted
    // client socket (2), video's UDP socket (1), audio's UDP socket (1), and - in a DEBUG build -
    // logger.c's nxlink debug connection (1) = 5 persistent sockets across 3 threads, previously
    // undercounted as "one per stream" (6 covered only 3). Confirmed live: running HID+video+audio
    // together crashed after ~2s under real concurrent load (not immediately at socket-open time),
    // consistent with transient bsd:u session pressure from overlapping blocking calls across
    // threads, not just the static socket count - raised well past the known persistent-socket
    // count to leave headroom for that concurrency, not just the minimum that fits today.
    cfg.num_bsd_sessions = 12;

    if (R_FAILED(socketInitialize(&cfg)))
        return false;

    mutexInit(&s_sessionConfigMutex);

    s_listenSock = socket(AF_INET, SOCK_STREAM, 0);
    if (s_listenSock < 0)
        return false;

    int reuse = 1;
    setsockopt(s_listenSock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(ORBITNX_HID_PORT);

    if (bind(s_listenSock, (struct sockaddr *)&addr, sizeof(addr)) != 0)
        return false;

    if (listen(s_listenSock, 1) != 0)
        return false;

    return true;
}

bool networkAcceptHid(void)
{
    if (s_listenSock < 0)
        return false;

    struct sockaddr_in clientAddr;
    socklen_t addrLen = sizeof(clientAddr);
    memset(&clientAddr, 0, sizeof(clientAddr));

    int sock = accept(s_listenSock, (struct sockaddr *)&clientAddr, &addrLen);
    if (sock < 0)
        return false;

    // Without this, Nagle's algorithm can batch/delay these small (208-byte), frequent (~43Hz)
    // sends waiting for an ACK - a well-known source of exactly the kind of several-hundred-ms
    // input lag confirmed live here, even though the host already sets NoDelay on its end
    // (SessionController.cs) - that only affects the host's own socket, not this one, and this
    // side is the one actually producing the small frequent sends.
    int noDelay = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &noDelay, sizeof(noDelay));

    // sendAll()'s send() calls are otherwise unbounded blocking - if the host ever stops draining
    // this socket for any reason (confirmed live: HID silently stopped responding after several
    // minutes, no error logged anywhere, consistent with the send thread being stuck inside a
    // blocking send() forever once the TCP send buffer filled), the thread would hang forever
    // with nothing to log, since it never gets back out of the syscall to log anything. This turns
    // that into a bounded wait: send() starts returning -1/EAGAIN-ish behavior once nothing drains
    // for this long, which sendAll() (via networkSendHid's caller) already treats as a failure -
    // triggering the normal reconnect path instead of hanging indefinitely.
    struct timeval sendTimeout;
    sendTimeout.tv_sec = 3;
    sendTimeout.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &sendTimeout, sizeof(sendTimeout));

    // The host (SessionController.cs) writes this immediately after connecting, before any
    // OrbitNxHidPacket traffic starts the other way - block reading it now, before this accept is
    // considered successful. A failure here (connection dropped mid-handshake, etc.) is treated
    // exactly like a failed accept - the caller goes back to waiting for a fresh connection.
    OrbitNxSessionConfig config;
    if (!recvAll(sock, &config, sizeof(config)))
    {
        close(sock);
        return false;
    }

    mutexLock(&s_sessionConfigMutex);
    s_sessionConfig = config;
    s_sessionConfigReady = true;
    mutexUnlock(&s_sessionConfigMutex);

    s_clientSock = sock;
    return true;
}

bool networkWaitForSessionConfig(OrbitNxSessionConfig *out)
{
    while (appletMainLoop())
    {
        mutexLock(&s_sessionConfigMutex);
        bool ready = s_sessionConfigReady;
        if (ready)
            *out = s_sessionConfig;
        mutexUnlock(&s_sessionConfigMutex);

        if (ready)
            return true;

        svcSleepThread(50000000ULL); // 50ms - only actually waited on before the first session ever connects
    }

    return false;
}

bool networkSendHid(const OrbitNxHidPacket *pkt)
{
    if (s_clientSock < 0)
        return false;

    if (!sendAll(s_clientSock, s_hidMarker, sizeof(s_hidMarker)))
        return false;

    return sendAll(s_clientSock, pkt, sizeof(*pkt));
}

void networkCloseHidClient(void)
{
    if (s_clientSock >= 0)
    {
        close(s_clientSock);
        s_clientSock = -1;
    }
}

void networkExit(void)
{
    networkCloseHidClient();

    if (s_listenSock >= 0)
    {
        close(s_listenSock);
        s_listenSock = -1;
    }

    socketExit();
}
