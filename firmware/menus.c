// ATAboy Menu System — runs on core 1
// All terminal I/O goes through cdc_printf/cdc_putchar/cdc_getchar_timeout_us
// which use pico SDK queues. Core 0 drains/fills these queues in cdc_task().
// NEVER call tud_task(), tud_cdc_*(), or any TinyUSB function from here.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "ide.h"
#include "config.h"
#include "drive.h"
#include "version.h"
#include "pico/util/queue.h"

// ---------------------------------------------------------------------------
//  CDC queue I/O (queues owned by ATAboy.c)
// ---------------------------------------------------------------------------

extern queue_t cdc_tx_queue;
extern queue_t cdc_rx_queue;
extern volatile bool cdc_connected;
extern volatile bool is_mounted;
extern volatile bool media_changed_waiting;

static void cdc_putchar(char c) {
    queue_add_blocking(&cdc_tx_queue, &c);
}

static void cdc_puts(const char *s) {
    while (*s) cdc_putchar(*s++);
}

static void cdc_printf(const char *fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    cdc_puts(buf);
}

static int cdc_getchar_timeout_us(uint64_t timeout_us) {
    char c;
    if (queue_try_remove(&cdc_rx_queue, &c)) return (uint8_t)c;
    absolute_time_t deadline = make_timeout_time_us(timeout_us);
    while (!time_reached(deadline)) {
        if (queue_try_remove(&cdc_rx_queue, &c)) return (uint8_t)c;
        busy_wait_us_32(100);
    }
    return PICO_ERROR_TIMEOUT;
}

static void cdc_flush(void) {
    sleep_ms(5);
}

// ---------------------------------------------------------------------------
//  Box-drawing helper — emit a UTF-8 box char repeated N times
// ---------------------------------------------------------------------------

static void emit_n(const char *utf8_char, int n) {
    for (int i = 0; i < n; i++) cdc_puts(utf8_char);
}

// Box-drawing character constants (UTF-8, 3 bytes each)
#define BOX_HH   "\xe2\x95\x90"  /* double horizontal */
#define BOX_HL   "\xe2\x94\x80"  /* single horizontal */
#define BOX_VH   "\xe2\x95\x91"  /* double vertical   */
#define BOX_VL   "\xe2\x94\x82"  /* single vertical   */
#define BOX_TL   "\xe2\x95\x94"  /* top-left double   */
#define BOX_TR   "\xe2\x95\x97"  /* top-right double  */
#define BOX_BL   "\xe2\x95\x9a"  /* bot-left double   */
#define BOX_BR   "\xe2\x95\x9d"  /* bot-right double  */
#define BOX_TD   "\xe2\x95\xa4"  /* T-down mixed      */
#define BOX_TU   "\xe2\x94\xb4"  /* T-up single       */
#define BOX_ML   "\xe2\x95\x9f"  /* left-T mixed      */
#define BOX_MR   "\xe2\x95\xa2"  /* right-T mixed     */
#define BOX_MLD  "\xe2\x95\xa0"  /* left-T double     */
#define BOX_MRD  "\xe2\x95\xa3"  /* right-T double    */
#define BOX_LTD  "\xe2\x94\x9c"  /* left-T single     */
#define BOX_ARROW "\xe2\x96\xba" /* right arrow       */
#define BOX_ARRU "\xe2\x86\x91"  /* up arrow          */
#define BOX_ARRD "\xe2\x86\x93"  /* down arrow        */
#define BOX_ARRR "\xe2\x86\x92"  /* right arrow thin  */
#define BOX_ARRL "\xe2\x86\x90"  /* left arrow thin   */

// ---------------------------------------------------------------------------
//  State
// ---------------------------------------------------------------------------

static char hdd_model_raw[48] = "";
static char hdd_status_text[80] = "";
static uint16_t detect_cyls = 0;
static uint8_t  detect_heads = 0;
static uint8_t  detect_spt = 0;
static uint16_t cur_cyls = 0;
static uint8_t  cur_heads = 0;
static uint8_t  cur_spt = 0;
static uint64_t total_lba_sectors = 0;
static bool     use_lba_mode = false;

static uint32_t seen_api_gen = 0;      // drive.api_gen the menu last synced to
static bool show_detect_result = false;
static bool force_detect = false;
static bool error_is_notice = false;   // error box is informational: dismissing it changes nothing
static int  confirm_type = 0;

#define RESET       "\033[0m"
#define BG_BLUE     "\033[44m"
#define FG_WHITE    "\033[37;1m"
#define FG_RED      "\033[91m"
#define FG_GREEN    "\033[92m"
#define FG_YELLOW   "\033[33;1m"
#define HIDE_CUR    "\033[?25l"
#define CLR_SCR     "\033[2J"
#define SEL_RED     "\033[41;37;1m"

#define KEY_UP    1001
#define KEY_DOWN  1002
#define KEY_LEFT  1003
#define KEY_RIGHT 1004
#define KEY_ENTER 13
#define KEY_ESC   27
#define KEY_F10   1010
#define KEY_CTRL_L 12     // redraw the whole screen

typedef enum {
    SCREEN_MAIN,
    SCREEN_FEATURES,
    SCREEN_CONFIRM,
    SCREEN_MOUNTED,
    SCREEN_DEBUG
} screen_t;

static screen_t current_screen = SCREEN_MAIN;
static screen_t confirm_return_screen = SCREEN_MAIN;
static bool needs_full_redraw = true;
static bool last_cdc_connected = false;

// ---------------------------------------------------------------------------
//  Helpers
// ---------------------------------------------------------------------------

static void draw_at(int x, int y, const char *text) {
    cdc_printf("\033[%d;%dH%s", y, x, text);
}

static int visible_strlen(const char *s) {
    int len = 0;
    while (*s) {
        if (*s == '\033') {
            s++;
            if (*s == '[') { s++; while (*s && (*s < '@' || *s > '~')) s++; if (*s) s++; }
        } else { len++; s++; }
    }
    return len;
}

static void decode_ata_string(uint16_t *buf, int offset, int len_words, char *out) {
    int idx = 0;
    for (int i = 0; i < len_words; i++) {
        uint16_t w = buf[offset + i];
        out[idx++] = (char)(w >> 8);
        out[idx++] = (char)(w & 0xFF);
    }
    out[idx] = '\0';
    while (idx > 0 && out[idx - 1] == ' ') out[--idx] = '\0';
}

static void get_large_geometry(uint16_t n_cyl, uint8_t n_hd, uint8_t n_spt,
                               uint16_t *l_cyl, uint8_t *l_hd) {
    uint32_t total = (uint32_t)n_cyl * n_hd;
    uint8_t heads = n_hd;
    while (heads < 255 && (total / heads) > 65535) heads++;
    *l_hd = heads;
    *l_cyl = (uint16_t)(total / heads);
}

// ---------------------------------------------------------------------------
//  Help text display (right panel)
// ---------------------------------------------------------------------------

static void print_help(const char *text) {
    int start_row = 6, col = 57, width = 21, max_rows = 11;
    for (int i = 0; i < max_rows; i++)
        cdc_printf("\033[%d;%dH%*s", start_row + i, col, width, "");
    cdc_printf("\033[%d;%dH", start_row, col);
    int current_row = start_row, line_pos = 0, i = 0;
    while (text[i] && (current_row - start_row) < max_rows) {
        int word_len = 0;
        while (text[i + word_len] && text[i + word_len] != ' ' && text[i + word_len] != '\n') word_len++;
        if (line_pos + word_len > width && line_pos > 0) {
            current_row++;
            if ((current_row - start_row) >= max_rows) break;
            line_pos = 0;
            cdc_printf("\033[%d;%dH", current_row, col);
        }
        for (int j = 0; j < word_len; j++) { cdc_putchar(text[i++]); line_pos++; }
        if (text[i] == '\n') {
            current_row++;
            if ((current_row - start_row) >= max_rows) break;
            line_pos = 0;
            cdc_printf("\033[%d;%dH", current_row, col);
            i++;
        } else if (text[i] == ' ') {
            if (line_pos < width) { cdc_putchar(' '); line_pos++; }
            i++;
        }
    }
}

