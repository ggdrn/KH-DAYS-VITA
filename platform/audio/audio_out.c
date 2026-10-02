#include "audio_out.h"

#include "audio/snd7.h"
#include "log.h"
#include "nitro/arm7.h"

#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>

/* 256 frames: 5.3 ms, about one driver frame */
#define GRAIN 256

static int s_port = -1;

static void to_arm9(uint32_t word)
{
    kh_arm7_reply(7, word, 0); /* an alarm: the ARM9's PxiFifoCallback hands it to its handler */
}

static int audio_thread(SceSize args, void *argp)
{
    static int16_t buf[2][GRAIN * 2];
    int k = 0;
    (void)args;
    (void)argp;
    for (;;) {
        snd7_render(buf[k], GRAIN);
        sceAudioOutOutput(s_port, buf[k]);
        k ^= 1;
    }
    return 0;
}

void audio_out_init(void)
{
    SceUID th;
    snd7_send_to_arm9 = to_arm9;
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
    th = sceKernelCreateThread("kh_audio", audio_thread, 0x10000100 - 40, 0x8000, 0,
                               SCE_KERNEL_CPU_MASK_USER_2, NULL);
    if (th < 0 || sceKernelStartThread(th, 0, NULL) < 0) {
        LOG("audio: no thread (%08x): silent", th);
        return;
    }
    snd7_set_running(1);
    LOG("audio: 48 kHz stereo, %d-frame grain, ARM7 sound driver on core 2", GRAIN);
}
