/*
 * i_stm32sound.c - doomgeneric sound-effect module for STM32
 */
#include <stdio.h>
#include <string.h>

#include "deh_str.h"
#include "doomtype.h"
#include "i_sound.h"
#include "i_system.h"
#include "m_misc.h"
#include "w_wad.h"
#include "z_zone.h"

#include "doomgeneric_audio.h"

#ifndef DG_SFX_KEEP_RESIDENT
#define DG_SFX_KEEP_RESIDENT    0
#endif

#define NUM_CHANNELS            DG_AUDIO_MAX_SFX_CHANNELS
#define SFX_CACHE_SIZE          192     /* distinct sfx lumps; Doom 2 uses 109 */

/* Config variables that i_sound.c / m_config.c reference when FEATURE_SOUND is defined
 * (they used to live in i_sdlsound.c). */
int   use_libsamplerate   = 0;
float libsamplerate_scale = 0.65f;

typedef struct {
    boolean         used;
    int             lumpnum;
    int             use_count;      /* channels currently playing from this lump */
    byte           *lump;           /* NULL when released to PU_CACHE */
    const byte     *pcm;            /* 8-bit unsigned mono samples (inside lump) */
    uint32_t        len;
    uint32_t        rate;
} sfx_entry_t;

static sfx_entry_t  sfx_cache[SFX_CACHE_SIZE];  /* keyed by lump (linked sfx share lumps) */
static sfx_entry_t *channels_playing[NUM_CHANNELS];

static boolean use_sfx_prefix;
static boolean sound_initialized = false;

/* ------------------------------------------------------------------------- */

static void GetSfxLumpName(sfxinfo_t *sfx, char *buf, size_t buf_len)
{
    /* Linked sfx lumps? Get the lump number for the sound linked to. */
    if (sfx->link != NULL) {
        sfx = sfx->link;
    }

    /* Doom adds a DS* prefix to sound lumps; Heretic and Hexen don't. */
    if (use_sfx_prefix) {
        M_snprintf(buf, buf_len, "ds%s", DEH_String(sfx->name));
    } else {
        M_StringCopy(buf, DEH_String(sfx->name), buf_len);
    }
}

/*
 * DMX sound lump layout:
 *   u16 format (=3) | u16 rate | u32 length | 16 pad bytes | samples... | 16 pad bytes
 * `length` counts everything after the 8-byte header, padding included.
 */
static boolean ParseSfxLump(byte *lump, unsigned int lumplen,
                            const byte **pcm, uint32_t *len, uint32_t *rate)
{
    if (lumplen < 8 || lump[0] != 0x03 || lump[1] != 0x00) {
        return false;
    }

    uint32_t samplerate = (lump[3] << 8) | lump[2];
    uint32_t length = ((uint32_t)lump[7] << 24) | (lump[6] << 16) | (lump[5] << 8) | lump[4];

    /* Same sanity rules as the SDL port / DMX. */
    if (length > lumplen - 8 || length <= 48 || samplerate == 0) {
        return false;
    }

    *pcm  = lump + 8 + 16;
    *len  = length - 32;
    *rate = samplerate;
    return true;
}

static sfx_entry_t *SfxLock(int lumpnum)
{
    sfx_entry_t *e = NULL, *free_slot = NULL;

    for (int i = 0; i < SFX_CACHE_SIZE; i++) {
        if (sfx_cache[i].used && sfx_cache[i].lumpnum == lumpnum) {
            e = &sfx_cache[i];
            break;
        }
        if (!sfx_cache[i].used && free_slot == NULL) {
            free_slot = &sfx_cache[i];
        }
    }

    if (e == NULL) {
        if (free_slot == NULL) {
            return NULL;
        }
        e = free_slot;
        memset(e, 0, sizeof(*e));
        e->used    = true;
        e->lumpnum = lumpnum;
    }

    if (e->lump == NULL) {
        byte *lump = W_CacheLumpNum(lumpnum, PU_STATIC);
        if (!ParseSfxLump(lump, W_LumpLength(lumpnum), &e->pcm, &e->len, &e->rate)) {
            Z_ChangeTag(lump, PU_CACHE);
            return NULL;
        }
        e->lump = lump;
    }

    e->use_count++;
    return e;
}

