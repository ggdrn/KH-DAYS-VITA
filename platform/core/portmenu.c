/* The port menu (L+R+Select): the port's settings while the game runs, drawn on the overlay
 * layer. The game is held while it is open (no VBlank reaches it, see platform/hw/game.c);
 * changes apply at once, and config.ini is written when the menu closes.
 *
 * The font is ASCII only, so the texts go without accents. */
#include "portmenu.h"

#include "config.h"
#include "font8x8.h"
#include "video.h"

#include <psp2/ctrl.h>
#include <stdio.h>
#include <string.h>

/* what a change of a setting needs besides the value itself; the platform sets the hooks */
void (*portmenu_on_scale)(int scale);
void (*portmenu_on_texture_filter)(void);
void (*portmenu_on_volume)(int percent);

#define W VIDEO_OVERLAY_W
#define H VIDEO_OVERLAY_H
#define COLS (W / 8)

static uint32_t s_px[W * H];
static volatile int s_open;
static int s_sel, s_top;
static uint32_t s_prev;
static uint64_t s_repeat_at;
static int s_repeat_dir;
static char s_note[64]; /* a line under the list: what the selected item does */

enum {
    IT_HEADER,                           /* a section title, not selectable */
    IT_SCALE, IT_FPS, IT_ASPECT, IT_LAYOUT, IT_INSET, IT_TEXFILTER, IT_SHOWFPS,
    IT_CAMERA, IT_CAMSPEED, IT_INVX, IT_INVY, IT_DPAD,
    IT_BUTTON,                           /* + the DS button index */
    IT_VOLUME,
    IT_DEFAULTS, IT_CLOSE,
};

typedef struct {
    int kind, arg;
    const char *label;
    const char *help;
} Item;

static const Item s_items[] = {
    { IT_HEADER, 0, "VIDEO", NULL },
    { IT_SCALE, 0, "Resolucao 3D", "Resolucao interna do 3D. 4x suaviza as bordas (mais GPU)." },
    { IT_FPS, 0, "FPS do 3D", "60: quadros intermediarios entre os do jogo. 30: original." },
    { IT_ASPECT, 0, "Proporcao", "Widescreen: 3D 16:9 no campo. Esticado: imagem do DS." },
    { IT_LAYOUT, 0, "Layout das telas", "Toque na tela pequena para trocar as telas." },
    { IT_INSET, 0, "Tamanho da tela pequena", "Largura da tela pequena, em pixels do Vita." },
    { IT_TEXFILTER, 0, "Filtro de texturas 3D", "Suave: filtragem bilinear. Nitido: como no DS." },
    { IT_SHOWFPS, 0, "Mostrar FPS", "Contador de quadros no canto superior esquerdo." },
    { IT_HEADER, 0, "CONTROLES", NULL },
    { IT_CAMERA, 0, "Camera no analogico direito", "Gira a camera do campo com o analogico direito." },
    { IT_CAMSPEED, 0, "Velocidade da camera", "Quanto inclinar o analogico para a velocidade maxima." },
    { IT_INVX, 0, "Inverter camera horizontal", NULL },
    { IT_INVY, 0, "Inverter camera vertical", NULL },
    { IT_DPAD, 0, "D-pad no menu de comandos", "Liga: o D-pad move o cursor dos comandos no campo." },
    { IT_HEADER, 0, "BOTOES (botao do DS = botao do Vita)", NULL },
    { IT_BUTTON, KH_BTN_A, "A", NULL },
    { IT_BUTTON, KH_BTN_B, "B", NULL },
    { IT_BUTTON, KH_BTN_X, "X", NULL },
    { IT_BUTTON, KH_BTN_Y, "Y", NULL },
    { IT_BUTTON, KH_BTN_L, "L", NULL },
    { IT_BUTTON, KH_BTN_R, "R", NULL },
    { IT_BUTTON, KH_BTN_START, "Start", NULL },
    { IT_BUTTON, KH_BTN_SELECT, "Select", NULL },
    { IT_HEADER, 0, "SOM", NULL },
    { IT_VOLUME, 0, "Volume", NULL },
    { IT_HEADER, 0, "", NULL },
    { IT_DEFAULTS, 0, "Restaurar padroes", "Aperte Cruz para voltar todas as opcoes ao padrao." },
    { IT_CLOSE, 0, "Salvar e voltar ao jogo", "Aperte Cruz (ou L+R+Select) para salvar e voltar." },
};
#define NITEMS ((int)(sizeof(s_items) / sizeof(s_items[0])))

