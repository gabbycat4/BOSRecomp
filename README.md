# Blades of Steel Recompiled

A native PC port of **Blades of Steel** (NES, USA), built with
[NESRecomp](https://github.com/mstan/nesrecomp). The game's own 6502 code is
recompiled to C and runs natively, cycle-accurate, at the speed of the real
console, with additions made to be **easy on the eyes**:

- **No sprite flicker.** The NES can only draw 8 sprites per line, so the
  players flash when they bunch up. Every sprite is now drawn every frame,
  and overlaps always draw the same way (lower on the screen in front).
- **Gentle puck-carrier highlight.** The game flashes the player with the
  puck bright white ten times a second. By default he gets a calm, steady
  glow instead (gold, or one of five other colours); a slow pulse, a faint
  flash, the original flash or no highlight at all are one menu row away.
- **Eased camera.** When the puck is shot down the ice, the game's camera
  jumps from 2 to 7 pixels a frame, stutters and stops dead. The camera now
  speeds up, rides through the stutters and brakes smoothly onto the game's
  own camera (never past it). Three strengths: Quick, Gentle, Softest.
- **120 / 180 pictures a second.** In-between pictures move the rink and the
  players a fraction of a pixel each, for high-refresh and FreeSync / G-Sync
  monitors. Players glide between the game's positions (Smooth: up to 1/60 s
  behind; or Low lag: no delay).
- **Edge crop** that hides the NES's scrolling garbage at the screen edges.
- An in-game **settings menu**, save states, two players with two controllers.

Everything above changes only the picture; the game itself runs exactly as
on a NES. Each feature can be switched off in the menu.

**You need your own ROM.** This repository contains no game data. You must
supply a dump of *Blades of Steel (USA)*; the program checks it is the right
one before it runs (the tested dump's SHA-1 is
`26ab09e19d3f78d749443c116f29ffdd4186231d`).

## Recommended settings

A fresh install starts with the eye-friendly setup already on, except
**Pictures per second**, which starts at 60 because only high-refresh
monitors benefit from more. In game, press **Escape** (or the right shoulder
button) for the menu:

| Menu | Setting | Recommended |
|---|---|---|
| Smooth motion | Smooth camera | **Softest** (Gentle or Quick if you like it closer to the game) |
| Smooth motion | Pictures per second | **120** or **180** on a 144 Hz+ FreeSync/G-Sync monitor (180 on 240 Hz); **60 (off)** on a 60 Hz monitor |
| Smooth motion | Player glide | **Smooth** (Low lag if you are sensitive to delay) |
| Picture | Player flashing | **Steady glow** |
| Picture | Highlight colour | **Gold** (try Sky blue or Lavender against an orange team) |
| Picture | No sprite flicker | **On** |
| Picture | Overlapping sprites | **Lower on screen in front** |
| Picture | Edge crop | **Standard** |
| Graphics | VSync | **Off** with FreeSync/G-Sync (on Linux/Wayland, also set the desktop's *Adaptive sync* to *Always*); **On** without |

Settings are saved in `config.ini` beside the program.

**Rewind starts off.** Its snapshot every few frames slows the frame after
it, which shows as hitches at 120/180 pictures a second. To use it anyway,
set `Enabled = 1` under `[Rewind]` in `config.ini` (with the game closed).

## Building

You need Git, CMake 3.20+, a C/C++ compiler, Python 3.11+ and SDL2.

**Linux** (Arch/CachyOS: `sudo pacman -S git cmake ninja sdl2 python`;
Debian/Ubuntu: `sudo apt install git cmake ninja-build build-essential libsdl2-dev python3`):

```sh
git clone --recursive https://github.com/gabbycat4/BOSRecomp.git
cd BOSRecomp
cp /path/to/your/rom.nes bos.nes
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/bosrecomp bos.nes
```

`--recursive` also downloads the engine and the menu (they are submodules).
If you cloned without it, run `git submodule update --init --recursive`.

The first configure takes a while: it builds the recompiler and translates
the game. Everything the game runs is recompiled (`seeds.txt` lists the code
addresses, collected by playing; it holds no game data).

## Controls

Keyboard and controllers are rebindable in the launcher's Controls page. A
second controller plays as player 2.

| | |
|---|---|
| Escape / right shoulder | Menu |
| F7 | Save states |
| F11 | Fullscreen |
| Tab | Fast forward |
| F12 | Screenshot |

## Command line

Every menu setting also has an option for one run (it does not change
`config.ini`): `--smooth 0-3`, `--present-rate N`, `--char-glide 0|1|2`,
`--flash-soften 0-4`, `--flash-colour 0-5`, `--no-sprite-limit 0|1`,
`--sprite-order 0|1`, `--overscan T,B,L,R`. Developer logs:
`--scroll-log FILE`, `--oam-log FILE`, and `CYC_PACING_LOG=FILE` in the
environment for picture timing.

## How it works

The NESRecomp engine runs the recompiled game cycle by cycle. This project
adds a presentation layer (`src/bos_extras.c`) through the engine's
`HOST_EXTRAS` interface:

- every sprite is drawn from the frame's sprite list over the background
  alone (the engine's background capture), so none are dropped;
- a player's flash is recognised as his sprites switching to the bright
  palette for 2 frames in 6; the highlight is drawn in his own colours;
- the eased camera follows the game's per-line scroll with limited
  acceleration; columns the game has already scrolled away are drawn from
  the nametables, which still hold them;
- sprites are matched frame to frame and grouped into players; a player
  glides only when his pieces agree on one movement;
- emulating a frame takes about half of its 16.6 ms on a typical desktop, so
  the engine runs the emulation on a second thread while it shows the
  in-between pictures, which are drawn only from a snapshot of the finished
  frame, and holds each frame's picture to an even slot.

`notes.txt` has the measurements that started it.

## Credits and licences

- [NESRecomp](https://github.com/mstan/nesrecomp) and
  [recomp-ui](https://github.com/mstan/recomp-ui) by Matthew Stanley; their
  own licences apply (see the `nesrecomp` and `recomp-ui` folders). The engine
  submodule points at a fork with window additions (in-between pictures and
  an emulation thread for them, display-sized windows, a VSync menu row, a
  per-game rewind default).
- This project's code is licensed under the PolyForm Noncommercial License
  1.0.0 (`LICENSE`): free to use, share and change; not to be sold.
- *Blades of Steel* is © Konami. This project is not affiliated with Konami
  and includes none of the game.
