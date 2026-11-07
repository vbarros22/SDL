/*
  Simple DirectMedia Layer
  Copyright (C) 2018-2018 Ash Logan <ash@heyquark.com>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "../../SDL_internal.h"

#if SDL_AUDIO_DRIVER_WIIU

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#include "SDL_audio.h"
#include "SDL_error.h"
#include "SDL_timer.h"
#include "../SDL_audio_c.h"
#include "../SDL_audiodev_c.h"
#include "../SDL_sysaudio.h"
#include "SDL_wiiuaudio.h"
#include "SDL_wiiuaudio_mix.h"

#include <sndcore2/core.h>
#include <sndcore2/voice.h>
#include <sndcore2/drcvs.h>
#include <coreinit/core.h>
#include <coreinit/cache.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>

#define WIIUAUDIO_DRIVER_NAME "wiiu"

#define AX_MAIN_AFFINITY OS_THREAD_ATTRIB_AFFINITY_CPU1

static void _WIIUAUDIO_framecallback();
static SDL_AudioDevice* cb_this;
#define cb_hidden cb_this->hidden

/*  Some helpers for AX-related math */
/*  Absolute address to an AXVoiceOffsets offset */
#define calc_ax_offset(offs, addr) (((void*)addr - offs.data) \
    / sizeof_sample(offs))

#define sizeof_sample(offs) (offs.dataType == AX_VOICE_FORMAT_LPCM8 ? 1 : 2)

/*  +1, but never goes above NUM_BUFFERS */
#define next_id(id) (id + 1) % NUM_BUFFERS

