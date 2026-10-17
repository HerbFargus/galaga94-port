# Galaga '94 — native port

A native Windows and Linux port of **Galaga '94 v2.1** (1994), the DOS shoot-'em-up by Arnold Pichler / APSS Austria. The game was released as *MailWare*: free to play and share, with a request to write to the author.

The port runs the original game code, not a remake. `GALAGA.EXE` (Turbo Pascal 6, 16-bit real mode) is **statically recompiled to C**: every x86 instruction becomes a line of C. That C runs on a small machine layer, and SDL2 stands in for the 1994 PC hardware:

| 1994 hardware | Port |
|---|---|
| VGA mode 13h / Mode X (planar, page flipping, 70 Hz retrace) | `src/vga.c` → SDL texture, 4:3, resizable, Alt+Enter for fullscreen |
| Keyboard interrupt (INT 9, port 60h scancodes) | SDL keys → XT scancodes; a game controller works alongside the keys |
| BIOS timer tick (INT 8 → INT 1Ch, 18.2 Hz) | real-time PIT |
| Sound Blaster through `FX_SB.DRV` (CT-VOICE) | `.VOC` decoding through SDL audio |
| PC speaker | square wave |
| DOS files | the `data/` folder; high scores go to the user's save folder |

Gameplay, speed and timing are the original's. The game paces itself on the 70 Hz VGA retrace, so it plays like it did on a fast 1994 PC.

## Playing

Run `galaga94` (`galaga94.exe` on Windows) next to the `data/` folder. `-f` starts fullscreen.

| | Keyboard | Controller |
|---|---|---|
| Move | ← → | D-pad / left stick |
| Fire | Right Shift (also Space or Left Ctrl) | A / right trigger |
| Megabomb | Enter | B |
| Start / continue | ↑ | D-pad up |
| New game (title) | F1 | Start |
| Difficulty (title) | F2 | Y |
| Back to title | Esc | Back |
| Quit (title) | F10 | |

High scores are saved to `%APPDATA%\APSS\Galaga94\` on Windows and `~/.local/share/APSS/Galaga94/` on Linux.

## Building

`src/game.c` is generated, and it's checked in so a build only needs a C compiler and SDL2.

- **Windows:** MSYS2 `mingw32` gcc plus the SDL2 VC dev package. Run `./build.sh` (set `SDL=` to the SDL2 folder; `CONSOLE=1` gives a console build).
- **Linux:** `sudo apt install libsdl2-dev`, then `./build.sh`.

To regenerate `src/game.c` from `data/GALAGA.EXE` (needs Python and `capstone`), run `REGEN=1 ./build.sh`.

Debug options:
- `CFLAGS=-DTRACE` logs every routine the first time it runs, plus frame rate, keys and sounds.
- `GALAGA_INPUT="ms:scancode,..."` and `GALAGA_SHOT="ms:file.bmp,..."` script keypresses and screenshots.
- `GALAGA_QUIT_MS` exits after a set time.

## Layout

- `tools/recomp.py`: the recompiler. It descends from the entry point, every procedure prologue and the hand-written asm routines, then sweeps the gaps.
- `tools/disasm.py`: listing for reading the code (`out/galaga.asm`).
- `src/cpu.h`: registers, memory and 286 flag semantics.
- `src/runtime.c`: loader, run loop, interrupts, DOS/BIOS.
- `src/vga.c`, `src/sound.c`, `src/platform.c`: the devices and the SDL front end.
- `data/`: the game files, unchanged from `reference-files/gal94_v2.zip` except `GALAGA.CFG`, which is set to Sound Blaster sound. `SETUP.EXE` isn't needed.

## Credits

Galaga '94 © 1994 Arnold Pichler, APSS Austria, distributed as MailWare. "Galaga" is a Namco trademark; this is the 1994 fan game, not Namco's.