// ---------------------------------------------------------------------------
//  BIOS Frame — 80 columns
//  Row 2 top, rows 3-17 content+divider, row 18 mid, 19-20 keys,
//  row 21 mid, 22-23 status, row 24 bottom
// ---------------------------------------------------------------------------

static void draw_bios_frame(void) {
    cdc_printf(RESET BG_BLUE FG_WHITE CLR_SCR HIDE_CUR "\033[H");
    // Paint every cell blue: terminals without background-color erase
    // (screen, some serial terminals) clear to their default colour.
    for (int r = 1; r <= 24; r++) cdc_printf("\033[%d;1H%80s", r, "");
    cdc_printf("\033[1;8H" FG_WHITE "ATAboy Setup Utility v" FW_VERSION " - Copyright (C) 2026 obsoletetech.us");

    bool feat = (current_screen == SCREEN_FEATURES);

    // Row 2: top border (80 visible chars)
    // Main:     1 + 38 + 1 + 39 + 1 = 80
    // Features: 1 + 53 + 1 + 24 + 1 = 80
    cdc_puts("\033[2;1H");
    cdc_puts(BOX_TL);
    if (!feat) { emit_n(BOX_HH, 38); cdc_puts(BOX_TD); emit_n(BOX_HH, 39); }
    else       { emit_n(BOX_HH, 53); cdc_puts(BOX_TD); emit_n(BOX_HH, 24); }
    cdc_puts(BOX_TR);

    // Rows 3-23: side pipes + vertical divider
    for (int i = 3; i < 24; i++) {
        cdc_printf("\033[%d;1H" BOX_VH, i);
        cdc_printf("\033[%d;80H" BOX_VH, i);
        if (i < 18) {
            if (!feat) cdc_printf("\033[%d;40H" BOX_VL, i);
            else       cdc_printf("\033[%d;55H" BOX_VL, i);
        }
    }

    // Row 18: mid border (80 visible chars)
    // Main:     1 + 38 + 1 + 39 + 1 = 80
    // Features: 1 + 53 + 1 + 24 + 1 = 80
    cdc_puts("\033[18;1H");
    cdc_puts(BOX_ML);
    if (!feat) { emit_n(BOX_HL, 38); cdc_puts(BOX_TU); emit_n(BOX_HL, 39); }
    else       { emit_n(BOX_HL, 53); cdc_puts(BOX_TU); emit_n(BOX_HL, 24); }
    cdc_puts(BOX_MR);

    // Row 21: full-width mid border  1 + 78 + 1 = 80
    cdc_puts("\033[21;1H");
    cdc_puts(BOX_ML); emit_n(BOX_HL, 78); cdc_puts(BOX_MR);

    // Row 24: bottom border  1 + 78 + 1 = 80
    cdc_puts("\033[24;1H");
    cdc_puts(BOX_BL); emit_n(BOX_HH, 78); cdc_puts(BOX_BR);
}

// ---------------------------------------------------------------------------
//  HDD Status (rows 22-23) — shared by main + features screens
// ---------------------------------------------------------------------------

static void draw_hdd_status(void) {
    const char *display;
    bool is_error = false;
    if (show_detect_result && hdd_status_text[0]) { display = hdd_status_text; is_error = true; }
    else if (hdd_model_raw[0]) { display = hdd_model_raw; }
    else { display = "IDENT not run yet"; }

    const char *dev_label = (config.dev_base == 0xB0) ? " Slave" : " Master";
    int label_len = 13;
    int text_len = visible_strlen(display);
    int dev_len = (hdd_model_raw[0] && !is_error) ? (int)strlen(dev_label) : 0;
    int start_x = (80 - (label_len + text_len + dev_len)) / 2;

    // Blank both status rows with spaces rather than erase-to-end-of-line,
    // which clears to the terminal's default colour without bce.
    cdc_printf(RESET BG_BLUE FG_WHITE "\033[22;2H%78s\033[23;2H%78s", "", "");
    draw_at(start_x, 22, FG_WHITE "Current HDD: ");
    if (is_error)        cdc_printf("\033[91;1m%s" RESET BG_BLUE FG_WHITE, display);
    else if (hdd_model_raw[0]) cdc_printf("\033[32;1m%s" FG_YELLOW "%s" RESET BG_BLUE FG_WHITE, display, dev_label);
    else                 cdc_printf(FG_YELLOW "%s" RESET BG_BLUE FG_WHITE, display);

    cdc_puts(RESET BG_BLUE FG_WHITE);
    cdc_puts("\033[22;78H  " BOX_VH);

    char geo_vals[64];
    if (use_lba_mode) {
        uint32_t mb = (uint32_t)((uint64_t)total_lba_sectors * 512 / 1048576);
        snprintf(geo_vals, sizeof(geo_vals), "LBA Mode Active (%lu MB)", (unsigned long)mb);
    } else {
        snprintf(geo_vals, sizeof(geo_vals), "%d Cyl / %d Hd / %d SPT", cur_cyls, cur_heads, cur_spt);
    }

    int geo_total_len = 18 + strlen(geo_vals);
    int geo_x = (80 - geo_total_len) / 2;
    draw_at(geo_x, 23, FG_WHITE "Current Geometry: ");

    bool is_valid = (use_lba_mode && total_lba_sectors > 0) ||
                    (!use_lba_mode && cur_cyls > 0 && cur_heads > 0 && cur_spt > 0);
    cdc_printf(is_valid ? "\033[92;1m%s" : "\033[91;1m%s", geo_vals);

    cdc_puts(RESET BG_BLUE FG_WHITE);
    cdc_puts("\033[23;78H  " BOX_VH);
    cdc_puts("\033[24;79H");
    cdc_flush();
}

// ---------------------------------------------------------------------------
//  Main Menu
// ---------------------------------------------------------------------------

static void update_main_menu(void) {
    const char *items[] = {
        "  Auto Detect & Set Geometry",        // 0 — left,  row 6
        "  Mount HDD to USB Mass Storage",     // 1 — left,  row 8
        BOX_ARROW " ATAboy Features Setup",    // 2 — right, row 6
        "  Load Setup Defaults",               // 3 — right, row 8
        "  Save Setup to EEPROM",              // 4 — right, row 10
        "  Park Heads for Power-Off"           // 5 — left,  row 10
    };

    for (int i = 0; i < 6; i++) {
        int col = (i < 2 || i == 5) ? 4 : 43;
        int row = (i == 5) ? 10 : 6 + (i < 2 ? i : (i - 2)) * 2;
        cdc_printf("\033[%d;%dH", row, col);
        if (i == config.main_selected)
            cdc_printf(SEL_RED " %-28s " RESET BG_BLUE FG_WHITE, items[i]);
        else
            cdc_printf(FG_YELLOW " %-28s  " FG_WHITE, items[i]);
    }

    cdc_printf("\033[19;3H ESC: Quit to Main Menu                         "
               BOX_ARRU " " BOX_ARRD " " BOX_ARRR " " BOX_ARRL ": Select Item");
    cdc_puts("\033[20;3H F10: Save Current Setup to EEPROM               Enter: Select");

    draw_hdd_status();
}

// ---------------------------------------------------------------------------
//  Features Menu
// ---------------------------------------------------------------------------

#define FEAT_ITEMS 7
#define FEAT_DEBUG 6

static void cycle_read_timeout(void) {
    static const uint32_t steps[] = {1000, 2000, 5000, 10000, 15000, 30000, 60000};
    int n = sizeof(steps) / sizeof(steps[0]);
    int i = 0;
    while (i < n && steps[i] <= config.read_timeout_ms) i++;
    config.read_timeout_ms = (i < n) ? steps[i] : steps[0];
}

