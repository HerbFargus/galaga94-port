/* SDL front end: window, keyboard/controller -> XT scancodes, file lookup.
 *
 * Game files are read from the data directory (next to the executable, or
 * its data/ subfolder).  Files the game writes (SCORES.DTA) go to the user's
 * save directory, which is also checked first when reading, so the shipped
 * data stays untouched.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <SDL.h>
#include "cpu.h"
#include "platform.h"

static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static char data_dir[1024], save_dir[1024];
static SDL_GameController *pad;

/* ------------------------------------------------------------ files */
const char *platform_data_dir(void) { return data_dir; }

static int exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

/* dir + name, matching the name case-insensitively (DOS names on Linux) */
static void resolve(const char *dir, const char *name, char *out, size_t n)
{
    snprintf(out, n, "%s%s", dir, name);
    if (exists(out)) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!SDL_strcasecmp(e->d_name, name)) { snprintf(out, n, "%s%s", dir, e->d_name); break; }
    }
    closedir(d);
}

static const char *basename_dos(const char *name)
{
    const char *b = name;
    for (const char *p = name; *p; p++) if (*p == '\\' || *p == '/' || *p == ':') b = p + 1;
    return b;
}

FILE *platform_open(const char *name, const char *mode)
{
    char path[1100];
    name = basename_dos(name);
    if (mode[0] == 'w') {
        resolve(save_dir, name, path, sizeof path);
        return fopen(path, mode);
    }
    resolve(save_dir, name, path, sizeof path);
    if (exists(path)) return fopen(path, mode);
    resolve(data_dir, name, path, sizeof path);
    FILE *f = fopen(path, mode);
    if (!f && strchr(mode, '+')) f = fopen(path, "rb");
    return f;
}

void platform_delete(const char *name)
{
    char path[1100];
    resolve(save_dir, basename_dos(name), path, sizeof path);
    remove(path);
}

void platform_truncate(FILE *f) { (void)f; }

