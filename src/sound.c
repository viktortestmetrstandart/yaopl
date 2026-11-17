/*
 Copyright 2022, Thanks to SP193
 Licenced under Academic Free License version 3.0
 Review OpenPS2Loader README & LICENSE files for further details.
 */

#include <audsrv.h>
#include <vorbis/vorbisfile.h>

#include "include/sound.h"
#include "include/opl.h"
#include "include/ioman.h"
#include "include/themes.h"

// Silence unused variable warnings from vorbisfile.h
static ov_callbacks OV_CALLBACKS_NOCLOSE __attribute__((unused));
static ov_callbacks OV_CALLBACKS_STREAMONLY __attribute__((unused));
static ov_callbacks OV_CALLBACKS_STREAMONLY_NOCLOSE __attribute__((unused));
static ov_callbacks OV_CALLBACKS_DEFAULT __attribute__((unused));

/*--    Theme Sound Effects    ----------------------------------------------------------------------------------------------
---------------------------------------------------------------------------------------------------------------------------*/

extern unsigned char boot_adp[];
extern unsigned int size_boot_adp;

extern unsigned char cancel_adp[];
extern unsigned int size_cancel_adp;

extern unsigned char confirm_adp[];
extern unsigned int size_confirm_adp;

extern unsigned char cursor_adp[];
extern unsigned int size_cursor_adp;

extern unsigned char message_adp[];
extern unsigned int size_message_adp;

extern unsigned char transition_adp[];
extern unsigned int size_transition_adp;

extern unsigned char bd_connect_adp[];
extern unsigned int size_bd_connect_adp;

extern unsigned char bd_disconnect_adp[];
extern unsigned int size_bd_disconnect_adp;

struct sfxEffect
{
    const char *name;
    void *buffer;
    int size;
    int builtin;
    int duration_ms;
};

static struct sfxEffect sfx_files[SFX_COUNT] = {
    {"boot.adp"},
    {"cancel.adp"},
    {"confirm.adp"},
    {"cursor.adp"},
    {"message.adp"},
    {"transition.adp"},
    {"bd_connect.adp"},
    {"bd_disconnect.adp"},
};

static struct audsrv_adpcm_t sfx[SFX_COUNT];
static int audio_initialized = 0;

static int sfxRead(const char *full_path, struct sfxEffect *sfx)
{
    int adpcm;
    void *buffer;
    int ret, size;

    LOG("SFX: sfxRead('%s')\n", full_path);

    adpcm = open(full_path, O_RDONLY);
    if (adpcm < 0) {
        LOG("SFX: %s: Failed to open adpcm file %s\n", __FUNCTION__, full_path);
        return -ENOENT;
    }

    size = lseek(adpcm, 0, SEEK_END);
    lseek(adpcm, 0L, SEEK_SET);

    buffer = memalign(64, size);
    if (buffer == NULL) {
        LOG("SFX: Failed to allocate memory for SFX\n");
        close(adpcm);
        return -ENOMEM;
    }

    ret = read(adpcm, buffer, size);
    close(adpcm);

    if (ret != size) {
        LOG("SFX: Failed to read SFX: %d (expected %d)\n", ret, size);
        free(buffer);
        return -EIO;
    }

    sfx->buffer = buffer;
    sfx->size = size;
    sfx->builtin = 0;

    return 0;
}

static int sfxCalculateSoundDuration(int nSamples)
{
    float sampleRate = 44100; // 44.1kHz

    return (nSamples / sampleRate) * 1000;
}

static void sfxInitDefaults(void)
{
    int i;

    for (i = 0; i < SFX_COUNT; i++)
        sfx_files[i].builtin = 1;

    sfx_files[SFX_BOOT].buffer = boot_adp;
    sfx_files[SFX_BOOT].size = size_boot_adp;
    sfx_files[SFX_CANCEL].buffer = cancel_adp;
    sfx_files[SFX_CANCEL].size = size_cancel_adp;
    sfx_files[SFX_CONFIRM].buffer = confirm_adp;
    sfx_files[SFX_CONFIRM].size = size_confirm_adp;
    sfx_files[SFX_CURSOR].buffer = cursor_adp;
    sfx_files[SFX_CURSOR].size = size_cursor_adp;
    sfx_files[SFX_MESSAGE].buffer = message_adp;
    sfx_files[SFX_MESSAGE].size = size_message_adp;
    sfx_files[SFX_TRANSITION].buffer = transition_adp;
    sfx_files[SFX_TRANSITION].size = size_transition_adp;
    sfx_files[SFX_BD_CONNECT].buffer = bd_connect_adp;
    sfx_files[SFX_BD_CONNECT].size = size_bd_connect_adp;
    sfx_files[SFX_BD_DISCONNECT].buffer = bd_disconnect_adp;
    sfx_files[SFX_BD_DISCONNECT].size = size_bd_disconnect_adp;
}

