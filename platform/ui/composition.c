/* The compositions file, read and applied (composition.h). Ported from khdays-port's
 * port/ui/composition.c (MIT): the same lines, the same placing of shapes. Plain C. */
#include "composition.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    CORNER_CENTRE,
    CORNER_TOP_LEFT,
    CORNER_TOP,
    CORNER_TOP_RIGHT,
    CORNER_RIGHT,
    CORNER_BOTTOM_RIGHT,
    CORNER_BOTTOM,
    CORNER_BOTTOM_LEFT,
    CORNER_LEFT,
    CORNER_KEEP
};
static const char *const corner_names[] = { "centre",       "top-left", "top",         "top-right", "right",
                                            "bottom-right", "bottom",   "bottom-left", "left",      "keep" };
static const char *const screens_names[] = { "upper", "lower", "stacked", "side-by-side" };
static const char *const brightness_names[] = { "default", "upper", "lower", "halves", "own" };
static const float anchors[][2] = { { 0.5f, 0.5f }, { 0.0f, 0.0f }, { 0.5f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 0.5f },
                                    { 1.0f, 1.0f }, { 0.5f, 1.0f }, { 0.0f, 1.0f }, { 0.0f, 0.5f }, { 0.5f, 0.5f } };

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* A shape as the file gives it: its lengths at the HUD's scale and their `fixed(N)` parts. */
typedef struct ShapeDef {
    int lower, whole;
    float x, y, w, h;
    int corner;
    float margin[4], margin_fixed[4];
    float sx, sy, sx_fixed, sy_fixed;
    int capped;
    float cap, cap_fixed;
    int cover;
    uint32_t flags;
    float fade[4], opacity, round;
    uint32_t see_through;
    uint16_t keys[KH_SHAPE_KEYS];
    int key_count;
} ShapeDef;

/* `shape3d hud [<x> <y>] [<w> <h>] at <corner> [margin ...] [scale <s>]` */
typedef struct Shape3dDef {
    float x, y, w, h;
    int corner;
    float margin[4];
    float scale;
    int fixed; /* `scale fixed(N)`: its scale and margins do not follow the HUD's */
} Shape3dDef;

enum { D_SCREENS, D_BRIGHTNESS, D_SHAPE, D_SHAPE3D, D_BACKDROP, D_HIDE3D_ALL, D_HIDE3D_HUD, D_MENU, D_STOP,
       D_NOTHING };
typedef struct Directive {
    int kind, value;
} Directive;

typedef struct Condition {
    int state; /* KH_ST_*, or -1 for the picture's width */
    int negate;
    int width; /* 1 `wide`, 2 `narrow` */
} Condition;

typedef struct Section {
    int scene;
    int first_condition, conditions;
    int first_directive, directives;
} Section;

typedef struct Backdrop {
    char name[128];
    float ax, ay;
    int fit;
} Backdrop;

#define WIDE_COLUMNS 336 /* 16:9's 342 and wider */

static Section *sections;
static int section_count, section_room;
static Condition *conditions;
static int condition_count, condition_room;
static Directive *directives;
static int directive_count, directive_room;
static ShapeDef *shape_defs;
static int shape_def_count, shape_def_room;
static Shape3dDef *shape3d_defs;
static int shape3d_def_count, shape3d_def_room;
static Backdrop *backdrops;
static int backdrop_count, backdrop_room;
static int current_columns = 256;
static float hud_f = 1.0f;
static int (*backdrop_check)(const char *name);

static int grow(void **array, int *room, int count, size_t size)
{
    void *bigger;
    int n;
    if (count < *room)
        return 1;
    n = *room ? *room * 2 : 16;
    bigger = realloc(*array, (size_t)n * size);
    if (!bigger)
        return 0;
    *array = bigger;
    *room = n;
    return 1;
}

static int number(const char *s, float *out)
{
    char *end;
    double v;
    if (!s)
        return 0;
    v = strtod(s, &end);
    if (end == s || *end != '\0')
        return 0;
    *out = (float)v;
    return 1;
}