uint8_t *platform_load_file(const char *name, size_t *len)
{
    FILE *f = platform_open(name, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(*len);
    if (fread(buf, 1, *len, f) != *len) { free(buf); fclose(f); return NULL; }
    fclose(f);
    return buf;
}

static void find_data_dir(void)
{
    char *base = SDL_GetBasePath();
    const char *cands[4];
    char a[1024], b[1024];
    snprintf(a, sizeof a, "%s", base ? base : "./");
    snprintf(b, sizeof b, "%sdata/", base ? base : "./");
    cands[0] = b; cands[1] = a; cands[2] = "data/"; cands[3] = "./";
    for (int i = 0; i < 4; i++) {
        char p[1100];
        resolve(cands[i], "GALAGA.EXE", p, sizeof p);
        if (exists(p)) { snprintf(data_dir, sizeof data_dir, "%s", cands[i]); break; }
    }
    if (!data_dir[0]) snprintf(data_dir, sizeof data_dir, "%s", b);
    SDL_free(base);
    char *pref = SDL_GetPrefPath("APSS", "Galaga94");
    snprintf(save_dir, sizeof save_dir, "%s", pref ? pref : data_dir);
    SDL_free(pref);
}

/* ------------------------------------------------------------ keyboard */
static uint8_t kbq[64];
static int kbq_head, kbq_tail;

int kb_queue_size(void) { return (kbq_tail - kbq_head) & 63; }
uint8_t kb_queue_pop(void) { uint8_t v = kbq[kbq_head]; kbq_head = (kbq_head + 1) & 63; return v; }
static void kb_push(uint8_t v) { if (kb_queue_size() < 63) { kbq[kbq_tail] = v; kbq_tail = (kbq_tail + 1) & 63; } }

/* SDL scancode -> XT set 1; 0x100 marks an E0-prefixed key */
static int xt_code(SDL_Scancode s)
{
    static const struct { SDL_Scancode s; int c; } map[] = {
        {SDL_SCANCODE_ESCAPE, 0x01}, {SDL_SCANCODE_1, 0x02}, {SDL_SCANCODE_2, 0x03}, {SDL_SCANCODE_3, 0x04},
        {SDL_SCANCODE_4, 0x05}, {SDL_SCANCODE_5, 0x06}, {SDL_SCANCODE_6, 0x07}, {SDL_SCANCODE_7, 0x08},
        {SDL_SCANCODE_8, 0x09}, {SDL_SCANCODE_9, 0x0A}, {SDL_SCANCODE_0, 0x0B}, {SDL_SCANCODE_MINUS, 0x0C},
        {SDL_SCANCODE_EQUALS, 0x0D}, {SDL_SCANCODE_BACKSPACE, 0x0E}, {SDL_SCANCODE_TAB, 0x0F},
        {SDL_SCANCODE_Q, 0x10}, {SDL_SCANCODE_W, 0x11}, {SDL_SCANCODE_E, 0x12}, {SDL_SCANCODE_R, 0x13},
        {SDL_SCANCODE_T, 0x14}, {SDL_SCANCODE_Y, 0x15}, {SDL_SCANCODE_U, 0x16}, {SDL_SCANCODE_I, 0x17},
        {SDL_SCANCODE_O, 0x18}, {SDL_SCANCODE_P, 0x19}, {SDL_SCANCODE_LEFTBRACKET, 0x1A},
        {SDL_SCANCODE_RIGHTBRACKET, 0x1B}, {SDL_SCANCODE_RETURN, 0x1C}, {SDL_SCANCODE_LCTRL, 0x1D},
        {SDL_SCANCODE_A, 0x1E}, {SDL_SCANCODE_S, 0x1F}, {SDL_SCANCODE_D, 0x20}, {SDL_SCANCODE_F, 0x21},
        {SDL_SCANCODE_G, 0x22}, {SDL_SCANCODE_H, 0x23}, {SDL_SCANCODE_J, 0x24}, {SDL_SCANCODE_K, 0x25},
        {SDL_SCANCODE_L, 0x26}, {SDL_SCANCODE_SEMICOLON, 0x27}, {SDL_SCANCODE_APOSTROPHE, 0x28},
        {SDL_SCANCODE_GRAVE, 0x29}, {SDL_SCANCODE_LSHIFT, 0x2A}, {SDL_SCANCODE_BACKSLASH, 0x2B},
        {SDL_SCANCODE_Z, 0x2C}, {SDL_SCANCODE_X, 0x2D}, {SDL_SCANCODE_C, 0x2E}, {SDL_SCANCODE_V, 0x2F},
        {SDL_SCANCODE_B, 0x30}, {SDL_SCANCODE_N, 0x31}, {SDL_SCANCODE_M, 0x32}, {SDL_SCANCODE_COMMA, 0x33},
        {SDL_SCANCODE_PERIOD, 0x34}, {SDL_SCANCODE_SLASH, 0x35}, {SDL_SCANCODE_RSHIFT, 0x36},
        {SDL_SCANCODE_KP_MULTIPLY, 0x37}, {SDL_SCANCODE_LALT, 0x38}, {SDL_SCANCODE_SPACE, 0x39},
        {SDL_SCANCODE_CAPSLOCK, 0x3A}, {SDL_SCANCODE_F1, 0x3B}, {SDL_SCANCODE_F2, 0x3C}, {SDL_SCANCODE_F3, 0x3D},
        {SDL_SCANCODE_F4, 0x3E}, {SDL_SCANCODE_F5, 0x3F}, {SDL_SCANCODE_F6, 0x40}, {SDL_SCANCODE_F7, 0x41},
        {SDL_SCANCODE_F8, 0x42}, {SDL_SCANCODE_F9, 0x43}, {SDL_SCANCODE_F10, 0x44},
        {SDL_SCANCODE_KP_7, 0x47}, {SDL_SCANCODE_KP_8, 0x48}, {SDL_SCANCODE_KP_9, 0x49},
        {SDL_SCANCODE_KP_MINUS, 0x4A}, {SDL_SCANCODE_KP_4, 0x4B}, {SDL_SCANCODE_KP_5, 0x4C},
        {SDL_SCANCODE_KP_6, 0x4D}, {SDL_SCANCODE_KP_PLUS, 0x4E}, {SDL_SCANCODE_KP_1, 0x4F},
        {SDL_SCANCODE_KP_2, 0x50}, {SDL_SCANCODE_KP_3, 0x51}, {SDL_SCANCODE_KP_0, 0x52},
        {SDL_SCANCODE_KP_PERIOD, 0x53}, {SDL_SCANCODE_F11, 0x57}, {SDL_SCANCODE_F12, 0x58},
        {SDL_SCANCODE_KP_ENTER, 0x11C}, {SDL_SCANCODE_RCTRL, 0x11D}, {SDL_SCANCODE_RALT, 0x138},
        {SDL_SCANCODE_HOME, 0x147}, {SDL_SCANCODE_UP, 0x148}, {SDL_SCANCODE_PAGEUP, 0x149},
        {SDL_SCANCODE_LEFT, 0x14B}, {SDL_SCANCODE_RIGHT, 0x14D}, {SDL_SCANCODE_END, 0x14F},
        {SDL_SCANCODE_DOWN, 0x150}, {SDL_SCANCODE_PAGEDOWN, 0x151}, {SDL_SCANCODE_INSERT, 0x152},
        {SDL_SCANCODE_DELETE, 0x153},
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        if (map[i].s == s) return map[i].c;
    return 0;
}

static void send_key(int code, int down)
{
    if (!code) return;
#ifdef TRACE
    fprintf(stderr, "send_key %x %d q=%d IF=%d\n", code, down, kb_queue_size(), R.iff);
#endif
    if (code & 0x100) kb_push(0xE0);
    kb_push((uint8_t)((code & 0x7F) | (down ? 0 : 0x80)));
}

/* Controller: d-pad/stick = arrows, A = fire (right shift), B = megabomb
   (Enter), Start = F1, Back = Esc, Y = F2 (difficulty).  Keyboard and pad
   work together. */
static int pad_state[16];
static void pad_key(int slot, int code, int down)
{
    if (pad_state[slot] == down) return;
    pad_state[slot] = down;
    send_key(code, down);
}

static void poll_pad(void)
{
    if (!pad) return;
    int lx = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
    int ly = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
    pad_key(0, 0x14B, SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT) || lx < -12000);
    pad_key(1, 0x14D, SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || lx > 12000);
    pad_key(2, 0x148, SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP) || ly < -20000);
    pad_key(3, 0x36, SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A));
    pad_key(4, 0x1C, SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B));
    pad_key(5, 0x3B, SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_START));
    pad_key(6, 0x01, SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_BACK));
    pad_key(7, 0x3C, SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_Y));
    pad_key(8, 0x36, SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000);
}

