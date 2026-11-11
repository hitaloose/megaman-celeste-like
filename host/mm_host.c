/* Mega Man (NES) for handhelds: a small libretro host.
 *
 * Loads an NES core (fceumm) with SDL_LoadObject, runs it one frame per display
 * refresh and hands every frame to the game layer (mm_game.c), which adds
 * the checkpoints, the death counter, L/R weapon switching and the HUD.
 *
 *   mm_host [--core PATH] [--save-dir DIR] ROM
 *
 * Test/headless options (no window, no audio, as fast as possible):
 *   --headless --frames N        run N frames and exit
 *   --input FILE                 "<frame> BUTTONS" lines, BUTTONS joined by +
 *                                (A B SELECT START UP DOWN LEFT RIGHT L R X Y MENU),
 *                                "-" releases all; each line holds until the next
 *   --shot PREFIX --shot-every N write PREFIX_<frame>.ppm of the canvas
 *   --shot-at F                  one more shot at frame F (repeatable)
 *   --watch 1C,6A,...            print these RAM bytes whenever one changes
 *   --ramdump F FILE             write the 2 KiB RAM at the end of frame F
 *   --poke ADDR=VAL[@F]          write this RAM byte before every frame, or only
 *                                before frame F (testing)
 *   --opt KEY=VALUE              set a core option (e.g. fceumm_overclocking=disabled)
 *
 * Benchmark (window and rendering as in play, but no vsync wait and no audio):
 *   --bench STATE [--frames N]   load STATE, run N frames (default 1800) and print
 *                                one line with the time each frame's work took
 *                                (emulation + game layer + drawing; then with the
 *                                present too: load and over_budget count that)
 *                                and the game's lag frames (its own slowdown)
 *
 * SELECT on the options screen (or F5 on a computer) writes the machine to
 * <save dir>/bench.state, for --bench.
 *
 * Window keys on a computer: arrows, Z = B, X = A, Enter = Start,
 * A = Y (rapid fire), Right Shift = Select, Q/W = L/R, Esc = MENU.
 */
#include "libretro.h"
#include "mm_game.h"
#include "mm_host.h"
#include "mm_overlay.h"
#include <SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define make_dir(path) _mkdir(path)
#define CORE_FILE "fceumm_libretro.dll"
#elif defined(__APPLE__)
#define make_dir(path) mkdir(path, 0755)
#define CORE_FILE "fceumm_libretro.dylib"
#else
#define make_dir(path) mkdir(path, 0755)
#define CORE_FILE "fceumm_libretro.so"
#endif

#ifndef DISPLAY_HZ
#define DISPLAY_HZ 60.0
#endif

/* ---- the core ------------------------------------------------------------ */
static struct {
    void* so;
    void (*init)(void);
    void (*deinit)(void);
    void (*set_environment)(retro_environment_t);
    void (*set_video_refresh)(retro_video_refresh_t);
    void (*set_audio_sample)(retro_audio_sample_t);
    void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
    void (*set_input_poll)(retro_input_poll_t);
    void (*set_input_state)(retro_input_state_t);
    void (*get_system_av_info)(struct retro_system_av_info*);
    void (*set_controller_port_device)(unsigned, unsigned);
    bool (*load_game)(const struct retro_game_info*);
    void (*unload_game)(void);
    void (*run)(void);
    void (*reset)(void);
    size_t (*serialize_size)(void);
    bool (*serialize)(void*, size_t);
    bool (*unserialize)(const void*, size_t);
    void* (*get_memory_data)(unsigned);
} core;

static enum retro_pixel_format s_pixfmt = RETRO_PIXEL_FORMAT_0RGB1555;
static char     s_save_dir[512] = ".";
static uint32_t s_frame;
static bool     s_quit;
static uint32_t s_nes_pad;                 /* what the NES reads this frame */
static uint32_t s_canvas[CANVAS_W * CANVAS_H];   /* HUD and menus, over the picture (alpha) */
static uint32_t s_nes[256 * 240];          /* latest NES picture */
static bool     s_mute;
static bool     s_headless;

static bool s_vars_updated;
static struct { const char* key; const char* value; } k_vars[] = {
    { "fceumm_region", "NTSC" },
    { "fceumm_overscan", "disabled" },
    { "fceumm_overscan_h", "disabled" },
    { "fceumm_overscan_v", "disabled" },
    { "fceumm_overscan_h_left", "0" },
    { "fceumm_overscan_h_right", "0" },
    { "fceumm_overscan_v_top", "0" },
    { "fceumm_overscan_v_bottom", "0" },
    { "fceumm_sndrate_hint", "44KHz" },
    { "fceumm_sndquality", "Low" },
    { "fceumm_sndlowpass", "enabled" },       /* RF filter: the TV's muffled sound, less harsh highs (free; Low quality only) */
    { "fceumm_nospritelimit", "enabled" },    /* less flicker */
    { "fceumm_aspect", "1:1 PAR" },
    { "fceumm_palette", "smooth-v2-fbx" },    /* the options screen changes it */
    { "fceumm_overclocking", "2x-Postrender" },   /* NES CPU time doubled: no slowdown with many enemies */
};

