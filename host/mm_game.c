/* Mega Man (NES) for handhelds: the game layer.
 *
 * Celeste-style play on top of the original game, all from outside the NES
 * (RAM reads/writes and whole-machine snapshots, no ROM changes):
 *
 *  - Checkpoints: a snapshot is taken when Mega Man lands at the start of a
 *    stage, at the end of every screen transition going up or sideways (the
 *    freeze + camera move between areas; not when falling down into the next
 *    area, he could be falling onto spikes), and in the corridor behind the
 *    first of a boss's two doors. A transition's snapshot only becomes the checkpoint
 *    once he has stayed alive CP_PROBATION frames after it (or reached the
 *    next transition): a room entered falling onto spikes doesn't trap him.
 *    Three deaths in a row, each soon after respawning, also move the
 *    checkpoint back to the previous one. After the death animation he is back at the last
 *    one, with full health, instead of the game restarting the stage.
 *  - Deaths instead of lives: lives never run out; the HUD counts deaths.
 *    The goal is to finish with as few as possible.
 *  - L / R switch weapons (among the ones you have) without opening the
 *    pause menu: the game's own menu does the switch, hidden and fast, so the
 *    weapon graphics and colours are loaded the way the game does it.
 *  - Y fires repeatedly while held.
 *  - Three files: START on the title screen opens SELECT FILE. A file keeps
 *    a snapshot of the stage select screen (written each time the game gets
 *    there) or, in Dr. Wily's castle, of the start of the current Wily stage,
 *    plus its own death count (written at each death, so quitting doesn't
 *    undo deaths). Files: <save dir>/file<N>.state and file<N>.cfg.
 *  - MENU opens the options screen: PALETTE (the NES colours, seen live),
 *    ASPECT (PIXEL PERFECT 512x480, NTSC 8:7 585x480, FULL 4:3 640x480),
 *    FILTER (SMOOTH = sharp-bilinear, CRISP = nearest),
 *    REFILL WEAPONS on respawn, RESUME, MAIN MENU (back to the title screen
 *    to pick another file), QUIT GAME.
 *
 * RAM (Mega Man USA; Data Crystal's RAM map, checked against the ROM and runs):
 *   $1C camera state ($40 scrolling to a new area, $02 freeze, $01 nametable work;
 *       a boss door is $02 twice, opening then closing, with no $40)
 *   $31 stage  $3C frame counter ($C0 down while READY / the death sequence)
 *   $42 $05 while playing, $06 in the pause menu
 *   $5D weapons owned (bits, see k_menu_bit)  $5F weapon (game id; menu index in the menu)
 *   $6A health ($1C full)  $6B-$71 weapon energy  $A6 lives
 *   $0480 / $0600 Mega Man X / Y (object 0)
 * Death: every kind (health, pit, spikes) ends in the routine at $C219, which
 * sets $0600 = $F8 and $3C = $C0; a health death first hits $6A = 0.
 * Stage start: $0600 = $04 and $3C = $C0 (READY, beaming down).
 */
#include "mm_game.h"
#include "mm_host.h"
#include "mm_ui.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define R_CAMERA   0x1C
#define R_STAGE    0x31
#define R_COUNTER  0x3C
#define R_MODE     0x42
#define R_OWNED    0x5D
#define R_WEAPON   0x5F
#define R_HP       0x6A
#define R_ENERGY   0x6B          /* 7 bytes */
#define R_LIVES    0xA6
#define R_Y        0x600

#define HP_FULL    0x1C
#define MODE_PLAY  0x05

/* Pause menu order <-> game weapon ids (ROM $B50D / $B51F / $B515, bank 6). */
static const uint8_t k_game_to_menu[8] = { 0, 5, 4, 1, 6, 2, 3, 7 };
static const uint8_t k_menu_bit[8]     = { 0xFF, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80 };
/* HUD letter per game weapon id: Mega Buster, Hyper Bomb, Thunder Beam,
 * Super Arm, Ice Slasher, Rolling Cutter, Fire Storm, Magnet Beam. */
static const char k_weapon_letter[8] = { 'P', 'B', 'E', 'G', 'I', 'C', 'F', 'M' };

/* ---- state ---------------------------------------------------------------- */
static uint8_t* s_cp;              /* checkpoint snapshot */
static size_t   s_cp_size;
static bool     s_cp_valid;
static uint8_t* s_cp_prev;         /* the one before it (loop guard) */
static bool     s_cp_prev_valid;
static uint8_t* s_cand;            /* a transition's snapshot on probation */
static int      s_cand_timer;      /* frames left before it becomes the checkpoint; 0 = none */
static int      s_since_respawn = 1 << 20;   /* frames alive since the last respawn */
static int      s_quick_deaths;    /* deaths in a row soon after respawning */
#define CP_PROBATION   120         /* 2 s alive after a transition */
#define QUICK_DEATH    150         /* a death this soon after a respawn counts as quick */