static void update_features_menu(void) {
    const char *labels[] = {"Write Protect", "Auto Mount at Start", "IORDY", "INTRQ",
                            "Read Timeout", "No-Retry Reads", "Debug Mode"};
    const char *helps[] = {
        "Prevents any write commands from reaching the HDD.",
        "Automatically mounts the drive to USB on power-up sequence.",
        "Enables hardware IORDY (pin 27) flow control on the IDE bus.  Toggling this may help with picky drives.",
        "Enables hardware INTRQ (pin 28) for faster IDE command completion.  Toggling this may help with picky drives.",
        "How long to wait for each sector before giving up. Longer lets a failing drive finish its own retries. Enter cycles.",
        "Read with the drive's internal retries off (old drives only). Fast first pass over bad areas.",
        "Open low-level drive diagnostics and register status screen."
    };

    cdc_puts("\033[3;63H" FG_WHITE "Item Help");
    // col 55: single-T left + 24 dashes + mixed-T right = 26 chars
    cdc_puts("\033[4;55H" BOX_LTD);
    emit_n(BOX_HL, 24);
    cdc_puts(BOX_MR);

    for (int i = 0; i < FEAT_ITEMS; i++) {
        int row = 4 + i;
        cdc_printf("\033[%d;4H" FG_WHITE "%-25s", row, labels[i]);
        cdc_printf("\033[%d;35H" FG_YELLOW "[", row);
        cdc_puts(i == config.feat_selected ? SEL_RED : FG_YELLOW);

        if (i == 0)      cdc_printf("%-8s", config.drive_write_protected ? "Enabled" : "Disabled");
        else if (i == 1) cdc_printf("%-8s", config.auto_mount ? "Enabled" : "Disabled");
        else if (i == 2) cdc_printf("%-8s", config.iordy_enabled ? "Enabled" : "Disabled");
        else if (i == 3) cdc_printf("%-8s", config.intrq_enabled ? "Enabled" : "Disabled");
        else if (i == 4) { char t[12]; snprintf(t, sizeof(t), "%lu s", (unsigned long)(config.read_timeout_ms / 1000));
                           if (config.read_timeout_ms < 1000) snprintf(t, sizeof(t), "%lu ms", (unsigned long)config.read_timeout_ms);
                           cdc_printf("%-8s", t); }
        else if (i == 5) cdc_printf("%-8s", config.no_retry ? "Enabled" : "Disabled");
        else if (i == 6) cdc_printf("%-8s", "Enter");

        cdc_puts(RESET BG_BLUE FG_WHITE "]");
        if (i == config.feat_selected) print_help(helps[i]);
    }
    cdc_printf("\033[19;3H ESC: Quit to Main Menu                         "
               BOX_ARRU " " BOX_ARRD ": Select Item");
    cdc_puts("\033[20;3H F10: Save Current Setup to EEPROM               Enter: Select");

    draw_hdd_status();
}

// ---------------------------------------------------------------------------
//  Input handler
// ---------------------------------------------------------------------------

static int get_input(void) {
    int c = cdc_getchar_timeout_us(100000);
    if (c == PICO_ERROR_TIMEOUT) return -1;
    if (c == 27) {
        int n1 = cdc_getchar_timeout_us(10000);
        if (n1 == '[') {
            int n2 = cdc_getchar_timeout_us(10000);
            if (n2 == 'A') return KEY_UP;
            if (n2 == 'B') return KEY_DOWN;
            if (n2 == 'C') return KEY_RIGHT;
            if (n2 == 'D') return KEY_LEFT;
            if (n2 == '2') { if (cdc_getchar_timeout_us(10000) == '1') if (cdc_getchar_timeout_us(10000) == '~') return KEY_F10; }
            // Consume unrecognized CSI sequences (e.g. PgUp/PgDn) so they don't trigger ESC
            if (n2 >= '0' && n2 <= '9') {
                int ch;
                do { ch = cdc_getchar_timeout_us(10000); } while (ch != '~' && ch != PICO_ERROR_TIMEOUT);
                return -1;
            }
        }
        return KEY_ESC;
    }
    return c;
}

// ---------------------------------------------------------------------------
//  Confirm box — 52 wide, red bg + shadow
// ---------------------------------------------------------------------------

static void draw_confirm_box(const char *message) {
    int box_w = 52, start_col = 14, start_row = 10;

    // Shadow
    for (int i = 0; i < 5; i++) cdc_printf("\033[%d;%dH\033[40m%*s", start_row+1+i, start_col+2, box_w, "");
    // Red bg
    for (int i = 0; i < 5; i++) cdc_printf("\033[%d;%dH\033[41m%*s", start_row+i, start_col, box_w, "");

    cdc_puts(SEL_RED);
    cdc_printf("\033[%d;%dH", start_row, start_col);
    cdc_puts(BOX_TL); emit_n(BOX_HH, 50); cdc_puts(BOX_TR);

    cdc_printf("\033[%d;%dH" BOX_VH "%50s" BOX_VH, start_row+1, start_col, "");

    int msg_len = visible_strlen(message);
    if (msg_len > 50) msg_len = 50;
    int pl = (50 - msg_len) / 2, pr = 50 - msg_len - pl;
    cdc_printf("\033[%d;%dH" BOX_VH "%*s%s%*s" BOX_VH, start_row+2, start_col, pl, "", message, pr, "");

    cdc_printf("\033[%d;%dH" BOX_VH "%50s" BOX_VH, start_row+3, start_col, "");

    cdc_printf("\033[%d;%dH", start_row+4, start_col);
    cdc_puts(BOX_BL); emit_n(BOX_HH, 50); cdc_puts(BOX_BR);
    cdc_puts(RESET);
}

// ---------------------------------------------------------------------------
//  Error box — 52 wide, 7 tall
// ---------------------------------------------------------------------------

static const char *error_title = "ERROR!";

static void draw_error_box(const char *error_msg, bool show_force) {
    int box_w = 52, start_col = 14, start_row = 9;

    for (int i = 0; i < 7; i++) cdc_printf("\033[%d;%dH\033[40m%*s", start_row+1+i, start_col+2, box_w, "");
    for (int i = 0; i < 7; i++) cdc_printf("\033[%d;%dH\033[41m%*s", start_row+i, start_col, box_w, "");

    cdc_puts(SEL_RED);
    cdc_printf("\033[%d;%dH", start_row, start_col);
    cdc_puts(BOX_TL); emit_n(BOX_HH, 50); cdc_puts(BOX_TR);

    int tl = (int)strlen(error_title), tp = (50 - tl) / 2;
    cdc_printf("\033[%d;%dH" BOX_VH "%*s%s%*s" BOX_VH, start_row+1, start_col, tp, "", error_title, 50 - tl - tp, "");

    cdc_printf("\033[%d;%dH", start_row+2, start_col);
    cdc_puts(BOX_MLD); emit_n(BOX_HH, 50); cdc_puts(BOX_MRD);

    cdc_printf("\033[%d;%dH" BOX_VH "%50s" BOX_VH, start_row+3, start_col, "");
    int vl = visible_strlen(error_msg);
    int xo = (50 - vl) / 2;
    cdc_printf("\033[%d;%dH\033[41;97m%s", start_row+3, start_col+1+xo, error_msg);

    cdc_puts(SEL_RED);
    cdc_printf("\033[%d;%dH" BOX_VH "%50s" BOX_VH, start_row+4, start_col, "");

    if (show_force) {
        cdc_printf("\033[%d;%dH" BOX_VH "          \033[103;30m[OK]\033[0m" SEL_RED "                  " FG_YELLOW "[F: Force]" SEL_RED "        " BOX_VH, start_row+5, start_col);
    } else {
        cdc_printf("\033[%d;%dH" BOX_VH "                       \033[103;30m[OK]\033[0m" SEL_RED "                       " BOX_VH, start_row+5, start_col);
    }

    cdc_printf("\033[%d;%dH", start_row+6, start_col);
    cdc_puts(BOX_BL); emit_n(BOX_HH, 50); cdc_puts(BOX_BR);
    cdc_puts(RESET);
    cdc_flush();
}

