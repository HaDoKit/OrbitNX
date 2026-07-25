#ifndef _NETWORK_H
#define _NETWORK_H

#include <switch.h>
#include "context.h"

// UDP, not TCP: a TCP connection to the host dies every ~7-20 seconds under load; UDP has no
// connection to drop - a lost packet just corrupts part of one frame instead of killing the
// stream. Host side matches with "-f mpegts udp://..." (see OrbitNX/Capture/VideoEncoder.cs)
// - MPEG-TS specifically because it's self-synchronizing (188-byte packets, sync bytes, continuity
// counters), so this demuxer can recover from lost UDP packets. overrun_nonfatal keeps the demuxer
// alive instead of erroring out if the receive buffer briefly overruns under load; fifo_size (in
// 188-byte-packet units) gives it more slack to absorb bursts.
#define URL "udp://0.0.0.0:2222?overrun_nonfatal=1&fifo_size=100000"

// Audio's own UDP port, separate from the video URL above - audio.c opens this directly via avio's
// udp:// protocol (see audio.c) and parses RTP/Opus itself rather than going through an avformat
// demuxer, so unlike video there's no SDP/URL string built from this constant elsewhere.
#define AUDIO_PORT 2224

// Keep in sync with the AppTarget enum in OrbitNX/Input/HidProtocol.cs (separate repo, no shared
// build) - chosen once via the on-screen menu (menu.c) before the input thread starts, then sent
// unchanged on every JoyPkg for the rest of the process's life.
typedef enum
{
    APP_TARGET_BIG_PICTURE = 0,
    APP_TARGET_SHADPS4 = 1,
} AppTarget;

/* Data to send to server */
typedef struct
{
    uint64_t streamStart;
    uint32_t heldKeys1;
    int32_t lJoyX1;
    int32_t lJoyY1;
    int32_t rJoyX1;
    int32_t rJoyY1;
    uint32_t heldKeys2;
    int32_t lJoyX2;
    int32_t lJoyY2;
    int32_t rJoyX2;
    int32_t rJoyY2;
    uint32_t heldKeys3;
    int32_t lJoyX3;
    int32_t lJoyY3;
    int32_t rJoyX3;
    int32_t rJoyY3;
    uint32_t heldKeys4;
    int32_t lJoyX4;
    int32_t lJoyY4;
    int32_t rJoyX4;
    int32_t rJoyY4;
    uint32_t touchX1;
    uint32_t touchY1;
    uint32_t touchX2;
    uint32_t touchY2;
    float_t accelX;
    float_t accelY;
    float_t accelZ;
    float_t gyroX;
    float_t gyroY;
    float_t gyroZ;
    uint32_t renderFPS;
    uint32_t controllerCount;
    // uint64_t, not uint32_t: controllerCount ends on an already-8-byte-aligned offset, so a
    // uint64_t here adds zero compiler padding before streamEnd. A uint32_t would force the AArch64
    // ABI to silently pad to keep streamEnd aligned, leaving 4 bytes of garbage inside what the PC
    // side (OrbitNX/Input/HidProtocol.cs) parses as payload.
    uint64_t appTarget;
    uint64_t streamEnd;
} JoyPkg;

// shadPS4 game-library picker protocol - separate TCP channel from the JoyPkg/HID one above
// (that one is fixed-size/fixed-rate/one-way, no room for variable-length titles+icons or a
// request/response shape). Keep every constant below numerically identical to
// OrbitNX/ShadPs4/LibraryProtocol.cs in the PC repo - no shared build, a mismatch fails at
// runtime, not compile time (same class of risk as the JoyPkg/HidPacket discrepancy already on
// record). Wire integers are read/written as raw native bytes (no explicit byte-swap) because
// both ends are little-endian (Switch is AArch64, PC is x86/x64) - the same implicit assumption
// JoyPkg itself already relies on.
#define LIBRARY_PORT 2225
#define LIBRARY_MAGIC0 'O'
#define LIBRARY_MAGIC1 'N'
#define LIBRARY_MAGIC2 'X'
#define LIBRARY_MAGIC3 'L'
#define LIBRARY_PROTOCOL_VERSION 1
#define LIBRARY_MSG_LIST 1
#define LIBRARY_MSG_SELECTION 2
#define LIBRARY_SELECTION_STATUS_SELECTED 0
#define LIBRARY_SELECTION_STATUS_CANCELLED 1
#define LIBRARY_MAX_GAMES 64
#define LIBRARY_TITLE_ID_WIRE_MAX 16
#define LIBRARY_TITLE_WIRE_MAX 512
#define LIBRARY_ICON_MAX_BYTES (4 * 1024 * 1024)
#define LIBRARY_SELECTION_MSG_SIZE 24
// In-memory buffer sizes (include the NUL terminator) - separate from the wire-max constants
// above since a title longer than this is truncated on store but still fully consumed off the
// socket (see library.c) so the stream stays aligned.
#define LIBRARY_TITLE_ID_BUF_LEN 17
#define LIBRARY_TITLE_BUF_LEN 128

typedef struct
{
    int lissock;
    int sock;
} LibrarySocket;

LibrarySocket *createLibrarySocket(void);

/* Closes both fds (if open) and frees the struct - full teardown, unlike freeJoyConSocket which
 * only ever frees the struct itself (JoyConSocket's fds live for the whole process). A fresh
 * LibrarySocket is created per runLibraryPickerScreen() call, so this is always a full teardown. */
void freeLibrarySocket(LibrarySocket *connection);

/* Idempotent bind+listen (mirrors connectJoyConSocket's lissock-reuse check), then waits up to
 * timeoutMs (via select(), not SO_RCVTIMEO on the listener - the latter is unproven for accept()
 * timeouts and this is more portably well-defined) for an incoming connection.
 * Returns 1 on a client accepted, 0 on timeout (caller should retry / check for cancel), -1 on a
 * hard error (caller should give up). This is the cancellable-wait mechanism the existing
 * connectJoyConSocket's own TODO (network.c) already flags as missing - applied only to this new,
 * separate socket; the real HID socket's blocking accept() is intentionally left untouched. */
int acceptLibraryConnection(LibrarySocket *connection, int timeoutMs);

/* Reads exactly len bytes, looping over short reads, waiting up to timeoutMs (via select()) before
 * each underlying recv(). Returns 1 on success, 0 on timeout, -1 on error/orderly close. */
int recvLibraryExact(LibrarySocket *connection, void *buf, size_t len, int timeoutMs);

/* Writes exactly len bytes, looping over short writes. Returns 1 on success, -1 on error. */
int sendLibraryExact(LibrarySocket *connection, const void *buf, size_t len);

/* Closes the accepted client socket only - the listener stays up. Not relied on today
 * (runLibraryPickerScreen tears down the whole LibrarySocket after one exchange via
 * freeLibrarySocket) but kept symmetric/reusable rather than baking in a one-shot assumption. */
void closeLibraryClientConnection(LibrarySocket *connection);

/* Init nx network and av network */
void network_init(const SocketInitConfig *conf);

/* Deinitialize nx network and av network*/
void network_unInit();

/* Creates the context for sending joycon inputs */
JoyConSocket *createJoyConSocket();

/* Deallocate from memory the constext used to sent joycon inputs */
void freeJoyConSocket(JoyConSocket *connection);

/* Send joycon input over the network */
void sendJoyConInput(JoyConSocket *connection, const JoyPkg *pkg);

/*
 * Binds, listens and accepts connection with the server
 * If the connection was previously opened reuses it
 */
int connectJoyConSocket(JoyConSocket *connection, int port);

#endif