/* The machine the recompiled game runs on: loader, run loop, interrupts,
 * DOS and BIOS services, I/O ports and the SDL front end.
 *
 * Execution model: game.c has one function per code segment.  run_loop()
 * calls the function for the current CS until something leaves it (a far
 * call, a far return or an iret into another segment).  CS = F000 is the
 * fake BIOS: every interrupt vector starts out as F000:<n>, and reaching one
 * runs the C version of that service.  Hardware interrupts are delivered at
 * POLL points (backward jumps and port reads) by pushing a return frame to
 * F000:FFFE and running the handler in a nested run_loop until it irets
 * there.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <SDL.h>
#include "cpu.h"
#include "platform.h"

cpu_t R;
uint8_t M[0x110000];
volatile int poll_counter = 1;

#define PSPSEG (LOADSEG - 0x10)
#define ENVSEG (PSPSEG - 0x10)
#define BIOS_SEG 0xF000
#define SENTINEL_IP 0xFFFE
#define MEM_TOP 0xA000

static int nest_exit;
static uint16_t dos_free_seg;   /* first paragraph DOS can hand out */
static uint16_t dta_seg, dta_off;

void fatal(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "galaga94: %s\n", buf);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Galaga '94", buf, NULL);
    exit(1);
}

void bad_target(void)
{
    fatal("jump to unknown code at %04X:%04X (image seg %04X)", R.cs, R.ip, (uint16_t)(R.cs - LOADSEG));
}

void bad_insn(uint16_t ip, const char *what)
{
    fatal("unsupported instruction at %04X:%04X: %s", (uint16_t)(R.cs - LOADSEG), ip, what);
}

void divide_error(uint16_t ip)
{
    fatal("Runtime error 200 (division by zero) at %04X:%04X", (uint16_t)(R.cs - LOADSEG), ip);
}

#ifdef TRACE
/* log each routine the first time it runs, and main-segment calls always
   when GALAGA_TRACE=all */
void trace_sub(uint16_t seg, uint16_t off)
{
    static uint8_t seen[0x10000 >> 3][4];
    static int all = -1;
    if (all < 0) { const char *e = getenv("GALAGA_TRACE"); all = e && !strcmp(e, "all"); }
    static uint32_t ring[64]; static int rp; static int dumped;
    ring[rp++ & 63] = (uint32_t)seg << 16 | off;
    if (seg == 0 && off == 0 && !dumped) {
        dumped = 1;
        fprintf(stderr, "--- reached 0000:0000, last calls:\n");
        for (int i = 0; i < 64; i++) { uint32_t v = ring[(rp + i) & 63]; fprintf(stderr, " %04x:%04x", v >> 16, v & 0xffff); }
        fprintf(stderr, "\nSS:SP=%04x:%04x BP=%04x stack:", R.ss, SP, BP);
        for (int i = 0; i < 16; i++) fprintf(stderr, " %04x", rw(R.ss, SP + i * 2));
        fprintf(stderr, "\n");
    }
    int si = seg == 0 ? 0 : seg == 0x6d9 ? 1 : seg == 0x6dc ? 2 : 3;
    int first = !(seen[off >> 3][si] & (1 << (off & 7)));
    seen[off >> 3][si] |= 1 << (off & 7);
    if (first || (all && seg == 0))
        fprintf(stderr, "%9.1f %04x:%04x%s\n", now_ms(), seg, off, first ? " *" : "");
}
#endif

/* ------------------------------------------------------------ run loop */
static void run_loop(void)
{
    for (;;) {
        if (nest_exit) { nest_exit = 0; return; }
        run_segment((uint16_t)(R.cs - LOADSEG));
    }
}

static void bios_int(uint8_t n);

/* Run interrupt vector n to completion from C (hardware IRQs, INT 1Ch). */
static void call_vector(uint8_t n)
{
    uint16_t off = rw(0, n * 4), seg = rw(0, n * 4 + 2);
    if (seg == BIOS_SEG && off == n) { bios_int(n); return; }
    uint16_t save_cs = R.cs, save_ip = R.ip;
    push16(get_flags());
    push16(BIOS_SEG);
    push16(SENTINEL_IP);
    IF = 0;
    R.cs = seg; R.ip = off;
    run_loop();
    R.cs = save_cs; R.ip = save_ip;
}

