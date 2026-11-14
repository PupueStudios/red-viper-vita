Red Viper for PS Vita
=====================

A Virtual Boy emulator for the PlayStation Vita, ported from [Red Viper](https://github.com/skyfloogle/red-viper) for the Nintendo 3DS.
It keeps Red Viper's V810 core and ARM dynamic recompiler, which runs natively on the Vita's Cortex-A9, and renders on the GPU through [vitaGL](https://github.com/Rinnegatamante/vitaGL). Colour modes are ported from [VBoy Color](https://github.com/yuk27/VBoyColor).

> **Personal build - do not distribute.** This tree combines Red Viper's Reality Boy-derived code, whose license forbids making money from it, with code from VBoy Color, which is GPL-3.0. GPL-3.0 does not allow extra restrictions like that, so the combined program may be built and used privately but not shared, unless the authors of VBoy Color or Reality Boy grant permission.

## Features

* V810 dynarec with busywait detection
* Red Viper's hardware VIP renderer, running on the GPU through vitaGL
* Colour modes from VBoy Color: Tint (11 colours), Multicolor (16 palettes, one colour per Virtual Boy shade) and Gradient (6 ramps), with per-game suggestions and per-game memory
* Integer 2x, fit or stretch scaling, sharp or smooth filtering, and an LED-grid shader
* Stereo output as left eye, right eye, red/cyan, red/blue or red/green anaglyph, or side by side
* Battery saves, plus 10 savestate slots with screenshot previews
* Game browser with built-in box art, remembers the last folder
* PS button opens the pause menu in-game

## Installing

* Install the VPK with VitaShell. The dynarec needs executable memory, so the app is built as **unsafe homebrew**: enable *Unsafe Homebrew* in HENkaku settings.
* vitaGL compiles its shaders on the console, so `libshacccg.suprx` must be extracted to `ur0:data/` with [ShaRKF00D](https://github.com/Rinnegatamante/ShaRKF00D) (needed by every vitaGL homebrew).
* Put `.vb` or `.zip` ROMs in `ux0:data/red-viper/roms` (any folder works; the browser remembers the last one).
* Box art for the official library is built in and matched by ROM name, ignoring region tags if there's no exact match. To add your own art, put `<rom name>.png` or `.jpg` in `ux0:data/red-viper/images`; your images take priority.
* Colour choices made while a game is running are remembered for that game in `ux0:data/red-viper/colors.ini`; games without one start in the scheme VBoy Color suggests for them.
* Battery saves go to `ux0:data/red-viper/saves`, savestates and their thumbnails to `ux0:data/red-viper/savestates`, settings to `ux0:data/red-viper/vita.ini`.

If you're looking for games, check out [Planet Virtual Boy's homebrew section](https://www.virtual-boy.com/homebrew/)!

## Controls

| | Standard layout | Dual D-Pad layout |
|---|---|---|
| Left D-Pad | D-Pad / left stick | D-Pad / left stick |
| Right D-Pad | Right stick | Triangle / Cross / Square / Circle |
| A / B | Circle / Cross (Square is also B) | Rear touch right / left half, or right stick right / left |
| L / R | L / R | L / R |
| Fast-forward | Hold or toggle Triangle | Pause menu toggle |
| Pause menu | PS button (press it again in the menu to go to the LiveArea), or tap the screen | Same |

## Building

Install [VitaSDK](https://vitasdk.org) with the `vitaGL`, `vitaShaRK`, `libmathneon`, `libpng`, `libjpeg`, `zlib` and `minizip` (with its `bzip2`, `lzma`, `zstd`, `ppmd` and `openssl` dependencies) packages (all available through `vdpm`). Fetch the inih submodule, then build:

```
git submodule update --init
make
```

This produces `build/red_viper.vpk`.

## Layout

* `source/common` - V810 core, dynarec, hardware VIP renderer logic, VSU sound
* `source/arm` - dynarec entry/exit and relocation table
* `source/vita` - Vita frontend: vitaGL renderer backend, colour engine, audio, input, JIT memory, menus
* `sce_sys` - LiveArea assets
* `assets` - logo and box art packed into the VPK

## License

Some of the code is distributed under the MIT License (check source files for that) but, since
this is a port of Reality Boy, here is (part of) the original readme:

```
This Reality Boy emulator is copyright (C) David Tucker 1997-2008, all rights
reserved.   You may use this code as long as you make no money from the use of
this code and you acknowledge the original author (Me).  I reserve the right to
dictate who can use this code and how (Just so you don't do something stupid
with it).
Most Importantly, this code is swap ware.  If you use It send along your new
program (with code) or some other interesting tidbits you wrote, that I might be
interested in.
This code is in beta, there are bugs!  I am not responsible for any damage
done to your computer, reputation, ego, dog, or family life due to the use of
this code.  All source is provided as is, I make no guaranties, and am not
responsible for anything you do with the code (legal or otherwise).
Virtual Boy is a trademark of Nintendo, and V810 is a trademark of NEC.  I am
in no way affiliated with either party and all information contained hear was
found freely through public domain sources.

Acknowledgments:
----------------

Frostgiant, Parasyte, and DogP (and the rest of people that have contributed
to the VB sceen in the last five years) - Their work on Red_Dragon has been a
real inspiration. Its amazing how far they have gone with so little to start
with.

Bob VanderClay - most of the original code is based off
of his VB disassembler.

Ben Haynor - Provided me with a much better understanding of
the VB internals.

Joseph LoCicero, Dave Shadoff - I stole the jump table ideas from their tg16
disassembler, thanks guys.

Neill Corlett - took many ideas (and some code)
from his Starscream CPU core

Kevin Banks - for donating a very nice pair of Frenzle 3D
viewers, and being an all around great guy.

Megan Tucker - For putting up with my tinkering all night, and resisting the
urge to toss all my video games out the window.

v810 is a trademark of NEC co.
Virtual Boy is a trade mark of Nintendo
Reality Boy is in no way affiliated with either of these parties
```

### Credits

* Red Viper (skyfloogle and contributors) - the emulator this port is based on.
* VBoy Color (yuk27) - colour schemes, per-game suggestions and the shade colorizer model (GPL-3.0).
* Rinnegatamante - vitaGL.
* Everyone mentioned in the license. Without Reality Boy and Red Dragon it wouldn't have been possible.
* smealum and contributors - ctrulib.
* Vappy, Team Fail, HtheB, hippy dave and kane159 on GBAtemp - early testing.
* benhoyt - inih.
* Myria - libkhax
* thunderstruck - CIA banner sound (taken from Fishbone).
* nop90 - Reality Boy backports and fixes.
* danielps - Initial 3DS port and V810 dynarec.
* Floogle - 3DS hardware renderer; many optimizations, bugfixes, and other improvements.
* djedditt - Enhanced 3D depth slider support.
* Morintari - splash screen artwork
* nevumx - Custom control scheme
* All GitHub contributors
