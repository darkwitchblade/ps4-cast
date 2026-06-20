// avplayer_abi.h — hand-written ABI for libSceAvPlayer.
//
// OpenOrbis only ships empty stub declarations (`void sceAvPlayerInit();`) plus
// the link stub libSceAvPlayer.so. We deliberately DO NOT include <orbis/AvPlayer.h>
// (its prototypes would conflict). The dynamic linker resolves these symbols by
// name against the stub, so the signatures below just need to match the real ABI.
//
// Layout follows the documented PS4/Vita SceAvPlayer SDK structures.
#ifndef PS4CAST_AVPLAYER_ABI_H
#define PS4CAST_AVPLAYER_ABI_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// IMPORTANT: the handle is a 64-bit pointer (shadPS4: `using AvPlayerHandle =
// AvPlayer*`). Declaring it 32-bit truncates the value returned by Init and
// crashes the next call (AddSource). Keep it 64-bit.
typedef int64_t SceAvPlayerHandle;

// ---- memory allocator callbacks --------------------------------------------
typedef void *(*SceAvPlayerAllocate)(void *argP, uint32_t argAlignment, uint32_t argSize);
typedef void  (*SceAvPlayerDeallocate)(void *argP, void *argMemory);
typedef void *(*SceAvPlayerAllocateTexture)(void *argP, uint32_t argAlignment, uint32_t argSize);
typedef void  (*SceAvPlayerDeallocateTexture)(void *argP, void *argMemory);

typedef struct SceAvPlayerMemAllocator {
    void *objectPointer;
    SceAvPlayerAllocate          allocate;
    SceAvPlayerDeallocate        deallocate;
    SceAvPlayerAllocateTexture   allocateTexture;
    SceAvPlayerDeallocateTexture deallocateTexture;
} SceAvPlayerMemAllocator;

// ---- file replacement callbacks (unused for URL streaming) -----------------
typedef int      (*SceAvPlayerOpenFile)(void *argP, const char *argFilename);
typedef int      (*SceAvPlayerCloseFile)(void *argP);
typedef int      (*SceAvPlayerReadOffsetFile)(void *argP, uint8_t *argBuffer, uint64_t argPosition, uint32_t argLength);
typedef uint64_t (*SceAvPlayerSizeFile)(void *argP);

typedef struct SceAvPlayerFileReplacement {
    void *objectPointer;
    SceAvPlayerOpenFile        open;
    SceAvPlayerCloseFile       close;
    SceAvPlayerReadOffsetFile  readOffset;
    SceAvPlayerSizeFile        size;
} SceAvPlayerFileReplacement;

// ---- event callback --------------------------------------------------------
typedef void (*SceAvPlayerEventCallback)(void *p, int32_t argEventId, int32_t argSourceId, void *argEventData);

typedef struct SceAvPlayerEventReplacement {
    void *objectPointer;
    SceAvPlayerEventCallback eventCallback;
} SceAvPlayerEventReplacement;

// ---- init data -------------------------------------------------------------
typedef struct SceAvPlayerInitData {
    SceAvPlayerMemAllocator     memoryReplacement;
    SceAvPlayerFileReplacement  fileReplacement;
    SceAvPlayerEventReplacement eventReplacement;
    int32_t   debugLevel;
    uint32_t  basePriority;
    int32_t   numOutputVideoFrameBuffers;
    uint8_t   autoStart;
    uint8_t   reserved[3];
    const char *defaultLanguage;
} SceAvPlayerInitData;

// ---- stream detail / frame info --------------------------------------------
typedef struct SceAvPlayerAudio {
    uint16_t channelCount;
    uint8_t  reserved[2];
    uint32_t sampleRate;
    uint32_t size;
    uint8_t  languageCode[4];
} SceAvPlayerAudio;

typedef struct SceAvPlayerVideo {
    uint32_t width;
    uint32_t height;
    float    aspectRatio;
    uint8_t  languageCode[4];
} SceAvPlayerVideo;

typedef struct SceAvPlayerTextPosition {
    uint16_t top;
    uint16_t left;
    uint16_t bottom;
    uint16_t right;
} SceAvPlayerTextPosition;

typedef struct SceAvPlayerTimedText {
    uint8_t  languageCode[4];
    uint16_t textSize;
    uint16_t fontSize;
    SceAvPlayerTextPosition position;
} SceAvPlayerTimedText;

typedef union SceAvPlayerStreamDetails {
    uint8_t          reserved[16];
    SceAvPlayerAudio audio;
    SceAvPlayerVideo video;
    SceAvPlayerTimedText subs;
} SceAvPlayerStreamDetails;

typedef struct SceAvPlayerFrameInfo {
    uint8_t                 *pData;
    uint32_t                 reserved;
    uint64_t                 timeStamp;
    SceAvPlayerStreamDetails details;
} SceAvPlayerFrameInfo;

// Event ids we care about.
#define SCE_AVPLAYER_STATE_STOP       0x01
#define SCE_AVPLAYER_STATE_READY      0x02
#define SCE_AVPLAYER_STATE_PLAY       0x03
#define SCE_AVPLAYER_STATE_PAUSE      0x04
#define SCE_AVPLAYER_STATE_BUFFERING  0x05
#define SCE_AVPLAYER_TIMED_TEXT_DELIVERY 0x10
#define SCE_AVPLAYER_WARNING_ID       0x20
#define SCE_AVPLAYER_ENCRYPTION       0x30

// ---- functions (resolved by name against libSceAvPlayer stub) --------------
SceAvPlayerHandle sceAvPlayerInit(SceAvPlayerInitData *data);
int32_t  sceAvPlayerPostInit(SceAvPlayerHandle handle, void *postInitData);
int32_t  sceAvPlayerAddSource(SceAvPlayerHandle handle, const char *filename);
int32_t  sceAvPlayerSetLooping(SceAvPlayerHandle handle, uint32_t loopFlag);
int32_t  sceAvPlayerStart(SceAvPlayerHandle handle);
int32_t  sceAvPlayerStop(SceAvPlayerHandle handle);
int32_t  sceAvPlayerPause(SceAvPlayerHandle handle);
int32_t  sceAvPlayerResume(SceAvPlayerHandle handle);
int32_t  sceAvPlayerClose(SceAvPlayerHandle handle);
bool     sceAvPlayerIsActive(SceAvPlayerHandle handle);
bool     sceAvPlayerGetVideoData(SceAvPlayerHandle handle, SceAvPlayerFrameInfo *frameInfo);
bool     sceAvPlayerGetAudioData(SceAvPlayerHandle handle, SceAvPlayerFrameInfo *frameInfo);
uint64_t sceAvPlayerCurrentTime(SceAvPlayerHandle handle);

#ifdef __cplusplus
}
#endif

#endif