static int WIIUAUDIO_OpenDevice(_THIS, const char* devname) {
    AXVoiceOffsets offs;
    AXVoiceVeData vol = {
        .volume = 0x8000,
    };
    uint32_t old_affinity;
    float srcratio;
    int result = 0;

    this->hidden = (struct SDL_PrivateAudioData*)SDL_malloc(sizeof(*this->hidden));
    if (this->hidden == NULL) return SDL_OutOfMemory();
    SDL_zerop(this->hidden);

/*  We *must not* change cores when setting stuff up */
    old_affinity = OSGetThreadAffinity(OSGetCurrentThread());
    OSSetThreadAffinity(OSGetCurrentThread(), AX_MAIN_AFFINITY);

/*  Take a quick aside to init the wiiu audio */
    if (!AXIsInit()) {
    /*  Init the AX audio engine */
        AXInitParams initparams = {
            .renderer = AX_INIT_RENDERER_48KHZ,
            .pipeline = AX_INIT_PIPELINE_SINGLE,
        };
        AXInitWithParams(&initparams);
    } else printf("DEBUG: AX already up?\n");

    if (this->spec.channels < 1) this->spec.channels = 1;
    if (this->spec.channels > WIIU_MAX_VALID_CHANNELS)
        this->spec.channels = WIIU_MAX_VALID_CHANNELS;

/*  Force wiiu-compatible audio formats.
    TODO verify - unsigned or signed? */
    switch (SDL_AUDIO_BITSIZE(this->spec.format)) {
        case 8:
        /*  TODO 8-bit audio sounds broken */
            /*this->spec.format = AUDIO_S8;
            break;*/
        case 16:
        default:
            this->spec.format = AUDIO_S16MSB;
            break;
    }

    //TODO maybe round this->spec.samples up even when >?
    //maybe even force at least 2* so we get more frame callbacks to think
    if (this->spec.samples < AXGetInputSamplesPerFrame()) {
        this->spec.samples = AXGetInputSamplesPerFrame();
    }

/*  We changed channels and samples, so recalculate the spec */
    SDL_CalculateAudioSpec(&this->spec);

/*  Allocate buffers for double-buffering and samples.
    All mixing buffers live in ONE contiguous allocation: AX expresses the
    loop/end offsets relative to the voice's data pointer (mixbufs[0]), so
    every buffer must sit after it in memory. Separate allocations could
    place mixbufs[1] below mixbufs[0] and produce bogus offsets.
    Align to the cache line size because AX reads these through DMA. */
    this->hidden->rawbuf = memalign(0x40, this->spec.size * NUM_BUFFERS);
    if (this->hidden->rawbuf == NULL) {
        result = SDL_OutOfMemory();
        goto done;
    }

    for (int i = 0; i < NUM_BUFFERS; i++) {
        this->hidden->mixbufs[i] = this->hidden->rawbuf + (this->spec.size * i);
    }
    memset(this->hidden->rawbuf, 0, this->spec.size * NUM_BUFFERS);
    DCStoreRange(this->hidden->rawbuf, this->spec.size * NUM_BUFFERS);

/*  Allocate a scratch buffer for deinterleaving operations */
    this->hidden->deintvbuf = SDL_malloc(this->spec.size);
    if (this->hidden->deintvbuf == NULL) {
        result = SDL_OutOfMemory();
        goto done;
    }


    for (int i = 0; i < this->spec.channels; i++) {
    /*  Get a voice, top priority */
        this->hidden->voice[i] = AXAcquireVoice(31, NULL, NULL);
        if (!this->hidden->voice[i]) {
            result = SDL_SetError("Couldn't acquire an AX voice");
            goto done;
        }

    /*  Start messing with it */
        AXVoiceBegin(this->hidden->voice[i]);
        AXSetVoiceType(this->hidden->voice[i], 0);

    /*  Set the voice's volume. */
        AXSetVoiceVe(this->hidden->voice[i], &vol);
        switch (this->spec.channels) {
            case 1: /* mono */ {
                AXSetVoiceDeviceMix(this->hidden->voice[i],
                    AX_DEVICE_TYPE_DRC, 0, mono_mix[i]);
                AXSetVoiceDeviceMix(this->hidden->voice[i],
                    AX_DEVICE_TYPE_TV, 0, mono_mix[i]);
            } break;
            case 2: /* stereo */ {
                AXSetVoiceDeviceMix(this->hidden->voice[i],
                    AX_DEVICE_TYPE_DRC, 0, stereo_mix[i]);
                AXSetVoiceDeviceMix(this->hidden->voice[i],
                    AX_DEVICE_TYPE_TV, 0, stereo_mix[i]);
            } break;
        }

    /*  Set the samplerate conversion ratio
        <source sample rate> / <target sample rate> */
        srcratio = (float)this->spec.freq / (float)AXGetInputSamplesPerSec();
        AXSetVoiceSrcRatio(this->hidden->voice[i], srcratio);
        AXSetVoiceSrcType(this->hidden->voice[i], AX_VOICE_SRC_TYPE_LINEAR);

    /*  Set up the offsets for the first mixbuf */
        switch (SDL_AUDIO_BITSIZE(this->spec.format)) {
            case 8:
                offs.dataType = AX_VOICE_FORMAT_LPCM8;
                offs.endOffset = this->spec.samples;
                break;
            case 16:
            default:
                offs.dataType = AX_VOICE_FORMAT_LPCM16;
                offs.endOffset = this->spec.samples;
                break;
        }
        offs.loopingEnabled = AX_VOICE_LOOP_ENABLED;
        offs.loopOffset = 0;
        offs.currentOffset = 0;

        if (offs.dataType == AX_VOICE_FORMAT_LPCM8) {
            offs.data = this->hidden->mixbufs[0]
                + this->spec.samples * i * sizeof(Uint8);
        } else if (offs.dataType == AX_VOICE_FORMAT_LPCM16) {
            offs.data = this->hidden->mixbufs[0]
                + this->spec.samples * i * sizeof(Uint16);
        }
        AXSetVoiceOffsets(this->hidden->voice[i], &offs);

    /*  Set the last good loopcount */
        this->hidden->last_loopcount = AXGetVoiceLoopCount(this->hidden->voice[i]);

    /*  Offsets are set for playing the first mixbuf, so we should render the second */
        this->hidden->playingid = 0;
        this->hidden->renderingid = 1;

    /*  Start playing. */
        AXSetVoiceState(this->hidden->voice[i], AX_VOICE_STATE_PLAYING);
        AXVoiceEnd(this->hidden->voice[i]);
    }

    cb_this = this; //wish there was a better way
    AXRegisterAppFrameCallback(_WIIUAUDIO_framecallback);

done:
/*  Put the thread affinity back to normal - we won't call any more AX funcs.
    This must also happen on failure, otherwise the calling thread (usually
    the game's main thread) stays pinned to CPU1. On failure SDL calls
    WIIUAUDIO_CloseDevice, which releases everything allocated above. */
    OSSetThreadAffinity(OSGetCurrentThread(), old_affinity);
    return result;
}