// ---------------------------------------------------------------------------
//  Geometry selection overlay — 64 wide, 14 tall
// ---------------------------------------------------------------------------

static void draw_selection_menu_ex(const uint16_t *id, int selected_idx, bool force_mode, const char *note) {
    int box_w = 64, start_col = 9, start_row = 5;

    uint16_t n_cyl = id[1];
    uint8_t  n_hd  = (uint8_t)id[3];
    uint8_t  n_spt = (uint8_t)id[6];

    bool lba_supp = (id[49] & 0x0200);
    uint32_t lba28 = id[60] | ((uint32_t)id[61] << 16);
    uint64_t lba48 = ((uint64_t)id[103] << 48) | ((uint64_t)id[102] << 32) |
                     ((uint64_t)id[101] << 16) | ((uint64_t)id[100]);
    uint64_t lba_total = lba48 ? lba48 : (uint64_t)lba28;

    uint16_t l_cyl; uint8_t l_hd;
    get_large_geometry(n_cyl, n_hd, n_spt, &l_cyl, &l_hd);

    uint32_t n_size = (uint32_t)(((uint64_t)n_cyl * n_hd * n_spt * 512) / 1048576);
    uint32_t l_size = (uint32_t)(((uint64_t)l_cyl * l_hd * n_spt * 512) / 1048576);
    uint32_t lba_size = (uint32_t)((lba_total * 512ULL) / 1048576ULL);
    bool lba48_supp = ((id[83] & (1 << 10)) != 0) || (lba_total > 0x0FFFFFFFULL);

    // Shadow + bg
    for (int i = 0; i < 14; i++) cdc_printf("\033[%d;%dH\033[40m%*s", start_row+1+i, start_col+2, box_w, "");
    for (int i = 0; i < 14; i++) cdc_printf("\033[%d;%dH\033[41m%*s", start_row+i, start_col, box_w, "");

    cdc_puts(SEL_RED);

    // Row 0: 1+62+1 = 64
    cdc_printf("\033[%d;%dH", start_row, start_col);
    cdc_puts(BOX_TL); emit_n(BOX_HH, 62); cdc_puts(BOX_TR);

    // Row 1: model name centered
    int interior_w = 62;
    const char *det_dev_label = (config.dev_base == 0xB0) ? " Slave" : " Master";
    int vis_model = strlen(hdd_model_raw);
    int vis_total = 14 + vis_model + (int)strlen(det_dev_label);
    int title_x = start_col + 1 + ((interior_w - vis_total) / 2);
    cdc_printf("\033[%d;%dH%*s", start_row+1, start_col+1, interior_w, "");
    cdc_printf("\033[%d;%dH" BOX_VH, start_row+1, start_col);
    cdc_printf("\033[%d;%dHDetected HDD: \033[32;1m%s" FG_YELLOW "%s\033[0m", start_row+1, title_x, hdd_model_raw, det_dev_label);
    cdc_printf(SEL_RED "\033[%d;%dH" BOX_VH, start_row+1, start_col+63);

    cdc_printf("\033[%d;%dH" BOX_VH "                Select Drive Geometry Option:                 " BOX_VH, start_row+2, start_col);

    // Row 3: double separator
    cdc_printf("\033[%d;%dH", start_row+3, start_col);
    cdc_puts(BOX_MLD); emit_n(BOX_HH, 62); cdc_puts(BOX_MRD);

    cdc_printf("\033[%d;%dH" BOX_VH "  MODE         SIZE       CYLS      HEADS      SPT            " BOX_VH, start_row+4, start_col);

    // Row 5: single-line separator
    cdc_printf("\033[%d;%dH" BOX_VH " ", start_row+5, start_col);
    emit_n(BOX_HL, 60);
    cdc_puts(" " BOX_VH);

    const char *modes[] = {"NORMAL  ", "LARGE   ", "LBA     ", "MANUAL  "};
    const char *lba_label = lba48_supp ? "LBA48  " : "LBA     ";

    for (int i = 0; i < 4; i++) {
        cdc_printf("\033[%d;%dH" BOX_VH " ", start_row+6+i, start_col);
        bool is_lba = (i == 2), lba_na = (is_lba && !lba_supp);
        bool row_na = lba_na || (force_mode && (i == 0 || i == 1 || i == 2));

        if (i == selected_idx) {
            if (row_na) cdc_printf("\033[41;33m %-8s \033[0m" SEL_RED, is_lba ? lba_label : modes[i]);
            else        cdc_printf("\033[103;30m %-8s \033[0m" SEL_RED, is_lba ? lba_label : modes[i]);
        } else {
            if (row_na) cdc_printf("\033[90m %-8s ", is_lba ? lba_label : modes[i]);
            else        cdc_printf(FG_WHITE " %-8s ", is_lba ? lba_label : modes[i]);
        }

        if (force_mode && (i == 0 || i == 1 || i == 2)) {
            cdc_puts("\033[90m      --- MB    (No IDENTIFY Data)                 \033[0m" SEL_RED);
        } else if (lba_na) {
            cdc_puts("\033[90m      --- MB    (Not Supported by Drive)           \033[0m" SEL_RED);
        } else if (i == 0) {
            cdc_printf("   %4lu MB    %-5u       %-3u       %-3u            ", (unsigned long)n_size, n_cyl, n_hd, n_spt);
        } else if (i == 1) {
            cdc_printf("   %4lu MB    %-5u       %-3u       %-3u            ", (unsigned long)l_size, l_cyl, l_hd, n_spt);
        } else if (i == 2) {
            const char *bits = lba48_supp ? "48-bit" : "28-bit";
            if (lba_size > 9999) cdc_printf("   %4lu GB           LBA (%s)                  ", (unsigned long)(lba_size/1024), bits);
            else                 cdc_printf("   %4lu MB           LBA (%s)                  ", (unsigned long)lba_size, bits);
        } else if (i == 3) {
            if (detect_cyls > 0) {
                uint32_t ms = (uint32_t)((uint64_t)detect_cyls * detect_heads * detect_spt * 512 / 1048576);
                cdc_printf("   %4lu MB    %-5u       %-3u       %-3u            ", (unsigned long)ms, detect_cyls, detect_heads, detect_spt);
            } else cdc_puts("   ---- MB    -----       ---       ---            ");
        }
        cdc_puts(BOX_VH);
    }

    // Row 10
    cdc_printf("\033[%d;%dH", start_row+10, start_col);
    cdc_puts(BOX_MLD); emit_n(BOX_HH, 62); cdc_puts(BOX_MRD);

    cdc_printf("\033[%d;%dH" BOX_VH "  " BOX_ARRU " " BOX_ARRD ": Mode    TAB: Change CHS     Enter: Select    Esc: Quit " BOX_VH, start_row+11, start_col);
    if (note && note[0])
        cdc_printf("\033[%d;%dH" BOX_VH "\033[93;1m%-62.62s\033[0m" SEL_RED BOX_VH, start_row+12, start_col, note);
    else
        cdc_printf("\033[%d;%dH" BOX_VH "  \033[33m   LBA Recommended for modern drives; NORMAL for legacy.    \033[0m" SEL_RED BOX_VH, start_row+12, start_col);

    // Row 13: bottom
    cdc_printf("\033[%d;%dH", start_row+13, start_col);
    cdc_puts(BOX_BL); emit_n(BOX_HH, 62); cdc_puts(BOX_BR);

    cdc_puts(RESET);
    cdc_flush();
}

// ---------------------------------------------------------------------------
//  Geometry picker + manual CHS entry (Auto Detect and F: Force)
// ---------------------------------------------------------------------------

#define PICK_ROW 14                       // MANUAL row of the selection box

// Ctrl+L inside an overlay on the main screen: repaint what's underneath;
// the caller repaints its overlay.
static void redraw_main_underlay(void) {
    draw_bios_frame();
    update_main_menu();
}