static const char *const s_vita_names_pt[KH_VITA_BUTTONS] = { "Cruz", "Circulo", "Quadrado",
                                                             "Triangulo", "L", "R", "Start",
                                                             "Select" };

static void value(const Item *it, char *out, size_t n)
{
    static const char *const aspects[] = { "Widescreen 3D", "Esticado", "4:3 original" };
    static const char *const layouts[] = { "Tela de cima grande", "Tela de baixo grande",
                                           "Lado a lado" };
    static const char *const speeds[] = { "", "Lenta", "Normal", "Rapida" };
    switch (it->kind) {
    case IT_SCALE: snprintf(out, n, "%dx (%dx%d)", kh_config.render_scale,
                            256 * kh_config.render_scale, 192 * kh_config.render_scale); break;
    case IT_FPS: snprintf(out, n, "%d", kh_config.frame_interpolation ? 60 : 30); break;
    case IT_ASPECT: snprintf(out, n, "%s", aspects[kh_config.aspect % 3]); break;
    case IT_LAYOUT: snprintf(out, n, "%s", layouts[kh_config.layout % 3]); break;
    case IT_INSET: snprintf(out, n, "%d", kh_config.inset_width); break;
    case IT_TEXFILTER: snprintf(out, n, "%s", kh_config.texture_filter ? "Suave" : "Nitido"); break;
    case IT_SHOWFPS: snprintf(out, n, "%s", kh_config.show_fps ? "Sim" : "Nao"); break;
    case IT_CAMERA: snprintf(out, n, "%s", kh_config.camera_stick ? "Ligada" : "Desligada"); break;
    case IT_CAMSPEED: snprintf(out, n, "%s", speeds[kh_config.camera_speed & 3]); break;
    case IT_INVX: snprintf(out, n, "%s", kh_config.camera_invert_x ? "Sim" : "Nao"); break;
    case IT_INVY: snprintf(out, n, "%s", kh_config.camera_invert_y ? "Sim" : "Nao"); break;
    case IT_DPAD: snprintf(out, n, "%s", kh_config.dpad_deck ? "Ligado" : "Desligado"); break;
    case IT_BUTTON:
        snprintf(out, n, "%s", s_vita_names_pt[config_vita_button_index(kh_config.button[it->arg])]);
        break;
    case IT_VOLUME: snprintf(out, n, "%d%%", kh_config.volume); break;
    default: out[0] = 0; break;
    }
}

static int wrap(int v, int lo, int hi, int d)
{
    v += d;
    return v < lo ? hi : v > hi ? lo : v;
}

