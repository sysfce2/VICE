/*
 * pethre.c - PET Hi-Res Emulator Emulation
 *
 * Written by
 *  Olaf 'Rhialto' Seibert <rhialto@falu.nl>
 *
 * This file is part of VICE, the Versatile Commodore Emulator.
 * See README for copyright notice.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 *  02111-1307  USA.
 *
 */

#include "vice.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cmdline.h"
#include "crtc-draw.h"
#include "crtc.h"
#include "crtctypes.h"
#include "log.h"
#include "monitor.h"
#include "pethre.h"
#include "petmem.h"
#include "pets.h"
#include "resources.h"
#include "snapshot.h"
#include "types.h"
#include "uiapi.h"

/*
 * A HRE board consists of a few 74LS-type chips, and is plugged
 * into the sockets for the CRTC and the character ROM.
 * It also plugs into the memory management jumpers /RAM SEL 9, /RAM
 * SEL A, /RAM ON, and the CRTC's MA12.
 *
 * At $E888 there is a write-only register that manipulates the memory
 * mapping through those jumper connections.
 *
 * The CRTC is re-programmed to 512x256 pixels (32 x 2 characters wide
 * by 32 characters high).
 *
 * The RAM-under-ROM from $A000...$DFFF, 16 KB, is used as video
 * memory. It is laid out linearly: the first 64 bytes are the first
 * line of the graphics. This can be accomplished by clever shuffling
 * of the MA (Matrix Address) lines and the RA (Row Address) lines
 * from the CRTC, which count 0...1023 and 0...7 respectively.
 * (In text mode there are only 2000 screen positions so MA counts
 *  0...999).
 *
 * The hi-res is turned ON by resetting the 12th bit ($10), in the
 * high byte, of the screen address. Since this line, MA12, is routed
 * through a jumper, it can be detected by the board.
 * In previous board revisions, this was the "inverse" bit.
 * The value written there ($02) is calculated such that after
 * shuffling it as above, the first byte of screen memory is at $A000.
 *
 * For ROM support code, you want -rom9 324992-02.bin -romA 324993-02.bin
 */

#define HRE_DEBUG_GFX       0

static log_t pethre_log = LOG_DEFAULT;

static int pethre_activate(void);
static int pethre_deactivate(void);

static void pethre_DRAW(uint8_t *p, int xstart, int xend, int scr_rel, int ymod8);

/* ------------------------------------------------------------------------- */

/* Flag: Do we enable the PET HRE?  */
int pethre_enabled = 0;

/* The value last written to the register. It is not reset on reset. */
static uint8_t reg_E888;

static int set_pethre_enabled(int value, void *param)
{
    int val = value ? 1 : 0;

    if (!val) {
        if (pethre_enabled) {
            if (pethre_deactivate() < 0) {
                return -1;
            }
        }
        pethre_enabled = 0;
        return 0;
    } else {
        if (!pethre_enabled) {
            if (pethre_activate() < 0) {
                return -1;
            }
        }
        pethre_enabled = 1;
        return 0;
    }
}

static const resource_int_t resources_int[] = {
    { "PETHRE", 0, RES_EVENT_SAME, NULL,
      &pethre_enabled, set_pethre_enabled, NULL },
    RESOURCE_INT_LIST_END
};

int pethre_resources_init(void)
{
    return resources_register_int(resources_int);
}

void pethre_resources_shutdown(void)
{
}

/* ------------------------------------------------------------------------- */

static const cmdline_option_t cmdline_options[] =
{
    { "-pethre", SET_RESOURCE, CMDLINE_ATTRIB_NONE,
      NULL, NULL, "PETHRE", (resource_value_t)1,
      NULL, "Enable HiRes Emulation Board" },
    { "+pethre", SET_RESOURCE, CMDLINE_ATTRIB_NONE,
      NULL, NULL, "PETHRE", (resource_value_t)0,
      NULL, "Disable HiRes Emulation Board" },
    CMDLINE_LIST_END
};

int pethre_cmdline_options_init(void)
{
    return cmdline_register_options(cmdline_options);
}

/* ------------------------------------------------------------------------- */

void pethre_init(void)
{
    pethre_log = log_open("PETHRE");
}

void pethre_powerup(void)
{
    /* Reset the HRE only on powerup; not on reset. */
    reg_E888 = 0x0F;
    petmem_ramON = 0;
}

void pethre_reset(void)
{
}

static int pethre_activate(void)
{
    if (petres.map != PET_MAP_8296) {
        ui_error("Cannot enable HRE: requires PET model 8296.");
        log_message(pethre_log, "Cannot enable HRE: requires PET model 8296.");
        return -1;
    }

    pethre_reset();
    return 0;
}

static int pethre_deactivate(void)
{
    return 0;
}

void pethre_shutdown(void)
{
    pethre_deactivate();
}

int e888_dump(void)
{
    if (pethre_enabled) {
        char *s = "";
        if (reg_E888 != 0x0F && reg_E888 != 0x83) {
            s = "(unusual value) ";
        }
        mon_out("e888 = %02x %sramON = %d\n", reg_E888, s, petmem_ramON);

        return 0;
    }
    return -1;
}