static void SfxUnlock(sfx_entry_t *e)
{
    if (e->use_count <= 0) {
        I_Error("Sound effect released more times than it was locked...");
    }

    if (--e->use_count == 0 && !DG_SFX_KEEP_RESIDENT) {
        Z_ChangeTag(e->lump, PU_CACHE);
        e->lump = NULL;
    }
}

static void ReleaseChannel(int channel)
{
    if (channels_playing[channel] != NULL) {
        /* Order matters: silence the voice first so the mixer can't read a purged lump. */
        dg_mix_sfx_stop(channel);
        SfxUnlock(channels_playing[channel]);
        channels_playing[channel] = NULL;
    }
}

/* ------------------------------------------------------------------------- */
/* sound_module_t                                                            */
/* ------------------------------------------------------------------------- */

static boolean I_STM32_InitSound(boolean _use_sfx_prefix)
{
    use_sfx_prefix = _use_sfx_prefix;

    memset(sfx_cache, 0, sizeof(sfx_cache));
    memset(channels_playing, 0, sizeof(channels_playing));

    sound_initialized = true;
    return true;
}

static void I_STM32_ShutdownSound(void)
{
    if (!sound_initialized) {
        return;
    }
    for (int i = 0; i < NUM_CHANNELS; i++) {
        ReleaseChannel(i);
    }
    sound_initialized = false;
}

static int I_STM32_GetSfxLumpNum(sfxinfo_t *sfx)
{
    char namebuf[9];

    GetSfxLumpName(sfx, namebuf, sizeof(namebuf));
    return W_GetNumForName(namebuf);
}

static void I_STM32_UpdateSound(void)
{
    /* Voices that ran off the end of their sample were deactivated by the mixer (IRQ);
     * drop their lump references here, on the main thread. */
    for (int i = 0; i < NUM_CHANNELS; i++) {
        if (channels_playing[i] != NULL && !dg_mix_sfx_playing(i)) {
            ReleaseChannel(i);
        }
    }
}

static void I_STM32_UpdateSoundParams(int channel, int vol, int sep)
{
    if (!sound_initialized || channel < 0 || channel >= NUM_CHANNELS) {
        return;
    }
    dg_mix_sfx_params(channel, vol, sep);
}

static int I_STM32_StartSound(sfxinfo_t *sfxinfo, int channel, int vol, int sep)
{
    if (!sound_initialized || channel < 0 || channel >= NUM_CHANNELS) {
        return -1;
    }

    /* Release a sound effect if there is already one playing on this channel */
    ReleaseChannel(channel);

    sfx_entry_t *e = SfxLock(sfxinfo->lumpnum);
    if (e == NULL) {
        return -1;
    }

    if (dg_mix_sfx_start(channel, e->pcm, e->len, e->rate, vol, sep) != 0) {
        SfxUnlock(e);
        return -1;
    }

    channels_playing[channel] = e;
    return channel;
}

static void I_STM32_StopSound(int channel)
{
    if (!sound_initialized || channel < 0 || channel >= NUM_CHANNELS) {
        return;
    }
    ReleaseChannel(channel);
}

static boolean I_STM32_SoundIsPlaying(int channel)
{
    if (!sound_initialized || channel < 0 || channel >= NUM_CHANNELS) {
        return false;
    }
    return dg_mix_sfx_playing(channel);
}

static void I_STM32_PrecacheSounds(sfxinfo_t *sounds, int num_sounds)
{
    /* Nothing to do: lumps are used in place, no conversion step. */
    (void)sounds;
    (void)num_sounds;
}

static snddevice_t sound_stm32_devices[] =
{
    SNDDEVICE_SB,
    SNDDEVICE_PAS,
    SNDDEVICE_GUS,
    SNDDEVICE_WAVEBLASTER,
    SNDDEVICE_SOUNDCANVAS,
    SNDDEVICE_AWE32,
};

sound_module_t DG_sound_module =
{
    sound_stm32_devices,
    arrlen(sound_stm32_devices),
    I_STM32_InitSound,
    I_STM32_ShutdownSound,
    I_STM32_GetSfxLumpNum,
    I_STM32_UpdateSound,
    I_STM32_UpdateSoundParams,
    I_STM32_StartSound,
    I_STM32_StopSound,
    I_STM32_SoundIsPlaying,
    I_STM32_PrecacheSounds,
};