/* `fixed(N)`: N. */
static int fixed_value(const char *s, float *out)
{
    char *end;
    double v;
    if (strncmp(s, "fixed(", 6) != 0)
        return 0;
    v = strtod(s + 6, &end);
    if (end == s + 6 || strcmp(end, ")") != 0)
        return 0;
    *out = (float)v;
    return 1;
}

/* A length: `N` (at the HUD's scale), `fixed(N)` or `A+fixed(B)`. */
static int measure(const char *s, float *a, float *b)
{
    char *end;
    double first;
    const char *plus;
    if (!s)
        return 0;
    plus = strchr(s, '+');
    if (fixed_value(s, b)) {
        *a = 0.0f;
        return 1;
    }
    first = strtod(s, &end);
    if (end == s)
        return 0;
    if (*end == '\0') {
        *a = (float)first;
        *b = 0.0f;
        return 1;
    }
    if (plus == end && fixed_value(plus + 1, b)) {
        *a = (float)first;
        return 1;
    }
    return 0;
}

static int four_measures(char **t, int n, int at, float out[4], float fixed[4])
{
    int i;
    if (at + 4 > n)
        return 0;
    for (i = 0; i < 4; i++)
        if (!measure(t[at + i], &out[i], &fixed[i]))
            return 0;
    return 1;
}

static int four(char **t, int n, int at, float out[4])
{
    int i;
    if (at + 4 > n)
        return 0;
    for (i = 0; i < 4; i++)
        if (!number(t[at + i], &out[i]))
            return 0;
    return 1;
}

static int named(const char *const *names, int count, const char *s)
{
    int i;
    for (i = 0; s && i < count; i++)
        if (strcmp(names[i], s) == 0)
            return i;
    return -1;
}

/* An 8-bit colour as the file gives it, in the DS's 5 bits a channel (BGR555). */
static int colour(char **t, int n, int at, uint32_t *out)
{
    float c[3];
    int i;
    if (at + 3 > n || !number(t[at], &c[0]) || !number(t[at + 1], &c[1]) || !number(t[at + 2], &c[2]))
        return 0;
    for (i = 0; i < 3; i++)
        if (c[i] < 0.0f || c[i] > 255.0f)
            return 0;
    *out = ((uint32_t)c[0] >> 3) | (((uint32_t)c[1] >> 3) << 5) | (((uint32_t)c[2] >> 3) << 10);
    return 1;
}