/* a left/right on the item: the next or previous value, applied at once */
static void change(const Item *it, int d)
{
    switch (it->kind) {
    case IT_SCALE:
        kh_config.render_scale = wrap(kh_config.render_scale, 1, 4, d);
        if (portmenu_on_scale)
            portmenu_on_scale(kh_config.render_scale);
        break;
    case IT_FPS: kh_config.frame_interpolation ^= 1; break;
    case IT_ASPECT:
        kh_config.aspect = wrap(kh_config.aspect, 0, 2, d);
        video_relayout();
        break;
    case IT_LAYOUT:
        kh_config.layout = wrap(kh_config.layout, 0, 2, d);
        video_set_layout((ScreenLayout)kh_config.layout);
        break;
    case IT_INSET:
        kh_config.inset_width = wrap(kh_config.inset_width, 128, 480, d * 32);
        if (kh_config.inset_width % 32)
            kh_config.inset_width -= kh_config.inset_width % 32;
        video_relayout();
        break;
    case IT_TEXFILTER:
        kh_config.texture_filter ^= 1;
        if (portmenu_on_texture_filter)
            portmenu_on_texture_filter();
        break;
    case IT_SHOWFPS: kh_config.show_fps ^= 1; break;
    case IT_CAMERA: kh_config.camera_stick ^= 1; break;
    case IT_CAMSPEED: kh_config.camera_speed = wrap(kh_config.camera_speed, 1, 3, d); break;
    case IT_INVX: kh_config.camera_invert_x ^= 1; break;
    case IT_INVY: kh_config.camera_invert_y ^= 1; break;
    case IT_DPAD: kh_config.dpad_deck ^= 1; break;
    case IT_BUTTON: {
        const int i = wrap(config_vita_button_index(kh_config.button[it->arg]), 0,
                           KH_VITA_BUTTONS - 1, d);
        kh_config.button[it->arg] = config_vita_button_mask(i);
        break;
    }
    case IT_VOLUME:
        kh_config.volume = wrap(kh_config.volume, 0, 100, d * 10);
        if (portmenu_on_volume)
            portmenu_on_volume(kh_config.volume);
        break;
    default: break;
    }
}

int portmenu_is_open(void)
{
    return s_open;
}

static void close_menu(void)
{
    s_open = 0;
    config_save();
}

void portmenu_toggle(void)
{
    if (s_open) {
        close_menu();
        return;
    }
    s_open = 1;
    s_prev = 0xffffffffu; /* the buttons that opened it count as held */
    if (s_items[s_sel].kind == IT_HEADER)
        s_sel = 1;
}

static void move(int d)
{
    do
        s_sel = (s_sel + d + NITEMS) % NITEMS;
    while (s_items[s_sel].kind == IT_HEADER);
}

void portmenu_input(uint32_t buttons, uint64_t now_us)
{
    const uint32_t pressed = buttons & ~s_prev;
    const uint32_t dirs = SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT;
    int dir = 0;

    if (!s_open)
        return;
    s_prev = buttons;
    /* the d-pad repeats when held */
    if (pressed & dirs) {
        dir = (int)(pressed & dirs);
        s_repeat_dir = dir;
        s_repeat_at = now_us + 350000;
    } else if ((buttons & (uint32_t)s_repeat_dir) && now_us >= s_repeat_at) {
        dir = s_repeat_dir;
        s_repeat_at = now_us + 90000;
    }
    if (dir & SCE_CTRL_UP)
        move(-1);
    if (dir & SCE_CTRL_DOWN)
        move(1);
    if (dir & SCE_CTRL_LEFT)
        change(&s_items[s_sel], -1);
    if (dir & SCE_CTRL_RIGHT)
        change(&s_items[s_sel], 1);
    if (pressed & (SCE_CTRL_CROSS | SCE_CTRL_CIRCLE)) {
        if (s_items[s_sel].kind == IT_CLOSE) {
            close_menu();
        } else if (s_items[s_sel].kind == IT_DEFAULTS) {
            config_defaults();
            if (portmenu_on_scale)
                portmenu_on_scale(kh_config.render_scale);
            if (portmenu_on_texture_filter)
                portmenu_on_texture_filter();
            if (portmenu_on_volume)
                portmenu_on_volume(kh_config.volume);
            video_set_layout((ScreenLayout)kh_config.layout);
            snprintf(s_note, sizeof(s_note), "Padroes restaurados.");
        } else {
            change(&s_items[s_sel], 1);
        }
    }
}

/* ---- drawing ------------------------------------------------------------------------------ */