static int sfxLoad(struct sfxEffect *sfxData, audsrv_adpcm_t *sfx)
{
    int ret;

    sfxData->duration_ms = sfxCalculateSoundDuration(((u32 *)sfxData->buffer)[3]);
    if (sfxData->duration_ms == 0)
        sfxData->duration_ms = sfxData->size / 47;

    ret = audsrv_load_adpcm(sfx, sfxData->buffer, sfxData->size);
    if (sfxData->builtin == 0) {
        free(sfxData->buffer);
        sfxData->buffer = NULL;
    }

    return ret;
}

int sfxInit(int bootSnd)
{
    char full_path[256];
    int ret, loaded;
    int i;

    if (!audio_initialized) {
        LOG("SFX: %s: ERROR: not initialized!\n", __FUNCTION__);
        return -1;
    }

    audsrv_adpcm_init();
    sfxInitDefaults();
    audioSetVolume();

    loaded = 0;
    i = bootSnd ? 0 : 1;
    for (; i < SFX_COUNT; i++) {
        snprintf(full_path, sizeof(full_path), "builtin/%s", sfx_files[i].name);

        ret = sfxLoad(&sfx_files[i], &sfx[i]);
        if (ret == 0) {
            LOG("SFX: Loaded %s, size=%d, duration=%dms\n", full_path, sfx_files[i].size, sfx_files[i].duration_ms);
            loaded++;
        } else {
            LOG("SFX: failed to load %s, error %d\n", full_path, ret);
        }
    }

    (void)&sfxRead;
    return loaded;
}

int sfxGetSoundDuration(int id)
{
    if (!audio_initialized) {
        LOG("SFX: %s: ERROR: not initialized!\n", __FUNCTION__);
        return 0;
    }

    return sfx_files[id].duration_ms;
}

void sfxPlay(int id)
{
    if (!audio_initialized) {
        LOG("SFX: %s: ERROR: not initialized!\n", __FUNCTION__);
        return;
    }

    if (gEnableSFX) {
        audsrv_ch_play_adpcm(id, &sfx[id]);
    }
}

/*--    External Background Music (Loaded into EE RAM)    ------------------------------------------------------------------
---------------------------------------------------------------------------------------------------------------------------*/

#define BGM_RING_BUFFER_COUNT 16
#define BGM_RING_BUFFER_SIZE  4096
#define BGM_THREAD_BASE_PRIO  0x40
#define BGM_THREAD_STACK_SIZE 0x1000
#define BGM_MAX_FILE_SIZE     (6 * 1024 * 1024)

extern void *_gp;

static int bgmThreadID, bgmIoThreadID;
static int outSema, inSema;
static s32 bgmStateSema = -1;
static unsigned char terminateFlag, bgmIsPlaying;
static volatile unsigned char bgmLaunchBlocked = 0;
static unsigned char rdPtr, wrPtr;
static char bgmBuffer[BGM_RING_BUFFER_COUNT][BGM_RING_BUFFER_SIZE] __attribute__((aligned(64)));
static volatile unsigned char bgmThreadRunning, bgmIoThreadRunning;

static u8 bgmThreadStack[BGM_THREAD_STACK_SIZE] __attribute__((aligned(16)));
static u8 bgmIoThreadStack[BGM_THREAD_STACK_SIZE] __attribute__((aligned(16)));

static OggVorbis_File *vorbisFile;
static unsigned char *bgmFileBuffer = NULL;

typedef struct
{
    const unsigned char *data;
    unsigned int size;
    unsigned int pos;
} bgm_mem_t;

static bgm_mem_t bgmMem;

