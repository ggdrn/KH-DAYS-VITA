/* ov024's MobiClip payloads that this game never runs, in place of their ROM machine code (left
 * out of the build, tools/decomp_sources.py):
 *
 *   - the deblocking post-filter, which Ov024_MobiClip_BlitFrame calls only in display modes
 *     1 and 2 (KH Days always plays in mode 0);
 *   - FastAudio (audio coding 2), which Ov024_MobiClip_StepAudio dispatches to only for such
 *     streams (KH Days' use IMA ADPCM).
 *
 * The frame decoder itself is the portable model in libs/mobiclip/video/portable. If either of
 * these is reached after all, the log says so instead of running DS code. */
#include "log.h"

void func_ov024_02092e60_unk(void *request)
{
    static int logged;
    (void)request;
    if (!logged++)
        LOG("mobiclip: deblocking filter requested (display mode 1/2): not available, skipped");
}

int func_ov024_02087318_unk(void *track)
{
    static int logged;
    (void)track;
    if (!logged++)
        LOG("mobiclip: FastAudio stream: not available, track left silent");
    return 0;
}

/* The movie player's busy-wait (Ov024_MobiClip_UpdatePlayback): whether its last pass over the
 * stream slots did any work (Ov024_MobiClip_TickSlot sets it); with none, it sleeps until an
 * interrupt (kh_cpu_wait_irq) instead of spinning. */
volatile int kh_mobiclip_progress;
volatile unsigned int kh_mobiclip_passes; /* the loop's passes, for the log */