/*  Called every 3ms before a frame of audio is rendered. Keep it fast! */
static void _WIIUAUDIO_framecallback() {
    int playing_buffer = -1;
    AXVoiceOffsets offs[6];

    for (int i = 0; i < cb_this->spec.channels; i++) {
        AXGetVoiceOffsets(cb_hidden->voice[i], &offs[i]);
    }

/*  Figure out which buffer is being played by the hardware */
    for (int i = 0; i < NUM_BUFFERS; i++) {
        void* buf = cb_hidden->mixbufs[i];
        uint32_t startOffset = calc_ax_offset(offs[0], buf);
        uint32_t endOffset = startOffset + cb_this->spec.samples;

    /*  NOTE endOffset definitely needs to be <= (AX plays the sample at
        endOffset), dunno about startOffset */
        if (offs[0].currentOffset >= startOffset &&
            offs[0].currentOffset <= endOffset) {
            playing_buffer = i;
            break;
        }
    }

    if (playing_buffer < 0 || playing_buffer >= NUM_BUFFERS) {
    /*  UM */
    /*  Uncomment for craploads of debug info */
        /*printf("bad buffer %d\n" "|> %08X, %08X-%08X\n" \
            "0: xxxxxxxx, %08X-%08X (%08X@%08X)\n" \
            "1: xxxxxxxx, %08X-%08X (%08X@%08X)\n", \
            playing_buffer, offs.currentOffset, offs.loopOffset, offs.endOffset,
            calc_ax_offset(offs, (void*)cb_hidden->mixbufs[0]),
                calc_ax_offset(offs, (void*)cb_hidden->mixbufs[0] + cb_this->spec.size),
                cb_this->spec.size, (void*)cb_hidden->mixbufs[0],
            calc_ax_offset(offs, (void*)cb_hidden->mixbufs[1]),
                calc_ax_offset(offs, (void*)cb_hidden->mixbufs[1] + cb_this->spec.size),
                cb_this->spec.size, (void*)cb_hidden->mixbufs[1]);*/
        printf("DEBUG: Playing an invalid buffer? This is not a good sign.\n");
        playing_buffer = 0;
    }

/*  Make sure playingid is in sync with the hardware */
    cb_hidden->playingid = playing_buffer;

/* Update AX end and loop offsets for double buffering. */
    for (int i = 0; i < cb_this->spec.channels; i++) {
        Uint8 *endaddr;
        Uint8 *loopaddr;
        int loopTarget;

        endaddr = cb_hidden->mixbufs[cb_hidden->playingid]
            + cb_this->spec.samples * sizeof_sample(offs[i]) * (i + 1)
            - sizeof_sample(offs[i]);

        AXSetVoiceEndOffset(
            cb_hidden->voice[i],
            calc_ax_offset(offs[i], endaddr)
        );

        loopTarget = cb_hidden->playingid;
        if (cb_hidden->renderingid != next_id(cb_hidden->playingid)) {
            loopTarget = next_id(cb_hidden->playingid);
        }

        loopaddr = cb_hidden->mixbufs[loopTarget]
            + cb_this->spec.samples * sizeof_sample(offs[i]) * i;

        AXSetVoiceLoopOffset(
            cb_hidden->voice[i],
            calc_ax_offset(offs[i], loopaddr)
        );
    }
}

static void WIIUAUDIO_PlayDevice(_THIS) {
/*  Kick the voices once more after the first buffer has been mixed. Some
    setups leave them stopped after OpenDevice. Tracked per device (not in a
    static) so it also happens after the device is closed and reopened. */
    if (!this->hidden->voices_started) {
        this->hidden->voices_started = SDL_TRUE;

        for (int i = 0; i < this->spec.channels; i++) {
            if (this->hidden->voice[i]) {
                AXVoiceBegin(this->hidden->voice[i]);
                AXSetVoiceState(this->hidden->voice[i], AX_VOICE_STATE_PLAYING);
                AXVoiceEnd(this->hidden->voice[i]);
            }
        }
    }
/*  Deinterleave stereo audio */
    switch (SDL_AUDIO_BITSIZE(this->spec.format)) {
        case 8: {
            Uint8* samples = (Uint8*)this->hidden->mixbufs[this->hidden->renderingid];
            Uint8* deintv = (Uint8*)this->hidden->deintvbuf;

            /* Store the samples in a separate deinterleaved buffer */
            for (int ch = 0; ch < this->spec.channels; ch++) {
                for (int i = 0; i < this->spec.samples; i++) {
                    deintv[this->spec.samples * ch + i] = samples[i * this->spec.channels + ch];
                }
            }
        } break;
        case 16: {
            Uint16* samples = (Uint16*)this->hidden->mixbufs[this->hidden->renderingid];
            Uint16* deintv = (Uint16*)this->hidden->deintvbuf;

            /* Store the samples in a separate deinterleaved buffer */
            for (int ch = 0; ch < this->spec.channels; ch++) {
                for (int i = 0; i < this->spec.samples; i++) {
                    deintv[this->spec.samples * ch + i] = samples[i * this->spec.channels + ch];
                }
            }
        } break;
        default: {} break;
    }

/*  Copy the deinterleaved buffer to the mixing buffer */
    memcpy(
        this->hidden->mixbufs[this->hidden->renderingid],
        this->hidden->deintvbuf,
        this->spec.size
    );
/*  Comment this out for broken-record mode ;3 */
    DCStoreRange(this->hidden->mixbufs[this->hidden->renderingid], this->spec.size);
/*  Signal we're no longer rendering this buffer, AX callback will notice later */
    this->hidden->renderingid = next_id(this->hidden->renderingid);
}

