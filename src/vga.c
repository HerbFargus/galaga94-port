/* VGA: mode 13h and its unchained "Mode X" form, as the game uses them.
 * Four 64 KB planes, the sequencer map mask, graphics-controller write
 * modes 0/1 with latches, CRTC start address and offset, and the DAC.
 */
#include <string.h>
#include "cpu.h"
#include "platform.h"

static uint8_t planes[4][0x10000];
static uint8_t latch[4];
static uint8_t seq_idx, seq[8];
static uint8_t gc_idx, gc[9];
static uint8_t crtc_idx, crtc[0x20];
static uint8_t attr_idx, attr[0x20], attr_ff;
static uint8_t dac[256][3];
static uint8_t dac_widx, dac_ridx, dac_wcomp, dac_rcomp;
static int graphics;
int vga_flipped;            /* start address written: a new page to show */
static uint32_t retrace_toggle;

int vga_is_graphics(void) { return graphics; }

void vga_set_mode(int mode)
{
    memset(seq, 0, sizeof seq);
    memset(gc, 0, sizeof gc);
    memset(crtc, 0, sizeof crtc);
    graphics = mode == 0x13;
    if (graphics) {
        memset(planes, 0, sizeof planes);
        seq[2] = 0x0F; seq[4] = 0x0E;
        gc[5] = 0x40; gc[6] = 0x05; gc[8] = 0xFF;
        crtc[0x13] = 40; crtc[0x14] = 0x40; crtc[0x17] = 0xA3; crtc[0x09] = 0x41;
        /* default 256-colour palette is replaced by the game straight away;
           start from a grey ramp so nothing is invisible before that */
        for (int i = 0; i < 256; i++) dac[i][0] = dac[i][1] = dac[i][2] = (uint8_t)(i >> 2);
    }
}

static inline uint8_t alu(int p, uint8_t v)
{
    uint8_t mask = gc[8];
    switch ((gc[3] >> 3) & 3) {
    case 1: v &= latch[p]; break;
    case 2: v |= latch[p]; break;
    case 3: v ^= latch[p]; break;
    }
    return (uint8_t)((v & mask) | (latch[p] & ~mask));
}

void vga_write(uint32_t off, uint8_t v)
{
    if (!graphics) return;
    uint8_t mm = seq[2] & 0x0F;
    if (seq[4] & 0x08) {                      /* chain-4: plane = low bits */
        int p = off & 3;
        if (mm & (1 << p)) planes[p][off >> 2] = v;
        return;
    }
    off &= 0xFFFF;
    switch (gc[5] & 3) {
    case 0: {
        int rot = gc[3] & 7;
        uint8_t d = rot ? (uint8_t)((v >> rot) | (v << (8 - rot))) : v;
        for (int p = 0; p < 4; p++) {
            if (!(mm & (1 << p))) continue;
            uint8_t b = d;
            if (gc[1] & (1 << p)) b = (gc[0] & (1 << p)) ? 0xFF : 0x00;
            planes[p][off] = alu(p, b);
        }
        break;
    }
    case 1:
        for (int p = 0; p < 4; p++)
            if (mm & (1 << p)) planes[p][off] = latch[p];
        break;
    case 2:
        for (int p = 0; p < 4; p++)
            if (mm & (1 << p)) planes[p][off] = alu(p, (v & (1 << p)) ? 0xFF : 0x00);
        break;
    case 3: {
        uint8_t save = gc[8];
        int rot = gc[3] & 7;
        uint8_t d = rot ? (uint8_t)((v >> rot) | (v << (8 - rot))) : v;
        gc[8] &= d;
        for (int p = 0; p < 4; p++)
            if (mm & (1 << p)) planes[p][off] = alu(p, (gc[0] & (1 << p)) ? 0xFF : 0x00);
        gc[8] = save;
        break;
    }
    }
}

uint8_t vga_read(uint32_t off)
{
    if (!graphics) return 0;
    if (seq[4] & 0x08) {
        int p = off & 3;
        off >>= 2;
        for (int i = 0; i < 4; i++) latch[i] = planes[i][off];
        return planes[p][off];
    }
    off &= 0xFFFF;
    for (int i = 0; i < 4; i++) latch[i] = planes[i][off];
    return planes[gc[4] & 3][off];
}

