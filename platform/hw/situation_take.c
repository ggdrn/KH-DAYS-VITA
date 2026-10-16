/* The one-screen situation's inputs (ui/situation.h), read from the game's own data by the
 * decomp's names, on the display thread. After khdays-port's port/ui/situation_take.c (MIT); the
 * offsets are its own (its docs/RUNTIME_MAP.md section 15), checked against this port's decomp. */
#include "hw/situation_take.h"
#include "hw/io.h"
#include "nitro/overlay.h"

#include <stdint.h>
#include <string.h>

extern int kh_is_game_heap(unsigned long a, unsigned long n); /* nitro/os.c */

/* The game's data the inputs read (decomp names). */
extern char gSceneCtl[];                  /* obj, entry, curId, pendId, pendArg */
extern unsigned char gPauseMode;
extern unsigned char data_0204c240;       /* the mission's kind */
extern uint32_t data_ov002_0207f9fc;      /* ov002: a tutorial's page */
extern uint32_t data_ov002_0207f614;      /* ov002: the mission panel's context */
extern uint32_t data_ov023_0208a784[];    /* ov023: the event's root, [1] its scene */
extern uint32_t data_ov022_020b2e78[];    /* ov022: the battle's root, [1] the party */
extern int data_ov008_02090f0c[];         /* ov008 (Mission Mode's camp): its screen index */
extern int data_ov025_020b574c[];         /* ov025 (the story's camp): its screen index */

/* The watched update functions, in KH_UPD_* order. */
extern void Ov000_TickMenuLoop(void);
extern void Ov000_TickLoadScene(void);
extern void Ov000_TickSelectionScene(void);
extern void Ov008_OpenShopState(void);
extern void Ov008_WaitShopClosed(void);
/* the boot logos' states (the scene object's update) */
extern void Ov000_LogoFadeState(void);
extern void Ov000_LogoFadeState2(void);
extern void Ov000_LogoFadeState3(void);
extern void Ov000_HandoffState(void);

#define SCENE_OBJECT 0x0u
#define SCENE_CUR 0x8u
#define SCENE_PENDING 0xcu
#define OBJECT_UPDATE 0x14u
#define MISSION_START_STATE 0x18cu
#define EVENT_STATUS 0x8758cu

#define OV_TITLE 0
#define OV_FIELD 2
#define OV_CAMP_MISSION 8
#define OV_BATTLE 22
#define OV_EVENT 23
#define OV_CAMP_STORY 25

/* Which watched updates ran: marked by the game's thread (Obj_UpdateAll, decomp patch), taken
 * here. A frame in which no object ran (the game steps every two VBlanks in its menus) keeps
 * the set before. */
static volatile uint32_t s_ran, s_any;
static uint32_t s_last;

void kh_situation_update_ran(int fn)
{
    static const uintptr_t *watched;
    static uintptr_t table[KH_UPDATES];
    int i;
    if (!watched) {
        table[KH_UPD_Ov000_TickMenuLoop] = (uintptr_t)Ov000_TickMenuLoop;
        table[KH_UPD_Ov000_TickLoadScene] = (uintptr_t)Ov000_TickLoadScene;
        table[KH_UPD_Ov000_TickSelectionScene] = (uintptr_t)Ov000_TickSelectionScene;
        table[KH_UPD_Ov008_OpenShopState] = (uintptr_t)Ov008_OpenShopState;
        table[KH_UPD_Ov008_WaitShopClosed] = (uintptr_t)Ov008_WaitShopClosed;
        watched = table;
    }
    s_any = 1;
    for (i = 0; i < KH_UPDATES; i++)
        if (watched[i] == (uintptr_t)(unsigned)fn) {
            __atomic_fetch_or(&s_ran, 1u << i, __ATOMIC_RELAXED);
            return;
        }
}

static uint32_t u32_at(uintptr_t a)
{
    return *(const volatile uint32_t *)a;
}

static uint8_t title_logo(void)
{
    const uintptr_t object = u32_at((uintptr_t)gSceneCtl + SCENE_OBJECT);
    uintptr_t update;
    if (!kh_is_game_heap(object, OBJECT_UPDATE + 4))
        return 0;
    update = u32_at(object + OBJECT_UPDATE);
    return update == (uintptr_t)Ov000_LogoFadeState    ? 1
           : update == (uintptr_t)Ov000_LogoFadeState2 ? 2
           : update == (uintptr_t)Ov000_LogoFadeState3 || update == (uintptr_t)Ov000_HandoffState ? 3
                                                                                                 : 0;
}

void kh_situation_take(KhSituationInputs *in, int movie, int dual)
{
    int n;
    memset(in, 0, sizeof(*in));
    if (__atomic_exchange_n(&s_any, 0, __ATOMIC_RELAXED))
        s_last = __atomic_exchange_n(&s_ran, 0, __ATOMIC_RELAXED);
    in->updates = s_last;
    in->scene = (int32_t)u32_at((uintptr_t)gSceneCtl + SCENE_CUR);
    in->pending = (int32_t)u32_at((uintptr_t)gSceneCtl + SCENE_PENDING);
    for (n = 0; n < 64; n++)
        if (kh_overlay_loaded(n))
            in->overlays |= 1ull << n;
    in->powcnt1 = KH_IO16(0x04000304);
    in->pause_mode = gPauseMode;
    in->movie = (uint8_t)(movie != 0);
    in->engine_swaps = dual ? 2u : 0u;
    in->mission_flags = data_0204c240;
    if (kh_overlay_loaded(OV_TITLE) && in->scene == 1)
        in->title_logo = title_logo();
    in->mission_start = -1;
    if (kh_overlay_loaded(OV_FIELD)) {
        const uintptr_t panel = data_ov002_0207f614;
        in->tutorial = data_ov002_0207f9fc;
        if (kh_is_game_heap(panel, MISSION_START_STATE + 4))
            in->mission_start = (int32_t)u32_at(panel + MISSION_START_STATE);
    }
    if (kh_overlay_loaded(OV_BATTLE)) {
        const uintptr_t root = data_ov022_020b2e78[1];
        in->party = kh_is_game_heap(root, 0x35) ? *(const volatile uint8_t *)(root + 0x34) : 0;
    }
    in->event_status = -1;
    if (kh_overlay_loaded(OV_EVENT)) {
        const uintptr_t scene = data_ov023_0208a784[1];
        if (kh_is_game_heap(scene, EVENT_STATUS + 2))
            in->event_status = *(const volatile uint16_t *)(scene + EVENT_STATUS);
    }
    in->camp_screen = KH_CAMP_NONE;
    if (kh_overlay_loaded(OV_CAMP_MISSION)) {
        in->camp_screen = (int8_t)data_ov008_02090f0c[0];
        in->camp_story = 0;
    } else if (kh_overlay_loaded(OV_CAMP_STORY)) {
        in->camp_screen = (int8_t)data_ov025_020b574c[0];
        in->camp_story = 1;
    }
}