/* `shape <upper|lower> [<x> <y> <w> <h>] at <corner> ...` */
static int read_shape(char **t, int n, ShapeDef *s, const char **why)
{
    int i = 1;
    memset(s, 0, sizeof(*s));
    s->sx = s->sy = 1.0f;
    s->opacity = 1.0f;
    if (i >= n || (strcmp(t[i], "upper") != 0 && strcmp(t[i], "lower") != 0)) {
        *why = "a shape is taken from the upper or the lower screen";
        return 0;
    }
    s->lower = strcmp(t[i++], "lower") == 0;
    s->whole = 1;
    if (i + 4 <= n && strcmp(t[i], "at") != 0) {
        if (!number(t[i], &s->x) || !number(t[i + 1], &s->y) || !number(t[i + 2], &s->w) ||
            !number(t[i + 3], &s->h)) {
            *why = "a rectangle is four numbers";
            return 0;
        }
        if (s->x < 0.0f || s->y < 0.0f || s->w <= 0.0f || s->x + s->w > 256.0f || s->h <= 0.0f ||
            s->y + s->h > 192.0f) {
            *why = "the rectangle is not within its screen";
            return 0;
        }
        s->whole = 0;
        i += 4;
    }
    if (s->whole) {
        s->w = 256.0f;
        s->h = 192.0f;
    }
    if (i + 1 >= n || strcmp(t[i], "at") != 0 || (s->corner = named(corner_names, COUNT(corner_names), t[i + 1])) < 0) {
        *why = "`at` and a corner";
        return 0;
    }
    i += 2;
    while (i < n) {
        const char *w = t[i];
        if (strcmp(w, "margin") == 0 && four_measures(t, n, i + 1, s->margin, s->margin_fixed)) {
            i += 5;
        } else if (strcmp(w, "scale") == 0 && i + 1 < n && strcmp(t[i + 1], "cover") == 0) {
            s->cover = 1;
            i += 2;
        } else if (strcmp(w, "scale") == 0 && i + 1 < n && measure(t[i + 1], &s->sx, &s->sx_fixed)) {
            s->sy = s->sx;
            s->sy_fixed = s->sx_fixed;
            i += 2;
            if (i < n && measure(t[i], &s->sy, &s->sy_fixed))
                i++;
            if (s->sx < 0.0f || s->sx_fixed < 0.0f || s->sx + s->sx_fixed <= 0.0f || s->sy < 0.0f ||
                s->sy_fixed < 0.0f || s->sy + s->sy_fixed <= 0.0f) {
                *why = "a scale is above 0";
                return 0;
            }
            if (i + 1 < n && strcmp(t[i], "at-most") == 0) {
                if (!measure(t[i + 1], &s->cap, &s->cap_fixed) || s->cap < 0.0f || s->cap_fixed < 0.0f ||
                    s->cap + s->cap_fixed <= 0.0f) {
                    *why = "`at-most` and a length above 0";
                    return 0;
                }
                s->capped = 1;
                i += 2;
            }
        } else if (strcmp(w, "mirror") == 0 && i + 1 < n &&
                   (strcmp(t[i + 1], "x") == 0 || strcmp(t[i + 1], "y") == 0 || strcmp(t[i + 1], "xy") == 0)) {
            s->flags |= (strchr(t[i + 1], 'x') ? KH_SHAPE_MIRROR_X : 0u) |
                        (strchr(t[i + 1], 'y') ? KH_SHAPE_MIRROR_Y : 0u);
            i += 2;
        } else if (strcmp(w, "repeat") == 0) {
            if (i + 1 < n && strcmp(t[i + 1], "x") == 0) {
                s->flags |= KH_SHAPE_REPEAT_X;
                i += 2;
            } else if (i + 1 < n && strcmp(t[i + 1], "y") == 0) {
                s->flags |= KH_SHAPE_REPEAT_Y;
                i += 2;
            } else {
                s->flags |= KH_SHAPE_REPEAT_X | KH_SHAPE_REPEAT_Y;
                i++;
            }
        } else if (strcmp(w, "3d") == 0) {
            s->flags |= KH_SHAPE_3D;
            i++;
        } else if (strcmp(w, "force") == 0) {
            s->flags |= KH_SHAPE_FORCE;
            i++;
        } else if (strcmp(w, "round") == 0 && i + 1 < n && number(t[i + 1], &s->round) && s->round > 0.0f) {
            s->flags |= KH_SHAPE_ROUND;
            i += 2;
        } else if (strcmp(w, "key") == 0) {
            uint32_t c;
            if (!colour(t, n, i + 1, &c)) {
                *why = "a key is a colour: three values 0-255";
                return 0;
            }
            if (s->key_count < KH_SHAPE_KEYS)
                s->keys[s->key_count++] = (uint16_t)c;
            i += 4;
        } else if (strcmp(w, "see-through") == 0) {
            uint32_t c;
            float a;
            if (!colour(t, n, i + 1, &c) || i + 4 >= n || !number(t[i + 4], &a) || a < 0.0f || a > 1.0f) {
                *why = "see-through is a colour (three values 0-255) and an opacity 0-1";
                return 0;
            }
            s->see_through = c | (uint32_t)(a * 255.0f + 0.5f) << 16;
            s->flags |= KH_SHAPE_SEE_THROUGH;
            i += 5;
        } else if (strcmp(w, "dim") == 0 && i + 1 < n && !(s->flags & KH_SHAPE_WEIGHT) &&
                   number(t[i + 1], &s->opacity) && s->opacity >= 0.0f && s->opacity <= 1.0f) {
            s->flags |= KH_SHAPE_DIM;
            i += 2;
        } else if (strcmp(w, "opacity") == 0 && i + 1 < n && !(s->flags & KH_SHAPE_DIM) &&
                   number(t[i + 1], &s->opacity) && s->opacity >= 0.0f && s->opacity <= 1.0f) {
            s->flags |= KH_SHAPE_WEIGHT;
            i += 2;
        } else if (strcmp(w, "fade") == 0 && !(s->flags & KH_SHAPE_DIM) && four(t, n, i + 1, s->fade)) {
            s->flags |= KH_SHAPE_WEIGHT;
            i += 5;
        } else {
            *why = "an effect this version does not know, or its values";
            return 0;
        }
    }
    return 1;
}