static int      s_stage = -1;
static bool     s_in_stage;        /* stage started (READY seen) and not left */
static bool     s_start_pending;   /* waiting for Mega Man to land after READY */
static int      s_land_frames;
static uint8_t  s_last_y;
static bool     s_scrolling;       /* a transition is under way */
static int      s_settle;
static uint8_t  s_prev_hp;
static uint8_t  s_prev_camera;
static bool     s_cam_scrolled;    /* this $02 freeze had a $40 scroll: a transition, not a door */
static int      s_door_halves;     /* $02 freezes without a scroll (door opens, door closes) */
static int      s_doors, s_cp_doors, s_cp_prev_doors, s_cand_doors;   /* doors passed this stage */
static bool     s_door_settle;

static bool     s_dying;           /* counted, waiting for respawn or the game's own */
static bool     s_respawn_wait;    /* load the checkpoint when the death sequence ends */
static bool     s_death_seq;       /* the game's death sequence ($C219) has begun */
static uint8_t  s_energy_at_death[7];
static uint32_t s_deaths;

enum { WS_IDLE, WS_OPEN, WS_NAV, WS_CLOSE };
static int      s_ws = WS_IDLE;
static int      s_ws_frames, s_ws_target, s_ws_presses;
static uint32_t s_ws_dir;
static int      s_ws_step;         /* NAV: frames into the current press (held, then let go) */
#define WS_HOLD 3                  /* a button is held this many frames, then let go as long: */
#define WS_GAP  5                  /* the menu doesn't read the pad on every frame */

static uint32_t s_held, s_pressed, s_out;
static uint32_t s_swallow;         /* buttons that closed one of our screens: not passed to the game until let go */
static bool     s_hold_picture;
static uint32_t s_frozen[256 * 240];

static bool     s_menu_open;
static int      s_menu_sel;
static bool     s_refill;          /* option: refill weapon energy on respawn */
static int      s_palette = 1;     /* option: index in k_palettes */
enum { ASPECT_PIXEL, ASPECT_NTSC, ASPECT_FULL, ASPECTS };
static int      s_aspect = ASPECT_PIXEL;   /* option */
static bool     s_smooth = true;   /* option: FILTER SMOOTH (sharp-bilinear) / CRISP (nearest) */
static const char* k_aspect_names[ASPECTS] = { "PIXEL PERFECT", "NTSC 8:7", "FULL 4:3" };

/* NES palettes of the fceumm core (the NES makes a video signal, not RGB:
 * each palette is one way of turning it into colours). */
static const struct { const char* name; const char* core; } k_palettes[] = {
    { "DEFAULT",     "default" },
    { "SMOOTH",      "smooth-v2-fbx" },
    { "NES CLASSIC", "nes-classic-fbx-fs" },
    { "COMPOSITE",   "composite-direct-fbx" },
    { "PVM",         "pvm-style-d93-fbx" },
    { "WII VC",      "restored-wii-vc" },
    { "WAVEBEAM",    "wavebeam" },
};
#define PALETTES ((int)(sizeof(k_palettes) / sizeof(k_palettes[0])))
static bool     s_cfg_dirty;

/* files */
#define FILES 3
#define STAGE_WILY1 6              /* Wily stages are $06-$09 */
#define STAGE_TITLE 0x0A
#define STAGE_ENDING 0x0B
typedef struct { bool used, cleared, in_stage; uint32_t deaths; int bosses, wily; } FileInfo;
static FileInfo s_files[FILES];
static int      s_file = -1;       /* the file being played */
static bool     s_title = true;    /* no file picked yet */
static bool     s_files_open, s_erase_confirm;
static int      s_files_sel;
static int      s_inject_start;    /* frames left holding START (new game) */
static bool     s_select_saved;    /* this visit to the stage select is in the file */

/* ---- settings ---------------------------------------------------------------- */
static void cfg_path(char* buf, size_t n) { snprintf(buf, n, "%s/settings.cfg", host_save_dir()); }

static void cfg_load(void) {
    char path[600], line[128];
    cfg_path(path, sizeof(path));
    FILE* f = fopen(path, "r");
    if (!f) return;
    int v;
    while (fgets(line, sizeof(line), f))
    {
        if (sscanf(line, "refill=%d", &v) == 1) s_refill = v != 0;
        if (sscanf(line, "palette=%d", &v) == 1 && v >= 0 && v < PALETTES) s_palette = v;
        if (sscanf(line, "aspect=%d", &v) == 1 && v >= 0 && v < ASPECTS) s_aspect = v;
        if (sscanf(line, "smooth=%d", &v) == 1) s_smooth = v != 0;
    }
    fclose(f);
}

static void cfg_save(void) {
    char path[600], tmp[610];
    cfg_path(path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE* f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "refill=%d\npalette=%d\naspect=%d\nsmooth=%d\n", s_refill ? 1 : 0, s_palette, s_aspect, s_smooth ? 1 : 0);
    fclose(f);
    rename(tmp, path);
}

