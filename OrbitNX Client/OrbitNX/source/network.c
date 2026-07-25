#include "network.h"
#include "logger.h"
#include <libavutil/timestamp.h>
#include <libavformat/avformat.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>

void network_init(const SocketInitConfig *conf)
{
    printf("socketInitialize\n");
    socketInitialize(conf);

    nxlinkStdio();
}

void network_unInit()
{
    socketExit();
}

JoyConSocket *createJoyConSocket()
{
    JoyConSocket *socket = (JoyConSocket *)malloc(sizeof(JoyConSocket));
    socket->sock = -1;
    socket->lissock = -1;
    return socket;
}

void freeJoyConSocket(JoyConSocket *connection)
{
    free(connection);
}

int connectJoyConSocket(JoyConSocket *connection, int port)
{
    if (connection->lissock == -1)
    {
        if (connection->sock != -1)
        {
            close(connection->sock);
            connection->sock = -1;
        }

        struct sockaddr_in server;

        connection->lissock = socket(AF_INET, SOCK_STREAM, 0);
        server.sin_family = AF_INET;
        server.sin_addr.s_addr = INADDR_ANY;
        server.sin_port = htons(port);

        if (bind(connection->lissock, (struct sockaddr *)&server, sizeof(server)) < 0)
        {
            orbitnx_log("network", "HID bind() on port %d failed, errno=%d", port, errno);
            close(connection->lissock);
            connection->lissock = -1;
            return 0;
        }

        listen(connection->lissock, 1);
        orbitnx_log("network", "HID listening on port %d", port);
    }

    if (connection->sock == -1)
    {
        // TODO: We might want to be able to not block if no client is connected
        struct sockaddr_in client;
        int c = sizeof(struct sockaddr_in);
        connection->sock = accept(connection->lissock, (struct sockaddr *)&client, (socklen_t *)&c);
        if (connection->sock < 0)
        {
            orbitnx_log("network", "HID accept() failed, errno=%d", errno);
            close(connection->lissock);
            connection->sock = -1;
            connection->lissock = -1;
            return 0;
        }
        orbitnx_log("network", "HID client connected");
    }
    return 1;
}

void sendJoyConInput(JoyConSocket *connection, const JoyPkg *pkg)
{
    if (send(connection->sock, pkg, sizeof(JoyPkg), 0) != sizeof(JoyPkg))
    {
        // Close the fd before dropping it - the Switch's socket/fd table is small, and each
        // reconnect that abandoned the old fd without closing it would leak one more.
        orbitnx_log("network", "HID send() failed, errno=%d - closing socket and waiting for reconnect", errno);
        close(connection->sock);
        connection->sock = -1;
    }
}

// --- shadPS4 game-library picker channel (see network.h for the protocol/timeout rationale) ---

LibrarySocket *createLibrarySocket(void)
{
    LibrarySocket *connection = (LibrarySocket *)malloc(sizeof(LibrarySocket));
    connection->sock = -1;
    connection->lissock = -1;
    return connection;
}

void freeLibrarySocket(LibrarySocket *connection)
{
    if (connection->sock != -1) close(connection->sock);
    if (connection->lissock != -1) close(connection->lissock);
    free(connection);
}

// Waits up to timeoutMs for fd to become readable. Returns >0 ready, 0 timeout, <0 error.
static int waitReadable(int fd, int timeoutMs)
{
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(fd, &readfds);
    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    return select(fd + 1, &readfds, NULL, NULL, &tv);
}

int acceptLibraryConnection(LibrarySocket *connection, int timeoutMs)
{
    if (connection->lissock == -1)
    {
        struct sockaddr_in server;
        connection->lissock = socket(AF_INET, SOCK_STREAM, 0);
        server.sin_family = AF_INET;
        server.sin_addr.s_addr = INADDR_ANY;
        server.sin_port = htons(LIBRARY_PORT);

        if (bind(connection->lissock, (struct sockaddr *)&server, sizeof(server)) < 0)
        {
            orbitnx_log("network", "library bind() on port %d failed, errno=%d", LIBRARY_PORT, errno);
            close(connection->lissock);
            connection->lissock = -1;
            return -1;
        }

        listen(connection->lissock, 1);
        orbitnx_log("network", "library listening on port %d", LIBRARY_PORT);
    }

    int ready = waitReadable(connection->lissock, timeoutMs);
    if (ready == 0) return 0; // timed out this attempt - caller retries or checks for cancel
    if (ready < 0)
    {
        orbitnx_log("network", "library select() on listener failed, errno=%d", errno);
        return -1;
    }

    struct sockaddr_in client;
    socklen_t c = sizeof(client);
    int accepted = accept(connection->lissock, (struct sockaddr *)&client, &c);
    if (accepted < 0)
    {
        orbitnx_log("network", "library accept() failed, errno=%d", errno);
        return -1;
    }

    connection->sock = accepted;
    orbitnx_log("network", "library client connected");
    return 1;
}

int recvLibraryExact(LibrarySocket *connection, void *buf, size_t len, int timeoutMs)
{
    size_t received = 0;
    uint8_t *p = (uint8_t *)buf;
    while (received < len)
    {
        int ready = waitReadable(connection->sock, timeoutMs);
        if (ready == 0) return 0; // timed out
        if (ready < 0)
        {
            orbitnx_log("network", "library recv select() failed, errno=%d", errno);
            return -1;
        }

        ssize_t n = recv(connection->sock, p + received, len - received, 0);
        if (n <= 0)
        {
            orbitnx_log("network", "library recv() failed/closed, n=%zd errno=%d", n, errno);
            return -1;
        }
        received += (size_t)n;
    }
    return 1;
}

int sendLibraryExact(LibrarySocket *connection, const void *buf, size_t len)
{
    size_t sent = 0;
    const uint8_t *p = (const uint8_t *)buf;
    while (sent < len)
    {
        ssize_t n = send(connection->sock, p + sent, len - sent, 0);
        if (n <= 0)
        {
            orbitnx_log("network", "library send() failed, errno=%d", errno);
            return -1;
        }
        sent += (size_t)n;
    }
    return 1;
}

void closeLibraryClientConnection(LibrarySocket *connection)
{
    if (connection->sock != -1)
    {
        close(connection->sock);
        connection->sock = -1;
    }
}