static void fill(int x0, int y0, int x1, int y1, uint32_t c)
{
    int x, y;
    for (y = y0; y < y1 && y < H; y++)
        for (x = x0; x < x1 && x < W; x++)
            s_px[y * W + x] = c;
}

static void text(int x, int y, const char *s, uint32_t fg)
{
    for (; *s && x + 8 <= W; s++, x += 8) {
        const unsigned char c = (unsigned char)*s;
        const uint8_t *g = font8x8_basic[(c >= 0x20 && c < 0x80) ? c - 0x20 : '?' - 0x20];
        int gy, gx;
        for (gy = 0; gy < 8; gy++)
            for (gx = 0; gx < 8; gx++)
                if (g[gy] & (1 << gx))
                    s_px[(y + gy) * W + x + gx] = fg;
    }
}

#define ROW_H 10
#define LIST_Y 24
#define LIST_ROWS 21

const uint32_t *portmenu_render(void)
{
    int i, row;
    char v[48], line[COLS + 1];

    fill(0, 0, W, H, 0xd8100808u);
    fill(0, 0, W, 16, 0xff402018u);
    text(8, 4, "KINGDOM HEARTS 358/2 DAYS - MENU DO PORT", 0xff80e0ffu);

    /* keep the selection in view */
    if (s_sel < s_top + 1)
        s_top = s_sel > 0 ? s_sel - 1 : 0;
    if (s_sel >= s_top + LIST_ROWS - 1)
        s_top = s_sel - LIST_ROWS + 2;
    if (s_top < 0)
        s_top = 0;

    for (row = 0, i = s_top; i < NITEMS && row < LIST_ROWS; i++, row++) {
        const Item *it = &s_items[i];
        const int y = LIST_Y + row * ROW_H;
        if (it->kind == IT_HEADER) {
            text(16, y + 1, it->label, 0xff70b8ffu);
            continue;
        }
        if (i == s_sel)
            fill(12, y - 1, W - 12, y + 9, 0xff6a3a20u);
        if (it->kind == IT_BUTTON)
            snprintf(line, sizeof(line), "  Botao %s do DS", it->label);
        else
            snprintf(line, sizeof(line), "  %s", it->label);
        text(16, y, line, 0xffffffffu);
        value(it, v, sizeof(v));
        if (v[0]) {
            snprintf(line, sizeof(line), "< %s >", v);
            text(W - 16 - (int)strlen(line) * 8, y, line, i == s_sel ? 0xff60ffffu : 0xffc0c0c0u);
        }
    }
    if (s_top > 0)
        text(W - 24, LIST_Y - 10, "^", 0xffc0c0c0u);
    if (s_top + LIST_ROWS < NITEMS)
        text(W - 24, LIST_Y + LIST_ROWS * ROW_H, "v", 0xffc0c0c0u);

    fill(0, H - 34, W, H, 0xff402018u);
    if (s_note[0] && s_items[s_sel].kind == IT_DEFAULTS)
        text(8, H - 30, s_note, 0xff80ff80u);
    else if (s_items[s_sel].help)
        text(8, H - 30, s_items[s_sel].help, 0xffe0e0e0u);
    text(8, H - 14, "Cima/Baixo escolhe  Esq/Dir muda  L+R+Select salva e sai",
         0xffa0a0a0u);
    video_overlay_changed();
    return s_px;
}

/* the frame-rate counter alone: the rest of the layer clear */
const uint32_t *portmenu_render_fps(const char *label)
{
    static char last[32];
    if (strcmp(last, label) != 0 || s_px[0] != 0) {
        snprintf(last, sizeof(last), "%s", label);
        memset(s_px, 0, sizeof(s_px));
        fill(0, 0, 8 + (int)strlen(label) * 8, 12, 0x90000000u);
        text(4, 2, label, 0xff60ff60u);
        video_overlay_changed();
    }
    return s_px;
}