static const int chs_col[3]    = {9+26, 9+38, 9+48};
static const int chs_digits[3] = {5, 2, 2};

static bool api_took_over(void) {
    return drive.api_gen != seen_api_gen || is_mounted;
}

// Ask for C/H/S on the MANUAL row.  Returns 1 with valid values in *c/*h/*s,
// 0 when aborted (Esc, or the API took over): nothing is applied either way.
// An out-of-range value leaves a message in 'note' and returns -1.
static int manual_chs_entry(const uint16_t *id, bool force_mode, uint16_t *c, uint8_t *h, uint8_t *s,
                            char *note, size_t note_len) {
    long mf[3] = {detect_cyls, detect_heads, detect_spt};
    for (int f = 0; f < 3; f++) {
        char ib[8] = {0};
        int p = 0;
        draw_at(chs_col[f], PICK_ROW, "\033[103;30m     \033[0m");
        cdc_printf("\033[103;30m\033[%d;%dH", PICK_ROW, chs_col[f]);
        while (true) {
            int k = get_input();
            if (api_took_over()) return 0;
            if (k == -1) { tight_loop_contents(); continue; }
            if (k == KEY_CTRL_L) {
                redraw_main_underlay();
                draw_selection_menu_ex(id, 3, force_mode, note);
                for (int i = 0; i < f; i++) { draw_at(chs_col[i], PICK_ROW, SEL_RED); cdc_printf("%-5ld", mf[i]); }
                ib[p] = '\0';
                draw_at(chs_col[f], PICK_ROW, "\033[103;30m     ");
                cdc_printf("\033[%d;%dH%s", PICK_ROW, chs_col[f], ib);
                continue;
            }
            if (k >= '0' && k <= '9' && p < chs_digits[f]) { ib[p++] = (char)k; cdc_putchar((char)k); }
            else if ((k == 8 || k == 127) && p > 0) { p--; cdc_printf("\b \b\033[%d;%dH", PICK_ROW, chs_col[f] + p); }
            else if (k == KEY_ENTER || k == '\t') {
                if (p > 0) mf[f] = atol(ib);
                draw_at(chs_col[f], PICK_ROW, RESET SEL_RED);
                cdc_printf("%-5ld", mf[f]);
                break;
            }
            else if (k == KEY_ESC) { cdc_puts(RESET); return 0; }
        }
    }
    cdc_puts(RESET);
    if (mf[0] < 1 || mf[0] > 65535) { snprintf(note, note_len, "  Cylinders must be 1..65535"); return -1; }
    if (mf[1] < 1 || mf[1] > 16)    { snprintf(note, note_len, "  Heads must be 1..16"); return -1; }
    if (mf[2] < 1 || mf[2] > 63)    { snprintf(note, note_len, "  Sectors per track must be 1..63"); return -1; }
    detect_cyls = (uint16_t)mf[0]; detect_heads = (uint8_t)mf[1]; detect_spt = (uint8_t)mf[2];
    *c = detect_cyls; *h = detect_heads; *s = detect_spt;
    return 1;
}

// Geometry selection for the drive in drive.id[] (force_mode: no IDENTIFY
// data, MANUAL only).  Returns true once a geometry has been applied.
static bool run_geometry_picker(const uint16_t *id, bool force_mode) {
    bool ls = !force_mode && (id[49] & 0x0200);
    int geo_idx = force_mode ? 3 : (ls ? 2 : 0);
    bool sd = true;
    char note[64] = "";

    while (true) {
        if (sd) { draw_selection_menu_ex(id, geo_idx, force_mode, note); sd = false; }
        int ch = get_input();
        if (api_took_over()) return false;
        if (ch == -1) { tight_loop_contents(); continue; }
        if (ch == KEY_CTRL_L) { redraw_main_underlay(); sd = true; continue; }

        if (!force_mode && ch == KEY_UP && geo_idx > 0)   { geo_idx--; if (!ls && geo_idx == 2) geo_idx--; sd = true; }
        else if (!force_mode && ch == KEY_DOWN && geo_idx < 3) { geo_idx++; if (!ls && geo_idx == 2) geo_idx++; sd = true; }
        else if (ch == KEY_ESC) return false;
        else if (ch == '\t' || (ch == KEY_ENTER && geo_idx == 3)) {
            geo_idx = 3;
            note[0] = '\0';
            draw_selection_menu_ex(id, geo_idx, force_mode, note);
            uint16_t c; uint8_t h, sp;
            int m = manual_chs_entry(id, force_mode, &c, &h, &sp, note, sizeof(note));
            if (api_took_over()) return false;
            if (m == 1) {
                int r = drive_apply_geometry(GEO_MANUAL, c, h, sp);
                if (r == R_OK) return true;
                snprintf(note, sizeof(note), "  Geometry rejected (error %d)", r);
            }
            sd = true;
        } else if (ch == KEY_ENTER) {
            uint8_t mode = geo_idx == 0 ? GEO_NORMAL : geo_idx == 1 ? GEO_LARGE : GEO_LBA;
            int r = drive_apply_geometry(mode, 0, 0, 0);
            if (r == R_OK) return true;
            snprintf(note, sizeof(note), r == R_UNSUPPORTED ? "  Not supported by this drive"
                                                            : "  Not usable here (needs 1..16 heads); try MANUAL");
            sd = true;
        }
    }
}

// ---------------------------------------------------------------------------
//  Debug mode — 72 wide, 20 tall overlay
// ---------------------------------------------------------------------------

static void debug_print(int line_idx, const char *color, const char *fmt, ...) {
    if (line_idx > 16) return;
    char buf[128];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    cdc_printf("\033[%d;%dH\033[40m%s%-*s", 4 + line_idx, 7, color, 68, buf);
    cdc_puts(RESET);
}

static void debug_cls(void) { for (int i = 0; i < 17; i++) debug_print(i, FG_WHITE, ""); }

static void draw_debug_overlay(void) {
    int box_w = 72, box_h = 20, sc = 5, sr = 3;
    int ix = sc+2, iy = sr+1, iw = box_w-4, ih = box_h-3;

    cdc_puts(RESET);
    for (int i = 0; i < box_h; i++) cdc_printf("\033[%d;%dH\033[40m%*s", sr+1+i, sc+2, box_w, "");
    for (int i = 0; i < box_h; i++) cdc_printf("\033[%d;%dH\033[44m%*s", sr+i, sc, box_w, "");
    for (int i = 0; i < ih; i++)    cdc_printf("\033[%d;%dH\033[40m%*s", iy+i, ix, iw, "");

    cdc_puts(FG_WHITE BG_BLUE);
    // Top: 1+70+1=72
    cdc_printf("\033[%d;%dH", sr, sc);
    cdc_puts(BOX_TL); emit_n(BOX_HH, 70); cdc_puts(BOX_TR);
    for (int i = 1; i < box_h-1; i++) {
        cdc_printf("\033[%d;%dH" BOX_VH, sr+i, sc);
        cdc_printf("\033[%d;%dH" BOX_VH, sr+i, sc+box_w-1);
    }
    // Bottom
    cdc_printf("\033[%d;%dH", sr+box_h-1, sc);
    cdc_puts(BOX_BL); emit_n(BOX_HH, 70); cdc_puts(BOX_BR);

    draw_at(sc+29, sr, "[ Debug Mode ]");
    cdc_printf("\033[%d;%dH" FG_WHITE BG_BLUE " ESC: Return  I: IDENT  T: Task File  E: Errors  S: Seek  R: Reset", sr+box_h-2, sc+3);
    cdc_puts(RESET);
    cdc_flush();
}

