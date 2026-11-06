# Splatoon Raiders PC Recomp (Native PC Port)

Native Windows x64 port of **Splatoon Raiders** (Nintendo Switch 2) built via static recompilation, targeting Vulkan 1.3+ with 60 FPS support, high resolutions (up to 4K), FSR/NIS upscaling, and controller gyro motion controls.

> [!IMPORTANT]
> **Legal Notice**: This repository contains **NO copyrighted game assets, ROMs, or proprietary Nintendo binaries**.
> To build and play, you **must provide your own legally obtained dump** of Splatoon Raiders (Update version 65536).

---

## 🎮 Features & Built-in Launcher

The port includes a built-in launcher and configuration window (`splatoon.ini`):
- **Display Modes**: Windowed or Borderless Fullscreen (resolutions from 720p up to 4K).
- **Console Modes**: TV mode (internal 1440p upscaled to 4K) or Portable/Handheld (720p for modest hardware).
- **Upscaling & Sharpening**: AMD FSR 1.0, NVIDIA Image Scaling (NIS), or Bilinear filtering.
- **Framerate Limits**: 30 FPS, 60 FPS (recommended), 120 FPS (experimental), or Uncapped.
- **Controls & Motion Aiming**:
  - Full XInput support (Xbox controllers) and PlayStation controllers (DualShock 4, DualSense with rumble & gyro aiming).
  - Customizable button mapping (Switch layout or Xbox/PlayStation layout).
  - Keyboard & mouse support.
- **Game Directory Selector**: Graphical folder picker under **Geral > Pasta do jogo** if the game folder is moved.

For in-game keybindings and details, see [COMO_JOGAR.md](COMO_JOGAR.md) (Português) or [HOW_TO_PLAY.md](HOW_TO_PLAY.md) (English).

---

## 🛠️ Build Requirements

- **Operating System**: Windows 10 or 11 (64-bit)
- **GPU**: Graphics card supporting **Vulkan 1.3** or newer (NVIDIA RTX / GTX 16-series, AMD RDNA, Intel Arc)
- **Compiler**: [LLVM Clang](https://releases.llvm.org/) (version 19+)
- **Build System**: [Ninja](https://ninja-build.org/) and [CMake](https://cmake.org/) (3.22+)
- **SDK**: [Vulkan SDK](https://vulkan.lunarg.com/) (1.3 or 1.4)
- **Python**: Python 3.10+ with `numpy` (`pip install numpy`)
- **Game Files**: An extracted Switch 2 dump of update 65536 containing:
  - `exefs/` (`main`, `sdk`, `subsdk0`, etc.)
  - `romfs/` (game data files)

---

## 🚀 Building from Source

### 1. Generate Recompiled C Code
From the repository root, run the static lifter on your extracted `exefs`:
```powershell
python recomp/gen.py --exefs "<path-to-game>/exefs" --out work/gen
```

### 2. Prelink Modules
Generate prelinked memory images:
```powershell
python tools/prelink.py --exefs "<path-to-game>/exefs" --gen work/gen --out work/prelinked
```

### 3. Build Shader Translator (CMake)
```powershell
cmake -B shader/build -S shader
cmake --build shader/build --config Release
```

### 4. Compile the Host Executable
Compile the native PC executable with Clang and Ninja:
```powershell
python tools/build.py --host --gen work/gen --out build --game-dir "<path-to-game>"
```

### 5. Package
Assemble the ready-to-play distribution:
```powershell
python tools/package.py
```
This produces `dist/Splatoon Raiders Recomp/SplatoonRaiders.exe`.

---

## 🛡️ Repository Integrity & Pre-commit Hook

To ensure no copyrighted files or keys are ever committed to the repository, install the pre-commit hook:
```bash
git config core.hooksPath .githooks
```
You can also run the verification script manually at any time:
```powershell
python tools/check_content.py
```

---

## 📜 Licenses & Credits

- Port and Switch 2 modifications: GPL-3.0-or-later.
- Recompilation engine and runtime architecture adapted from **TotK Recomp** (MIT, Copyright (c) 2026 Valiantsin Dzerakh) and **LAS Recomp** (MIT, Copyright (c) 2026 Peiikai).
- Shader recompiler and NVN2 research adapted from **Eglantine** (GPL-3.0-or-later).
- For complete third-party notices, see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