int do_int(uint8_t n)
{
    uint16_t off = rw(0, n * 4), seg = rw(0, n * 4 + 2);
    if (seg == BIOS_SEG && off == n) { bios_int(n); return 0; }
    push16(get_flags());
    push16(R.cs);
    push16(R.ip);
    IF = 0;
    R.cs = seg; R.ip = off;
    return 1;
}

void do_iret(void)
{
    R.ip = pop16();
    R.cs = pop16();
    set_flags(pop16());
}

/* A far transfer landed outside the recompiled segments. */
void hle_far(void)
{
    if (R.cs == BIOS_SEG) {
        if (R.ip == SENTINEL_IP) { nest_exit = 1; return; }   /* the handler's iret already popped */
        bios_int((uint8_t)R.ip);   /* a game ISR chained to the old vector */
        do_iret();
        return;
    }
    /* Anything else is a far call into FX_SB.DRV, the CT-VOICE driver the
       game loaded into its heap. */
    ctvoice_call();
    R.ip = pop16();
    R.cs = pop16();
}

/* ------------------------------------------------------------ timing */
static uint64_t perf_freq, t0;
static double next_tick_ms;          /* PIT channel 0, 18.2065 Hz */
static int irq_pending;              /* bit 0 timer, bit 1 keyboard */
static uint8_t pic_mask;
static uint8_t kb_latch;
static int kb_busy;
static double last_frame_ms;
static uint32_t spin_ms, spin_count;

double now_ms(void)
{
    return (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)perf_freq;
}

#define PIT_MS (65536.0 * 1000.0 / 1193182.0)
#define FRAME_MS (1000.0 / 70.086)

void poll_service(void)
{
    poll_counter = 1000;
    double t = now_ms();

    platform_pump();
    if (t >= next_tick_ms) {
        irq_pending |= 1;
        next_tick_ms += PIT_MS;
        if (t > next_tick_ms + 500) next_tick_ms = t + PIT_MS;   /* after a stall, don't race */
    }
    if (!kb_busy && kb_queue_size() > 0)
        irq_pending |= 2;
    /* Show each page the game flips to; screens that never flip (palette
       effects) still refresh at the VGA's 70 Hz. */
    if (vga_flipped || t - last_frame_ms >= FRAME_MS) {
        vga_flipped = 0;
        last_frame_ms = t;
        platform_present();
#ifdef TRACE
        static int frames; static double fsec;
        frames++;
        if (t - fsec >= 1000) { fprintf(stderr, "%9.1f fps %d\n", t, frames); frames = 0; fsec = t; }
#endif
    }
    ctvoice_poll();

    if (IF) {
        if ((irq_pending & 1) && !(pic_mask & 1)) {
            irq_pending &= ~1;
            call_vector(8);
        }
        if ((irq_pending & 2) && !(pic_mask & 2) && kb_queue_size() > 0) {
            irq_pending &= ~2;
            kb_latch = kb_queue_pop();
#ifdef TRACE
            fprintf(stderr, "%9.1f key %02x\n", t, kb_latch);
#endif
            kb_busy = 1;
            call_vector(9);
            kb_busy = 0;
        }
    }

    /* Busy-waiting on the tick counter or the retrace: give the CPU back. */
    uint32_t ms = (uint32_t)t;
    if (ms == spin_ms) {
        if (++spin_count > 40) { SDL_Delay(1); spin_count = 0; }
    } else {
        spin_ms = ms; spin_count = 0;
    }
}

/* ------------------------------------------------------------ ports */
static uint8_t port61;
static int pit2_lohi, pit2_count;

uint8_t port_in(uint16_t port)
{
    switch (port) {
    case 0x60: return kb_latch;
    case 0x61: return port61;
    case 0x21: return pic_mask;
    case 0x201: return 0xFF;
    }
    if (port >= 0x3C0 && port <= 0x3DF) return vga_port_in(port);
    return 0xFF;
}

