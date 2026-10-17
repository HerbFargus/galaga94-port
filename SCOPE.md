# Galaga '94 port: scope

> **Outcome (2026-10-17):** the user chose a quick port over the bit-for-bit rewrite. It was done the same day by statically recompiling the EXE to C on an SDL2 machine layer (see README.md), so the milestones below were not followed.

Draft, 2026-10-17. Nothing built yet.

## What we have

`reference-files/gal94_v2.zip`: Galaga '94 v2.1 (title screen says 2.1b), by Arnold Pichler / APSS Austria, 1994. It was released as MailWare: free, and the author asked players to write to him.

| File | What it is |
|---|---|
| `GALAGA.EXE` (36 KB) | the game: 16-bit real-mode DOS MZ, **Turbo Pascal 6.0** ("Portions Copyright (c) 1983,90 Borland"), not packed, 281 relocations |
| `SETUP.EXE` (8 KB) | writes `GALAGA.CFG` (sound type, IRQ, port, DMA, joystick calibration). Also Turbo Pascal 6 |
| `FX_SB.DRV` | Creative's CT-VOICE driver (1992), which the game far-calls to play sound |
| 23 `.VOC` | the sound effects, Creative Voice 1.10 (8-bit PCM) |
| `BIGDATA.GAL` (118 KB) | an archive of about 70 named assets (`figh1.spr`, `expl1.spr`, `e1af.blk`, `gt1.spr` … `apss.spr`); the names are in the EXE, the format isn't decoded yet |
| `MONST*.SPR` (30 × 7200), `TILE*.SPR` (10 × 3000), `CHARS.BLK` | loose 8-bit sprites, tiles and the font |
| `GALAGA.PAL` | 256 × RGB palette, 6-bit VGA |
| `SCORES.DTA`, `GALAGA.CFG` | the top 10 table and the setup settings |

### Inside GALAGA.EXE

- **Code segments:** main program 28 KB, one small unit 1.5 KB (probably Crt), System RTL 4.2 KB. The rest is data.
- **Mostly hand-written assembler.** The main segment has only 10 Turbo Pascal `push bp; mov bp,sp` prologues but about 225 returns. Most routines are `asm` blocks that save registers with `push ax bx cx dx si di` themselves. Rough estimate: **about 200 routines**. That's under a fifth of one Bomberman stage.
- **Video:** mode 13h, then sequencer writes (port 3C4, 9 hits), so it's **Mode X** (unchained planar, page flipping). The palette goes through port 3C8, vsync through 3DA, and there are 30 A000h references.
- **Input:** its own **INT 9 keyboard handler** reads port 60h directly. Joystick goes through INT 15h, with keys or joystick but not both.
- **Timing:** it reprograms the **PIT** (out 43h), so the game runs off a custom timer tick.
- **Sound:** the CT-VOICE driver plays .VOC files on Sound Blaster, and there's a PC speaker fallback (port 61h).
- **76 `66h` prefixes**, meaning some 32-bit (386) instructions in the asm even though the readme says it runs on a 286. To check in M0.

## Goal (proposed, to confirm)

Do the same as Bomberman: a **faithful native port**. Every routine is rewritten in C and checked bit-for-bit against the original. It runs on SDL2 and builds as a Windows .exe and a native Linux binary. Gameplay, timing and RNG match the 1994 game, and the hardware layers become SDL.

### How this differs from Viper and Bomberman

Those games were Win32 PE files, so we could put our code in a `dinput.dll` and swap functions one at a time inside the running game. A 16-bit DOS exe can't load a DLL, so we flip it around:

- **The reference is an emulator we build ourselves.** A small 8086/286 interpreter (plus the 386 ops the game turns out to use) loads `GALAGA.EXE`, fakes DOS, the VGA, the PIT, the keyboard and the CT-VOICE driver, and runs the original **headless**: no DOSBox needed, scriptable input, frame dumps, deterministic.
- **Each rewritten routine is checked against it:** same input state → run the original in the emulator and the C version natively → compare registers and memory. That's the same approach as the Bomberman fuzz harness, just with the emulator holding the original instead of the live process.
- The game is small enough that "every routine" is realistic. No static recompiler needed.

## Milestones

**M0 Recon** (1 session)
- Disassemble the whole thing (capstone is installed; Ghidra is in `../ghidra`) and map out the routines, globals, the ISR install and the timer rate.
- Decode `BIGDATA.GAL`, `.SPR`, `.BLK`, `SCORES.DTA` and `GALAGA.CFG`. Write a viewer that renders every sprite to PNG.
- Pin down the Mode X layout (320×200 or 320×240, how many pages).

**M1 Harness** (1–2 sessions)
- Build the 16-bit emulator in C: the CPU, MZ loading and relocations, the DOS calls the game uses (file I/O, memory, get/set vector, exit), plus fake VGA, PIT, INT 9 and INT 15h, and a fake CT-VOICE driver that logs each "play voc" call.
- **Done when** the original reaches the title screen, plays a scripted game and dumps frames that match the real thing. A one-time DOSBox-X capture is the ground truth (DOSBox-X isn't installed yet).

**M2 Rewrite** (2–4 sessions)
- Rewrite the routines in C, from the leaves up: the System RTL bits it uses, the Mode X blitters, sprites, then the enemy and formation logic, collisions, extras, stage and bonus screens, scores, title and menus.
- Each routine gets a fuzz or diff test against the emulator. Whole-game replay tests compare frames and state at every tick.

**M3 Platform** (1 session)
- Turn each fake device into an SDL2 one:
  - Mode X becomes a 320×200 texture with integer scaling and aspect correction.
  - INT 9 becomes SDL scancodes, with keys and controller usable together.
  - The PIT tick becomes a fixed-step clock.
  - CT-VOICE becomes .VOC decoding mixed through SDL audio, and the speaker becomes a square wave.
- Fold SETUP's options into a config file and command-line flags.

**M4 Ship**
- Windows .exe, a Linux build (via WSL2, as with Bomberman), a GitHub repo and a release.

Rough total: **5–8 sessions**.

## Open questions

1. **Goal:** a faithful bit-for-bit port like Bomberman (above), or a quicker port that just plays right?
2. **Game data:** it's 1994 MailWare and the author asked for mail, not money. Do we ship the data files with the port, or ship only the engine and have players supply `gal94_v2.zip` (as with the other ports)?
3. **DOSBox-X:** install it for the one-time reference capture (a download), or trust the harness against a few screenshots the user takes?
4. **Repo:** a new `HerbFargus/galaga94-port` (public at release), or keep it local for now?