/* ---- helpers ------------------------------------------------------------------- */
static void freeze_picture(bool on) {
    if (on && !s_hold_picture) memcpy(s_frozen, host_nes_frame(), sizeof(s_frozen));
    s_hold_picture = on;
}

static void swap_buf(uint8_t** a, uint8_t** b) { uint8_t* t = *a; *a = *b; *b = t; }

/* The probation snapshot becomes the checkpoint; the old one is kept as previous. */
static void commit_candidate(void) {
    if (!s_cand_timer) return;
    s_cand_timer = 0;
    swap_buf(&s_cp_prev, &s_cp);
    s_cp_prev_valid = s_cp_valid;
    s_cp_prev_doors = s_cp_doors;
    swap_buf(&s_cp, &s_cand);
    s_cp_doors = s_cand_doors;
    s_cp_valid = true;
    s_quick_deaths = 0;
    fprintf(stderr, "[MM] checkpoint confirmed f%u\n", host_frame());
}

static void take_checkpoint(const char* why) {
    if (!s_cp || !s_cand || !s_cp_prev) return;
    bool probation = !strcmp(why, "transition");
    if (probation) {
        commit_candidate();                    /* he got from the last one to here alive */
        if (!host_state_save(s_cand, s_cp_size)) return;
        s_cand_doors = s_doors;
        s_cand_timer = CP_PROBATION;
    } else {
        if (!host_state_save(s_cp, s_cp_size)) return;
        s_cp_doors = s_doors;
        s_cp_valid = true;
        s_cp_prev_valid = false;
        s_cand_timer = 0;
        s_quick_deaths = 0;
    }
    fprintf(stderr, "[MM] checkpoint (%s%s) stage %d x=%02X y=%02X f%u\n", why, probation ? ", on probation" : "",
            s_stage, host_ram()[0x480], host_ram()[R_Y], host_frame());
}

static void respawn(void) {
    uint8_t* r = host_ram();
    if (!s_cp_valid || !host_state_load(s_cp, s_cp_size)) return;
    r = host_ram();
    r[R_HP] = HP_FULL;
    if (s_refill) memset(&r[R_ENERGY], HP_FULL, 7);
    else memcpy(&r[R_ENERGY], s_energy_at_death, 7);
    s_dying = false;
    s_scrolling = false;
    s_settle = 0;
    s_prev_hp = HP_FULL;
    s_prev_camera = r[R_CAMERA];
    s_doors = s_cp_doors;
    s_door_halves = 0;
    s_door_settle = false;
    s_since_respawn = 0;
    fprintf(stderr, "[MM] respawn, deaths %u\n", s_deaths);
}

static bool owned(const uint8_t* r, int menu) { return menu == 0 || (r[R_OWNED] & k_menu_bit[menu]); }

static bool can_switch(const uint8_t* r) {
    return s_in_stage && !s_dying && !s_start_pending && !s_scrolling && r[R_HP] > 0 &&
           r[R_CAMERA] == 0 && r[R_MODE] == MODE_PLAY;   /* $3C stays nonzero after the menu */
}

static void start_switch(int dir) {
    uint8_t* r = host_ram();
    int cur = k_game_to_menu[r[R_WEAPON] & 7], t = cur;
    for (int i = 0; i < 8; i++) {
        t = (t + dir + 8) % 8;
        if (owned(r, t)) break;
    }
    if (t == cur) return;
    s_ws = WS_OPEN;
    s_ws_frames = 0;
    s_ws_target = t;
    s_ws_presses = 0;
    s_ws_dir = dir > 0 ? PAD_DOWN : PAD_UP;
    s_ws_step = 0;
    freeze_picture(true);
    host_mute(true);
}

static void end_switch(void) {
    s_ws = WS_IDLE;
    freeze_picture(s_menu_open);
    host_mute(false);
}

/* ---- files ------------------------------------------------------------------------- */
static void file_path(char* buf, size_t n, int i, const char* ext) {
    snprintf(buf, n, "%s/file%d.%s", host_save_dir(), i + 1, ext);
}

static void file_info_load(int i) {
    FileInfo* fi = &s_files[i];
    memset(fi, 0, sizeof(*fi));
    char path[600], line[128];
    file_path(path, sizeof(path), i, "state");
    FILE* f = fopen(path, "rb");
    if (f) { fi->used = true; fclose(f); }
    file_path(path, sizeof(path), i, "cfg");
    if (!(f = fopen(path, "r"))) return;
    unsigned u;
    int v;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "deaths=%u", &u) == 1) fi->deaths = u;
        if (sscanf(line, "bosses=%d", &v) == 1) fi->bosses = v;
        if (sscanf(line, "wily=%d", &v) == 1) fi->wily = v;
        if (sscanf(line, "cleared=%d", &v) == 1) fi->cleared = v != 0;
        if (sscanf(line, "in_stage=%d", &v) == 1) fi->in_stage = v != 0;
    }
    fclose(f);
}