static size_t bgmMemRead(void *ptr, size_t size, size_t nmemb, void *datasource)
{
    bgm_mem_t *mem = (bgm_mem_t *)datasource;
    size_t bytes = size * nmemb;
    size_t avail = (mem->pos < mem->size) ? (mem->size - mem->pos) : 0;

    if (bytes > avail)
        bytes = avail;

    if (bytes > 0) {
        memcpy(ptr, mem->data + mem->pos, bytes);
        mem->pos += bytes;
    }

    return (size > 0) ? (bytes / size) : 0;
}

static int bgmMemSeek(void *datasource, ogg_int64_t offset, int whence)
{
    bgm_mem_t *mem = (bgm_mem_t *)datasource;
    ogg_int64_t newpos;

    switch (whence) {
        case SEEK_SET:
            newpos = offset;
            break;
        case SEEK_CUR:
            newpos = (ogg_int64_t)mem->pos + offset;
            break;
        case SEEK_END:
            newpos = (ogg_int64_t)mem->size + offset;
            break;
        default:
            return -1;
    }

    if (newpos < 0 || newpos > (ogg_int64_t)mem->size)
        return -1;

    mem->pos = (unsigned int)newpos;
    return 0;
}

static int bgmMemClose(void *datasource)
{
    (void)datasource;
    return 0;
}

static long bgmMemTell(void *datasource)
{
    bgm_mem_t *mem = (bgm_mem_t *)datasource;
    return (long)mem->pos;
}

static void bgmThread(void *arg)
{
    bgmThreadRunning = 1;

    while (!terminateFlag) {
        SleepThread();

        while (PollSema(outSema) == outSema) {
            audsrv_wait_audio(BGM_RING_BUFFER_SIZE);
            audsrv_play_audio(bgmBuffer[rdPtr], BGM_RING_BUFFER_SIZE);
            rdPtr = (rdPtr + 1) % BGM_RING_BUFFER_COUNT;

            SignalSema(inSema);
        }
    }

    audsrv_stop_audio();

    rdPtr = 0;
    wrPtr = 0;

    bgmThreadRunning = 0;
    bgmIsPlaying = 0;
}

static void bgmIoThread(void *arg)
{
    int partsToRead, decodeTotal, bitStream, i;

    bgmIoThreadRunning = 1;
    do {
        WaitSema(inSema);
        partsToRead = 1;

        while ((wrPtr + partsToRead < BGM_RING_BUFFER_COUNT) && (PollSema(inSema) == inSema))
            partsToRead++;

        decodeTotal = BGM_RING_BUFFER_SIZE;
        int bufferPtr = 0;
        do {
            int ret = ov_read(vorbisFile, bgmBuffer[wrPtr] + bufferPtr, decodeTotal, 0, 2, 1, &bitStream);
            if (ret > 0) {
                bufferPtr += ret;
                decodeTotal -= ret;
            } else if (ret < 0) {
                LOG("BGM: I/O error while reading.\n");
                terminateFlag = 1;
                break;
            } else if (ret == 0)
                ov_pcm_seek(vorbisFile, 0);
        } while (decodeTotal > 0);

        wrPtr = (wrPtr + partsToRead) % BGM_RING_BUFFER_COUNT;
        for (i = 0; i < partsToRead; i++)
            SignalSema(outSema);
        WakeupThread(bgmThreadID);
    } while (!terminateFlag && gEnableBGM);

    bgmIoThreadRunning = 0;
    terminateFlag = 1;
    WakeupThread(bgmThreadID);
}