uint8_t* host_ram(void) { return (uint8_t*)core.get_memory_data(RETRO_MEMORY_SYSTEM_RAM); }
size_t   host_state_size(void) { return core.serialize_size(); }
bool     host_state_save(void* buf, size_t size) { return core.serialize(buf, size); }
bool     host_state_load(const void* buf, size_t size) { return core.unserialize(buf, size); }
const char* host_save_dir(void) { return s_save_dir; }
uint32_t host_frame(void) { return s_frame; }
void     host_mute(bool on) { s_mute = on; }
void     host_reset(void) { core.reset(); }

void host_set_option(const char* key, const char* value) {
    for (size_t i = 0; i < sizeof(k_vars) / sizeof(k_vars[0]); i++)
        if (!strcmp(k_vars[i].key, key)) { k_vars[i].value = value; s_vars_updated = true; }
}

/* Re-renders the current picture with the latest core options: one frame is
 * run and the machine put back, so the game doesn't move. */
void host_refresh_picture(void) {
    size_t n = core.serialize_size();
    void* buf = malloc(n);
    if (!buf) return;
    if (core.serialize(buf, n)) {
        bool mute = s_mute;
        s_mute = true;
        core.run();
        core.unserialize(buf, n);
        s_mute = mute;
    }
    free(buf);
}
const uint32_t* host_nes_frame(void) { return s_nes; }
void     host_request_quit(void) { s_quit = true; }

static void core_log(enum retro_log_level level, const char* fmt, ...) {
    if (level < RETRO_LOG_WARN) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[core] ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static bool env_cb(unsigned cmd, void* data) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
        enum retro_pixel_format f = *(const enum retro_pixel_format*)data;
        if (f != RETRO_PIXEL_FORMAT_RGB565 && f != RETRO_PIXEL_FORMAT_XRGB8888) return false;
        s_pixfmt = f;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable* v = (struct retro_variable*)data;
        for (size_t i = 0; i < sizeof(k_vars) / sizeof(k_vars[0]); i++)
            if (!strcmp(v->key, k_vars[i].key)) { v->value = k_vars[i].value; return true; }
        v->value = NULL;
        return false;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: *(bool*)data = s_vars_updated; s_vars_updated = false; return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE: *(bool*)data = true; return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: *(const char**)data = s_save_dir; return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((struct retro_log_callback*)data)->log = core_log;
        return true;
    default: return false;
    }
}

/* Latest NES picture, kept as 256x240 (centred if the core sends less). */
static void video_cb(const void* data, unsigned w, unsigned h, size_t pitch) {
    if (!data) return;                         /* duped frame: keep the last one */
    if (w > 256) w = 256;
    if (h > 240) h = 240;
    int y0 = (240 - (int)h) / 2, x0 = (256 - (int)w) / 2;
    for (unsigned y = 0; y < h; y++) {
        uint32_t* dst = &s_nes[(y0 + y) * 256 + x0];
        const uint8_t* row = (const uint8_t*)data + y * pitch;
        if (s_pixfmt == RETRO_PIXEL_FORMAT_XRGB8888) {
            const uint32_t* src = (const uint32_t*)row;
            for (unsigned x = 0; x < w; x++) dst[x] = 0xFF000000u | src[x];
        } else {
            const uint16_t* src = (const uint16_t*)row;
            for (unsigned x = 0; x < w; x++) {
                uint16_t c = src[x];
                uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
                dst[x] = 0xFF000000u | ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
            }
        }
    }
}

/* ---- audio: queued, resampled to follow the display (dynamic rate control) */
static SDL_AudioDeviceID s_audio;
static double s_audio_rate = 44100, s_core_fps = 60.0988, s_display_hz = DISPLAY_HZ;