static void run_debug_identify(void) {
    debug_cls();
    debug_print(0, FG_YELLOW, "Sending IDENTIFY DEVICE (0xEC)...");

    uint16_t id[256];
    if (!ide_identify(id)) {
        debug_print(1, "\033[91;1m", "ERROR: Drive not ready or no data from IDENTIFY.");
        debug_print(2, FG_WHITE, "Check cabling, Master/Slave jumper, or power.");
        return;
    }

    char model[41], serial[21], fw[9];
    decode_ata_string(id, 27, 20, model);
    decode_ata_string(id, 10, 10, serial);
    decode_ata_string(id, 23, 4, fw);

    debug_print(0, "\033[92;1m", "IDENTIFY SUCCESSFUL");
    debug_print(2, FG_WHITE, "Model:  \033[96m%s", model);
    debug_print(3, FG_WHITE, "Serial: \033[96m%s", serial);
    debug_print(4, FG_WHITE, "FW Rev: \033[96m%s", fw);
    debug_print(5, FG_WHITE, "--------------------------------------------------");
    debug_print(6, FG_YELLOW, "[CHS Geometry]");
    debug_print(7, FG_WHITE, "Cyls: %-5d  Heads: %-3d  Sectors: %-3d", id[1], id[3], id[6]);

    bool lba_supp = (id[49] & 0x0200), lba48_supp = (id[83] & (1<<10));
    uint32_t lba28_cap = id[60] | ((uint32_t)id[61] << 16);
    uint64_t lba48_cap = ((uint64_t)id[103]<<48)|((uint64_t)id[102]<<32)|((uint64_t)id[101]<<16)|((uint64_t)id[100]);

    debug_print(9, FG_YELLOW, "[Capabilities]");
    debug_print(10, FG_WHITE, "LBA Supported:    %s", lba_supp ? "\033[92mYes" : "\033[91mNo");
    debug_print(11, FG_WHITE, "LBA48 Supported:  %s", lba48_supp ? "\033[92mYes" : "\033[91mNo");
    if (lba48_supp) {
        uint32_t gb = (uint32_t)(((uint64_t)lba48_cap*512)/1000000000);
        debug_print(12, FG_WHITE, "Capacity:         %lu Sectors (~%lu GB)", (unsigned long)lba48_cap, (unsigned long)gb);
    } else if (lba_supp) {
        uint32_t mb = (uint32_t)(((uint64_t)lba28_cap*512)/1000000);
        debug_print(12, FG_WHITE, "Capacity:         %lu Sectors (~%lu MB)", (unsigned long)lba28_cap, (unsigned long)mb);
    }
    debug_print(14, FG_YELLOW, "[Advanced]");
    debug_print(15, FG_WHITE, "DMA Support: %04X  PIO Support: %04X", id[49], id[64]);
    debug_print(16, FG_WHITE, "ATA Major Ver: %04X", id[80]);
}

static void run_debug_taskfile(void) {
    debug_cls();
    uint8_t tf[8];
    ide_read_taskfile(tf);
    const char *sc = (tf[7] & 0x81) ? "\033[91;1m" : "\033[92m";
    debug_print(0, FG_RED, "[Task File] " FG_WHITE "ERR:%02X SEC:%02X SN:%02X CL:%02X CH:%02X DH:%02X ST:%s%02X" RESET,
                tf[1], tf[2], tf[3], tf[4], tf[5], tf[6], sc, tf[7]);
}

static void run_debug_errors(void) {
    debug_cls();
    uint8_t tf[8];
    ide_read_taskfile(tf);
    uint8_t err = tf[1];
    char eb[64] = {0};
    if (!err) snprintf(eb, sizeof(eb), "\033[92mNo Errors Reported\033[0m");
    else {
        if (err&0x80) strcat(eb,"BBK "); if (err&0x40) strcat(eb,"UNC ");
        if (err&0x20) strcat(eb,"MC ");  if (err&0x10) strcat(eb,"IDNF ");
        if (err&0x08) strcat(eb,"MCR "); if (err&0x04) strcat(eb,"ABRT ");
        if (err&0x02) strcat(eb,"TK0 "); if (err&0x01) strcat(eb,"AMNF ");
    }
    debug_print(0, FG_RED, "[Error Bits] %s", eb);
}

static void run_seek_test(void) {
    debug_cls();
    debug_print(0, FG_YELLOW, "Seek Test Running...");

    uint16_t id[256];
    if (!ide_identify(id)) { debug_print(1, FG_RED, "ERROR: Drive not ready."); sleep_ms(1500); return; }

    bool lba = (id[49] & 0x0200);
    uint32_t max_range = lba ? (id[60]|((uint32_t)id[61]<<16)) : id[1];
    if (!max_range) max_range = 1024;

    double x = 0;
    while (true) {
        int k = cdc_getchar_timeout_us(0);
        if (k == 27) break;
        if (k == KEY_CTRL_L) {
            draw_bios_frame();
            draw_debug_overlay();
            debug_print(0, FG_YELLOW, "Seek Test Running...  (Esc stops)");
        }
        double pos = (sin(x)*0.45)+0.5;
        uint32_t target = (uint32_t)(pos*(max_range-1));

        uint8_t st = ide_seek_read_one(target, lba);

        char bar[61]; memset(bar, '-', 60); bar[(int)(pos*59)] = '#'; bar[60] = '\0';
        debug_print(14, FG_GREEN, "[%s]", bar);
        debug_print(15, FG_WHITE, "Target %s: %lu (ST:%02X)", lba?"LBA":"CYL", (unsigned long)target, st);
        x += 0.12;
    }
    debug_print(14, FG_WHITE, "%*s", 60, ""); debug_print(15, FG_WHITE, "%*s", 60, "");
    sleep_ms(50);
}

// ---------------------------------------------------------------------------
//  Sync local state to/from config_t
// ---------------------------------------------------------------------------

static void sync_from_config(void) {
    cur_cyls = config.cyls; cur_heads = config.heads; cur_spt = config.spt;
    use_lba_mode = config.use_lba_mode; total_lba_sectors = config.lba_sectors;
    ide_select_device(config.dev_base);
}

static void sync_to_config(void) {
    config.cyls = cur_cyls; config.heads = cur_heads; config.spt = cur_spt;
    config.use_lba_mode = use_lba_mode; config.lba_sectors = total_lba_sectors;
}

// ---------------------------------------------------------------------------
//  Follow changes made through the USB API (detect, geometry, mount, config)
// ---------------------------------------------------------------------------

static void sync_from_api(void) {
    if (drive.api_gen == seen_api_gen) return;
    seen_api_gen = drive.api_gen;
    sync_from_config();
    if (drive.identified || drive.forced) {
        strncpy(hdd_model_raw, drive.model, sizeof(hdd_model_raw) - 1);
        hdd_model_raw[sizeof(hdd_model_raw) - 1] = '\0';
    } else {
        hdd_model_raw[0] = '\0';
    }
    show_detect_result = false;
    error_is_notice = false;
    error_title = "ERROR!";
    hdd_status_text[0] = '\0';
    if (is_mounted) current_screen = SCREEN_MOUNTED;
    else if (current_screen == SCREEN_MOUNTED || current_screen == SCREEN_CONFIRM) current_screen = SCREEN_MAIN;
    needs_full_redraw = true;
}

// ---------------------------------------------------------------------------
//  Auto-mount — runs on core 1 so IDE ops never block USB on core 0
// ---------------------------------------------------------------------------

static void show_notice(const char *msg) {
    error_title = "NOTICE";
    snprintf(hdd_status_text, sizeof(hdd_status_text), "\033[91;1m%s", msg);
    show_detect_result = true;
    error_is_notice = true;
    force_detect = false;
}