static int read_shape3d(char **t, int n, Shape3dDef *s, const char **why)
{
    int i = 2;
    memset(s, 0, sizeof(*s));
    s->scale = 1.0f;
    s->w = 256.0f;
    s->h = 192.0f;
    if (i + 4 <= n && strcmp(t[i + 2], "at") != 0 && number(t[i], &s->x) && number(t[i + 1], &s->y) &&
        number(t[i + 2], &s->w) && number(t[i + 3], &s->h))
        i += 4;
    else if (i + 2 <= n && number(t[i], &s->w) && number(t[i + 1], &s->h))
        i += 2;
    if (s->w <= 0.0f || s->h <= 0.0f) {
        *why = "the rectangle is not within its screen";
        return 0;
    }
    if (i + 1 >= n || strcmp(t[i], "at") != 0 || (s->corner = named(corner_names, COUNT(corner_names), t[i + 1])) < 0) {
        *why = "`at` and a corner";
        return 0;
    }
    i += 2;
    while (i < n) {
        if (strcmp(t[i], "margin") == 0 && four(t, n, i + 1, s->margin)) {
            i += 5;
        } else if (strcmp(t[i], "scale") == 0 && i + 1 < n && fixed_value(t[i + 1], &s->scale) && s->scale > 0.0f) {
            s->fixed = 1;
            i += 2;
        } else if (strcmp(t[i], "scale") == 0 && i + 1 < n && number(t[i + 1], &s->scale) && s->scale > 0.0f) {
            i += 2;
        } else {
            *why = "margin or scale";
            return 0;
        }
    }
    return 1;
}

static int add_directive(int kind, int value)
{
    if (!grow((void **)&directives, &directive_room, directive_count, sizeof(Directive)))
        return 0;
    directives[directive_count].kind = kind;
    directives[directive_count].value = value;
    directive_count++;
    sections[section_count - 1].directives++;
    return 1;
}

/* `backdrop <file> [at <corner>] [fit]`; `preload <file>` is the HD title's, read and left out */
static int read_backdrop(char **t, int n, const char **why)
{
    Backdrop b;
    int corner = CORNER_CENTRE;
    if (strcmp(t[0], "preload") == 0)
        return n == 2 || (*why = "preload <file>", 0);
    memset(&b, 0, sizeof(b));
    b.fit = n > 2 && strcmp(t[n - 1], "fit") == 0;
    if (b.fit)
        n--;
    if (n != 2 && !(n == 4 && strcmp(t[2], "at") == 0)) {
        *why = "backdrop <file> [at <corner>] [fit]";
        return 0;
    }
    if (n == 4 && (corner = named(corner_names, COUNT(corner_names), t[3])) < 0) {
        *why = "an unknown corner";
        return 0;
    }
    snprintf(b.name, sizeof(b.name), "%s", t[1]);
    b.ax = anchors[corner][0];
    b.ay = anchors[corner][1];
    if (!grow((void **)&backdrops, &backdrop_room, backdrop_count, sizeof(Backdrop)) ||
        !add_directive(D_BACKDROP, backdrop_count)) {
        *why = "no memory";
        return 0;
    }
    backdrops[backdrop_count++] = b;
    return 1;
}