static void file_info_save(int i) {
    const FileInfo* fi = &s_files[i];
    char path[600], tmp[610];
    file_path(path, sizeof(path), i, "cfg");
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE* f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "deaths=%u\nbosses=%d\nwily=%d\ncleared=%d\nin_stage=%d\n",
            fi->deaths, fi->bosses, fi->wily, fi->cleared ? 1 : 0, fi->in_stage ? 1 : 0);
    fclose(f);
    rename(tmp, path);
}

static int bosses_beaten(const uint8_t* r) {
    int n = 0;
    for (int b = 1; b <= 6; b++) n += (r[R_OWNED] >> b) & 1;   /* the six robot masters' weapons */
    return n;
}

/* Writes the machine as it is now into the file being played. */
static void file_save_state(bool in_stage) {
    if (s_file < 0 || !s_cp) return;
    static uint8_t* buf;
    if (!buf) buf = (uint8_t*)malloc(s_cp_size);
    if (!buf || !host_state_save(buf, s_cp_size)) return;
    char path[600], tmp[610];
    file_path(path, sizeof(path), s_file, "state");
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE* f = fopen(tmp, "wb");
    if (!f) return;
    bool ok = fwrite(buf, 1, s_cp_size, f) == s_cp_size;
    ok = fclose(f) == 0 && ok;
    if (!ok || rename(tmp, path) != 0) { fprintf(stderr, "[MM] can't write %s\n", path); return; }
    const uint8_t* r = host_ram();
    FileInfo* fi = &s_files[s_file];
    fi->used = true;
    fi->in_stage = in_stage;
    fi->bosses = bosses_beaten(r);
    fi->wily = in_stage && r[R_STAGE] >= STAGE_WILY1 && r[R_STAGE] < STAGE_TITLE ? r[R_STAGE] - STAGE_WILY1 + 1 : 0;
    fi->deaths = s_deaths;
    file_info_save(s_file);
    fprintf(stderr, "[MM] file %d saved (%s) f%u\n", s_file + 1, in_stage ? "wily stage" : "stage select", host_frame());
}

static void reset_tracking(void) {
    s_stage = -1;
    s_in_stage = false;
    s_cp_valid = false;
    s_start_pending = false;
    s_scrolling = false;
    s_dying = false;
    s_respawn_wait = false;
}

static bool file_load(int i) {
    char path[600];
    file_path(path, sizeof(path), i, "state");
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    uint8_t* buf = (uint8_t*)malloc(s_cp_size);
    bool ok = buf && fread(buf, 1, s_cp_size, f) == s_cp_size;
    fclose(f);
    ok = ok && host_state_load(buf, s_cp_size);
    free(buf);
    if (!ok) { fprintf(stderr, "[MM] can't load %s\n", path); return false; }
    reset_tracking();
    s_deaths = s_files[i].deaths;
    s_select_saved = true;
    const uint8_t* r = host_ram();
    if (s_files[i].in_stage) {                 /* the start of a Wily stage: also the checkpoint */
        s_stage = r[R_STAGE];
        s_in_stage = true;
        s_prev_hp = r[R_HP];
        take_checkpoint("file");
    }
    fprintf(stderr, "[MM] file %d loaded, deaths %u\n", i + 1, s_deaths);
    return true;
}

static void file_erase(int i) {
    char path[600];
    file_path(path, sizeof(path), i, "state");
    remove(path);
    file_path(path, sizeof(path), i, "cfg");
    remove(path);
    memset(&s_files[i], 0, sizeof(s_files[i]));
    fprintf(stderr, "[MM] file %d erased\n", i + 1);
}

static void files_close(void) {
    s_files_open = false;
    s_swallow = s_held & PAD_NES_MASK;   /* START held here would pick a stage */
    s_erase_confirm = false;
    freeze_picture(false);
}

static void files_input(uint32_t p) {
    if (s_erase_confirm) {
        if (p & PAD_A) { file_erase(s_files_sel); s_erase_confirm = false; }
        else if (p & (PAD_B | PAD_SELECT)) s_erase_confirm = false;
        return;
    }
    if (p & PAD_UP) s_files_sel = (s_files_sel + FILES - 1) % FILES;
    if (p & PAD_DOWN) s_files_sel = (s_files_sel + 1) % FILES;
    if ((p & PAD_SELECT) && s_files[s_files_sel].used) { s_erase_confirm = true; return; }
    if (p & (PAD_A | PAD_START)) {
        int i = s_files_sel;
        if (s_files[i].used) {
            if (!file_load(i)) return;
        } else {                               /* new game: let the title screen go on */
            s_deaths = 0;
            s_files[i].deaths = 0;
            s_inject_start = 3;
        }
        s_file = i;
        s_title = false;
        files_close();
        return;
    }
    if (p & PAD_B) files_close();
}