static size_t audio_batch_cb(const int16_t* data, size_t frames) {
    if (!s_audio || s_mute) return frames;
    /* The core makes rate*core_fps/display_hz samples per second of real time
     * (display_hz: the measured frame rate, see main); resample by that ratio,
     * nudged up to +-2% by the queue fill so it hovers at target. */
    static double pos;
    static int16_t prev[2];
    static int16_t out[4096 * 2];
    const uint32_t target = (uint32_t)(s_audio_rate * 0.06) * 4;     /* 60 ms, bytes */
    uint32_t fill = SDL_GetQueuedAudioSize(s_audio);
    if (fill == 0) {
        /* Ran dry (after a menu, or a hiccup): refill half the target with the
         * last sample held, so it is one soft step instead of a run of gaps. */
        static int16_t hold[4096 * 2];
        uint32_t n = target / 2 / 4;
        for (uint32_t i = 0; i < n; i++) { hold[i * 2] = prev[0]; hold[i * 2 + 1] = prev[1]; }
        SDL_QueueAudio(s_audio, hold, n * 4);
        fill = n * 4;
        if (s_frame > 60) fprintf(stderr, "[audio] underrun f%u\n", s_frame);
    }
    if (fill > target * 4) { SDL_ClearQueuedAudio(s_audio); fill = 0; fprintf(stderr, "[audio] queue cleared f%u\n", s_frame); }
    double err = ((double)target - fill) / target;
    if (err > 1) err = 1;
    if (err < -1) err = -1;
    double step = (s_core_fps / s_display_hz) * (1.0 + 0.02 * err);
    step = 1.0 / step;                                                /* input samples per output sample */
    size_t n = 0;
    while (pos < (double)frames && n < 4096) {
        size_t i = (size_t)pos;
        double t = pos - (double)i;
        int16_t l0 = i ? data[(i - 1) * 2] : prev[0], r0 = i ? data[(i - 1) * 2 + 1] : prev[1];
        out[n * 2]     = (int16_t)(l0 + (data[i * 2] - l0) * t);
        out[n * 2 + 1] = (int16_t)(r0 + (data[i * 2 + 1] - r0) * t);
        n++;
        pos += step;
    }
    pos -= (double)frames;
    if (pos < 0) pos = 0;
    if (frames) { prev[0] = data[(frames - 1) * 2]; prev[1] = data[(frames - 1) * 2 + 1]; }
    SDL_QueueAudio(s_audio, out, (Uint32)(n * 4));
    return frames;
}

static void audio_sample_cb(int16_t l, int16_t r) { int16_t s[2] = { l, r }; audio_batch_cb(s, 1); }

static void input_poll_cb(void) {}
static int16_t input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id) {
    (void)index;
    if (port != 0 || device != RETRO_DEVICE_JOYPAD) return 0;
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return (int16_t)(s_nes_pad & 0xFFFF);
    return id < 16 && (s_nes_pad >> id) & 1;
}

static bool load_core(const char* path) {
    core.so = SDL_LoadObject(path);
    if (!core.so) { fprintf(stderr, "can't load core %s: %s\n", path, SDL_GetError()); return false; }
#define SYM(field, name) if (!(*(void**)&core.field = SDL_LoadFunction(core.so, name))) { fprintf(stderr, "core lacks %s\n", name); return false; }
    SYM(init, "retro_init") SYM(deinit, "retro_deinit")
    SYM(set_environment, "retro_set_environment") SYM(set_video_refresh, "retro_set_video_refresh")
    SYM(set_audio_sample, "retro_set_audio_sample") SYM(set_audio_sample_batch, "retro_set_audio_sample_batch")
    SYM(set_input_poll, "retro_set_input_poll") SYM(set_input_state, "retro_set_input_state")
    SYM(get_system_av_info, "retro_get_system_av_info")
    SYM(set_controller_port_device, "retro_set_controller_port_device")
    SYM(load_game, "retro_load_game") SYM(unload_game, "retro_unload_game") SYM(run, "retro_run") SYM(reset, "retro_reset")
    SYM(serialize_size, "retro_serialize_size") SYM(serialize, "retro_serialize")
    SYM(unserialize, "retro_unserialize") SYM(get_memory_data, "retro_get_memory_data")
#undef SYM
    return true;
}

static void* read_file(const char* path, size_t* size) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void* buf = n > 0 ? malloc((size_t)n) : NULL;
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    *size = buf ? (size_t)n : 0;
    return buf;
}

/* ---- headless test driver -------------------------------------------------- */
typedef struct { uint32_t frame, pad; } InputStep;
static InputStep s_steps[4096];
static int s_nsteps;