/* `[<situation> [if <condition> ...]]` */
static int read_section(char **t, int n, const char **why)
{
    Section *s;
    int i = 1;
    const int scene = kh_situation_named(t[0], (int)strlen(t[0]));
    if (scene < 0) {
        *why = "an unknown situation";
        return 0;
    }
    if (!grow((void **)&sections, &section_room, section_count, sizeof(Section)))
        return 0;
    s = &sections[section_count];
    s->scene = scene;
    s->first_condition = condition_count;
    s->conditions = 0;
    s->first_directive = directive_count;
    s->directives = 0;
    if (n > 1) {
        if (strcmp(t[1], "if") != 0) {
            *why = "conditions follow `if`";
            return 0;
        }
        i = 2;
    }
    for (; i < n; i++) {
        Condition c;
        const char *w = t[i];
        c.negate = w[0] == '!';
        w += c.negate;
        c.width = 0;
        c.state = kh_situation_state_named(w, (int)strlen(w));
        if (strcmp(w, "wide") == 0 || strcmp(w, "narrow") == 0) {
            c.state = -1;
            c.width = w[0] == 'w' ? 1 : 2;
        } else if (c.state < 0) {
            *why = "an unknown state";
            return 0;
        }
        if (!grow((void **)&conditions, &condition_room, condition_count, sizeof(Condition)))
            return 0;
        conditions[condition_count++] = c;
        s->conditions++;
    }
    section_count++;
    return 1;
}

#define MAX_WORDS 64

static int split(char *line, const char *separators, char **words)
{
    int n = 0;
    char *save = NULL, *w;
    for (w = strtok_r(line, separators, &save); w && n < MAX_WORDS; w = strtok_r(NULL, separators, &save))
        words[n++] = w;
    return n;
}

/* Whether the text begins, after its comments and empty lines, with `units port`. */
static int in_port_units(const char *text)
{
    const char *at = text;
    while (*at) {
        const char *end = strchr(at, '\n'), *hash;
        size_t n = end ? (size_t)(end - at) : strlen(at);
        if ((hash = (const char *)memchr(at, '#', n)) != NULL)
            n = (size_t)(hash - at);
        while (n > 0 && (at[0] == ' ' || at[0] == '\t'))
            at++, n--;
        while (n > 0 && (at[n - 1] == ' ' || at[n - 1] == '\t' || at[n - 1] == '\r'))
            n--;
        if (n > 0)
            return n == 10 && strncmp(at, "units", 5) == 0 && (at[5] == ' ' || at[5] == '\t') &&
                   strncmp(at + 6, "port", 4) == 0;
        if (!end)
            break;
        at = end + 1;
    }
    return 0;
}

