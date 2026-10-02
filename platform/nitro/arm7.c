/* The ARM7 side of the DS, as far as the ARM9's NitroSDK can see it: the PXI FIFO.
 *
 * The ARM9 sends 32-bit words tagged by subsystem (sound, power management, RTC, touch panel,
 * card backup, ...). The ARM7 replies through the same FIFO, and the ARM9 dispatches each reply
 * to the callback registered for its tag (PXI_SetFifoRecvCallback, data_02046288). The port
 * replaces the FIFO functions and answers here: a reply runs that callback on the running
 * NitroSDK thread in IRQ mode, as the IPC receive interrupt did.
 *
 * Bring-up state: every tag is reported ready.
 *   SOUND    command lists are walked far enough for the ARM9 to see them finished (the SDK waits
 *            on that); the sound engine itself is to come
 *   RTC      the Vita's local time, in the DS's BCD layout
 *   TP/PM/NVRAM  SPI-style requests are acknowledged; touch sampling reports the Vita's front
 *            panel (kh_arm7_touch)
 * Other tags are logged but not answered yet (backup, wireless). */
#include "nitro/arm7.h"
#include "audio/snd7.h"
#include "nitro/backup.h"

#include "hw/shared_area.h"
#include "log.h"
#include "nitro/cpu.h"

#include <psp2/rtc.h>

#include <stdint.h>
#include <string.h>

typedef void (*PXIFifoCallback)(int tag, uint32_t data, int err);

extern PXIFifoCallback data_02046288[32]; /* the ARM9's FifoRecvCallbackTable */
extern uint16_t data_02046284;            /* PXI_InitFifo's "done" flag */

/* OSSystemWork.pxiHandleChecker[proc] (0x027ffc00 + 0x388): one bit per tag with a handler */
#define PXI_CHECKER_ARM9 0x027fff88u
#define PXI_CHECKER_ARM7 0x027fff8cu

static const char *const s_tag_names[32] = {
    "EX", "USER0", "USER1", "SYSTEM", "NVRAM", "RTC", "TOUCHPANEL", "SOUND", "PM", "MIC", "WM",
    "FS", "OS", "CTRDG", "CARD", "WVR", "CTRDG_Ex", "CTRDG_PHI",
};

static void deliver(void *tag, void *data, void *err)
{
    int t = (int)(uintptr_t)tag;
    if (data_02046288[t])
        data_02046288[t](t, (uint32_t)(uintptr_t)data, (int)(uintptr_t)err);
}

void kh_arm7_reply(int tag, uint32_t data, int err)
{
    kh_cpu_defer(deliver, (void *)(uintptr_t)tag, (void *)(uintptr_t)data, (void *)(uintptr_t)err);
}

void kh_arm7_init(void)
{
    /* the ARM7 has a handler for every tag */
    *(volatile uint32_t *)KH_SHARED(PXI_CHECKER_ARM7) = 0xffffffffu;
}

/* ---- SOUND (tag 7) ----------------------------------------------------------------------
 * The ARM9 sends the head of a linked list of SNDCommand {next, id, arg[4]} (0: "process
 * now"); the ARM7's sound driver (platform/audio/snd7.c) takes it from here. */

static void sound(uint32_t data)
{
    snd7_pxi(data);
}

/* ---- RTC (tag 5): command in bits 8-14, reply command << 8 | result ------------------------ */

#define HW_RTC_BUF 0x027ffde8u /* OSSystemWork.real_time_clock: date, then time */

static uint8_t bcd(int v)
{
    return (uint8_t)((v / 10) << 4 | (v % 10));
}

static void rtc_read_clock(void)
{
    SceDateTime t;
    volatile uint8_t *c = KH_SHARED(HW_RTC_BUF);
    int week;
    sceRtcGetCurrentClockLocalTime(&t);
    week = sceRtcGetDayOfWeek(t.year, t.month, t.day);
    c[0] = bcd(t.year % 100);
    c[1] = bcd(t.month);
    c[2] = bcd(t.day);
    c[3] = (uint8_t)week;
    c[4] = (uint8_t)(bcd(t.hour) | (t.hour >= 12 ? 0x40 : 0)); /* bit 6: afternoon */
    c[5] = bcd(t.minute);
    c[6] = bcd(t.second);
    c[7] = 0;
}

