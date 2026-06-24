/*
 * opensles_shim.h -- OpenSL ES to SDL2 audio bridge
 *
 * Syberia uses OpenSL ES for audio. This shim translates OpenSL ES
 * BufferQueue audio players to SDL2 audio output.
 */

#ifndef __OPENSLES_SHIM_H__
#define __OPENSLES_SHIM_H__

#include <stdint.h>

// OpenSL ES types
typedef uint32_t SLresult;
typedef uint32_t SLuint32;
typedef int32_t SLint32;
typedef uint16_t SLuint16;
typedef int16_t SLint16;
typedef uint8_t SLuint8;
typedef int8_t SLint8;
typedef int32_t SLmillibel;
typedef uint32_t SLmillisecond;
typedef uint32_t SLBoolean;

#define SL_RESULT_SUCCESS ((SLresult)0x00000000)
#define SL_BOOLEAN_TRUE ((SLBoolean)0x00000001)
#define SL_BOOLEAN_FALSE ((SLBoolean)0x00000000)

// Play states
#define SL_PLAYSTATE_STOPPED ((SLuint32)0x00000001)
#define SL_PLAYSTATE_PAUSED ((SLuint32)0x00000002)
#define SL_PLAYSTATE_PLAYING ((SLuint32)0x00000003)

// Play events
#define SL_PLAYEVENT_HEADATEND ((SLuint32)0x00000001)

// Data format
#define SL_DATAFORMAT_PCM 2
#define SL_DATALOCATOR_BUFFERQUEUE 0x800007BD
#define SL_DATALOCATOR_OUTPUTMIX 4

// SL interface IDs (opaque pointers)
typedef const void *SLInterfaceID;

// These are the interface IDs that libsyberia1.so imports
extern const SLInterfaceID sl_IID_ENGINE;
extern const SLInterfaceID sl_IID_PLAY;
extern const SLInterfaceID sl_IID_VOLUME;
extern const SLInterfaceID sl_IID_BUFFERQUEUE;
extern const SLInterfaceID sl_IID_EFFECTSEND;

// slCreateEngine
SLresult slCreateEngine_shim(void **pEngine, SLuint32 numOptions,
                              const void *pEngineOptions,
                              SLuint32 numInterfaces,
                              const SLInterfaceID *pInterfaceIds,
                              const SLBoolean *pInterfaceRequired);

// Fire pending audio callbacks (call from game thread, e.g. ALooper_pollAll)
void opensles_shim_pump_callbacks(void);

#endif