int kh_composition_read(const char *file, char *text, void (*say)(const char *line))
{
    char *words[MAX_WORDS];
    char report[256];
    int line = 0;
    char *next, *at;
    section_count = condition_count = directive_count = shape_def_count = backdrop_count = 0;
    shape3d_def_count = 0;
    if (!in_port_units(text))
        return -1;
    for (at = text; at; at = next) {
        const char *why = NULL;
        char shown[160];
        char *c;
        int n;
        line++;
        next = strchr(at, '\n');
        if (next)
            *next++ = '\0';
        if ((c = strchr(at, '#')) != NULL)
            *c = '\0';
        snprintf(shown, sizeof(shown), "%s", at);
        n = split(at, " \t\r", words);
        if (n == 0)
            continue;
        if (words[0][0] == '[') {
            char *open = strchr(shown, '['), *close = strchr(shown, ']');
            if (!close || !open) {
                why = "a section's name ends with ]";
            } else {
                char inside[160];
                *close = '\0';
                snprintf(inside, sizeof(inside), "%s", open + 1);
                *close = ']';
                n = split(inside, " \t", words);
                if (n <= 0 || !read_section(words, n, &why))
                    why = why ? why : "a section names a situation";
            }
        } else if (strcmp(words[0], "units") == 0) {
            if (n != 2 || strcmp(words[1], "port") != 0 || section_count != 0)
                why = "`units port`, before the file's first section";
        } else if (section_count == 0) {
            why = "a line before the first section";
        } else if (strcmp(words[0], "screens") == 0) {
            const int v = n == 2 ? named(screens_names, COUNT(screens_names), words[1]) : -1;
            if (v < 0 || !add_directive(D_SCREENS, v))
                why = "screens upper, lower, stacked or side-by-side";
        } else if (strcmp(words[0], "brightness") == 0) {
            const int v = n == 2 ? named(brightness_names, COUNT(brightness_names), words[1]) : -1;
            if (v < 0 || !add_directive(D_BRIGHTNESS, v))
                why = "brightness default, upper, lower, halves or own";
        } else if (strcmp(words[0], "shape") == 0) {
            ShapeDef s;
            if (read_shape(words, n, &s, &why)) {
                if (!grow((void **)&shape_defs, &shape_def_room, shape_def_count, sizeof(ShapeDef)) ||
                    !add_directive(D_SHAPE, shape_def_count))
                    why = "no memory";
                else
                    shape_defs[shape_def_count++] = s;
            }
        } else if (strcmp(words[0], "backdrop") == 0 || strcmp(words[0], "preload") == 0) {
            read_backdrop(words, n, &why);
        } else if (strcmp(words[0], "hide3d") == 0 && n == 2) {
            if (strcmp(words[1], "all") == 0)
                add_directive(D_HIDE3D_ALL, 0);
            else if (strcmp(words[1], "hud") == 0)
                add_directive(D_HIDE3D_HUD, 0);
            else
                why = "hide3d hud or all";
        } else if (strcmp(words[0], "shape3d") == 0) {
            Shape3dDef s3;
            if (n >= 2 && strcmp(words[1], "all") == 0) {
                /* every polygon of a menu's frame drawn larger elsewhere (the PC port's camp
                 * model window): not here */
                add_directive(D_NOTHING, 0);
            } else if (n < 2 || strcmp(words[1], "hud") != 0) {
                why = "a group of the HUD's 3D polygons";
            } else if (read_shape3d(words, n, &s3, &why)) {
                if (!grow((void **)&shape3d_defs, &shape3d_def_room, shape3d_def_count, sizeof(Shape3dDef)) ||
                    !add_directive(D_SHAPE3D, shape3d_def_count))
                    why = "no memory";
                else
                    shape3d_defs[shape3d_def_count++] = s3;
            }
        } else if (strcmp(words[0], "menu") == 0) {
            if (n != 1 || !add_directive(D_MENU, 0))
                why = "menu takes nothing";
        } else if (strcmp(words[0], "stop") == 0) {
            if (n != 1 || !add_directive(D_STOP, 0))
                why = "stop takes nothing";
        } else {
            why = "an unknown line";
        }
        if (why && say) {
            snprintf(report, sizeof(report), "%s, line %d: %s: %s (the line is ignored)", file, line, why, shown);
            say(report);
        }
    }
    return section_count;
}

void kh_composition_set_hud_factor(float f)
{
    hud_f = f < 0.3f ? 0.3f : f > 2.0f ? 2.0f : f;
}

void kh_composition_set_backdrop_check(int (*there)(const char *name))
{
    backdrop_check = there;
}

static int holds(const Section *s, const KhSituation *situation)
{
    int i;
    if (s->scene != situation->scene)
        return 0;
    for (i = 0; i < s->conditions; i++) {
        const Condition *c = &conditions[s->first_condition + i];
        const int truth = c->state >= 0   ? (int)((situation->states >> c->state) & 1u)
                          : c->width == 1 ? current_columns >= WIDE_COLUMNS
                                          : current_columns <= 256;
        if (truth == c->negate)
            return 0;
    }
    return 1;
}

static int backdrop_there(const char *name)
{
    const size_t n = strlen(name);
    if (strchr(name, '{') || (n > 4 && strcmp(name + n - 4, ".txt") == 0))
        return 0; /* the HD title's (its state in its name, or animated) */
    return backdrop_check && backdrop_check(name);
}