static void files_draw(uint32_t* cv) {
    const int x0 = NES_X + 16, x1 = NES_X + 240, y0 = 40, y1 = 200;
    ui_box(cv, x0, y0, x1, y1);
    const uint32_t white = 0xFFF8F8F8u, sel = 0xFFF8D030u, dim = 0xFFA0A0A0u;
    ui_text(cv, NES_X + 128 - ui_text_w("SELECT FILE", 1) / 2, y0 + 8, "SELECT FILE", white, 1);
    for (int i = 0; i < FILES; i++) {
        const FileInfo* fi = &s_files[i];
        int y = y0 + 28 + i * 34;
        bool here = i == s_files_sel;
        char name[16], detail[48];
        snprintf(name, sizeof(name), "FILE %d", i + 1);
        if (!fi->used) snprintf(detail, sizeof(detail), "NEW GAME");
        else if (fi->cleared) snprintf(detail, sizeof(detail), "CLEARED   DEATHS %u", fi->deaths);
        else if (fi->wily) snprintf(detail, sizeof(detail), "WILY STAGE %d   DEATHS %u", fi->wily, fi->deaths);
        else snprintf(detail, sizeof(detail), "BOSSES %d/6   DEATHS %u", fi->bosses, fi->deaths);
        if (here) ui_text(cv, x0 + 8, y, ">", sel, 1);
        ui_text(cv, x0 + 18, y, name, here ? sel : white, 1);
        ui_text(cv, x0 + 30, y + 11, detail, here ? white : dim, 1);
    }
    const char* foot = s_erase_confirm ? "ERASE THIS FILE?  A YES  B NO" : "A START  SELECT ERASE  B BACK";
    ui_text(cv, NES_X + 128 - ui_text_w(foot, 1) / 2, y1 - 14, foot, s_erase_confirm ? sel : dim, 1);
}

/* ---- options screen --------------------------------------------------------------- */
enum { ITEM_PALETTE, ITEM_ASPECT, ITEM_FILTER, ITEM_REFILL, ITEM_RESUME, ITEM_MAIN, ITEM_QUIT, ITEM_COUNT };
static bool s_main_confirm;        /* MAIN MENU asked: waiting for A (yes) or B (no) */

/* Back to the title screen (console reset), where START opens SELECT FILE.
 * The file keeps what was last saved in it; deaths are already written. */
static void go_main_menu(void) {
    host_reset();
    reset_tracking();
    s_title = true;
    s_file = -1;
    s_deaths = 0;
    s_select_saved = false;
    s_inject_start = 0;
    fprintf(stderr, "[MM] main menu\n");
}

static void set_palette(int i) {
    s_palette = (i + PALETTES) % PALETTES;
    host_set_option("fceumm_palette", k_palettes[s_palette].core);
}

static void menu_close(void) {
    s_menu_open = false;
    s_swallow = s_held & PAD_NES_MASK;
    s_main_confirm = false;
    freeze_picture(false);
    if (s_cfg_dirty) { cfg_save(); s_cfg_dirty = false; }
}

static void menu_input(uint32_t p) {
    if (s_main_confirm) {
        if (p & PAD_A) { menu_close(); go_main_menu(); }
        else if (p & (PAD_B | PAD_MENU)) s_main_confirm = false;
        return;
    }
    if (p & PAD_UP) s_menu_sel = (s_menu_sel + ITEM_COUNT - 1) % ITEM_COUNT;
    if (p & PAD_DOWN) s_menu_sel = (s_menu_sel + 1) % ITEM_COUNT;
    if ((p & (PAD_LEFT | PAD_RIGHT | PAD_A)) && s_menu_sel == ITEM_PALETTE) {
        set_palette(s_palette + ((p & PAD_LEFT) ? -1 : 1));
        host_refresh_picture();                /* the frozen picture in the new colours */
        memcpy(s_frozen, host_nes_frame(), sizeof(s_frozen));
        s_cfg_dirty = true;
        return;
    }
    if ((p & (PAD_LEFT | PAD_RIGHT | PAD_A)) && s_menu_sel == ITEM_ASPECT) {
        s_aspect = (s_aspect + ((p & PAD_LEFT) ? ASPECTS - 1 : 1)) % ASPECTS;
        s_cfg_dirty = true;
        return;
    }
    if ((p & (PAD_LEFT | PAD_RIGHT | PAD_A)) && s_menu_sel == ITEM_FILTER) {
        s_smooth = !s_smooth;
        s_cfg_dirty = true;
        return;
    }
    if ((p & (PAD_LEFT | PAD_RIGHT | PAD_A)) && s_menu_sel == ITEM_REFILL) {
        s_refill = !s_refill;
        s_cfg_dirty = true;
        return;
    }
    if (p & PAD_A) {
        if (s_menu_sel == ITEM_RESUME) menu_close();
        else if (s_menu_sel == ITEM_MAIN) {
            if (s_title) menu_close();         /* already there */
            else s_main_confirm = true;
        }
        else if (s_menu_sel == ITEM_QUIT) { menu_close(); host_request_quit(); }
        return;
    }
    if (p & (PAD_B | PAD_MENU)) menu_close();
}