static uint32_t last_pump;

/* Testing aid: GALAGA_INPUT="ms:code[+|-],..." presses (+) or releases (-)
   an XT scancode (hex, 1xx for E0 keys) at a time after start; no sign means
   a 100 ms tap.  GALAGA_SHOT="ms:file.bmp,..." saves the 320x200 screen. */
static void scripted_input(void)
{
    static const char *in, *shot;
    static int init;
    static struct { uint32_t t; int code, down; } ev[8192];
    static int nev, iev;
    static struct { uint32_t t; char file[200]; } sh[64];
    static int nsh, ish;
    if (!init) {
        init = 1;
        in = getenv("GALAGA_INPUT");
        for (const char *p = in; p && *p && nev < 8190;) {
            unsigned long t = strtoul(p, (char **)&p, 10);
            if (*p++ != ':') break;
            int code = (int)strtol(p, (char **)&p, 16);
            if (*p == '+' || *p == '-') { ev[nev].t = t; ev[nev].code = code; ev[nev++].down = *p++ == '+'; }
            else {
                ev[nev].t = t; ev[nev].code = code; ev[nev++].down = 1;
                ev[nev].t = t + 100; ev[nev].code = code; ev[nev++].down = 0;
            }
            if (*p == ',') p++;
        }
        shot = getenv("GALAGA_SHOT");
        for (const char *p = shot; p && *p && nsh < 64;) {
            unsigned long t = strtoul(p, (char **)&p, 10);
            if (*p++ != ':') break;
            int n = 0;
            while (*p && *p != ',' && n < 199) sh[nsh].file[n++] = *p++;
            sh[nsh].file[n] = 0; sh[nsh++].t = t;
            if (*p == ',') p++;
        }
    }
    uint32_t now = (uint32_t)now_ms();
    /* events are given in order except the generated releases; scan all */
    for (int i = iev; i < nev; i++)
        if (ev[i].code >= 0 && ev[i].t <= now) { send_key(ev[i].code, ev[i].down); ev[i].code = -1; }
    while (iev < nev && ev[iev].code < 0) iev++;
    while (ish < nsh && sh[ish].t <= now) {
        static uint32_t px[320 * 200];
        vga_render(px);
        SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(px, 320, 200, 32, 320 * 4, SDL_PIXELFORMAT_ARGB8888);
        SDL_SaveBMP(s, sh[ish].file);
        SDL_FreeSurface(s);
        ish++;
    }
    if (getenv("GALAGA_QUIT_MS") && now > (uint32_t)atoi(getenv("GALAGA_QUIT_MS"))) platform_quit(0);
}