static void try_auto_mount(void) {
    if (!config.auto_mount) return;
    if (!drive_geometry_valid()) return;

    // Wait for drive to spin up
    sleep_ms(5000);

    ide_select_device(config.dev_base);
    // 10 s, not the full 31 s: the IDE lock is held throughout, and an API
    // request arriving meanwhile would stall USB until it is released.
    ide_reset_drive_wait(10000);

    // Poll drive ready
    uint32_t start = to_ms_since_boot(get_absolute_time());
    bool ready = false;
    while (to_ms_since_boot(get_absolute_time()) - start < 5000) {
        uint8_t st = ide_read_reg(7);
        if (!(st & 0x80) && (st & 0x40)) { ready = true; break; }
        sleep_ms(10);
    }
    if (!ready) return;

    uint16_t id_buf[256];
    if (ide_identify(id_buf)) {
        drive_note_identify(id_buf);
    } else if (config.geo_mode == GEO_MANUAL && !config.geo_model[0] && !config.geo_serial[0]) {
        drive_note_forced();      // geometry was forced for a drive that doesn't answer IDENTIFY
        drive.present = true;
    } else {
        return;
    }
    strncpy(hdd_model_raw, drive.model, sizeof(hdd_model_raw) - 1);
    hdd_model_raw[sizeof(hdd_model_raw) - 1] = '\0';

    if (!config.use_lba_mode)
        ide_set_geometry(config.heads, config.spt);

    int r = drive_mount_ex(0);    // refuses geometry saved for a different drive
    if (r == R_GEO_MISMATCH) show_notice("Saved geometry is for another drive: not mounted");
    else if (r == R_NOT_READY) show_notice("Drive not ready: not mounted");
}

// ---------------------------------------------------------------------------
//  Park heads (power-off preparation for drives without automatic parking)
// ---------------------------------------------------------------------------

static void draw_park_box(const char *digits) {
    draw_confirm_box("Park heads: landing-zone cylinder?");
    cdc_printf(SEL_RED "\033[11;16H Optional, for stepper drives.  Enter = none ");
    cdc_printf("\033[13;16H Esc = cancel   Cylinder: \033[103;30m%-5s\033[0m" SEL_RED, digits);
    cdc_printf("\033[13;%dH", 42 + (int)strlen(digits));
}

static void run_park_dialog(void) {
    char ib[8] = {0};
    int p = 0;
    draw_park_box(ib);
    while (true) {
        int k = get_input();
        if (api_took_over()) return;
        if (k == -1) { tight_loop_contents(); continue; }
        if (k == KEY_ESC) { cdc_puts(RESET); return; }
        if (k == KEY_CTRL_L) { redraw_main_underlay(); draw_park_box(ib); continue; }
        if (k >= '0' && k <= '9' && p < 5) { ib[p++] = (char)k; ib[p] = '\0'; draw_park_box(ib); }
        else if ((k == 8 || k == 127) && p > 0) { ib[--p] = '\0'; draw_park_box(ib); }
        else if (k == KEY_ENTER) {
            long cyl = p ? atol(ib) : -1;
            if (cyl > 65535) { p = 0; ib[0] = '\0'; draw_park_box(ib); continue; }
            draw_confirm_box(cyl >= 0 ? "Seeking to landing zone, then STANDBY..." : "Sending STANDBY IMMEDIATE...");
            cdc_puts(RESET);
            drive_park_t r;
            int res = drive_park(cyl >= 0, cyl >= 0 ? (uint16_t)cyl : 0, &r);
            sync_from_api();
            char msg[64];
            if (res == R_MOUNTED)
                snprintf(msg, sizeof(msg), "Unmount first");
            else if (res == R_OK)
                snprintf(msg, sizeof(msg), "%sParked (%02Xh OK). Power off now.", r.seek_issued ? "Seek OK. " : "",
                         r.standby.command);
            else if (r.seek_issued && r.seek.result != IDE_OK)
                snprintf(msg, sizeof(msg), "SEEK failed (ST %02X ER %02X); STANDBY %s", r.seek.status, r.seek.error,
                         r.standby.result == IDE_OK ? "OK" : "failed");
            else
                snprintf(msg, sizeof(msg), "STANDBY %02Xh failed (ST %02X ER %02X)", r.standby.command,
                         r.standby.status, r.standby.error);
            show_notice(msg);
            error_title = res == R_OK ? "PARKED" : "PARK FAILED";
            return;
        }
    }
}

// ---------------------------------------------------------------------------
//  Core 1 entry point
// ---------------------------------------------------------------------------

