#include "audio_out.h"

#include "audio/snd7.h"
#include "log.h"
#include "nitro/arm7.h"
#include "threadstat.h"

#include <psp2/audioout.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

/* 320 frames at 32 kHz (10 ms, about two driver frames); 512 at 48 kHz (10.7 ms) */
#define GRAIN_32K 320
#define GRAIN_48K 512
static int s_grain = GRAIN_48K;

static int s_port = -1;

static void to_arm9(uint32_t word)
{
    kh_arm7_reply(7, word, 0); /* an alarm: the ARM9's PxiFifoCallback hands it to its handler */
}

static uint64_t clock_us(void)
{
    return sceKernelGetProcessTimeWide();
}

/* the thread's own run time inside sceAudioOutOutput (us): with the rest of its run time,
 * what the render costs and what the output does (audio_out_take_stats) */
static volatile uint32_t s_output_run_us, s_output_wall_us;

static int audio_thread(SceSize args, void *argp)
{
    static int16_t buf[2][GRAIN_48K * 2];
    int k = 0;
    (void)args;
    (void)argp;
    for (;;) {
        uint64_t r, w;
        snd7_render(buf[k], s_grain);
        r = threadstat_self_us();
        w = sceKernelGetProcessTimeWide();
        sceAudioOutOutput(s_port, buf[k]);
        s_output_run_us += (uint32_t)(threadstat_self_us() - r);
        s_output_wall_us += (uint32_t)(sceKernelGetProcessTimeWide() - w);
        k ^= 1;
    }
    return 0;
}

void audio_out_take_stats(uint32_t *output_run_us, uint32_t *output_wall_us)
{
    *output_run_us = s_output_run_us;
    *output_wall_us = s_output_wall_us;
    s_output_run_us = s_output_wall_us = 0;
}

void audio_out_init(void)
{
    SceUID th;
    snd7_send_to_arm9 = to_arm9;
    snd7_clock_us = clock_us;
    /* the DS's rate on the BGM port (the system resamples it), else 48 kHz on the main one */
    s_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, GRAIN_32K, 32000,
                                 SCE_AUDIO_OUT_MODE_STEREO);
    if (s_port >= 0) {
        snd7_rate = 32000;
        s_grain = GRAIN_32K;
    } else {
        LOG("audio: no 32 kHz BGM port (%08x), 48 kHz on the main one", s_port);
        s_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, GRAIN_48K, 48000,
                                     SCE_AUDIO_OUT_MODE_STEREO);
        snd7_rate = 48000;
        s_grain = GRAIN_48K;
    }
    if (s_port < 0) {
        LOG("audio: no output port (%08x): silent", s_port);
        return;
    }
    {
        int vol[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
        sceAudioOutSetVolume(s_port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);
    }
    th = sceKernelCreateThread("kh_audio", audio_thread, 0x10000100 - 30, 0x8000, 0,
                               SCE_KERNEL_CPU_MASK_USER_2, NULL);
    if (th < 0 || sceKernelStartThread(th, 0, NULL) < 0) {
        LOG("audio: no thread (%08x): silent", th);
        return;
    }
    threadstat_add("audio", th);
    snd7_set_running(1);
    LOG("audio: %d Hz stereo, %d-frame grain, ARM7 sound driver on core 2", snd7_rate, s_grain);
}
