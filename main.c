#include <bslug.h>
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <rvl/vi.h>
#include <rvl/OSThread.h>
#include <rvl/cache.h>
#include <rvl/OSTime.h>
#include <rvl/so.h>

BSLUG_MODULE_GAME("????");
BSLUG_MODULE_NAME("WiiSee");
BSLUG_MODULE_VERSION("1.0");
BSLUG_MODULE_AUTHOR("InvoxiPlayGames");
BSLUG_MODULE_LICENSE("GPL");

// VI registers
static char* const VIRegisters = (char*)0xCC002000;
// whether GXDrawDone should copy out the framebuffer
static char gHandleFrame = 0;
// whether our thread should send out the framebuffer
static char gFrameReady = 0;
// the timestamp that the frame was fetched
static clock_t gFrameTimestamp = 0;
// our current frame state
#define FRAMEBUFFER_MAX_SIZE 0xCA800
static char gFrameBuffer[FRAMEBUFFER_MAX_SIZE] = { 0 };
static int gFrameSize = 0;
static int gFrameHeight = 0;
static int gFrameWidth = 0;
static int sOffset = 0;
// current network state
static so_fd_t gSocket = 0;
static char gConnected = 0;
// thread
#define THREAD_SIZE 0x1000
static char ThreadStack[THREAD_SIZE] = { 0 };
static OSThread_t Thread = { 0 };
// not in OSThread.h?
int OSResumeThread(OSThread_t *thread);
void OSSleepTicks(uint64_t ticks);
// LZO
#include "minilzo.h"
static char LZOWorkMem[LZO1X_1_MEM_COMPRESS] = { 0 };

// Networking code
#include "network.h"
#define FB_SEGMENT_SIZE 0x520
#define PROTOCOL_VERSION 0x00
#define WELCOME_CONNECTION 0x00
#define NEW_FRAME_PACKET 0x01
#define FRAME_UPDATE_PACKET 0x02
#define TIMESTAMP_PACKET 0x03
typedef struct _NewFrame {
    short Width;
    short Height;
    int FrameSize;
    int CompressedSize;
    int SegmentSize;
} NewFrame;
typedef struct _FrameUpdate {
    int Offset;
    char Data[FB_SEGMENT_SIZE];
} FrameUpdate;
typedef struct _Timestamp {
    clock_t RenderTimestamp;
    clock_t SendStartedTimestamp;
    clock_t SendFinishedTimestamp;
} Timestamp;
typedef struct _Header {
    char ProtocolVersion;
    char PacketType;
    short MaxPacketSize;
    int FrameID;
} Header;
static struct {
    Header Head;
    union {
        NewFrame New;
        FrameUpdate Update;
        Timestamp Time;
    } Data;
} __attribute__((packed)) OutgoingPacket = { 0 };
static char gFrameBufferCompressed[FRAMEBUFFER_MAX_SIZE + FRAMEBUFFER_MAX_SIZE / 16 + 64 + 3] = { 0 };

