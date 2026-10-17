/* Interfaces between the machine (runtime.c) and the devices behind it. */
#ifndef PLATFORM_H
#define PLATFORM_H
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

void fatal(const char *fmt, ...);
double now_ms(void);
void dos_int21(void);

/* platform.c: SDL window, input, files */
void platform_init(int argc, char **argv);
void platform_pump(void);
void platform_present(void);
void platform_quit(int code);
const char *platform_data_dir(void);
uint8_t *platform_load_file(const char *name, size_t *len);
FILE *platform_open(const char *name, const char *mode);
void platform_delete(const char *name);
void platform_truncate(FILE *f);
int kb_queue_size(void);
uint8_t kb_queue_pop(void);

/* vga.c */
void vga_set_mode(int mode);
uint8_t vga_port_in(uint16_t port);
void vga_port_out(uint16_t port, uint8_t v);
void vga_set_dac(int i, uint8_t r, uint8_t g, uint8_t b);
void vga_get_dac(int i, uint8_t *rgb);
void vga_render(uint32_t *pixels);   /* 320x200 ARGB */
int vga_is_graphics(void);
extern int vga_flipped;

/* sound.c: CT-VOICE driver and PC speaker */
void sound_init(void);
void ctvoice_call(void);
void ctvoice_poll(void);
void speaker_set(int pit_count, uint8_t port61);

#endif