static void menu_draw(uint32_t* cv) {
    const int x0 = NES_X + 20, x1 = NES_X + 236, y0 = 36, y1 = 204;
    ui_box(cv, x0, y0, x1, y1);
    const uint32_t white = 0xFFF8F8F8u, sel = 0xFFF8D030u, dim = 0xFFA0A0A0u;
    ui_text(cv, NES_X + 128 - ui_text_w("OPTIONS", 1) / 2, y0 + 8, "OPTIONS", white, 1);
    char deaths[32];
    snprintf(deaths, sizeof(deaths), "DEATHS: %u", s_deaths);
    ui_text(cv, NES_X + 128 - ui_text_w(deaths, 1) / 2, y0 + 22, deaths, dim, 1);
    static const char* labels[ITEM_COUNT] = { "PALETTE", "ASPECT", "FILTER", "REFILL WEAPONS", "RESUME", "MAIN MENU", "QUIT GAME" };
    for (int i = 0; i < ITEM_COUNT; i++) {
        int y = y0 + 42 + i * 14;
        uint32_t c = i == s_menu_sel ? sel : white;
        if (i == s_menu_sel) ui_text(cv, x0 + 8, y, ">", sel, 1);
        ui_text(cv, x0 + 18, y, labels[i], c, 1);
        const char* v = i == ITEM_PALETTE ? k_palettes[s_palette].name
                      : i == ITEM_ASPECT ? k_aspect_names[s_aspect]
                      : i == ITEM_FILTER ? (s_smooth ? "SMOOTH" : "CRISP")
                      : i == ITEM_REFILL ? (s_refill ? "ON" : "OFF") : NULL;
        if (v) {                               /* "< VALUE >" in the right column */
            const int vx0 = x0 + 106, vx1 = x1 - 6;
            ui_text(cv, vx0, y, "<", i == s_menu_sel ? sel : dim, 1);
            ui_text(cv, vx1 - 5, y, ">", i == s_menu_sel ? sel : dim, 1);
            ui_text(cv, (vx0 + vx1) / 2 - ui_text_w(v, 1) / 2, y, v, c, 1);
        }
    }
    const char* foot = NULL;
    if (s_main_confirm) foot = "STAGE PROGRESS IS LOST  A YES  B NO";
    else if (s_menu_sel == ITEM_REFILL) foot = "WEAPON ENERGY ON RESPAWN";
    else if (s_menu_sel == ITEM_MAIN) foot = "BACK TO THE TITLE AND FILES";
    else if (s_menu_sel == ITEM_FILTER) foot = s_aspect == ASPECT_PIXEL ? "NO EFFECT WITH PIXEL PERFECT"
                                        : s_smooth ? "EVEN PIXELS AND SOFT EDGES" : "SHARP BUT UNEVEN PIXELS";
    if (foot) ui_text(cv, NES_X + 128 - ui_text_w(foot, 1) / 2, y1 - 14, foot, s_main_confirm ? sel : dim, 1);
}

/* ---- the game layer ------------------------------------------------------------------ */
void game_init(void) {
    s_cp_size = host_state_size();
    s_cp = (uint8_t*)malloc(s_cp_size);
    s_cp_prev = (uint8_t*)malloc(s_cp_size);
    s_cand = (uint8_t*)malloc(s_cp_size);
    cfg_load();
    set_palette(s_palette);
    for (int i = 0; i < FILES; i++) file_info_load(i);
}

void game_tick(uint32_t held, uint32_t pressed) {
    s_held = held;
    s_pressed = pressed;
    s_swallow &= held;
    if (s_menu_open) { menu_input(pressed); return; }
    if (s_files_open) { files_input(pressed); return; }
    if (s_title && (pressed & PAD_START) && host_ram()[R_STAGE] == STAGE_TITLE) {
        s_files_open = true;
        s_erase_confirm = false;
        freeze_picture(true);
        return;
    }
    if ((pressed & PAD_MENU) && s_ws == WS_IDLE) {
        s_menu_open = true;
        s_menu_sel = ITEM_PALETTE;
        freeze_picture(true);
        return;
    }
    if (s_ws == WS_IDLE && (pressed & (PAD_L | PAD_R)) && can_switch(host_ram()))
        start_switch((pressed & PAD_R) ? +1 : -1);
    s_out = held & PAD_NES_MASK & ~s_swallow;
    if (s_title) s_out &= ~PAD_START;          /* START on the title screen is ours */
    if (s_inject_start > 0) { s_inject_start--; s_out |= PAD_START; }
    if (held & PAD_Y) {                        /* rapid fire: B on 4 frames, off 4 */
        if ((host_frame() >> 2) & 1) s_out |= PAD_B;
    }
}

int game_frames_this_tick(void) {
    if (s_menu_open || s_files_open) return 0;
    if (s_ws != WS_IDLE) return 8;
    return 1;
}