static uint32_t parse_buttons(char* s) {
    static const struct { const char* n; uint32_t b; } names[] = {
        { "A", PAD_A }, { "B", PAD_B }, { "SELECT", PAD_SELECT }, { "START", PAD_START },
        { "UP", PAD_UP }, { "DOWN", PAD_DOWN }, { "LEFT", PAD_LEFT }, { "RIGHT", PAD_RIGHT },
        { "L", PAD_L }, { "R", PAD_R }, { "X", PAD_X }, { "Y", PAD_Y }, { "MENU", PAD_MENU },
    };
    uint32_t pad = 0;
    for (char* tok = strtok(s, "+\r\n"); tok; tok = strtok(NULL, "+\r\n"))
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
            if (!strcmp(tok, names[i].n)) pad |= names[i].b;
    return pad;
}

static bool load_input(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) { fprintf(stderr, "can't read %s\n", path); return false; }
    char line[256];
    while (fgets(line, sizeof(line), f) && s_nsteps < 4096) {
        char btn[200] = "";
        unsigned fr;
        if (line[0] == '#' || sscanf(line, "%u %199s", &fr, btn) < 1) continue;
        s_steps[s_nsteps].frame = fr;
        s_steps[s_nsteps].pad = parse_buttons(btn);
        s_nsteps++;
    }
    fclose(f);
    return true;
}

static uint32_t scripted_pad(uint32_t frame) {
    uint32_t pad = 0;
    for (int i = 0; i < s_nsteps && s_steps[i].frame <= frame; i++) pad = s_steps[i].pad;
    return pad;
}

/* What the panel shows, at canvas resolution (320x240): the picture scaled
 * nearest into its place, the overlay blended on top. For test shots. */
static void write_ppm(const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    int dx, dy, dw, dh;
    game_present_rect(CANVAS_W, CANVAS_H, &dx, &dy, &dw, &dh);
    const uint32_t* pic = game_picture();
    fprintf(f, "P6\n%d %d\n255\n", CANVAS_W, CANVAS_H);
    for (int y = 0; y < CANVAS_H; y++)
        for (int x = 0; x < CANVAS_W; x++) {
            uint32_t c = 0;
            if (x >= dx && x < dx + dw && y >= dy && y < dy + dh)
                c = pic[((y - dy) * 240 / dh) * 256 + (x - dx) * 256 / dw];
            uint32_t o = s_canvas[y * CANVAS_W + x], a = o >> 24;
            unsigned char px[3];
            for (int k = 0; k < 3; k++) {
                int sh = 16 - 8 * k;
                px[k] = (unsigned char)((((o >> sh) & 255) * a + ((c >> sh) & 255) * (255 - a)) / 255);
            }
            fwrite(px, 1, 3, f);
        }
    fclose(f);
}

/* rename() that also replaces an existing file on Windows. */
bool host_replace_file(const char* tmp, const char* path) {
#ifdef _WIN32
    return MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING) != 0;
#else
    return rename(tmp, path) == 0;
#endif
}

/* The running machine into <save dir>/bench.state (for --bench). */
bool host_save_bench_state(void) {
    size_t n = core.serialize_size();
    void* buf = malloc(n);
    char path[600], tmp[610];
    snprintf(path, sizeof(path), "%s/bench.state", s_save_dir);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE* f = buf && core.serialize(buf, n) ? fopen(tmp, "wb") : NULL;
    bool ok = f && fwrite(buf, 1, n, f) == n;
    if (f) ok = fclose(f) == 0 && ok;
    ok = ok && host_replace_file(tmp, path);
    free(buf);
    fprintf(stderr, ok ? "[MM] bench state saved: %s\n" : "[MM] can't write %s\n", path);
    return ok;
}

static int cmp_double(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}