/* A section that starts with a backdrop is used only when its picture is there. */
static int section_there(const Section *s)
{
    const Directive *d = &directives[s->first_directive];
    if (s->directives == 0 || d->kind != D_BACKDROP)
        return 1;
    return backdrop_there(backdrops[d->value].name);
}

/* A shape for this frame in the C x 192 picture: its source's rectangle scaled (a DS dot becomes
 * `scale` picture dots), placed at its corner and moved by its margins. */
static void build_shape(const ShapeDef *d, int columns, KhShape *out)
{
    const float f = hud_f, c = (float)columns, w = d->w, h = d->h;
    const float cover = d->cover ? (c / w > 192.0f / h ? c / w : 192.0f / h) : 0.0f;
    const float cap = d->capped ? d->cap * f + d->cap_fixed : 0.0f;
    float sx = d->cover ? cover : d->sx * f + d->sx_fixed, sy = d->cover ? cover : d->sy * f + d->sy_fixed;
    float width, height, x1 = 0.0f, y1 = 0.0f;
    int i;
    if (d->capped) {
        sx = sx < cap ? sx : cap;
        sy = sy < cap ? sy : cap;
    }
    width = w * sx;
    height = h * sy;
    switch (d->corner) {
    case CORNER_CENTRE: x1 = (c - width) / 2.0f, y1 = (192.0f - height) / 2.0f; break;
    case CORNER_TOP_LEFT: break;
    case CORNER_TOP: x1 = (c - width) / 2.0f; break;
    case CORNER_TOP_RIGHT: x1 = c - width; break;
    case CORNER_RIGHT: x1 = c - width, y1 = (192.0f - height) / 2.0f; break;
    case CORNER_BOTTOM_RIGHT: x1 = c - width, y1 = 192.0f - height; break;
    case CORNER_BOTTOM: x1 = (c - width) / 2.0f, y1 = 192.0f - height; break;
    case CORNER_BOTTOM_LEFT: y1 = 192.0f - height; break;
    case CORNER_LEFT: y1 = (192.0f - height) / 2.0f; break;
    default: /* keep: the source's middle where it was, scaled */
        x1 = (d->x + w / 2.0f) * sx * c / 256.0f - width / 2.0f;
        y1 = (d->y + h / 2.0f) * sy - height / 2.0f;
        break;
    }
    x1 += d->margin[0] * f + d->margin_fixed[0] - (d->margin[2] * f + d->margin_fixed[2]);
    y1 += d->margin[1] * f + d->margin_fixed[1] - (d->margin[3] * f + d->margin_fixed[3]);
    memset(out, 0, sizeof(*out));
    out->flags = d->flags | (d->lower ? KH_SHAPE_LOWER : 0u);
    out->src[0] = d->x, out->src[1] = d->y, out->src[2] = w, out->src[3] = h;
    out->dst[0] = x1, out->dst[1] = y1, out->dst[2] = x1 + width, out->dst[3] = y1 + height;
    for (i = 0; i < 4; i++)
        out->fade[i] = d->fade[i] * f;
    out->opacity = d->opacity;
    out->round = d->round;
    out->see_through = d->see_through;
    memcpy(out->keys, d->keys, sizeof(out->keys));
    out->key_count = d->key_count;
}

/* The HUD's 3D at the HUD's scale, as the 2D shapes, unless fixed: its corner's place in the
 * picture and its scale */
