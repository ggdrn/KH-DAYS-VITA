/* The decision of what the game shows, for the one-screen picture (situation.h). Ported from
 * khdays-port's port/ui/situation_decide.c and situation.c (MIT): the rules and where each was
 * measured are its own (its docs/RUNTIME_MAP.md section 15.6; "play walk" its regression walks).
 * Plain C. */
#include "situation.h"

#include <stddef.h>
#include <string.h>

/* The scenes (gSceneCtl's ids). */
enum {
    SCENE_TITLE = 1,
    SCENE_FIELD = 2,
    SCENE_DAY_CARD = 5,
    SCENE_RESULTS = 6,
    SCENE_SELECT = 7,
    SCENE_MONOLOGUE = 10,
    SCENE_OPENING = 11,
    SCENE_CAMP = 19
};

/* The camp's screens (its screen index): 0 the menu and -1 for a frame between two; Mission
 * Mode's 1 Holo-Missions, 2 Panels (the menu's layout), 5 Preferences; the story's 4 Save. */
enum { CAMP_MENU = 0, CAMP_HOLO_MISSIONS = 1, CAMP_STORY_SAVE = 4, CAMP_PREFERENCES = 5, CAMP_BETWEEN = -1 };

/* The overlays the rules name. */
#define OV_SHOP 26      /* the moogle's shop, a sub-scene of the field */
#define OV_GAME_OVER 27 /* the game over */
#define OV_CREDITS 11   /* the credits */

static int previous = KH_SIT_UNKNOWN;
/* the last camp the game had loaded was the story's (ov025): the field after it is the story's */
static int story_camp = 1;
static int hud_mode = KH_HUD_COMPLETE;

int kh_situation_hud_mode(void)
{
    return hud_mode;
}

void kh_situation_hud_mode_next(void)
{
    hud_mode = (hud_mode + 1) % KH_HUD_MODES;
}

static int ran(const KhSituationInputs *in, int which)
{
    return (int)((in->updates >> which) & 1u);
}

static int loaded(const KhSituationInputs *in, int overlay)
{
    return (int)((in->overlays >> overlay) & 1u);
}

static int title_screens(const KhSituationInputs *in)
{
    /* the save list: its states run */
    if (ran(in, KH_UPD_Ov000_TickLoadScene) || ran(in, KH_UPD_Ov000_TickSelectionScene))
        return KH_SIT_LOAD_MENU;
    /* the boot logos */
    if (in->title_logo != 0)
        return KH_SIT_INTRO;
    return KH_SIT_TITLE;
}

static int camp_screens(const KhSituationInputs *in)
{
    /* Mission Mode's shop: no screen of the camp's (-1) while it is open */
    if (ran(in, KH_UPD_Ov008_OpenShopState) || ran(in, KH_UPD_Ov008_WaitShopClosed))
        return KH_SIT_SHOP;
    switch (in->camp_screen) {
    case CAMP_MENU: return KH_SIT_CAMP_MENU;
    case CAMP_HOLO_MISSIONS: return KH_SIT_WORLD_SELECTOR;
    case CAMP_PREFERENCES: return KH_SIT_SETTINGS;
    case CAMP_STORY_SAVE: return in->camp_story ? KH_SIT_SAVE_MENU : KH_SIT_CAMP_MENU;
    case CAMP_BETWEEN: return previous; /* the camp's first frames keep the screen before it */
    default: return KH_SIT_CAMP_MENU; /* Panels and the rest: the menu's layout */
    }
}

/* The field's panel of a mission's start: the mission's information on the upper screen and no
 * deck while its state is 3-6. */
static int mission_start_info(const KhSituationInputs *in)
{
    return in->mission_start >= 3 && in->mission_start <= 6;
}

/* A person talking in the field: the same panel in state 10 (its box across the bottom of the
 * upper screen). */
static int field_talk(const KhSituationInputs *in)
{
    return in->mission_start == 10;
}

static int field_screens(const KhSituationInputs *in)
{
    if (loaded(in, OV_GAME_OVER))
        return KH_SIT_GAME_OVER;
    if (loaded(in, OV_SHOP))
        return KH_SIT_SHOP;
    if (in->pause_mode != 0)
        return KH_SIT_PAUSE;
    if (in->tutorial != 0)
        return KH_SIT_TUTORIAL;
    /* the 3D on both screens: the game swaps engine A's screen every VBlank */
    if (in->engine_swaps >= 2u)
        return KH_SIT_FIELD_3D_BOTH;
    return KH_SIT_FIELD;
}