static int bgmLoad(void)
{
    const char *candidates[5];
    int count = 0;
    int i, fd, size, bytesRead;

    if (gDefaultBGMPath[0] != '\0')
        candidates[count++] = gDefaultBGMPath;
    candidates[count++] = "mc0:/OPL/DATA/bgm.ogg";
    candidates[count++] = "mc1:/OPL/DATA/bgm.ogg";
    candidates[count++] = "mc0:/OPL/bgm.ogg";
    candidates[count++] = "mc1:/OPL/bgm.ogg";

    for (i = 0; i < count; i++) {
        if (bgmLaunchBlocked)
            return -ECANCELED;

        fd = open(candidates[i], O_RDONLY);
        if (fd < 0)
            continue;

        size = lseek(fd, 0, SEEK_END);
        lseek(fd, 0L, SEEK_SET);
        if (size <= 0 || size > BGM_MAX_FILE_SIZE) {
            close(fd);
            continue;
        }

        bgmFileBuffer = memalign(64, size);
        if (bgmFileBuffer == NULL) {
            close(fd);
            return -ENOMEM;
        }

        bytesRead = read(fd, bgmFileBuffer, size);
        close(fd);

        if (bytesRead != size) {
            free(bgmFileBuffer);
            bgmFileBuffer = NULL;
            continue;
        }

        vorbisFile = malloc(sizeof(OggVorbis_File));
        if (vorbisFile == NULL) {
            free(bgmFileBuffer);
            bgmFileBuffer = NULL;
            return -ENOMEM;
        }
        memset(vorbisFile, 0, sizeof(OggVorbis_File));

        bgmMem.data = bgmFileBuffer;
        bgmMem.size = (unsigned int)size;
        bgmMem.pos = 0;

        ov_callbacks memCallbacks = {&bgmMemRead, &bgmMemSeek, &bgmMemClose, &bgmMemTell};
        if (ov_open_callbacks(&bgmMem, vorbisFile, NULL, 0, memCallbacks) < 0) {
            LOG("BGM: %s is not a valid Ogg bitstream.\n", candidates[i]);
            free(vorbisFile);
            vorbisFile = NULL;
            free(bgmFileBuffer);
            bgmFileBuffer = NULL;
            continue;
        }

        LOG("BGM: Loaded %s (%d bytes) into RAM.\n", candidates[i], size);
        return 0;
    }

    LOG("BGM: No external bgm.ogg found.\n");
    return -ENOENT;
}

static int bgmInit(void)
{
    ee_thread_t thread;
    ee_sema_t sema;
    int result;

    terminateFlag = 0;
    rdPtr = 0;
    wrPtr = 0;
    bgmThreadRunning = 0;
    bgmIoThreadRunning = 0;

    sema.max_count = BGM_RING_BUFFER_COUNT;
    sema.init_count = BGM_RING_BUFFER_COUNT;
    sema.attr = 0;
    sema.option = (u32) "bgm-in-sema";
    inSema = CreateSema(&sema);

    if (inSema >= 0) {
        sema.max_count = BGM_RING_BUFFER_COUNT;
        sema.init_count = 0;
        sema.attr = 0;
        sema.option = (u32) "bgm-out-sema";
        outSema = CreateSema(&sema);

        if (outSema < 0) {
            DeleteSema(inSema);
            return outSema;
        }
    } else
        return inSema;

    thread.func = &bgmThread;
    thread.stack = bgmThreadStack;
    thread.stack_size = sizeof(bgmThreadStack);
    thread.gp_reg = &_gp;
    thread.initial_priority = BGM_THREAD_BASE_PRIO;
    thread.attr = 0;
    thread.option = 0;

    bgmThreadID = CreateThread(&thread);

    if (bgmThreadID >= 0) {
        thread.func = &bgmIoThread;
        thread.stack = bgmIoThreadStack;
        thread.stack_size = sizeof(bgmIoThreadStack);
        thread.gp_reg = &_gp;
        thread.initial_priority = BGM_THREAD_BASE_PRIO + 1;
        thread.attr = 0;
        thread.option = 0;

        bgmIoThreadID = CreateThread(&thread);
        if (bgmIoThreadID >= 0) {
            result = 0;
        } else {
            DeleteSema(inSema);
            DeleteSema(outSema);
            DeleteThread(bgmThreadID);
            result = bgmIoThreadID;
        }
    } else {
        result = bgmThreadID;
        DeleteSema(inSema);
        DeleteSema(outSema);
    }

    return result;
}

static void bgmDeinit(void)
{
    DeleteSema(inSema);
    DeleteSema(outSema);
    DeleteThread(bgmThreadID);
    DeleteThread(bgmIoThreadID);

    if (vorbisFile != NULL) {
        ov_clear(vorbisFile);
        free(vorbisFile);
        vorbisFile = NULL;
    }

    if (bgmFileBuffer != NULL) {
        free(bgmFileBuffer);
        bgmFileBuffer = NULL;
    }
}

static void bgmShutdownDelayCallback(s32 alarm_id, u16 time, void *common)
{
    iWakeupThread((int)common);
}

void bgmSetBlocked(int blocked)
{
    bgmLaunchBlocked = blocked ? 1 : 0;
}

