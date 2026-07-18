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