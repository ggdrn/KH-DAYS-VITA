/* What the game shows this frame, for the one-screen picture (config single_screen 2): a
 * situation and its states, decided from what the game runs -- its scene and overlays, its
 * objects' update functions, the state the decomp names -- and a few checks of the screens'
 * pixels (hw/onescreen.c). Ported from khdays-port's port/ui/situation.h (MIT,
 * assets/onescreen_ref): the same situations and states, so its compositions file reads here
 * unchanged. Plain C: no Vita headers. */
#ifndef KH_UI_SITUATION_H
#define KH_UI_SITUATION_H

#include <stdint.h>

enum {
    KH_SIT_INTRO,
    KH_SIT_TITLE,
    KH_SIT_LOAD_MENU,
    KH_SIT_SAVE_MENU,
    KH_SIT_SETTINGS,
    KH_SIT_DAY_COUNTER,
    KH_SIT_MOVIE,
    KH_SIT_FIELD,
    KH_SIT_CAMP_MENU,
    KH_SIT_PAUSE,
    KH_SIT_TUTORIAL,
    KH_SIT_FIELD_3D_BOTH,
    KH_SIT_MISSION_REVIEW,
    KH_SIT_SHOP,
    KH_SIT_THOUGHTS,
    KH_SIT_WORLD_SELECTOR,
    KH_SIT_GAME_OVER,
    KH_SIT_THE_END,
    KH_SIT_UNKNOWN,
    KH_SITUATIONS
};

/* The states, a bit each (the compositions' conditions). */
enum {
    KH_ST_HUD,
    KH_ST_DIALOG,
    KH_ST_CHALLENGE,
    KH_ST_INFO_UPPER,
    KH_ST_INFO_LOWER,
    KH_ST_LOWER_3D,      /* the 3D on the lower screen alone */
    KH_ST_PARTY_4,       /* four members in the battle's party */
    KH_ST_INFO_NEW,      /* the lower screen's INFORMATION line changed a moment ago (pixels) */
    KH_ST_MISSION_PANEL, /* the lower screen shows the mission's panel (pixels) */
    KH_ST_TITLE_LIT,     /* the HD title's (khdays-port): never set here */
    KH_ST_EVENT,         /* the pause opened in an event of the field (ov023 running) */
    KH_STATES
};

/* The game's update functions the decision watches, a bit each (hw/situation_take.c). */
enum {
    KH_UPD_Ov000_TickMenuLoop,
    KH_UPD_Ov000_TickLoadScene,
    KH_UPD_Ov000_TickSelectionScene,
    KH_UPD_Ov008_OpenShopState,
    KH_UPD_Ov008_WaitShopClosed,
    KH_UPDATES
};

/* What the decision reads of the game, taken by hw/situation_take.c on the display thread. */
typedef struct KhSituationInputs {
    uint32_t updates;      /* bit KH_UPD_*: those that ran in the game's last frame */
    int32_t scene;         /* gSceneCtl's current scene id (+8) */
    int32_t pending;       /* and its pending one (+0xc) */
    uint64_t overlays;     /* bit n: overlay n loaded (below 64) */
    uint32_t powcnt1;      /* bit 15: engine A on the upper screen */
    uint8_t pause_mode;    /* gPauseMode */
    uint8_t movie;         /* a MobiClip movie plays */
    uint8_t mission_flags; /* data_0204c240: bit 1 a challenge after the story's camp */
    uint8_t party;         /* the battle's party members (ov022's root +0x34) */
    int32_t mission_start; /* the field's mission panel state (data_ov002_0207f614 +0x18c), -1 none */
    int32_t event_status;  /* the field's event status (ov023 scene +0x8758c), -1 without the event */
    uint32_t engine_swaps; /* the game shows its 3D on both screens (engine A swapping each VBlank) */
    int8_t camp_screen;    /* the loaded camp's screen (data_ov008_02090f0c / data_ov025_020b574c),
                            * KH_CAMP_NONE without a camp */
    uint8_t camp_story;    /* 1: the story's camp (ov025), 0 Mission Mode's (ov008) */
    uint32_t tutorial;     /* data_ov002_0207f9fc: a tutorial's page is open */
    uint8_t title_logo;    /* the title's boot logos' screen (1-3), else 0 */
} KhSituationInputs;

#define KH_CAMP_NONE (-2)

typedef struct KhSituation {
    int scene;        /* KH_SIT_* */
    uint32_t states;  /* 1 << KH_ST_* */
    uint32_t content; /* the screens that show something (1 the upper, 2 the lower) */
} KhSituation;

/* This frame's situation from the inputs (scene and the states the game's data gives; the pixel
 * states are added by hw/onescreen.c). */
void kh_situation_decide(const KhSituationInputs *in, KhSituation *out);

/* The HUD's modes: complete, exploration (no INFORMATION banner) and none. */
enum { KH_HUD_COMPLETE, KH_HUD_EXPLORATION, KH_HUD_NONE, KH_HUD_MODES };
int kh_situation_hud_mode(void);
void kh_situation_hud_mode_next(void);

/* Names, as the compositions file writes them; -1 for an unknown one. */
const char *kh_situation_name(int scene);
const char *kh_situation_state_name(int state);
int kh_situation_named(const char *name, int length);
int kh_situation_state_named(const char *name, int length);

#endif