uint32_t game_input(void) {
    switch (s_ws) {
    case WS_OPEN:  return s_ws_frames < WS_HOLD ? PAD_START : 0;
    case WS_CLOSE: return s_ws_frames < WS_HOLD ? PAD_START : 0;
    case WS_NAV:   return s_ws_step < WS_HOLD ? s_ws_dir : 0;
    default:       return s_out;
    }
}

static void weapon_switch_step(const uint8_t* r) {
    switch (s_ws) {
    case WS_OPEN:
        /* the menu takes ~16 frames to come up ($1C busy, then $00) */
        if (++s_ws_frames >= 24 && r[R_CAMERA] == 0) { s_ws = WS_NAV; s_ws_step = WS_HOLD + WS_GAP; }
        else if (s_ws_frames > 120) end_switch();
        break;
    case WS_NAV:
        /* after each press (and before the first): is the cursor there? */
        if (++s_ws_step >= WS_HOLD + WS_GAP) {
            if ((r[R_WEAPON] & 7) == s_ws_target || ++s_ws_presses > 16) { s_ws = WS_CLOSE; s_ws_frames = 0; }
            else s_ws_step = 0;
        }
        break;
    case WS_CLOSE:
        if (++s_ws_frames >= 20 && r[R_CAMERA] == 0 && r[R_MODE] == MODE_PLAY) {
            fprintf(stderr, "[MM] weapon %c\n", k_weapon_letter[r[R_WEAPON] & 7]);
            end_switch();
        } else if (s_ws_frames > 120) end_switch();
        break;
    }
}

void game_after_frame(void) {
    uint8_t* r = host_ram();
    if (!r) return;

    if (r[R_STAGE] != s_stage) {
        s_stage = r[R_STAGE];
        s_in_stage = false;
        s_cp_valid = s_cp_prev_valid = false;
        s_cand_timer = 0;
        s_start_pending = false;
        s_scrolling = false;
        s_dying = false;
    }

    /* stage start (READY): the next checkpoint is where Mega Man lands */
    if (r[R_Y] == 0x04 && r[R_COUNTER] == 0xC0) {
        s_in_stage = true;
        s_cp_valid = s_cp_prev_valid = false;
        s_cand_timer = 0;
        s_start_pending = true;
        s_land_frames = 0;
        s_dying = false;
        s_respawn_wait = false;
        s_select_saved = false;
        s_doors = s_door_halves = 0;
        s_door_settle = false;
        fprintf(stderr, "[MM] stage %d start f%u\n", s_stage, host_frame());
    }
    if (s_start_pending && r[R_COUNTER] == 0 && r[R_HP] > 0) {
        s_land_frames = r[R_Y] == s_last_y ? s_land_frames + 1 : 0;
        if (s_land_frames >= 8) {
            s_start_pending = false;
            take_checkpoint("start");
            if (s_stage >= STAGE_WILY1 && s_stage < STAGE_TITLE) file_save_state(true);
        }
    }
    s_last_y = r[R_Y];

    if (s_ws != WS_IDLE) { weapon_switch_step(r); s_prev_hp = r[R_HP]; return; }

    /* the stage select screen (portrait blinking): into the file, once per visit */
    if (!s_title && !s_select_saved && r[R_CAMERA] == 0x8A && r[R_MODE] == 0x06) {
        s_select_saved = true;
        file_save_state(false);
    }
    /* the ending */
    if (s_file >= 0 && r[R_STAGE] == STAGE_ENDING && !s_files[s_file].cleared) {
        s_files[s_file].cleared = true;
        s_files[s_file].deaths = s_deaths;
        file_info_save(s_file);
        fprintf(stderr, "[MM] file %d cleared, deaths %u\n", s_file + 1, s_deaths);
    }

    /* lives never run out */
    if (!(r[R_LIVES] & 0x80) && r[R_LIVES] < 2) r[R_LIVES] = 2;

    /* deaths */
    if (s_in_stage && !s_dying) {
        bool hp_death = s_prev_hp > 0 && r[R_HP] == 0;
        bool other_death = r[R_Y] == 0xF8 && r[R_COUNTER] == 0xC0;
        if (hp_death || other_death) {
            s_dying = true;
            s_deaths++;
            if (s_cand_timer) {                /* died on probation: back to the checkpoint before */
                s_cand_timer = 0;
                fprintf(stderr, "[MM] checkpoint on probation dropped\n");
            }
            s_quick_deaths = s_since_respawn < QUICK_DEATH ? s_quick_deaths + 1 : 0;
            if (s_quick_deaths >= 3 && s_cp_prev_valid) {
                swap_buf(&s_cp, &s_cp_prev);
                s_cp_doors = s_cp_prev_doors;
                s_cp_prev_valid = false;
                s_quick_deaths = 0;
                fprintf(stderr, "[MM] three quick deaths: checkpoint moved back\n");
            }
            if (s_file >= 0) { s_files[s_file].deaths = s_deaths; file_info_save(s_file); }
            memcpy(s_energy_at_death, &r[R_ENERGY], 7);
            s_respawn_wait = s_cp_valid;
            s_death_seq = false;
            fprintf(stderr, "[MM] death %u (%s)%s f%u\n", s_deaths, hp_death ? "health" : "fall/spikes",
                    s_cp_valid ? "" : ", no checkpoint: the game's own respawn", host_frame());
        }
    }
    /* The death sequence runs to its end (explosion orbs gone, $3C down from
     * $C0 to $00); its last frame is the moment to load the checkpoint, just
     * before the game takes a life and restarts the stage itself. */
    if (s_respawn_wait) {
        if (r[R_Y] == 0xF8 && r[R_COUNTER] == 0xC0) s_death_seq = true;
        if (s_death_seq && r[R_Y] == 0xF8 && r[R_COUNTER] <= 0x02) {
            s_respawn_wait = false;
            respawn();
            r = host_ram();
        }
    }

    if (s_in_stage && !s_dying) {
        s_since_respawn++;
        if (s_cand_timer && r[R_HP] > 0 && --s_cand_timer == 0) { s_cand_timer = 1; commit_candidate(); }
    }

    /* transitions: $40 while scrolling to a new area, then the freeze, then $00 */
    uint8_t cam = r[R_CAMERA];
    if (cam != s_prev_camera && (cam & 0xFE) != 0 && cam != 0x80 && cam != 0x81 && cam != 0x8A)
        fprintf(stderr, "[MM] camera %02X f%u\n", cam, host_frame());
    if (cam == 0x40) { s_scrolling = true; s_settle = 0; s_cam_scrolled = true; }
    if (cam == 0x02 && s_prev_camera != 0x02 && s_prev_camera != 0x40) s_cam_scrolled = false;
    if (s_prev_camera == 0x02 && cam < 0x02 && !s_cam_scrolled && s_in_stage && !s_dying) {
        if (++s_door_halves % 2 == 0) {        /* opened and closed: through a door */
            s_doors++;
            s_door_settle = true;
            s_settle = 0;
            fprintf(stderr, "[MM] door %d f%u\n", s_doors, host_frame());
        }
    }
    if ((s_scrolling || s_door_settle) && cam == 0) {
        if (++s_settle >= 3) {
            bool ok = s_in_stage && !s_dying && r[R_HP] > 0;
            if (s_scrolling) {
                /* Mega Man at the top of the new area: he fell or climbed down into it */
                if (r[R_Y] < 0x80) fprintf(stderr, "[MM] transition down: no checkpoint f%u\n", host_frame());
                else if (ok) take_checkpoint("transition");
            } else if (ok && s_doors % 2 == 1) {
                take_checkpoint("door");       /* the corridor before the boss's door */
            }
            s_scrolling = s_door_settle = false;
        }
    } else if (cam != 0) {
        s_settle = 0;
    }
    s_prev_camera = cam;
    s_prev_hp = r[R_HP];
}