void bgmStart(void)
{
    struct audsrv_fmt_t audsrvFmt;

    if (!audio_initialized || bgmLaunchBlocked || !gEnableBGM)
        return;

    if (bgmStateSema >= 0)
        WaitSema(bgmStateSema);

    if (!audio_initialized || bgmIsPlaying || bgmLaunchBlocked || !gEnableBGM) {
        if (bgmStateSema >= 0)
            SignalSema(bgmStateSema);
        return;
    }

    int ret = bgmInit();
    if (ret >= 0) {
        if (bgmLoad() != 0) {
            bgmDeinit();
            if (bgmStateSema >= 0)
                SignalSema(bgmStateSema);
            return;
        }

        vorbis_info *vi = ov_info(vorbisFile, -1);
        ov_pcm_seek(vorbisFile, 0);

        audsrvFmt.channels = vi->channels;
        audsrvFmt.freq = vi->rate;
        audsrvFmt.bits = 16;

        audsrv_set_format(&audsrvFmt);
        audsrv_set_volume(gBGMVolume);

        bgmIsPlaying = 1;

        StartThread(bgmIoThreadID, NULL);
        StartThread(bgmThreadID, NULL);
    }

    if (bgmStateSema >= 0)
        SignalSema(bgmStateSema);
}

static void bgmStartDeferred(void)
{
    bgmStart();
}

void bgmStartAsync(void)
{
    if (!audio_initialized || bgmLaunchBlocked || !gEnableBGM)
        return;

    ioPutRequest(IO_CUSTOM_SIMPLEACTION, &bgmStartDeferred);
}

void bgmStop(void)
{
    int threadId;

    if (!audio_initialized) {
        LOG("BGM: %s: ERROR: not initialized!\n", __FUNCTION__);
        return;
    }

    if (bgmStateSema >= 0)
        WaitSema(bgmStateSema);

    if (!bgmIsPlaying) {
        if (bgmStateSema >= 0)
            SignalSema(bgmStateSema);
        return;
    }

    LOG("BGM: terminating threads...\n");

    terminateFlag = 1;
    WakeupThread(bgmThreadID);

    threadId = GetThreadId();
    while (bgmIoThreadRunning) {
        SetAlarm(200 * 16, &bgmShutdownDelayCallback, (void *)threadId);
        SleepThread();
    }
    while (bgmThreadRunning) {
        SetAlarm(200 * 16, &bgmShutdownDelayCallback, (void *)threadId);
        SleepThread();
    }

    bgmDeinit();

    if (bgmStateSema >= 0)
        SignalSema(bgmStateSema);

    LOG("BGM: stopped.\n");
}

int isBgmPlaying(void)
{
    return (int)bgmIsPlaying;
}

void bgmMute(void)
{
    if (audio_initialized)
        audsrv_set_volume(0);
}

void bgmUnMute(void)
{
    if (audio_initialized)
        audsrv_set_volume(gBGMVolume);
}

/*--    General Audio    ------------------------------------------------------------------------------------------------------
-----------------------------------------------------------------------------------------------------------------------------*/

void audioInit(void)
{
    if (!audio_initialized) {
        if (bgmStateSema < 0) {
            ee_sema_t sema;
            sema.init_count = 1;
            sema.max_count = 1;
            sema.attr = 0;
            sema.option = (u32) "bgm-state";
            bgmStateSema = CreateSema(&sema);
        }

        if (audsrv_init() != 0) {
            LOG("AUDIO: Failed to initialize audsrv\n");
            LOG("AUDIO: Audsrv returned error string: %s\n", audsrv_get_error_string());
            return;
        }
        audio_initialized = 1;
    }
}

void audioEnd(void)
{
    if (!audio_initialized) {
        LOG("AUDIO: %s: ERROR: not initialized!\n", __FUNCTION__);
        return;
    }

    bgmSetBlocked(1);
    if (isBgmPlaying())
        bgmStop();

    audsrv_quit();
    audio_initialized = 0;

    if (bgmStateSema >= 0) {
        DeleteSema(bgmStateSema);
        bgmStateSema = -1;
    }
}

void audioSetVolume(void)
{
    int i;

    if (!audio_initialized) {
        LOG("AUDIO: %s: ERROR: not initialized!\n", __FUNCTION__);
        return;
    }

    for (i = 1; i < SFX_COUNT; i++)
        audsrv_adpcm_set_volume(i, gSFXVolume);

    audsrv_adpcm_set_volume(0, gBootSndVolume);
    audsrv_set_volume(gBGMVolume);
}
