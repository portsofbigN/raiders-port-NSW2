# Third-party code

| Files | From | License |
|---|---|---|
| `recomp/*.py`, `runtime/cpu.h`, `runtime/simd.h`, `runtime/simd_fp.h`, `runtime/fma.c`, `tests/diff/` (except `v83.py`) | TotK Recomp (github.com/valentderah/totk-pc-recomp), itself adapted in part from LAS Recomp (github.com/peiikai/LASRecomp) | MIT, Copyright (c) 2026 Valiantsin Dzerakh; MIT, Copyright (c) 2026 Peiikai. Full text: `recomp/LICENSE-totk-pc-recomp` |

Eglantine (GPL) was read for behaviour only until 2026-11-02; see "License of this project" below.

Game files are never stored here. Everything derived from them goes to `work/` or `build/`.

## License of this project

Since 2026-11-02 the project may include GPL code (the owner approved it): the project as a whole is therefore
GPL-3.0-or-later. That only matters if it is ever distributed; game files and anything derived from them are never
part of it.

| Files | From | License |
|---|---|---|
| `third_party/eglantine/` (planned: shader_recompiler with the Ampere front end) | Eglantine / Eden (git.eden-emu.dev/Franseven/PokeDinner-a-Eglantine-Fork), derived from yuzu | GPL-3.0-or-later (`projetos/eglantine/LICENSE.txt`) |
| `projetos/deps/fmt` 12.1.0 | github.com/fmtlib/fmt | MIT |
| `projetos/deps/sirit` v1.0.5 | github.com/eden-emulator/sirit | BSD-3-Clause |