uint16_t port_in16(uint16_t port)
{
    return port_in(port) | (port_in(port + 1) << 8);
}

void port_out(uint16_t port, uint8_t v)
{
    switch (port) {
    case 0x20: return;                       /* EOI */
    case 0x21: pic_mask = v; return;
    case 0x43: pit2_lohi = 0; return;
    case 0x42:
        if (!pit2_lohi) { pit2_count = v; pit2_lohi = 1; }
        else { pit2_count |= v << 8; pit2_lohi = 0; speaker_set(pit2_count, port61); }
        return;
    case 0x61: port61 = v; speaker_set(pit2_count, port61); return;
    }
    if (port >= 0x3C0 && port <= 0x3DF) { vga_port_out(port, v); return; }
}

void port_out16(uint16_t port, uint16_t v)
{
    port_out(port, (uint8_t)v);
    port_out(port + 1, v >> 8);
}

/* ------------------------------------------------------------ BIOS */
static void int10(void)
{
    switch (AH) {
    case 0x00:
        vga_set_mode(AL & 0x7F);
        M[0x449] = AL & 0x7F;
        break;
    case 0x0F: AL = M[0x449]; AH = 80; BH = 0; break;
    case 0x08: AX = 0x0720; break;
    case 0x03: CX = 0x0607; DX = 0; break;
    case 0x10:
        if (AL == 0x10) vga_set_dac(BX & 0xFF, DH, CH, CL);
        else if (AL == 0x12) {
            for (int i = 0; i < CX; i++)
                vga_set_dac((BX + i) & 0xFF, rb(R.es, DX + i * 3), rb(R.es, DX + i * 3 + 1), rb(R.es, DX + i * 3 + 2));
        } else if (AL == 0x17) {
            for (int i = 0; i < CX; i++) {
                uint8_t c[3];
                vga_get_dac((BX + i) & 0xFF, c);
                for (int k = 0; k < 3; k++) wb(R.es, DX + i * 3 + k, c[k]);
            }
        }
        break;
    case 0x11:
        if (AL == 0x30) { CX = 16; DL = 24; }
        break;
    case 0x12:
        if (BL == 0x10) { BH = 0; BL = 3; CX = 0; }
        break;
    case 0x1A: AL = 0x1A; BX = 0x0008; break;
    case 0x0E: case 0x09: case 0x0A:
        break;
    }
}

static void int16(void)
{
    switch (AH) {
    case 0x01: case 0x11: ZF = 1; break;
    case 0x02: AL = 0; break;
    case 0x00: case 0x10: AX = 0; break;
    }
}

static void bios_int(uint8_t n)
{
    switch (n) {
    case 0x08: {
        uint32_t t = M[0x46C] | M[0x46D] << 8 | M[0x46E] << 16 | (uint32_t)M[0x46F] << 24;
        t++;
        if (t >= 0x1800B0) { t = 0; M[0x470] = 1; }
        M[0x46C] = t; M[0x46D] = t >> 8; M[0x46E] = t >> 16; M[0x46F] = t >> 24;
        call_vector(0x1C);
        return;
    }
    case 0x09: return;                       /* the game reads port 60h itself */
    case 0x10: int10(); return;
    case 0x11: AX = M[0x410] | M[0x411] << 8; return;
    case 0x12: AX = 640; return;
    case 0x15:
        if (AH == 0x84) { AX = 0; BX = CX = DX = 0; CF = 1; return; }   /* no joystick BIOS */
        CF = 1; AH = 0x86;
        return;
    case 0x16: int16(); return;
    case 0x1A: {
        CX = M[0x46E] | M[0x46F] << 8; DX = M[0x46C] | M[0x46D] << 8; AL = M[0x470]; M[0x470] = 0;
        return;
    }
    case 0x1B: case 0x1C: case 0x23: case 0x24:
        return;
    case 0x21: dos_int21(); return;
    case 0x20: platform_quit(0); return;
    }
}

/* ------------------------------------------------------------ DOS */
#define MAX_FILES 32
static FILE *files[MAX_FILES];

