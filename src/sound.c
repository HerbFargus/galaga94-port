/* Sound: the CT-VOICE driver (FX_SB.DRV) the game far-calls, and the PC
 * speaker for SETUP's "speaker" option.
 *
 * CT-VOICE plays one Creative Voice stream at a time from memory.  The game
 * gives it a status word; the driver sets it to FFFFh while playing and 0
 * when done.  Function numbers come in BX.
 */
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include "cpu.h"
#include "platform.h"

#define RATE 44100

static SDL_AudioDeviceID dev;

/* current voice, converted to signed 16-bit at its own rate */
static int16_t *voice;
static int voice_len, voice_rate;
static double voice_pos;
static volatile int voice_done = 1;
static int playing;
static uint16_t status_seg, status_off;

/* speaker */
static volatile double spk_freq;
static volatile int spk_on;
static double spk_phase;

static void callback(void *ud, Uint8 *stream, int len)
{
    (void)ud;
    int16_t *out = (int16_t *)stream;
    int n = len / 2;
    for (int i = 0; i < n; i++) {
        int s = 0;
        if (!voice_done && voice) {
            int p = (int)voice_pos;
            if (p >= voice_len) voice_done = 1;
            else { s += voice[p]; voice_pos += (double)voice_rate / RATE; }
        }
        if (spk_on && spk_freq > 20 && spk_freq < 20000) {
            spk_phase += spk_freq / RATE;
            if (spk_phase >= 1) spk_phase -= 1;
            s += spk_phase < 0.5 ? 6000 : -6000;
        }
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        out[i] = (int16_t)s;
    }
}

void sound_init(void)
{
    SDL_AudioSpec want = {0}, have;
    want.freq = RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 512;
    want.callback = callback;
    dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (dev) SDL_PauseAudioDevice(dev, 0);
}

static void set_status(uint16_t v)
{
    if (status_seg || status_off) ww(status_seg, status_off, v);
}

/* Decode a VOC block chain starting at seg:off into 16-bit PCM. */
static void start_voice(uint16_t seg, uint16_t off)
{
    uint32_t a = lin(seg, off);
    if (!memcmp(M + a, "Creative Voice File", 19)) a += M[a + 20] | M[a + 21] << 8;
    int16_t *buf = NULL;
    int len = 0, cap = 0, rate = 11025;
    for (int guard = 0; guard < 4096; guard++) {
        uint8_t type = M[a];
        if (type == 0) break;
        uint32_t blen = M[a + 1] | M[a + 2] << 8 | M[a + 3] << 16;
        uint32_t d = a + 4;
        const uint8_t *src = NULL;
        uint32_t nsamp = 0;
        switch (type) {
        case 1: rate = 1000000 / (256 - M[d]); src = M + d + 2; nsamp = blen - 2; break;
        case 2: src = M + d; nsamp = blen; break;
        case 3: rate = 1000000 / (256 - M[d + 2]); nsamp = (M[d] | M[d + 1] << 8) + 1; break;
        case 9: rate = M[d] | M[d + 1] << 8 | M[d + 2] << 16; src = M + d + 12; nsamp = blen - 12; break;
        }
        if (nsamp) {
            if (len + (int)nsamp > cap) { cap = (len + nsamp) * 2; buf = realloc(buf, cap * sizeof *buf); }
            for (uint32_t i = 0; i < nsamp; i++) buf[len + i] = src ? (int16_t)((src[i] - 128) << 7) : 0;
            len += nsamp;
        }
        a = d + blen;
        if (a >= 0x100000) break;
    }
#ifdef TRACE
    fprintf(stderr, "%9.1f voice %d samples @ %d Hz\n", now_ms(), len, rate);
#endif
    SDL_LockAudioDevice(dev);
    free(voice);
    voice = buf; voice_len = len; voice_rate = rate; voice_pos = 0;
    voice_done = 0;
    SDL_UnlockAudioDevice(dev);
    playing = 1;
    set_status(0xFFFF);
}

static void stop_voice(void)
{
    SDL_LockAudioDevice(dev);
    voice_done = 1;
    SDL_UnlockAudioDevice(dev);
    playing = 0;
    set_status(0);
}

void ctvoice_call(void)
{
    switch (BX) {
    case 0: AX = 0x0202; break;                  /* driver version */
    case 1: case 2: AX = 0; break;               /* base port, IRQ */
    case 3: AX = 0; break;                       /* initialise: OK */
    case 4: break;                               /* speaker on/off */
    case 5: status_seg = R.es; status_off = DI; set_status(0); break;
    case 6: start_voice(R.es, DI); AX = 0; break;
    case 8: stop_voice(); break;
    case 9: stop_voice(); break;
    case 10: case 11: case 12: AX = 0; break;
    default: AX = 0; break;
    }
}

void ctvoice_poll(void)
{
    if (playing && voice_done) { playing = 0; set_status(0); }
}

void speaker_set(int count, uint8_t port61)
{
    spk_freq = count ? 1193182.0 / count : 0;
    spk_on = (port61 & 3) == 3;
}
