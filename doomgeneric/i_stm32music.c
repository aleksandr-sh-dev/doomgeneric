/*
 * i_stm32music.c - doomgeneric music module for STM32
 */
#include <stddef.h>

#include "doomtype.h"
#include "i_sound.h"

#include "doomgeneric_audio.h"

/* Referenced by i_sound.h / m_config.c (used to be defined in i_sdlmusic.c) */
char *timidity_cfg_path = "";

static boolean I_STM32_InitMusic(void)
{
    return true;
}

static void I_STM32_ShutdownMusic(void)
{
    dg_mix_set_music(NULL);
}

static void I_STM32_SetMusicVolume(int volume)
{
    dg_mix_set_music_volume(volume);    /* 0..127 */
}

static void I_STM32_PauseSong(void)  { }
static void I_STM32_ResumeSong(void) { }

static void *I_STM32_RegisterSong(void *data, int len)
{
    (void)data; (void)len;
    return NULL;
}

static void I_STM32_UnRegisterSong(void *handle) { (void)handle; }
static void I_STM32_PlaySong(void *handle, boolean looping) { (void)handle; (void)looping; }
static void I_STM32_StopSong(void) { }
static boolean I_STM32_MusicIsPlaying(void) { return false; }
static void I_STM32_PollMusic(void) { }

static snddevice_t music_stm32_devices[] =
{
    SNDDEVICE_PAS,
    SNDDEVICE_GUS,
    SNDDEVICE_WAVEBLASTER,
    SNDDEVICE_SOUNDCANVAS,
    SNDDEVICE_GENMIDI,
    SNDDEVICE_AWE32,
};

music_module_t DG_music_module =
{
    music_stm32_devices,
    arrlen(music_stm32_devices),
    I_STM32_InitMusic,
    I_STM32_ShutdownMusic,
    I_STM32_SetMusicVolume,
    I_STM32_PauseSong,
    I_STM32_ResumeSong,
    I_STM32_RegisterSong,
    I_STM32_UnRegisterSong,
    I_STM32_PlaySong,
    I_STM32_StopSong,
    I_STM32_MusicIsPlaying,
    I_STM32_PollMusic,
};