static void build_shape3d(const Shape3dDef *d, int columns, KhComposition *out)
{
    const float u = d->fixed ? 1.0f : 1.0f / hud_f, c = (float)columns;
    const float width = d->w * d->scale / u, height = d->h * d->scale / u;
    float x1 = 0.0f, y1 = 0.0f;
    switch (d->corner) {
    case CORNER_CENTRE: x1 = (c - width) / 2.0f, y1 = (192.0f - height) / 2.0f; break;
    case CORNER_TOP: x1 = (c - width) / 2.0f; break;
    case CORNER_TOP_RIGHT: x1 = c - width; break;
    case CORNER_RIGHT: x1 = c - width, y1 = (192.0f - height) / 2.0f; break;
    case CORNER_BOTTOM_RIGHT: x1 = c - width, y1 = 192.0f - height; break;
    case CORNER_BOTTOM: x1 = (c - width) / 2.0f, y1 = 192.0f - height; break;
    case CORNER_BOTTOM_LEFT: y1 = 192.0f - height; break;
    case CORNER_LEFT: y1 = (192.0f - height) / 2.0f; break;
    default: break;
    }
    out->hud3d = 1;
    out->hud3d_src[0] = d->x, out->hud3d_src[1] = d->y, out->hud3d_src[2] = d->w, out->hud3d_src[3] = d->h;
    out->hud3d_x = x1 + (d->margin[0] - d->margin[2]) / u;
    out->hud3d_y = y1 + (d->margin[1] - d->margin[3]) / u;
    out->hud3d_scale = d->scale / u;
}

void kh_composition_for(const KhSituation *situation, int columns, KhComposition *out)
{
    int s, i, stopped = 0, matched = 0, menu = 0;
    memset(out, 0, sizeof(*out));
    out->screens = -1;
    out->brightness = -1;
    out->backdrop_ax = out->backdrop_ay = 0.5f;
    current_columns = columns;
    for (s = 0; s < section_count && !stopped; s++) {
        if (!holds(&sections[s], situation) || !section_there(&sections[s]))
            continue;
        matched = 1;
        for (i = 0; i < sections[s].directives && !stopped; i++) {
            const Directive *d = &directives[sections[s].first_directive + i];
            switch (d->kind) {
            case D_SCREENS: out->screens = out->screens < 0 ? d->value : out->screens; break;
            case D_BRIGHTNESS: out->brightness = out->brightness < 0 ? d->value : out->brightness; break;
            case D_SHAPE:
                if (out->shape_count < KH_COMPOSITION_SHAPES)
                    build_shape(&shape_defs[d->value], columns, &out->shapes[out->shape_count++]);
                break;
            case D_SHAPE3D:
                if (!out->hud3d)
                    build_shape3d(&shape3d_defs[d->value], columns, out);
                break;
            case D_BACKDROP:
                if (!out->backdrop[0] && backdrop_there(backdrops[d->value].name)) {
                    snprintf(out->backdrop, sizeof(out->backdrop), "%s", backdrops[d->value].name);
                    out->backdrop_ax = backdrops[d->value].ax;
                    out->backdrop_ay = backdrops[d->value].ay;
                    out->backdrop_fit = backdrops[d->value].fit;
                }
                break;
            case D_HIDE3D_ALL: out->hide3d_all = 1; break;
            case D_HIDE3D_HUD: out->hide3d_hud = 1; break;
            case D_MENU: menu = 1; break;
            case D_NOTHING: break;
            default: stopped = 1; break;
            }
        }
    }
    if ((menu || !matched) && out->screens < 0) {
        /* the common rule: the screens that show something, whole, as large as the picture
         * allows -- both side by side, or the one alone at the DS's size */
        out->screens = situation->content == 2u   ? KH_SCREENS_LOWER
                       : situation->content == 3u ? KH_SCREENS_SIDE_BY_SIDE
                                                  : KH_SCREENS_UPPER;
        if (out->screens != KH_SCREENS_SIDE_BY_SIDE && out->shape_count < KH_COMPOSITION_SHAPES) {
            ShapeDef whole;
            memset(&whole, 0, sizeof(whole));
            whole.lower = out->screens == KH_SCREENS_LOWER;
            whole.whole = 1;
            whole.w = 256.0f;
            whole.h = 192.0f;
            whole.corner = CORNER_CENTRE;
            whole.sx_fixed = whole.sy_fixed = 1.0f;
            whole.opacity = 1.0f;
            build_shape(&whole, columns, &out->shapes[out->shape_count++]);
        }
    }
    out->screens = out->screens < 0 ? KH_SCREENS_UPPER : out->screens;
    out->brightness = out->brightness < 0 ? 0 : out->brightness;
}
