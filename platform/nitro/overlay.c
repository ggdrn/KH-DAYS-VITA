/* Overlays on the port: all 303 are linked in, so "loading" one only has to do what reloading
 * its image did to its state, and the game must be handed host addresses where it expects the
 * overlay's load address.
 *
 *   FS_LoadOverlayImage  restores the overlay's .data from a snapshot taken before the game
 *                        started and clears its .bss (the generated DS-layout block and the C
 *                        statics), as reading a fresh image did.
 *   FS_StartOverlay      reports the overlay's entry function as FSOverlayInfo.ram_address (the
 *                        game jumps there: Ov107_LoadEnemyOverlay) and runs its static
 *                        initializer.
 *   FS_EndOverlay        runs and unlinks the global destructors registered from inside the
 *                        overlay, as the SDK does, with "inside" meaning the overlay's sections
 *                        in the port's link (build/gen/overlays.ld).
 * FS_LoadOverlayInfo, FS_LoadOverlay and FS_UnloadOverlay stay the SDK's. */
#include "nitro/overlay.h"

#include "hw/overlays.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>

extern uint32_t OS_DisableInterrupts(void);
extern uint32_t OS_RestoreInterrupts(uint32_t state);

/* FSOverlayInfo (include/nitro/fs.h) */
typedef struct {
    uint32_t id;
    uintptr_t ram_address;
    uint32_t ram_size;
    uint32_t bss_size;
    uintptr_t sinit_init;
    uintptr_t sinit_init_end;
    uint32_t file_id;
    uint32_t flags;
    uint32_t target;
    uint32_t file_offset, file_length;
} FSOverlayInfo;

/* __global_destructor_chain: {next, destructor, object} nodes (MSL's __register_global_object) */
typedef struct DtorNode {
    struct DtorNode *next;
    void (*fn)(void *);
    void *obj;
} DtorNode;
extern DtorNode *data_0204bd80;

static uint8_t **s_data_snapshot;

static const KhOverlay *find(uint32_t id)
{
    return id < (uint32_t)kh_overlay_count && kh_overlays[id].id == id ? &kh_overlays[id] : NULL;
}

static size_t range_len(const KhRange *r)
{
    return r->start ? (size_t)(r->end - r->start) : 0;
}

void kh_overlays_snapshot(void)
{
    int i;
    size_t total = 0;
    s_data_snapshot = calloc(kh_overlay_count, sizeof(*s_data_snapshot));
    for (i = 0; i < kh_overlay_count; i++) {
        const KhRange *r = &kh_overlays[i].host[KH_OV_DATA];
        size_t n = range_len(r);
        if (!n)
            continue;
        s_data_snapshot[i] = malloc(n);
        memcpy(s_data_snapshot[i], r->start, n);
        total += n;
    }
    LOG("overlay: .data snapshot of %d overlays, %u bytes", kh_overlay_count, (unsigned)total);
}

static void reset_state(const KhOverlay *ov)
{
    const KhRange *data = &ov->host[KH_OV_DATA], *bss = &ov->host[KH_OV_BSS];
    if (s_data_snapshot && s_data_snapshot[ov->id])
        memcpy(data->start, s_data_snapshot[ov->id], range_len(data));
    if (range_len(bss))
        memset(bss->start, 0, range_len(bss));
    if (ov->bss_start)
        memset(ov->bss_start, 0, (size_t)(ov->bss_end - ov->bss_start));
}

void FS_ClearOverlayImage(FSOverlayInfo *ovi)
{
    const KhOverlay *ov = find(ovi->id);
    if (ov)
        reset_state(ov);
}

int FS_LoadOverlayImage(FSOverlayInfo *ovi)
{
    const KhOverlay *ov = find(ovi->id);
    if (!ov) {
        LOG("overlay: load of unknown overlay %u", (unsigned)ovi->id);
        return 0;
    }
    reset_state(ov);
    return 1;
}

void FS_StartOverlay(FSOverlayInfo *ovi)
{
    const KhOverlay *ov = find(ovi->id);
    if (!ov)
        return;
    ovi->ram_address = (uintptr_t)ov->entry;
    if (ov->sinit)
        ov->sinit();
}

static int inside(const KhOverlay *ov, uintptr_t a)
{
    int k;
    if (ov->bss_start && a >= (uintptr_t)ov->bss_start && a < (uintptr_t)ov->bss_end)
        return 1;
    for (k = 0; k < KH_OV_KINDS; k++)
        if (ov->host[k].start && a >= (uintptr_t)ov->host[k].start && a < (uintptr_t)ov->host[k].end)
            return 1;
    return 0;
}

void FS_EndOverlay(FSOverlayInfo *ovi)
{
    const KhOverlay *ov = find(ovi->id);
    if (!ov)
        return;
    for (;;) {
        DtorNode *collected = NULL, *last = NULL, *prev = NULL, *node, *next;
        uint32_t irq = OS_DisableInterrupts();
        for (node = data_0204bd80; node; node = next) {
            next = node->next;
            if ((!node->obj && inside(ov, (uintptr_t)node->fn)) || inside(ov, (uintptr_t)node->obj)) {
                if (last)
                    last->next = node;
                else
                    collected = node;
                if (data_0204bd80 == node)
                    data_0204bd80 = next;
                node->next = NULL;
                last = node;
                if (prev)
                    prev->next = next;
            } else {
                prev = node;
            }
        }
        OS_RestoreInterrupts(irq);
        if (!collected)
            return;
        for (node = collected; node; node = next) {
            next = node->next;
            if (node->fn)
                node->fn(node->obj);
        }
    }
}
