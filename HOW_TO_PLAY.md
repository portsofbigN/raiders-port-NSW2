# Splatoon Raiders (recomp): how to play

Open `SplatoonRaiders.exe` (in the ready-to-play folder `dist\Splatoon Raiders Recomp`, which can be moved anywhere).
The settings menu appears before the game, in categories (Display, Image, Performance, Controls, General). Hover an
option to see what it does, then click **PLAY!**.

**Requirements**: 64-bit Windows 10 or 11, a graphics card with Vulkan 1.3 and an up-to-date driver, and your own
copy of the game (update 65536, folders `exefs` and `romfs`). The first time, if the game is not found, choose its
folder under **General > Game folder**. The folder holds code derived from the game: it is for personal use with
your own copy; do not share it.

If something goes wrong, the last session's log is in `logs\splatoon.log`, next to the program.

## Settings (start menu)

| Option | What it does |
|---|---|
| Window size | 1280x720 up to 3840x2160. Your monitor's own size opens fullscreen; larger than the screen, the window shrinks to fit. The picture keeps 16:9 (black bars in a window of another shape) |
| Display mode | Windowed or borderless fullscreen |
| Console mode | **TV**: the game renders a 4K picture (best looking). **Handheld**: 1280x720 (lighter) |
| Image scaling | **AMD FSR 1**, **NVIDIA Image Scaling (NIS)** (sharper) or linear (softer) |
| Sharpness | How much FSR / NIS sharpen edges: none, medium, high (recommended) or maximum |
| Frame rate limit | 30, 60 (recommended), 120 (experimental) or unlimited |
| VSync | Off has less latency; on avoids tearing |
| Performance overlay | Off, FPS only, or full (FPS, frame times, CPU, GPU, memory) |
| Controller | Automatic (Xbox, PlayStation, keyboard) or keyboard only |
| Buttons | Switch positions (A on the right) or Xbox/PlayStation (A at the bottom) |
| Rumble | Off, weak or strong |
| Motion aiming | As on the Switch: move the DualSense / DualShock 4 to aim (gyro). Can be turned off |
| Vertical aim (right stick) | The game aims up and down with controller motion; the right stick does that here. Low, medium or high; turn it off if you disable motion controls in the game's options |
| Game folder | The game's folder with the `exefs` and `romfs` subfolders (update 65536). Click to choose |
| Language | Portuguese or English (automatic follows Windows); for the menu and the overlay |
| Show this menu at start | "No" opens the game directly. Hold **Shift** while opening to see the menu again |

Choices are saved in `splatoon.ini` next to the program (it can be edited by hand). Saves are in the `profile`
folder, also next to the program.

## Performance and resolution

- In **TV** mode the game always renders at 2560x1440 and upscales to 4K; the window size barely changes the cost.
  At 1440p (tested on an RTX 5060) it holds a steady 60 fps, the same as 720p.
- **First time in each area**: new effects need their shaders translated, which can cause a brief hitch. The
  translations are kept in `shader_cache.bin` next to the program, and from the second time on those hitches are
  gone. The file can be removed safely (the game makes a new one).

## Keys in game

| Key | Function |
|---|---|
| F1 | Overlay off → FPS → full |
| F11 or Alt+Enter | Fullscreen / window |

## Controllers

- **Xbox** (or any XInput controller, including through Steam): works directly.
- **PlayStation** (DualShock 4, DualSense, DualSense Edge): USB or Bluetooth, with rumble on both.
- Controllers can be connected and disconnected while the game runs.

Keyboard:

| Key | Switch button |
|---|---|
| W A S D | Left stick (move) |
| I J K L | Right stick (camera) |
| Enter / Space | A |
| Backspace / Esc | B |
| C | X |
| V | Y (puts the camera behind the character) |
| Q / E | L / R |
| 1 / 3 | ZL (squid) / ZR (shoot) |
| P / M | + / − |
| F / H | Stick clicks |
| Arrows | D-pad |