/* ---- main ------------------------------------------------------------------ */
int main(int argc, char** argv) {
    const char* core_path = getenv("MM_CORE");
    const char* rom_path = NULL;
    const char *input_file = NULL, *shot = NULL, *ramdump_file = NULL;
    long frames = -1, shot_every = 0, ramdump_frame = -1;
    long shot_at[64]; int nshot_at = 0;
    int watch[64], nwatch = 0;
    int poke_addr[16], poke_val[16], npoke = 0; long poke_at[16];
    const char* bench_state = NULL;
    bool save_dir_set = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--core") && i + 1 < argc) core_path = argv[++i];
        else if (!strcmp(argv[i], "--save-dir") && i + 1 < argc) snprintf(s_save_dir, sizeof(s_save_dir), "%s", argv[++i]), save_dir_set = true;
        else if (!strcmp(argv[i], "--headless")) s_headless = true;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atol(argv[++i]), s_headless = true;
        else if (!strcmp(argv[i], "--input") && i + 1 < argc) input_file = argv[++i];
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--shot-every") && i + 1 < argc) shot_every = atol(argv[++i]);
        else if (!strcmp(argv[i], "--shot-at") && i + 1 < argc && nshot_at < 64) shot_at[nshot_at++] = atol(argv[++i]);
        else if (!strcmp(argv[i], "--ramdump") && i + 2 < argc) ramdump_frame = atol(argv[++i]), ramdump_file = argv[++i];
        else if (!strcmp(argv[i], "--watch") && i + 1 < argc) {
            char* list = argv[++i];
            for (char* t = strtok(list, ","); t && nwatch < 64; t = strtok(NULL, ",")) watch[nwatch++] = (int)strtol(t, NULL, 16);
        } else if (!strcmp(argv[i], "--poke") && i + 1 < argc && npoke < 16) {
            unsigned a, v;
            long at = -1;
            if (sscanf(argv[++i], "%x=%x@%ld", &a, &v, &at) >= 2) { poke_addr[npoke] = (int)a; poke_val[npoke] = (int)v; poke_at[npoke++] = at; }
        } else if (!strcmp(argv[i], "--opt") && i + 1 < argc) {
            char* kv = argv[++i];
            char* eq = strchr(kv, '=');
            if (eq) { *eq = 0; host_set_option(kv, eq + 1); }
        } else if (!strcmp(argv[i], "--bench") && i + 1 < argc) bench_state = argv[++i];
        else if (argv[i][0] != '-') rom_path = argv[i];
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    /* Defaults next to the program: the core, the ROM ("megaman.nes" or
     * "Mega Man (USA).nes") and, unless --save-dir says otherwise, saves/. */
    static char def_core[1024], def_rom[1024];
    char* base = SDL_GetBasePath();
    if (base) {
        if (!core_path) snprintf(def_core, sizeof(def_core), "%s%s", base, CORE_FILE), core_path = def_core;
        if (!rom_path) {
            static const char* const names[] = { "megaman.nes", "Mega Man (USA).nes" };
            for (int i = 0; i < 2 && !rom_path; i++) {
                snprintf(def_rom, sizeof(def_rom), "%s%s", base, names[i]);
                FILE* f = fopen(def_rom, "rb");
                if (f) fclose(f), rom_path = def_rom;
            }
        }
        if (!save_dir_set) snprintf(s_save_dir, sizeof(s_save_dir), "%ssaves", base);
        SDL_free(base);
    }
    if (!rom_path) {
        fprintf(stderr, "usage: %s [--core PATH] [--save-dir DIR] ROM\n", argv[0]);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Mega Man",
            "Mega Man (USA) ROM not found.\n\nPut it next to the program as megaman.nes, "
            "or drop the .nes file on the program.", NULL);
        return 2;
    }
    if (!core_path) core_path = CORE_FILE;
    make_dir(s_save_dir);
    if (input_file && !load_input(input_file)) return 2;
    if (bench_state && frames < 0) frames = 1800;
    if (bench_state) s_headless = false;       /* --frames turned it on: the bench renders */

    if (!load_core(core_path)) return 1;
    core.set_environment(env_cb);
    core.init();
    core.set_video_refresh(video_cb);
    core.set_audio_sample(audio_sample_cb);
    core.set_audio_sample_batch(audio_batch_cb);
    core.set_input_poll(input_poll_cb);
    core.set_input_state(input_state_cb);

    struct retro_game_info gi = { rom_path, NULL, 0, NULL };
    void* rom = read_file(rom_path, &gi.size);
    if (!rom) { fprintf(stderr, "can't read ROM %s\n", rom_path); return 1; }
    gi.data = rom;
    if (!core.load_game(&gi)) { fprintf(stderr, "the core can't load %s\n", rom_path); return 1; }
    core.set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
    struct retro_system_av_info av;
    core.get_system_av_info(&av);
    s_core_fps = av.timing.fps > 0 ? av.timing.fps : 60.0988;
    s_audio_rate = av.timing.sample_rate > 0 ? av.timing.sample_rate : 44100;
    fprintf(stderr, "[host] %ux%u %.4f fps, %.0f Hz audio, state %zu bytes\n",
            av.geometry.base_width, av.geometry.base_height, s_core_fps, s_audio_rate, core.serialize_size());
    for (int i = 0; i < CANVAS_W * CANVAS_H; i++) s_canvas[i] = 0xFF000000u;
    game_init();
    double *bench_ms = NULL, *bench_full = NULL;   /* work before the present / with it */
    long bench_lag = 0, bench_runs = 0;
    double bench_core = 0;                     /* ms inside core.run (the rest is ours) */
    uint8_t bench_c23 = 0;
    if (bench_state) {
        size_t n = 0;
        void* st = read_file(bench_state, &n);
        if (!st || n != core.serialize_size() || !core.unserialize(st, n)) {
            fprintf(stderr, "can't load bench state %s\n", bench_state);
            return 1;
        }
        free(st);
        game_state_loaded();
        bench_c23 = host_ram()[0x23];
        bench_ms = (double*)calloc((size_t)frames, sizeof(double));
        bench_full = (double*)calloc((size_t)frames, sizeof(double));
        if (!bench_ms || !bench_full) return 1;
    }

    SDL_Window* win = NULL;
    SDL_Renderer* ren = NULL;
    SDL_Texture *tex = NULL, *nes_tex = NULL, *sharp = NULL;   /* overlay, picture, 2x prescale */
    SDL_GameController* pad = NULL;
    if (!s_headless) {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
            fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
            return 1;
        }
        SDL_ShowCursor(SDL_DISABLE);
        win = SDL_CreateWindow("Mega Man", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               CANVAS_W * 2, CANVAS_H * 2, 0);
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | (bench_state ? 0 : SDL_RENDERER_PRESENTVSYNC));
        if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
        SDL_RenderSetLogicalSize(ren, CANVAS_W * 2, CANVAS_H * 2);
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, CANVAS_W, CANVAS_H);
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(tex, SDL_ScaleModeNearest);
        nes_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 256, 240);
        SDL_SetTextureScaleMode(nes_tex, SDL_ScaleModeNearest);
        /* SMOOTH (sharp-bilinear): nearest 2x into this, then linear to the screen */
        sharp = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, 512, 480);
        if (sharp) SDL_SetTextureScaleMode(sharp, SDL_ScaleModeLinear);
        else fprintf(stderr, "[host] no render target (%s): SMOOTH falls back to CRISP\n", SDL_GetError());
        SDL_DisplayMode dm;
        if (DISPLAY_HZ == 60.0 && SDL_GetCurrentDisplayMode(0, &dm) == 0 && dm.refresh_rate > 45 && dm.refresh_rate < 75)
            s_display_hz = dm.refresh_rate;            /* first guess only: measured below */
        SDL_AudioSpec want = { 0 }, have;
        want.freq = (int)s_audio_rate;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 512;
        s_audio = bench_state ? 0 : SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (s_audio) {
            SDL_PauseAudioDevice(s_audio, 0);
            fprintf(stderr, "[host] audio %d Hz, %d samples per callback, driver %s\n", have.freq, have.samples, SDL_GetCurrentAudioDriver());
        }
        else if (!bench_state) fprintf(stderr, "no audio: %s\n", SDL_GetError());
        fprintf(stderr, "[host] display %.2f Hz\n", s_display_hz);
    }

    uint8_t prev_watch[64] = { 0 };
    uint32_t held = 0, prev_held = 0;
    while (!s_quit && (frames < 0 || (long)s_frame < frames)) {
        Uint64 t0 = SDL_GetPerformanceCounter();
        if (bench_ms) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) if (e.type == SDL_QUIT) s_quit = true;
            held = scripted_pad(s_frame);
        } else if (!s_headless) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_QUIT) s_quit = true;
                if (e.type == SDL_CONTROLLERDEVICEADDED && !pad) pad = SDL_GameControllerOpen(e.cdevice.which);
            }
            held = 0;
            if (pad) {
                static const struct { SDL_GameControllerButton b; uint32_t bit; } map[] = {
                    { SDL_CONTROLLER_BUTTON_A, PAD_A }, { SDL_CONTROLLER_BUTTON_B, PAD_B },
                    { SDL_CONTROLLER_BUTTON_X, PAD_X }, { SDL_CONTROLLER_BUTTON_Y, PAD_Y },
                    { SDL_CONTROLLER_BUTTON_BACK, PAD_SELECT }, { SDL_CONTROLLER_BUTTON_START, PAD_START },
                    { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, PAD_L }, { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, PAD_R },
                    { SDL_CONTROLLER_BUTTON_DPAD_UP, PAD_UP }, { SDL_CONTROLLER_BUTTON_DPAD_DOWN, PAD_DOWN },
                    { SDL_CONTROLLER_BUTTON_DPAD_LEFT, PAD_LEFT }, { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, PAD_RIGHT },
                    { SDL_CONTROLLER_BUTTON_GUIDE, PAD_MENU },
                };
                for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
                    if (SDL_GameControllerGetButton(pad, map[i].b)) held |= map[i].bit;
            }
            const Uint8* k = SDL_GetKeyboardState(NULL);
            if (k[SDL_SCANCODE_UP]) held |= PAD_UP;
            if (k[SDL_SCANCODE_DOWN]) held |= PAD_DOWN;
            if (k[SDL_SCANCODE_LEFT]) held |= PAD_LEFT;
            if (k[SDL_SCANCODE_RIGHT]) held |= PAD_RIGHT;
            if (k[SDL_SCANCODE_Z]) held |= PAD_B;
            if (k[SDL_SCANCODE_X]) held |= PAD_A;
            if (k[SDL_SCANCODE_A]) held |= PAD_Y;
            if (k[SDL_SCANCODE_RETURN]) held |= PAD_START;
            if (k[SDL_SCANCODE_RSHIFT]) held |= PAD_SELECT;
            if (k[SDL_SCANCODE_Q]) held |= PAD_L;
            if (k[SDL_SCANCODE_W]) held |= PAD_R;
            if (k[SDL_SCANCODE_ESCAPE]) held |= PAD_MENU;
            static bool f5;
            if (k[SDL_SCANCODE_F5] && !f5) host_save_bench_state();
            f5 = k[SDL_SCANCODE_F5];
        } else {
            held = scripted_pad(s_frame);
        }
        uint32_t pressed = held & ~prev_held;
        prev_held = held;
        game_tick(held, pressed);
        for (int n = game_frames_this_tick(); n > 0; n--) {
            s_nes_pad = game_input() & PAD_NES_MASK;
            for (int i = 0; i < npoke; i++)
                if (poke_at[i] < 0 || poke_at[i] == (long)s_frame) host_ram()[poke_addr[i] & 0x7FF] = (uint8_t)poke_val[i];
            Uint64 tc = bench_ms ? SDL_GetPerformanceCounter() : 0;
            core.run();
            if (bench_ms) {
                bench_core += (double)(SDL_GetPerformanceCounter() - tc) * 1000.0 / (double)SDL_GetPerformanceFrequency();                    /* the game's NMI counts $23 only when its main loop made the frame */
                uint8_t c = host_ram()[0x23];
                bench_lag += c == bench_c23;
                bench_c23 = c;
                bench_runs++;
            }
            game_after_frame();
        }
        game_draw(s_canvas);
        s_frame++;

        for (int i = 0; i < nwatch; i++) {
            uint8_t v = host_ram()[watch[i] & 0x7FF];
            if (v != prev_watch[i] || s_frame == 1) {
                printf("f%05u", s_frame);
                for (int j = 0; j < nwatch; j++) printf(" %02X=%02X", watch[j], host_ram()[watch[j] & 0x7FF]);
                printf("\n");
                for (int j = 0; j < nwatch; j++) prev_watch[j] = host_ram()[watch[j] & 0x7FF];
                break;
            }
        }
        bool want_shot = shot && shot_every > 0 && s_frame % shot_every == 0;
        for (int i = 0; i < nshot_at; i++) if ((long)s_frame == shot_at[i]) want_shot = shot != NULL;
        if (want_shot) {
            char path[600];
            snprintf(path, sizeof(path), "%s_%05u.ppm", shot, s_frame);
            write_ppm(path);
        }
        if ((long)s_frame == ramdump_frame) {
            FILE* f = fopen(ramdump_file, "wb");
            if (f) { fwrite(host_ram(), 1, 2048, f); fclose(f); }
        }

        if (!s_headless) {
            SDL_UpdateTexture(nes_tex, NULL, game_picture(), 256 * 4);
            SDL_UpdateTexture(tex, NULL, s_canvas, CANVAS_W * 4);
            SDL_Rect dst;
            game_present_rect(CANVAS_W * 2, CANVAS_H * 2, &dst.x, &dst.y, &dst.w, &dst.h);
            SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
            SDL_RenderClear(ren);
            bool integer = dst.w == 512 && dst.h == 480;
            if (game_smooth() && sharp && !integer && SDL_SetRenderTarget(ren, sharp) == 0) {
                SDL_RenderCopy(ren, nes_tex, NULL, NULL);
                SDL_SetRenderTarget(ren, NULL);
                SDL_RenderCopy(ren, sharp, NULL, &dst);
            } else {
                SDL_RenderCopy(ren, nes_tex, NULL, &dst);
            }
            if (game_overlay()) {                  /* CRT overlay on the picture (its frame on the picture's edges), under the menus */
                static SDL_Texture* crt;
                static int crt_i = -1, crt_w;
                int i = game_overlay() - 1;
                if (i != crt_i || dst.w != crt_w) {    /* rebuilt only when the overlay or ASPECT changes */
                    if (crt) SDL_DestroyTexture(crt);
                    crt = NULL;
                    crt_i = i;
                    crt_w = dst.w;
                    int w = dst.w < OVERLAY_W ? dst.w : OVERLAY_W;
                    const uint32_t* px = overlay_fit(i, w);
                    if (px && (crt = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, OVERLAY_H))) {
                        SDL_UpdateTexture(crt, NULL, px, w * 4);
                        SDL_SetTextureBlendMode(crt, SDL_BLENDMODE_BLEND);
                        SDL_SetTextureScaleMode(crt, SDL_ScaleModeNearest);
                    }
                }
                if (crt) SDL_RenderCopy(ren, crt, NULL, &dst);   /* 1:1 while the picture is 480 tall (every ASPECT at 640x480) */
            }
            SDL_RenderCopy(ren, tex, NULL, NULL);
            double work = (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
            if (bench_ms) bench_ms[s_frame - 1] = work;
            else if (work > 1000.0 / s_display_hz) fprintf(stderr, "[host] slow frame %.1f ms f%u\n", work, s_frame);
            SDL_RenderPresent(ren);
            static Uint64 last_present;
            Uint64 now = SDL_GetPerformanceCounter();
            double gap = last_present ? (double)(now - last_present) * 1000.0 / (double)SDL_GetPerformanceFrequency() : 0;
            if (!bench_ms && gap > 2.5 * 1000.0 / s_display_hz) fprintf(stderr, "[host] %.1f ms between frames f%u\n", gap, s_frame);
            Uint64 last_present_prev = last_present;
            last_present = now;
            /* The real refresh rate (60.46 Hz on the RG35XX). Under vsync the gap
             * between presents is a whole number of panel periods (2 when a frame
             * missed vsync), so count periods, not frames (counting frames, one
             * miss per window read 60.46 Hz as 59.96), over ~2 s windows and
             * smooth; a window with a stall (menu, loading) is thrown away. The
             * audio resampler follows the result. */
            static Uint64 hz_ticks;
            static unsigned hz_periods;
            static bool hz_seen;
            unsigned periods = (unsigned)(gap * s_display_hz / 1000.0 + 0.5);
            if (bench_ms || gap <= 0 || gap > 100.0 || periods == 0) { hz_ticks = 0; hz_periods = 0; }
            else {
                hz_ticks += now - last_present_prev;
                hz_periods += periods;
                if (hz_periods >= 120) {
                    double hz = hz_periods * (double)SDL_GetPerformanceFrequency() / (double)hz_ticks;
                    if (hz > 45 && hz < 75) {
                        s_display_hz = hz_seen ? s_display_hz * 0.75 + hz * 0.25 : hz;
                        if (!hz_seen) fprintf(stderr, "[host] display measured %.2f Hz f%u\n", hz, s_frame);
                        hz_seen = true;
                    }
                    hz_ticks = 0; hz_periods = 0;
                }
            }
            if (!bench_ms && s_frame % 1200 == 0)
                fprintf(stderr, "[host] %.2f Hz, audio queue %.0f ms f%u\n", s_display_hz,
                        s_audio ? SDL_GetQueuedAudioSize(s_audio) / 4 * 1000.0 / s_audio_rate : 0.0, s_frame);
            if (bench_ms) bench_full[s_frame - 1] = (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
        }
    }

    if (bench_ms && s_frame > 0) {
        long n = (long)s_frame;
        double sum = 0, budget = 1000.0 / s_display_hz;
        double fsum = 0;
        long over = 0;
        for (long i = 0; i < n; i++) { sum += bench_ms[i]; fsum += bench_full[i]; over += bench_full[i] > budget; }
        qsort(bench_ms, (size_t)n, sizeof(double), cmp_double);
        qsort(bench_full, (size_t)n, sizeof(double), cmp_double);
        double avg = sum / n;
        const char* oc = "?";
        for (size_t i = 0; i < sizeof(k_vars) / sizeof(k_vars[0]); i++)
            if (!strcmp(k_vars[i].key, "fceumm_overclocking")) oc = k_vars[i].value;
        double favg = fsum / n;
        printf("bench oc=%s frames=%ld avg=%.2f p99=%.2f max=%.2f with_present: avg=%.2f p99=%.2f max=%.2f "
               "core=%.2f over_budget=%ld budget=%.2f load=%.0f%% lag=%ld/%ld\n",
               oc, n, avg, bench_ms[n * 99 / 100], bench_ms[n - 1], favg, bench_full[n * 99 / 100], bench_full[n - 1],
               bench_core / n, over, budget, 100.0 * favg / budget, bench_lag, bench_runs);
        free(bench_ms);
        free(bench_full);
    }
    game_shutdown();
    core.unload_game();
    core.deinit();
    if (!s_headless) {
        if (s_audio) SDL_CloseAudioDevice(s_audio);
        SDL_Quit();
    }
    return 0;
}