void core1_entry(void) {
    multicore_lockout_victim_init();   // core 0 may save config to flash
    sync_from_config();
    if (config_load_status == CONFIG_LOAD_CRC_ERROR || config_load_status == CONFIG_LOAD_INVALID) {
        // Defaults are already in effect (no geometry, nothing to mount): just say so.
        strcpy(hdd_status_text, "\033[91;1mSaved settings corrupt - defaults loaded");
        show_detect_result = true;
        error_is_notice = true;
        force_detect = false;
    }
    try_auto_mount();
    bool trigger_overlay = false;

    // If auto-mount succeeded, start on the mounted screen
    if (is_mounted) {
        current_screen = SCREEN_MOUNTED;
        trigger_overlay = true;
        needs_full_redraw = true;
    }

    while (true) {
        sync_from_api();
        bool connected = cdc_connected;
        if (connected && !last_cdc_connected) { sleep_ms(200); needs_full_redraw = true; }
        last_cdc_connected = connected;
        if (!connected) { sleep_ms(100); continue; }

        if (needs_full_redraw) {
            draw_bios_frame();
            if (current_screen == SCREEN_MAIN || current_screen == SCREEN_CONFIRM || current_screen == SCREEN_MOUNTED)
                update_main_menu();
            else if (current_screen == SCREEN_FEATURES) update_features_menu();
            else if (current_screen == SCREEN_DEBUG) draw_debug_overlay();

            if (show_detect_result || current_screen == SCREEN_CONFIRM ||
                current_screen == SCREEN_MOUNTED || current_screen == SCREEN_DEBUG)
                trigger_overlay = true;
            needs_full_redraw = false;
        }

        if (!show_detect_result && current_screen != SCREEN_CONFIRM &&
            current_screen != SCREEN_MOUNTED && current_screen != SCREEN_DEBUG) {
            if (current_screen == SCREEN_MAIN) update_main_menu();
            else if (current_screen == SCREEN_FEATURES) update_features_menu();
        }

        if (show_detect_result) {
            if (trigger_overlay) { draw_error_box(hdd_status_text, force_detect); trigger_overlay = false; }
        } else if (current_screen == SCREEN_CONFIRM) {
            if (trigger_overlay) {
                if (confirm_type == 0)      draw_confirm_box("Load Defaults and Save to EEPROM (Y/N)?");
                else if (confirm_type == 1) draw_confirm_box("Save Current Setup to EEPROM (Y/N)?");
                else if (confirm_type == 3) draw_confirm_box("Are you sure you want to mount the drive (Y/N)?");
                else if (confirm_type == 4) draw_confirm_box("Are you sure you want to unmount (Y/N)?");
                else if (confirm_type == 5) draw_confirm_box("Geometry is from another drive! Mount (Y/N)?");
                trigger_overlay = false;
            }
        } else if (current_screen == SCREEN_MOUNTED) {
            if (trigger_overlay) {
                draw_confirm_box("Drive mounted!  Press 'U' to Unmount");
                trigger_overlay = false;
            }
        } else if (current_screen == SCREEN_DEBUG) {
            if (trigger_overlay) { draw_debug_overlay(); trigger_overlay = false; }
        }

        cdc_puts("\033[24;79H"); cdc_flush();
        int k = get_input();
        if (k == -1) { tight_loop_contents(); continue; }
        if (drive.api_gen != seen_api_gen) { sync_from_api(); continue; }   // state changed under us
        if (k == KEY_CTRL_L) { needs_full_redraw = true; continue; }        // every screen and overlay

        if (show_detect_result) {
            if (k == KEY_ENTER || k == KEY_ESC) {
                show_detect_result = false; hdd_status_text[0] = '\0';
                if (!error_is_notice) {
                    // A failed detect leaves no usable drive: forget the geometry too.
                    hdd_model_raw[0] = '\0';
                    drive_unmount();                     // flags the media change if it was mounted
                    drive_clear_geometry();
                    sync_from_config();
                }
                error_is_notice = false;
                error_title = "ERROR!";
                needs_full_redraw = true;
            } else if (force_detect && (k == 'f' || k == 'F')) {
                show_detect_result = false; hdd_status_text[0] = '\0';
                force_detect = false;
                strcpy(hdd_model_raw, "Manually Forced Drive");
                drive_note_forced();
                // Manual entry only, no IDENTIFY data
                if (run_geometry_picker(drive.id, true)) sync_from_config();
                needs_full_redraw = true;
            }
            continue;
        }

        if (current_screen == SCREEN_MOUNTED) {
            if (k == 'u' || k == 'U') { current_screen = SCREEN_CONFIRM; confirm_type = 4; trigger_overlay = true; needs_full_redraw = true; }
            continue;
        }

        if (current_screen == SCREEN_DEBUG) {
            if (k == KEY_ESC) { current_screen = SCREEN_FEATURES; needs_full_redraw = true; }
            else if (k == 'i' || k == 'I') run_debug_identify();
            else if (k == 't' || k == 'T') run_debug_taskfile();
            else if (k == 'e' || k == 'E') run_debug_errors();
            else if (k == 's' || k == 'S') { run_seek_test(); current_screen = SCREEN_DEBUG; needs_full_redraw = true; }
            else if (k == 'r' || k == 'R') {
                debug_cls();
                debug_print(0, FG_YELLOW, "Resetting drive...");
                ide_reset_drive();
                bool rdy = ide_wait_until_ready(5000);
                debug_print(1, rdy ? FG_GREEN : FG_RED, rdy ? "Drive ready." : "Drive not responding.");
            }
            continue;
        }

        if (current_screen == SCREEN_CONFIRM) {
            if (k == 'y' || k == 'Y') {
                if (confirm_type == 0) { config_defaults(); sync_from_config(); config_save(); current_screen = SCREEN_MAIN; }
                else if (confirm_type == 1) { sync_to_config(); config_save(); current_screen = confirm_return_screen; }
                else if (confirm_type == 3 || confirm_type == 5) {
                    int r = drive_mount_ex(confirm_type == 5 ? MOUNT_FORCE : 0);
                    sync_from_api();             // the mount may have identified the drive
                    if (r == R_OK) current_screen = SCREEN_MOUNTED;
                    else if (r == R_GEO_MISMATCH) { current_screen = SCREEN_CONFIRM; confirm_type = 5; trigger_overlay = true; }
                    else {
                        current_screen = SCREEN_MAIN;
                        show_notice(r == R_NO_DRIVE ? "No drive answers: not mounted" :
                                    r == R_NOT_READY ? "Drive not ready - check power" : "Mount failed");
                        trigger_overlay = true;
                    }
                }
                else if (confirm_type == 4) { drive_unmount(); current_screen = SCREEN_MAIN; }
                needs_full_redraw = true;
            } else if (k == 'n' || k == 'N' || k == KEY_ESC) {
                current_screen = (confirm_type == 1) ? confirm_return_screen :
                                 (confirm_type == 4) ? SCREEN_MOUNTED : SCREEN_MAIN;
                needs_full_redraw = true;
            }
            continue;
        }

        if (k == KEY_F10) { confirm_return_screen = current_screen; current_screen = SCREEN_CONFIRM; confirm_type = 1; trigger_overlay = true; needs_full_redraw = true; continue; }

        if (current_screen == SCREEN_MAIN) {
            // Left column 0, 1, 5; right column 2, 3, 4 (rows 6, 8, 10)
            static const uint8_t up[6]    = {0, 0, 2, 2, 3, 1};
            static const uint8_t down[6]  = {1, 5, 3, 4, 4, 5};
            static const uint8_t right[6] = {2, 3, 2, 3, 4, 4};
            static const uint8_t left[6]  = {0, 1, 0, 1, 5, 5};
            if (config.main_selected > 5) config.main_selected = 0;
            if (k == KEY_UP)         config.main_selected = up[config.main_selected];
            else if (k == KEY_DOWN)  config.main_selected = down[config.main_selected];
            else if (k == KEY_RIGHT) config.main_selected = right[config.main_selected];
            else if (k == KEY_LEFT)  config.main_selected = left[config.main_selected];
            else if (k == KEY_ENTER) {
                if (config.main_selected == 0) {
                    // Auto Detect — single reset, probe master then slave
                    uint16_t id_buf[256];
                    bool detected = false;
                    uint8_t found = ide_probe_devices(0);
                    drive.present = (found != 0);
                    if (found) {
                        ide_select_device(found);
                        config.dev_base = found;
                        if (ide_identify(id_buf)) detected = true;
                    }
                    if (detected) drive_note_identify(id_buf); else drive_clear_identity();
                    if (!detected) ide_select_device(config.dev_base);
                    if (detected) {
                        strncpy(hdd_model_raw, drive.model, sizeof(hdd_model_raw) - 1);
                        hdd_model_raw[sizeof(hdd_model_raw) - 1] = '\0';
                        if (!run_geometry_picker(id_buf, false) && !config.use_lba_mode && config.heads && config.spt)
                            ide_set_geometry(config.heads, config.spt);   // kept: the probe's reset cleared INIT PARAMS
                        sync_from_config();
                    } else {
                        if (found) {
                            strcpy(hdd_status_text, "\033[91;1mIDENTIFY failed");
                            force_detect = true;
                        } else {
                            strcpy(hdd_status_text, "\033[91;1mNo drive detected");
                            force_detect = false;
                        }
                        show_detect_result = true; trigger_overlay = true; hdd_model_raw[0] = '\0';
                    }
                    needs_full_redraw = true;
                } else if (config.main_selected == 1) {
                    bool dv = (hdd_model_raw[0] != '\0');
                    bool gv = (use_lba_mode && total_lba_sectors > 0) || (!use_lba_mode && cur_cyls > 0 && cur_heads > 0 && cur_spt > 0);
                    if (!dv || !gv) {
                        snprintf(hdd_status_text, sizeof(hdd_status_text), "\033[91;1mDetect drive and set geometry first!");
                        hdd_model_raw[0] = '\0'; cur_cyls = 0; cur_heads = 0; cur_spt = 0;
                        total_lba_sectors = 0; use_lba_mode = false; is_mounted = false;
                        show_detect_result = true; trigger_overlay = true; needs_full_redraw = true;
                    } else { current_screen = SCREEN_CONFIRM; confirm_type = 3; trigger_overlay = true; needs_full_redraw = true; }
                } else if (config.main_selected == 2) {
                    current_screen = SCREEN_FEATURES; config.feat_selected = 0; needs_full_redraw = true;
                } else if (config.main_selected == 3) { current_screen = SCREEN_CONFIRM; confirm_type = 0; trigger_overlay = true; needs_full_redraw = true; }
                else if (config.main_selected == 4) { current_screen = SCREEN_CONFIRM; confirm_type = 1; trigger_overlay = true; needs_full_redraw = true; }
                else if (config.main_selected == 5) { run_park_dialog(); needs_full_redraw = true; }
            }
        } else if (current_screen == SCREEN_FEATURES) {
            if (k == KEY_UP && config.feat_selected > 0) config.feat_selected--;
            else if (k == KEY_DOWN && config.feat_selected < FEAT_ITEMS - 1) config.feat_selected++;
            else if (k == KEY_ESC) current_screen = SCREEN_MAIN;
            else if (k == KEY_ENTER) {
                if (config.feat_selected == 0) config.drive_write_protected = !config.drive_write_protected;
                else if (config.feat_selected == 1) config.auto_mount = !config.auto_mount;
                else if (config.feat_selected == 2) { config.iordy_enabled = !config.iordy_enabled; ide_set_iordy(config.iordy_enabled); }
                else if (config.feat_selected == 3) config.intrq_enabled = !config.intrq_enabled;
                else if (config.feat_selected == 4) cycle_read_timeout();
                else if (config.feat_selected == 5) config.no_retry = !config.no_retry;
                else if (config.feat_selected == FEAT_DEBUG) current_screen = SCREEN_DEBUG;
            }
            needs_full_redraw = true;
        }
    }
}
