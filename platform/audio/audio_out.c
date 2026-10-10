#include "audio_out.h"

#include "audio/snd7.h"
#include "log.h"
#include "nitro/arm7.h"
#include "threadstat.h"

#include <psp2/audioout.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

/* 512 frames: 10.7 ms, about two driver frames. sceAudioOutOutput costs the thread CPU time
 * of its own on every call, more than the render at 256 (0.6.8's log: ~0.5 s of each 10 s
 * at 187 calls a second); half the calls, half of that */
#define GRAIN 512

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
    static int16_t buf[2][GRAIN * 2];
    int k = 0;
    (void)args;
    (void)argp;
    for (;;) {
        uint64_t r, w;
        snd7_render(buf[k], GRAIN);
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
    s_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, GRAIN, SND7_RATE,
                                 SCE_AUDIO_OUT_MODE_STEREO);
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
    LOG("audio: 48 kHz stereo, %d-frame grain, ARM7 sound driver on core 2", GRAIN);
}