static void rtc(uint32_t data)
{
    uint32_t cmd = (data >> 8) & 0x7f;
    if (cmd >= 0x10 && cmd <= 0x12) /* READ_DATETIME, READ_DATE, READ_TIME */
        rtc_read_clock();
    kh_arm7_reply(5, cmd << 8, 0); /* RTC_PXI_RESULT_SUCCESS */
}

/* ---- SPI devices: touch panel (6), power management (8), NVRAM (4) --------------------------
 * A request is one or more words: bit 25 starts it, bit 24 ends it, bits 16-23 number the
 * words, bits 8-14 of the first hold the command. The reply is END | command << 8 | result. */

#define SPI_START 0x02000000u
#define SPI_END 0x01000000u

#define HW_TOUCHPANEL_BUF 0x027fffaau
enum { TP_SAMPLING = 0x00, TP_AUTO_ON = 0x01, TP_AUTO_OFF = 0x02, TP_AUTO_SAMPLING = 0x10 };

static uint32_t s_spi_cmd[32];
static volatile int s_tp_auto;
static volatile uint32_t s_tp_sample; /* SPITpData: x:12, y:12, touch:1, validity:2 */

void kh_arm7_touch(int touching, int raw_x, int raw_y)
{
    s_tp_sample = touching ? ((uint32_t)raw_x & 0xfff) | ((uint32_t)raw_y & 0xfff) << 12 | 1u << 24
                           : 0;
    if (s_tp_auto) {
        volatile uint16_t *buf = KH_SHARED(HW_TOUCHPANEL_BUF);
        buf[0] = (uint16_t)s_tp_sample;
        buf[1] = (uint16_t)(s_tp_sample >> 16);
        kh_arm7_reply(6, TP_AUTO_SAMPLING << 8, 0);
    }
}

static void spi(int tag, uint32_t data)
{
    uint32_t cmd;
    if (data & SPI_START)
        s_spi_cmd[tag] = (data >> 8) & 0x7f;
    if (!(data & SPI_END))
        return;
    cmd = s_spi_cmd[tag];
    if (tag == 6) {
        volatile uint16_t *buf = KH_SHARED(HW_TOUCHPANEL_BUF);
        if (cmd == TP_SAMPLING) {
            buf[0] = (uint16_t)s_tp_sample;
            buf[1] = (uint16_t)(s_tp_sample >> 16);
        } else if (cmd == TP_AUTO_ON) {
            s_tp_auto = 1;
        } else if (cmd == TP_AUTO_OFF) {
            s_tp_auto = 0;
        }
    }
    kh_arm7_reply(tag, SPI_END | cmd << 8, 0); /* SPI_PXI_RESULT_SUCCESS */
}

static void receive(int tag, uint32_t data, int err)
{
    static uint32_t logged[32];
    if (logged[tag]++ < 8)
        LOGV("arm7: %s(%d) <- %08x%s", s_tag_names[tag] ? s_tag_names[tag] : "?", tag, data,
            err ? " err" : "");
    switch (tag) {
    case 7: sound(data); break;
    case 5: rtc(data); break;
    case 4: case 6: case 8: spi(tag, data); break;
    case 11: kh_backup_pxi(data); break; /* FS: the CARD library's backup requests */
    case 13: /* CTRDG: the ARM7 acknowledges INIT_MODULE_INFO (CTRDGi_InitCallback) */
        if ((data & 0x3f) == 1)
            kh_arm7_reply(13, 1, 0);
        break;
    default: break;
    }
}

/* ---- the ARM9's PXI functions ------------------------------------------------------------- */

void PXI_InitFifo(void)
{
    if (data_02046284)
        return;
    data_02046284 = 1;
    *(volatile uint32_t *)KH_SHARED(PXI_CHECKER_ARM9) = 0;
    memset(data_02046288, 0, sizeof(data_02046288));
    kh_arm7_init();
}

int32_t PXI_SendWordByFifo(uint32_t tag, uint32_t data, uint32_t err)
{
    KH_PROBE("PXI_SendWordByFifo");
    receive((int)(tag & 31), data, (int)err);
    return 0; /* PXI_FIFO_SUCCESS */
}