/* ------------------------------------------------------------------------- */
/* I/O and embedding the CRTC */

#define CRTC_MA12               0x10

/*
 * On zimmers.net is a 1-page document 324890-01_manual.pdf which
 * reads (my translation from German):
 * ------
 * The 8296 HIRES Graphics is an emulation of the 512*256 Commodore High Speed
 * Graphics, which uses the built-in RAM and 6502 microprocessor.
 *
 * A hardware addition is plugged into the socket of the CRTC (UC9) and the
 * character ROM (UC5).
 *
 * The software for emulating the High Speed Graphics is contained in a 4 KB
 * EPROM ($9000, UE10), een BASIC-extension to use the graphics is in a
 * further 4K EPROM ($A000, UE9).
 *
 * The "hidden" RAM behind the EPROM in UE9 and the BASIC ROM is used as screen
 * memory.
 *
 * On the extension board there is a 4-fold DIL switch (1 to 4), with which one
 * can set jumpers JU3, JU4, JU7, JU6 by hardware.
 *
 * By writing a latch at $E888 (decimal 59582), the jumpers JU3...JU7  are
 * controlled by software (independently of the DIL-settings).
 *
 *     Bit   Value   Jumper/Signal
 *
 *      0        1    JU4  /RAMSEL9    1)
 *      1        2    JU3  /RAMSELA    1)
 *      2        4    JU5  /RAMON      1)
 *      3        8       ---
 *      4       16    JU7              2)
 *      5       32    JU6              2)
 *      6       64       ---
 *      7      128          LATCHON    3)
 *
 * 1) On the original motherboard the signals /RAMSELA, /RAMSEL9 and /RAMON
 *    can be controlled by PA0, PA1 and PA3 of the user port, if jumpers
 *    JU3, JU4 and JU5 are closed.
 *    On the adapter, "off" of the DIL-switch means
 *    for 1 and 2: RAMSELA and RAMSEL9 : high
 *    for 3 and 4: Jumper J6 and J7: placed (closed)
 * 2) high = jumper placed
 * 3) high = Latch On
 * ------
 * (unfortunately the document doesn't say what "Latch On" does, but I guess
 * that if it is off, the /RAM* signals are set by the DIL switches.)
 *
 *  - JU1 : set /RAMSELA to GND (do not use JU1/JU3 together)
 *  - JU2 : set /RAMSEL9 to GND (do not use JU2/JU4 together)
 *  - JU3 : set /RAMSELA to Userport PA0 (do not use JU1/JU3 together)
 *  - JU4 : set /RAMSEL9 to Userport PA1 (do not use JU2/JU4 together)
 *  - JU5 : set /RAMON to Userport PA2
 *  - JU6 : set J4 expansion port pin /SELENP to /CSA ($A*** ROM)
 *  - JU7 : set J4 expansion port pin /SELENP to /CS9 ($9*** ROM)
 *
 *  - JU8/JU9 : set JU8, unset JU9: do not use video MA12 for RAM addressing;
 *              unset JU8, set JU9: use video MA12 for RAM addressing.
 */

#define E888_LATCH_ON           0x80
#define E888_NOT_RAM_ON         0x04
#define E888_NOT_RAMSEL_A       0x02
#define E888_NOT_RAMSEL_9       0x01

void crtc_store_hre(uint16_t addr, uint8_t value)
{
    if (pethre_enabled) {
        /* printf("HRE:     enabled... %4x %2x\n", addr, value); */
        /* E888 is the usual address */
        if (addr & 0x0008) {               /* turn ROMs on or off */
            if (value != reg_E888) {
                if (value == 0x0F) {            /* ROMs on */
                    /* printf("HRE: ROMs on\n"); */
                    petmem_ramON = 0;
                    petres.ramsel9 = 0;
                    petres.ramselA = 0;
                    ramsel_changed();
                } else if (value == 0x83) {     /* ROMs off */
                    /* printf("HRE: ROMs off\n"); */
                    petmem_ramON = 1;
                    petres.ramsel9 = 0;
                    petres.ramselA = 0;
                    ramsel_changed();
                }
                reg_E888 = value;
            } else {
                /* printf("HRE: $%04X <- %02X\n", addr, value); */
            }
        } else if (addr & 0x0001) {
            /*
             * The register that contains the high byte of the screen
             * address is used to turn the hi-res graphics on or off.
             * In real hardware, this address line (MA12) goes to a
             * jumper which the HRE board spies on.
             */
            if (crtc.regno == 0x0c) {
                if (value & CRTC_MA12) {     /* off */
                    /* printf("HRE: Hi-Res off: start=%02X\n", value); */
                    crtc_set_hires_draw_callback(NULL);
                } else {                     /* on */
                    /* printf("HRE: Hi-Res  on: start=%02X\n", value); */
                    crtc_set_hires_draw_callback(pethre_DRAW);
                }
            }
        }
    } else {
        /* printf("HRE: not enabled... %4x %2x\n", addr, value); */
    }
}