static void WIIUAUDIO_WaitDevice(_THIS) {
/*  TODO use real thread sync stuff */
/*  Give up after a few buffer lengths so that a stalled AX (e.g. while the
    app is in the background) can never hang the audio thread or block
    SDL_CloseAudioDevice. If we give up, GetDeviceBuf notices that no buffer
    is free and lets SDL mix into its scratch buffer instead. */
    const Uint32 bufferMs = (this->spec.samples * 1000) / this->spec.freq;
    const Uint32 timeoutMs = SDL_max(bufferMs * 4, 50);
    const Uint32 waitStart = SDL_GetTicks();

    while (SDL_AtomicGet(&this->enabled) && !SDL_AtomicGet(&this->shutdown) &&
           this->hidden->renderingid == this->hidden->playingid) {
        if ((Uint32)(SDL_GetTicks() - waitStart) >= timeoutMs) {
            break;
        }

        OSSleepTicks(OSMillisecondsToTicks(3));
    }
}

static Uint8* WIIUAUDIO_GetDeviceBuf(_THIS) {
/*  If the hardware is still playing the buffer we would render into (the wait
    above timed out), return NULL: SDL then mixes into its own work buffer and
    sleeps for one buffer length, instead of us overwriting audio that is
    currently being played (which would be heard as a pop). */
    const int renderingid = this->hidden->renderingid;
    if (renderingid == this->hidden->playingid) {
        return NULL;
    }

/*  SDL will write audio samples into this buffer */
    return this->hidden->mixbufs[renderingid];
}

static void WIIUAUDIO_CloseDevice(_THIS) {
    if (AXIsInit()) {
        AXDeregisterAppFrameCallback(_WIIUAUDIO_framecallback);
        for (int i = 0; i < SIZEOF_ARR(this->hidden->voice); i++) {
            if (this->hidden->voice[i]) {
                AXFreeVoice(this->hidden->voice[i]);
                this->hidden->voice[i] = NULL;
            }
        }
        AXQuit();
    }
    /* rawbuf comes from memalign(), so release it with free() */
    if (this->hidden->rawbuf) {
        free(this->hidden->rawbuf);
        this->hidden->rawbuf = NULL;
    }
    if (this->hidden->deintvbuf) SDL_free(this->hidden->deintvbuf);
    SDL_free(this->hidden);
}

static void WIIUAUDIO_ThreadInit(_THIS) {
/*  Bump our thread's priority a bit */
    OSThread* currentThread = OSGetCurrentThread();
    int32_t priority = OSGetThreadPriority(currentThread);
    priority -= 1;
    OSSetThreadPriority(currentThread, priority);
}

static SDL_bool WIIUAUDIO_Init(SDL_AudioDriverImpl* impl) {
    impl->OpenDevice = WIIUAUDIO_OpenDevice;
    impl->PlayDevice = WIIUAUDIO_PlayDevice;
    impl->WaitDevice = WIIUAUDIO_WaitDevice;
    impl->GetDeviceBuf = WIIUAUDIO_GetDeviceBuf;
    impl->CloseDevice = WIIUAUDIO_CloseDevice;
    impl->ThreadInit = WIIUAUDIO_ThreadInit;

    impl->OnlyHasDefaultOutputDevice = SDL_TRUE;

    return SDL_TRUE;
}

AudioBootStrap WIIUAUDIO_bootstrap = {
    WIIUAUDIO_DRIVER_NAME, "Wii U AX Audio Driver", WIIUAUDIO_Init, 0,
};

#endif //SDL_AUDIO_DRIVER_WIIU