static so_ret_t SetNonBlocking(so_fd_t fd) {
	int fcntl = SOFcntl(fd, F_GETFL, 0);
	so_ret_t r = fcntl < 0 ? fcntl : SOFcntl(fd, F_SETFL, fcntl | O_NONBLOCK);
	return r;
}
static void * HandlingThread(void *a) {
    clock_t cClockTime = 0;
    so_ret_t r = SO_OK;
    so_addr_t sBindAddr = {
		.sa_len = sizeof(so_addr_t),
		.sa_family = AF_INET,
		.sa_port = CLIENT_PORT,
		.sa_addr = 0
	};
    so_addr_t sConnectAddr = {
		.sa_len = sizeof(so_addr_t),
		.sa_family = AF_INET,
		.sa_port = SERVER_PORT,
		.sa_addr = SERVER_IP
	};
    printf("WiiSee starting\n");
    gSocket = SOSocket(PF_INET, SOCK_DGRAM, PROTO_STREAM_UDP);
    if (gSocket < SO_OK) {
        printf("SOSocket err:%i\n", gSocket);
        return 0;
    }
    if ((r = SOBind(gSocket, &sBindAddr)), r < SO_OK) {
        printf("SOBind err:%i\n", r);
        return 0;
    }
    OutgoingPacket.Head.ProtocolVersion = PROTOCOL_VERSION;
    OutgoingPacket.Head.PacketType = WELCOME_CONNECTION;
    OutgoingPacket.Head.MaxPacketSize = sizeof(OutgoingPacket);
    if ((r = SOSendTo(gSocket, &OutgoingPacket, sizeof(Header), 0, &sConnectAddr)), r < SO_OK) {
        printf("SOSendTo err:%i\n", r);
        SOShutdown(gSocket, 0);
        return 0;
    }
    if ((r = SetNonBlocking(gSocket)), r < SO_OK) {
        printf("SetNonBlocking err:%i\n", r);
        SOShutdown(gSocket, SHUT_RDWR);
        return 0;
    }
    gConnected = 1;
    printf("WiiSee sending\n");
    while (1) {
        if (gFrameReady) {
            gFrameReady = 0;
            cClockTime = OSGetTime();
            OutgoingPacket.Head.FrameID = gFrameTimestamp;
            // compress frame
            lzo_uint compressedFrameSize = sizeof(gFrameBufferCompressed);
            lzo1x_1_compress((const unsigned char *)gFrameBuffer, gFrameSize, (unsigned char *)gFrameBufferCompressed, &compressedFrameSize, LZOWorkMem);
            // send new frame packet
            OutgoingPacket.Head.PacketType = NEW_FRAME_PACKET;
            OutgoingPacket.Data.New.Width = gFrameWidth;
            OutgoingPacket.Data.New.Height = gFrameHeight;
            OutgoingPacket.Data.New.FrameSize = gFrameSize;
            OutgoingPacket.Data.New.CompressedSize = compressedFrameSize;
            OutgoingPacket.Data.New.SegmentSize = FB_SEGMENT_SIZE;
            SOSendTo(gSocket, &OutgoingPacket, sizeof(Header) + sizeof(NewFrame), 0, &sConnectAddr);
            // send frame update packet(s)
            OutgoingPacket.Head.PacketType = FRAME_UPDATE_PACKET;
            for (int i = 0; i < compressedFrameSize; i += FB_SEGMENT_SIZE) {
                OutgoingPacket.Data.Update.Offset = i;
                memcpy(OutgoingPacket.Data.Update.Data, gFrameBufferCompressed + i, FB_SEGMENT_SIZE);
                SOSendTo(gSocket, &OutgoingPacket, sizeof(Header) + sizeof(int) + FB_SEGMENT_SIZE, 0, &sConnectAddr);
            }
            // send timestamp packet
            OutgoingPacket.Head.PacketType = TIMESTAMP_PACKET;
            OutgoingPacket.Data.Time.RenderTimestamp = gFrameTimestamp;
            OutgoingPacket.Data.Time.SendStartedTimestamp = cClockTime;
            OutgoingPacket.Data.Time.SendFinishedTimestamp = OSGetTime();
            r = SOSendTo(gSocket, &OutgoingPacket, sizeof(Header) + sizeof(Timestamp), 0, &sConnectAddr);
            if (r < SO_OK) break;
        } else {
            gHandleFrame = 1;
        }
        OSSleepTicks(182250 * 25); // ~25ms
    }
    gConnected = 0;
    printf("Framebuffer SOSend err:%i\n", r);
    SOShutdown(gSocket, SHUT_RDWR);
    return 0;
}

static void VIFlushHook() {
    // if we aren't going to handle a frame, do original
    if (gHandleFrame == 0) {
        VIFlush();
        return;
    }
    // if we are going to handle a frame, make sure we don't next frame
    gHandleFrame = 0;
    // keep note of when the frame was taken
    gFrameTimestamp = OSGetTime();

    // get information from VI registers
    int sWidth = (VIRegisters[0x49] << 3);
    int sHeight = (((VIRegisters[0] << 5) | (VIRegisters[1] >> 3)) & 0x07FE);
    sOffset = ((VIRegisters[0x1D] << 16) | (VIRegisters[0x1E] << 8) | VIRegisters[0x1F]);
    if ((VIRegisters[0x1C] & 0x10) == 0x10) sOffset <<= 5;
    sOffset += 0x80000000;
    sOffset -= ((VIRegisters[0x1C] & 0xF) << 3);

    // set the size and dimensions of the framebuffer
    gFrameSize = sHeight * sWidth * 2;
    gFrameHeight = sHeight;
    gFrameWidth = sWidth;
    if (gFrameHeight > 600) {
        gFrameHeight /= 2;
        gFrameWidth *= 2;
    }
    memcpy(gFrameBuffer, (void *)sOffset, gFrameSize);
    // do original
    VIFlush();
    // we're ready to send our frame
    gFrameReady = 1;
}
BSLUG_MUST_REPLACE(VIFlush, VIFlushHook);

// Socket hackery
static so_ret_t SOStartupExHook(int timeout) {
    so_ret_t o = SOStartupEx(timeout);
    if (o == SO_EALREADY) o = SO_OK;
    if (o == SO_OK && gConnected == 0) {
        OSCreateThread(&Thread, HandlingThread, 0, ThreadStack + THREAD_SIZE, THREAD_SIZE, 0x18, false);
	    OSResumeThread(&Thread);
    }
    return o;
}
static so_ret_t SOInitHook(so_alloc_t *alloc) {
    so_ret_t o = SOInit(alloc);
    if (o == SO_EALREADY) o = SO_OK;
    return o;
}
// don't allow the library to deinitialise so we stay connected
static so_ret_t SOCleanupHook() {
    if (gConnected) return SO_OK;
    return SOCleanup();
}
static so_ret_t SOFinishHook() {
    if (gConnected) return SO_OK;
    return SOFinish();
}
// brainslug hooks
BSLUG_MUST_REPLACE(SOInit, SOInitHook);
BSLUG_MUST_REPLACE(SOStartupEx, SOStartupExHook);
BSLUG_MUST_REPLACE(SOCleanup, SOCleanupHook);
BSLUG_MUST_REPLACE(SOFinish, SOFinishHook);