/* ------------------------------------------------------------------------- */
/* Raster drawing */

#define MA_WIDTH        64
#define MA_LO           (MA_WIDTH - 1)          /* 6 bits */
#define MA_HI           (~MA_LO)
#define RA_SKIP         (7 * MA_WIDTH)          /* 448 */

static void pethre_DRAW(uint8_t *p, int xstart, int xend, int scr_rel, int ymod8)
{
    /*
     * MA = scr_rel starting at $0200, effectively multiplied by
     * 2 to $0400, will end up at $2000 by the shuffling below,
     * which corresponds to $8000 + $2000 as start of the hi-res
     * memory.
     */
    if (ymod8 < 8 && xstart < xend) {
        int ma_hi = scr_rel & MA_HI;    /* MA<11...6> MA is already multi- */
        int ma_lo = scr_rel & MA_LO;    /* MA< 5...0> ...plied by two.     */
        /* Form <MA 11-6><RA 2-0><MA 5-0> */
        uint8_t *screen_rel = mem_ram + 0x8000 +   /* == crtc.screen_base */
                           (ma_hi << 3) + (ymod8 << 6) + ma_lo;
        int width = xend - xstart;

        if (screen_rel >= mem_ram + 0xE000) {
            printf("screen_rel too large: scr_rel=%d, ymod8=%d, "
                    "screen_rel=%04x, xstart=%d xend=%d\n",
                    scr_rel, ymod8, (unsigned int)(screen_rel - mem_ram),
                    xstart, xend);
        }

        if (ma_lo == 0 && width <= MA_WIDTH) {
            /*
             * Simple case: the output is exactly (or fits within)
             * a single 64-char wide block, which corresponds to a
             * normal text line area when the normal ROM support code
             * is used.
             */
            uint32_t *pw = (uint32_t *)p;
            int i;

#if HRE_DEBUG_GFX
            log_message(pethre_log, "pethre_DRAW: xstart=%d, xend=%d, ymod8=%d, scr_rel=%04x screen_rel=%04x", xstart, xend, ymod8, (unsigned int)scr_rel, (unsigned int)(screen_rel - mem_ram));
#endif
            for (i = xstart; i < xend; i++) {
                int d = *screen_rel++;

                *pw++ = dwg_table[d >> 4];
                *pw++ = dwg_table[d & 0x0f];
            }
        } else {
            /*
             * General case: output straddles a 64-char block
             * boundary. This is used for instance if you just turn on
             * the hires without reprogramming the screen width.
             */
            int width0 = MA_WIDTH - ma_lo;
            int i;
            uint32_t *pw = (uint32_t *)p;

            /* printf("scr_rel=%d: ma_lo=%d; jump at %d chars\n", scr_rel, ma_lo, width0); */
            if (width < width0) {
                width0 = width;
            }

            for (i = 0; i < width0; i++) {
                int d = *screen_rel++;

                *pw++ = dwg_table[d >> 4];
                *pw++ = dwg_table[d & 0x0f];
            }
            screen_rel += RA_SKIP;
            for (; i < width; i++) {
                int d = *screen_rel++;

                *pw++ = dwg_table[d >> 4];
                *pw++ = dwg_table[d & 0x0f];
            }
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Snapshot support - not done yet */

static const char module_ram_name[] = "HREMEM";
#define HREMEM_DUMP_VER_MAJOR   1
#define HREMEM_DUMP_VER_MINOR   0

/* Format of the HRE ram snapshot
 *
 * xxx....
 *
 */

static int pethre_ram_write_snapshot_module(snapshot_t *s)
{
    snapshot_module_t *m;

    m = snapshot_module_create(s, module_ram_name,
                               HREMEM_DUMP_VER_MAJOR, HREMEM_DUMP_VER_MINOR);

    SMW_W(m, reg_E888);

    snapshot_module_close(m);

    return 0;
}

static int pethre_ram_read_snapshot_module(snapshot_t *s)
{
    snapshot_module_t *m;
    uint8_t vmajor, vminor;
    uint16_t w;

    m = snapshot_module_open(s, module_ram_name, &vmajor, &vminor);
    if (m == NULL) {
        return -1;
    }

    if (vmajor != HREMEM_DUMP_VER_MAJOR) {
        log_error(pethre_log,
                  "Cannot load HRE RAM module with major version %d",
                  vmajor);
        snapshot_module_close(m);
        return -1;
    }

    w = 0x0F;
    SMR_W(m, &w);
    reg_E888 = (uint8_t)w;

    snapshot_module_close(m);

    return 0;
}

int pethre_snapshot_write_module(snapshot_t *m)
{
    if (pethre_ram_write_snapshot_module(m) < 0) {
        return -1;
    }
    return 0;
}

int pethre_snapshot_read_module(snapshot_t *m)
{
    if (pethre_ram_read_snapshot_module(m) < 0) {
        return 0;       /* for now, to be able to read old snapshots */
    }
    return 0;
}