static void read_asciiz(uint16_t seg, uint16_t off, char *out, int max)
{
    int i = 0;
    for (; i < max - 1; i++) {
        uint8_t c = rb(seg, off + i);
        if (!c) break;
        out[i] = c;
    }
    out[i] = 0;
}

static void dos_error(uint16_t code) { CF = 1; AX = code; }

void dos_int21(void)
{
    CF = 0;
    switch (AH) {
    case 0x02: putchar(DL); break;
    case 0x09: {
        for (uint16_t i = DX; rb(R.ds, i) != '$'; i++) putchar(rb(R.ds, i));
        break;
    }
    case 0x0E: AL = 26; break;
    case 0x19: AL = 2; break;
    case 0x1A: dta_seg = R.ds; dta_off = DX; break;
    case 0x25: ww(0, AL * 4, DX); ww(0, AL * 4 + 2, R.ds); break;
    case 0x2A: {
        time_t tt = time(NULL); struct tm *tm = localtime(&tt);
        CX = tm->tm_year + 1900; DH = tm->tm_mon + 1; DL = tm->tm_mday; AL = tm->tm_wday;
        break;
    }
    case 0x2C: {
        time_t tt = time(NULL); struct tm *tm = localtime(&tt);
        uint32_t ms = SDL_GetTicks();
        CH = tm->tm_hour; CL = tm->tm_min; DH = tm->tm_sec; DL = (ms / 10) % 100;
        break;
    }
    case 0x30: AX = 0x0005; BX = 0; CX = 0; break;
    case 0x33: DL = 0; break;
    case 0x35: BX = rw(0, AL * 4); R.es = rw(0, AL * 4 + 2); break;
    case 0x3C: case 0x3D: {
        char name[128];
        read_asciiz(R.ds, DX, name, sizeof name);
        int h = 5;
        while (h < MAX_FILES && files[h]) h++;
        if (h == MAX_FILES) { dos_error(4); break; }
        FILE *f = AH == 0x3C ? platform_open(name, "w+b") : platform_open(name, (AL & 3) ? "r+b" : "rb");
        if (!f && AH == 0x3D) f = platform_open(name, "rb");
        if (!f) { dos_error(2); break; }
        files[h] = f;
        AX = h;
        break;
    }
    case 0x3E:
        if (BX < MAX_FILES && files[BX]) { fclose(files[BX]); files[BX] = NULL; }
        break;
    case 0x3F: {
        if (BX >= MAX_FILES || !files[BX]) { if (BX == 0) { AX = 0; break; } dos_error(6); break; }
        uint8_t buf[0x10000];
        size_t n = fread(buf, 1, CX, files[BX]);
        for (size_t i = 0; i < n; i++) wb(R.ds, DX + i, buf[i]);
        AX = (uint16_t)n;
        break;
    }
    case 0x40: {
        if (BX == 1 || BX == 2) {
            for (uint16_t i = 0; i < CX; i++) fputc(rb(R.ds, DX + i), BX == 1 ? stdout : stderr);
            AX = CX;
            break;
        }
        if (BX >= MAX_FILES || !files[BX]) { dos_error(6); break; }
        uint8_t buf[0x10000];
        for (uint16_t i = 0; i < CX; i++) buf[i] = rb(R.ds, DX + i);
        AX = (uint16_t)fwrite(buf, 1, CX, files[BX]);
        if (CX == 0) platform_truncate(files[BX]);
        break;
    }
    case 0x41: {
        char name[128];
        read_asciiz(R.ds, DX, name, sizeof name);
        platform_delete(name);
        break;
    }
    case 0x42: {
        if (BX >= MAX_FILES || !files[BX]) { dos_error(6); break; }
        long off = (long)(int32_t)((uint32_t)CX << 16 | DX);
        fseek(files[BX], off, AL == 0 ? SEEK_SET : AL == 1 ? SEEK_CUR : SEEK_END);
        long pos = ftell(files[BX]);
        AX = (uint16_t)pos; DX = (uint16_t)(pos >> 16);
        break;
    }
    case 0x44:
        if (AL == 0) DX = BX <= 2 ? 0x80D3 : 0x0002;
        else if (AL == 1) ;
        else dos_error(1);
        break;
    case 0x47: wb(R.ds, SI, 0); break;
    case 0x48:
        if (dos_free_seg + BX > MEM_TOP) { BX = MEM_TOP - dos_free_seg; dos_error(8); break; }
        AX = dos_free_seg; dos_free_seg += BX;
        break;
    case 0x49: break;
    case 0x4A:
        if (R.es == PSPSEG) dos_free_seg = PSPSEG + BX;
        break;
    case 0x4C:
        platform_quit(AL);
        break;
    case 0x62: BX = PSPSEG; break;
    default:
        fprintf(stderr, "galaga94: unhandled int 21h ah=%02X\n", AH);
        dos_error(1);
        break;
    }
}