void vga_set_dac(int i, uint8_t r, uint8_t g, uint8_t b)
{
    dac[i][0] = r & 63; dac[i][1] = g & 63; dac[i][2] = b & 63;
}

void vga_get_dac(int i, uint8_t *rgb) { memcpy(rgb, dac[i], 3); }

uint8_t vga_port_in(uint16_t port)
{
    switch (port) {
    case 0x3C4: return seq_idx;
    case 0x3C5: return seq[seq_idx & 7];
    case 0x3CE: return gc_idx;
    case 0x3CF: return gc_idx < 9 ? gc[gc_idx] : 0;
    case 0x3D4: return crtc_idx;
    case 0x3D5: return crtc[crtc_idx & 0x1F];
    case 0x3C7: return 3;
    case 0x3C8: return dac_widx;
    case 0x3C9: {
        uint8_t v = dac[dac_ridx][dac_rcomp];
        if (++dac_rcomp == 3) { dac_rcomp = 0; dac_ridx++; }
        return v;
    }
    case 0x3DA: {
        /* vertical retrace for the first ~1 ms of each 70 Hz frame.  A
           retrace that began since the last read is always reported at
           least once, so a poll that comes late (the host was busy) can't
           miss a whole frame. */
        static long long vr_frame;
        static int vr_pending;
        attr_ff = 0;
        double t = now_ms();
        long long n = (long long)(t / (1000.0 / 70.086));
        double phase = t - (double)n * (1000.0 / 70.086);
        if (n != vr_frame) { vr_frame = n; vr_pending = 1; }
        int vr = vr_pending || phase < 1.0;
        if (vr_pending && phase >= 1.0) vr_pending = 0;
        retrace_toggle ^= 1;
        return (uint8_t)((vr ? 0x09 : 0) | (retrace_toggle & 1));
    }
    }
    return 0xFF;
}

void vga_port_out(uint16_t port, uint8_t v)
{
    switch (port) {
    case 0x3C0:
        if (!attr_ff) attr_idx = v & 0x1F; else attr[attr_idx] = v;
        attr_ff ^= 1;
        return;
    case 0x3C4: seq_idx = v; return;
    case 0x3C5: seq[seq_idx & 7] = v; return;
    case 0x3C7: dac_ridx = v; dac_rcomp = 0; return;
    case 0x3C8: dac_widx = v; dac_wcomp = 0; return;
    case 0x3C9:
        dac[dac_widx][dac_wcomp] = v & 63;
        if (++dac_wcomp == 3) { dac_wcomp = 0; dac_widx++; }
        return;
    case 0x3CE: gc_idx = v; return;
    case 0x3CF: if (gc_idx < 9) gc[gc_idx] = v; return;
    case 0x3D4: crtc_idx = v; return;
    case 0x3D5:
        crtc[crtc_idx & 0x1F] = v;
        if ((crtc_idx & 0x1F) == 0x0C || (crtc_idx & 0x1F) == 0x0D) vga_flipped = 1;
        return;
    }
}

void vga_render(uint32_t *px)
{
    uint32_t pal[256];
    for (int i = 0; i < 256; i++) {
        uint32_t r = dac[i][0], g = dac[i][1], b = dac[i][2];
        pal[i] = 0xFF000000u | ((r << 2 | r >> 4) << 16) | ((g << 2 | g >> 4) << 8) | (b << 2 | b >> 4);
    }
    if (!graphics) { for (int i = 0; i < 320 * 200; i++) px[i] = 0xFF000000u; return; }
    uint16_t start = (uint16_t)(crtc[0x0C] << 8 | crtc[0x0D]);
    int stride = crtc[0x13] * 2;
    int pan = attr[0x13] & 3;
    for (int y = 0; y < 200; y++) {
        uint16_t row = (uint16_t)(start + y * stride);
        for (int x = 0; x < 320; x++) {
            int xx = x + pan;
            px[y * 320 + x] = pal[planes[xx & 3][(uint16_t)(row + (xx >> 2))]];
        }
    }
}