static int situation_of(const KhSituationInputs *in)
{
    if (in->movie)
        return KH_SIT_MOVIE;
    if (in->camp_screen != KH_CAMP_NONE && (in->scene == SCENE_CAMP || in->camp_story))
        return camp_screens(in);
    if (loaded(in, OV_CREDITS))
        return KH_SIT_THE_END;
    switch (in->scene) {
    case 0:
        /* no scene: the boot, or a scene's end before the next: the screen before it stays */
        return previous == KH_SIT_UNKNOWN ? KH_SIT_INTRO : previous;
    case SCENE_TITLE: return title_screens(in);
    case SCENE_SELECT: return KH_SIT_TITLE; /* Mission Mode's character select: the title's two screens */
    case SCENE_DAY_CARD: return KH_SIT_DAY_COUNTER;
    case SCENE_MONOLOGUE: return KH_SIT_THOUGHTS;
    case SCENE_RESULTS: return KH_SIT_MISSION_REVIEW;
    case SCENE_OPENING: return KH_SIT_MOVIE;
    case SCENE_FIELD: return field_screens(in);
    default: return KH_SIT_UNKNOWN;
    }
}

#define ST(n) (1u << KH_ST_##n)

static uint32_t states_of(const KhSituationInputs *in, int scene)
{
    uint32_t s = 0;
    if (scene == KH_SIT_FIELD || scene == KH_SIT_FIELD_3D_BOTH || scene == KH_SIT_TUTORIAL) {
        /* a dialog: an event's text (ov023's status bit 1) or a person talking */
        const int event_text = in->event_status >= 0 && (in->event_status & 2) != 0;
        if (mission_start_info(in))
            s |= ST(INFO_UPPER);
        else if (event_text || field_talk(in))
            s |= ST(DIALOG);
        else if (hud_mode != KH_HUD_NONE)
            s |= ST(HUD);
        /* a challenge: only after the story's camp (Mission Mode's byte is the session's) */
        if (story_camp && (in->mission_flags & 2u) != 0u)
            s |= ST(CHALLENGE);
        /* the 3D on the lower screen alone: engine A shows the lower screen and does not swap */
        if ((in->powcnt1 & 0x8000u) == 0u && in->engine_swaps == 0u && scene != KH_SIT_FIELD_3D_BOTH)
            s |= ST(LOWER_3D);
        if ((s & ST(HUD)) != 0u && in->party >= 4u)
            s |= ST(PARTY_4);
    }
    /* the pause opened in an event (a cutscene's "Continue", "Skip") */
    if (scene == KH_SIT_PAUSE && in->event_status >= 0)
        s |= ST(EVENT);
    return s;
}

void kh_situation_decide(const KhSituationInputs *in, KhSituation *out)
{
    if (in->camp_screen != KH_CAMP_NONE)
        story_camp = in->camp_story;
    else if (in->scene == SCENE_TITLE)
        story_camp = 1; /* a new game or a story's file follows, or Mission Mode's camp says otherwise */
    out->scene = situation_of(in);
    out->states = states_of(in, out->scene);
    out->content = 0;
    previous = out->scene;
}

/* ---- names ------------------------------------------------------------------------------- */

static const char *const scene_names[KH_SITUATIONS] = {
    "intro",    "title",          "load-menu", "save-menu", "settings",      "day-counter",    "movie",
    "field",    "camp-menu",      "pause",     "tutorial",  "field-3d-both", "mission-review", "shop",
    "thoughts", "world-selector", "game-over", "the-end",   "unknown",
};

static const char *const state_names[KH_STATES] = {
    "hud",     "dialog",   "challenge",     "info-upper", "info-lower", "lower-3d",
    "party-4", "info-new", "mission-panel", "title-lit",  "event",
};

const char *kh_situation_name(int scene)
{
    return scene >= 0 && scene < KH_SITUATIONS ? scene_names[scene] : "?";
}

const char *kh_situation_state_name(int state)
{
    return state >= 0 && state < KH_STATES ? state_names[state] : "?";
}

static int named(const char *const *names, int count, const char *name, int length)
{
    int i;
    for (i = 0; i < count; i++)
        if ((int)strlen(names[i]) == length && strncmp(names[i], name, (size_t)length) == 0)
            return i;
    return -1;
}

int kh_situation_named(const char *name, int length)
{
    return named(scene_names, KH_SITUATIONS, name, length);
}

int kh_situation_state_named(const char *name, int length)
{
    return named(state_names, KH_STATES, name, length);
}