void platform_pump(void)
{
    uint32_t t = SDL_GetTicks();
    if (t == last_pump) return;
    last_pump = t;
    scripted_input();
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT:
            platform_quit(0);
            break;
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            SDL_Scancode s = e.key.keysym.scancode;
            int down = e.type == SDL_KEYDOWN;
            if (down && s == SDL_SCANCODE_RETURN && (e.key.keysym.mod & KMOD_ALT)) {
                Uint32 fs = SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP;
                SDL_SetWindowFullscreen(win, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                break;
            }
            if (e.key.repeat) break;     /* the game sees make/break only */
            if (s == SDL_SCANCODE_LCTRL || s == SDL_SCANCODE_SPACE) send_key(0x36, down);   /* extra fire keys */
            send_key(xt_code(s), down);
            break;
        }
        case SDL_CONTROLLERDEVICEADDED:
            if (!pad) pad = SDL_GameControllerOpen(e.cdevice.which);
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            if (pad && e.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad))) {
                SDL_GameControllerClose(pad); pad = NULL;
            }
            break;
        }
    }
    poll_pad();
}

/* ------------------------------------------------------------ video */
static uint32_t frame[320 * 200];

void platform_present(void)
{
    vga_render(frame);
    SDL_UpdateTexture(tex, NULL, frame, 320 * 4);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, NULL, NULL);
    SDL_RenderPresent(ren);
}

void platform_init(int argc, char **argv)
{
    int fullscreen = 0;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "-f") || !strcmp(argv[i], "--fullscreen")) fullscreen = 1;
    SDL_SetMainReady();
#ifdef SDL_HINT_WINDOWS_DPI_AWARENESS
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0)
        fatal("SDL_Init: %s", SDL_GetError());
    find_data_dir();
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    /* largest whole multiple of 320x240 that fits comfortably on the desktop */
    SDL_Rect usable;
    int scale = 3;
    if (SDL_GetDisplayUsableBounds(0, &usable) == 0) {
        scale = 1;
        while (320 * (scale + 1) <= usable.w * 9 / 10 && 240 * (scale + 1) <= usable.h * 9 / 10) scale++;
    }
    win = SDL_CreateWindow("Galaga '94", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 320 * scale, 240 * scale,
                           SDL_WINDOW_RESIZABLE | (fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    if (!win) fatal("SDL_CreateWindow: %s", SDL_GetError());
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
    SDL_RenderSetLogicalSize(ren, 320, 240);          /* 320x200 shown at 4:3 */
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 320, 200);
    SDL_ShowCursor(SDL_DISABLE);
    sound_init();
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i)) { pad = SDL_GameControllerOpen(i); break; }
}

void platform_quit(int code)
{
    SDL_Quit();
    exit(code);
}

#ifdef _WIN32
#include <windows.h>
int main(int argc, char **argv);
int WINAPI WinMain(HINSTANCE a, HINSTANCE b, LPSTR c, int d)
{
    (void)a; (void)b; (void)c; (void)d;
    return main(__argc, __argv);
}
#endif