const uint32_t* game_picture(void) { return s_hold_picture ? s_frozen : host_nes_frame(); }

int game_smooth(void) { return s_smooth; }

void game_present_rect(int sw, int sh, int* x, int* y, int* w, int* h) {
    int ph = sh, pw;
    switch (s_aspect) {
    case ASPECT_NTSC: pw = sh * 256 * 8 / (240 * 7); break;   /* 8:7 pixels: 585 at 480 */
    case ASPECT_FULL: pw = sh * 4 / 3; break;                 /* the whole 4:3 TV picture */
    default:          pw = sh * 256 / 240; break;             /* square pixels: 512 at 480 */
    }
    if (pw > sw) { ph = ph * sw / pw; pw = sw; }
    *w = pw; *h = ph;
    *x = (sw - pw) / 2; *y = (sh - ph) / 2;
}

void game_draw(uint32_t* cv) {
    memset(cv, 0, CANVAS_W * CANVAS_H * sizeof(uint32_t));   /* transparent: the picture shows */
    const uint32_t white = 0xFFF8F8F8u;
    char n[16];
    snprintf(n, sizeof(n), "%u", s_deaths);
    /* the death badge, in the picture's top right corner */
    int px, py, pw, ph;
    game_present_rect(CANVAS_W, CANVAS_H, &px, &py, &pw, &ph);
    int bw = 14 + ui_text_w(n, 1), bx = px + pw - bw - 6;
    ui_fill(cv, bx, py + 4, bx + bw, py + 15, 0x90000000u);
    ui_skull(cv, bx + 2, py + 6, white, 1);
    ui_text(cv, bx + 10, py + 6, n, white, 1);
    if (s_files_open) files_draw(cv);
    if (s_menu_open) menu_draw(cv);
}

void game_shutdown(void) {
    if (s_cfg_dirty) cfg_save();
    free(s_cp);
    free(s_cp_prev);
    free(s_cand);
}