/* ------------------------------------------------------------ loader */
static void load_exe(const uint8_t *raw, size_t len)
{
    const uint16_t *h = (const uint16_t *)raw;
    size_t hdr = h[4] * 16u;
    size_t img = (h[2] - (h[1] ? 1 : 0)) * 512u + (h[1] ? h[1] : 0) - hdr;
    if (img > len - hdr) img = len - hdr;
    memcpy(M + LOADSEG * 16, raw + hdr, img);
    for (int i = 0; i < h[3]; i++) {
        uint16_t off = *(const uint16_t *)(raw + h[12] + 4 * i);
        uint16_t seg = *(const uint16_t *)(raw + h[12] + 4 * i + 2);
        uint32_t a = (uint32_t)(seg + LOADSEG) * 16 + off;
        uint16_t v = M[a] | M[a + 1] << 8;
        v += LOADSEG;
        M[a] = (uint8_t)v; M[a + 1] = v >> 8;
    }
    /* program memory: image + minalloc, then DOS's free pool */
    dos_free_seg = MEM_TOP;

    /* IVT: every vector points at its BIOS stub F000:n */
    for (int n = 0; n < 256; n++) { ww(0, n * 4, n); ww(0, n * 4 + 2, BIOS_SEG); }
    /* BIOS data area */
    M[0x410] = 0x21; M[0x411] = 0x00;
    M[0x413] = 640 & 0xFF; M[0x414] = 640 >> 8;
    M[0x449] = 3; M[0x44A] = 80;
    M[0x463] = 0xD4; M[0x464] = 0x03;
    M[0x484] = 24; M[0x485] = 16; M[0x487] = 0x60;

    /* environment and PSP */
    static const char env[] = "PATH=C:\\\0\0\1\0C:\\GALAGA.EXE";
    memcpy(M + ENVSEG * 16, env, sizeof env);
    uint8_t *psp = M + PSPSEG * 16;
    psp[0] = 0xCD; psp[1] = 0x20;
    psp[2] = MEM_TOP & 0xFF; psp[3] = MEM_TOP >> 8;
    psp[0x2C] = ENVSEG & 0xFF; psp[0x2D] = ENVSEG >> 8;
    psp[0x80] = 0; psp[0x81] = 0x0D;

    memset(&R, 0, sizeof R);
    R.ds = R.es = PSPSEG;
    R.ss = exe_ss + LOADSEG; SP = exe_sp;
    R.cs = entry_cs + LOADSEG; R.ip = entry_ip;
    IF = 1;
    dta_seg = PSPSEG; dta_off = 0x80;
}

uint8_t kb_port_latch(void) { return kb_latch; }

int main(int argc, char **argv)
{
    platform_init(argc, argv);
    perf_freq = SDL_GetPerformanceFrequency();
    t0 = SDL_GetPerformanceCounter();
    next_tick_ms = PIT_MS;

    size_t len;
    uint8_t *exe = platform_load_file("GALAGA.EXE", &len);
    if (!exe) fatal("GALAGA.EXE not found next to the port (looked in %s)", platform_data_dir());
    load_exe(exe, len);
    free(exe);
    run_loop();
    platform_quit(0);
    return 0;
}
