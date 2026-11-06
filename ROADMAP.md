# Splatoon Raiders recomp: findings and roadmap

Goal: native Windows x64 build of Splatoon Raiders (Switch 2), Vulkan renderer, 60 fps, no graphical bugs.
Game files stay in `C:\Program Files (x86)\nintendo_s_2games\splatoon_raiders` and are never copied into source.
Derived files go to `work\` (not source).

## Game modules (update 65536)

| Module | Text | ARM64 instructions | Notes |
|---|---|---|---|
| main | 63.3 MiB | 16.6 M | 1,338 symbols (mostly SDK imports), game code stripped |
| sdk | 8.5 MiB | 2.2 M | 28,484 exported `nn::*` symbols: the OS boundary |
| subsdk0 | 0.5 MiB | 0.14 M | |
| rtld | tiny | | dropped, host does linking (as in TotK Recomp) |

Instructions the TotK lifter does not handle yet (counts in main):
- Pointer authentication: `pacibsp` 134k, `retab` 111k, `autibsp` 75k. Treat as identity (Eglantine does the same).
- LSE atomics: `ldadd*` 23k, `ldset` 6k, `ldclr` 0.9k, `cas*` 8k, `swp*` 1.6k.
- `ldaprb`/`ldapr` 98k: plain acquire loads.

## Reference projects (`..\projetos`)

- `totk-pc-recomp`: the template. ARM64 -> C lifter (`recomp/`), Horizon host, NVN front end with Vulkan back end,
  Maxwell shader translator (Ryujinx). Switch 1 only.
- `LASRecomp`: origin of the lifter and runtime; smaller and easier to read.
- `eglantine`: Switch 2 emulator. `NX2_NOTES.md` documents Switch 2 boot (new SVCs, gsv/gpu/chat/camera sessions),
  NVN2 command streams (Turing MME64 macros, QMD 3.0, semaphore-gated command buffers) and
  `src/shader_recompiler/frontend/ampere/`: SM86 -> Maxwell IR lowering, which lets the TotK/Ryujinx pipeline
  translate Switch 2 shaders. It already boots Splatoon Raiders with uneven speed.

## Status

- Stage 1 done (2026-11-02): LLVM 21 Clang (`C:\Program Files\LLVM\bin`), CMake 4.4, Ninja, Vulkan SDK 1.4.363,
  VS Build Tools (MSVC 14.44). RTX 5060 reports Vulkan 1.4.
- Stage 2 done (2026-11-02): `recomp/` lifts every word of main, sdk, subsdk0 and rtld
  (`tools/coverage.py`, results in `work/coverage_after.txt`). Added: RETAA/RETAB, BRA*/BLRA*, PAC*/AUT*/XPAC*
  (identity), PACGA (`pacga64` in `runtime/cpu.h`), LDAPR{B,H}, FABS/FNEG .4H/.8H.
  Differential tests against Unicorn, all 0 failures, with Windows LLVM Clang (`CC` set to it):
  `tests/diff/lift.py` (179,940 cases sampled from the game), `simd.py` (159,976), `atomic.py` (12,672),
  `v83.py` (new: each ARMv8.3 word against its ARMv8.0 equivalent, 2,104 cases).
  Not checked against an oracle: PACGA and FP16 FABS/FNEG (Unicorn has neither).

- Stage 3 done (2026-11-02): `recomp/gen.py` emits 259,913 functions (main 212,406, sdk 45,972, subsdk0 1,535),
  519 chunks, 1.27 GB of C in `work/gen` (78 s). `tools/prelink.py` links the modules offline (450k relocations,
  0 unresolved imports) into `work/prelinked/*.img` + `boot.h`. `tools/build.py` compiles with LLVM Clang + Ninja
  into `build/splatoon.exe` (263 MB). `runtime/rt_min.c`: maps modules, flat dispatch table, host callbacks.
  Switch 2 modules have no crt0 at offset 0 (header: version 1, MOD0 offset, ...); the host does rtld's hand-off
  instead: `nn::init::Start(handle, 0, loader_ready, module_inits, module_finis)`, as TotK Recomp does.
  Run: `build\splatoon.exe work\prelinked` -> reaches svc 0x29 GetInfo(11, RandomEntropy) at sdk+0x1a4554.
  Open: 29,675 `br` in main go through dispatch (tail calls are expected there; any that jump into the middle of
  a function will show up as "dispatch miss" and need the jump-table pass extended).

- Stage 4 in progress (2026-11-02): `host/` is TotK Recomp's runtime; `tools/build.py --host` builds its core
  (kernel, memory, loader, dispatch, SVCs, IPC, cfg, platform) with `sp_main.c` (entry + rtld hand-off),
  `sp_stubs.c` (unported subsystems) and the new Switch 2 parts into `build/splatoon_host.exe`. Run it with no
  arguments; `TOTK_TRACE=svc,ipc` traces. Added for Switch 2:
  - svc GetInfo 29-33; program id 0400380020056000; application pool 9.55 GiB (`memory.c` DEFAULT_POOL).
  - MapMemory from a module's .bss (static stacks); shared memory objects + svc 0x13/0x14 (`nx2_shmem.c`).
  - CMIF domains in `ipc.c` (ConvertToDomain, CopyFromCurrentDomain, domain requests/close).
  - `nx2_services.c`: lm, appletOE (+ Switch 2 cmds 10/40/60/320/321, message queue with FocusStateChanged),
    aoc:u, pctl*, time:u (+ shared memory); any other service opens as an empty-success session, logged
    once per command (`TOTK_NX2_STRICT=1` makes unknown services fatal).
  - Filesystem: TotK's `fs_host.c` (nn::fs overrides, saves under `build/profile`) with `romfs_dir.c`, a new
    RomFS backend that serves the extracted folder `updates/65536/romfs` (a complete merged tree: unchanged
    files are hard links to the base, 135 files changed); indexes 46,350 files in 0.03 s. Also the Switch 2
    `MountRom(const char*)` and `EnableGlobalFileDataCache`.
  - TotK's `ov_system.c` (accounts, time, oe queries, prepo, nfp, err, aoc, pctl, pl fonts) and `amiibo.c`,
    except its nn::oe::Initialize override: the SDK's own applet set-up runs over nx2_services.c, because the
    Switch 2-only nn::oe functions need the applet proxy it creates.
  Reached (stage 4 target): SDK start-up -> nninitStartup -> nnMain -> RomFS mounted as `content:` -> display
  version -> main+0x31b8448 calls nv::InitializeGraphics, which boots the Switch 2 graphics system
  (nn::gs::sdk::BootGraphicsSystem -> gsv, nvsrv) and aborts there, as those services are empty.
  Graphics boundary in main's imports: nv::SetGraphicsAllocator, nv::InitializeGraphics, nvnBootstrapLoader,
  nvnPrivate0, 13 nn::vi functions. TotK overrides the same entry points (`ov_gfx.c`), so the Switch 2 graphics
  services never need emulating: stage 5 replaces them and implements NVN2 on Vulkan.
- Stage 5 started (2026-11-02): `host/nx2_gfx.c` replaces nv::SetGraphicsAllocator / InitializeGraphics and 13
  nn::vi functions (the graphics system is never booted) and serves NVN2 as a tracer: 2048 proc slots, each
  named when the game first asks for it; every call is logged (NX2_NVN_TRACE=n) and returns 0 (true for
  *Initialize). `hid` CreateAppletResource + 0x40000 shared memory added.
  Reached: nvnBootstrapLoader -> the game's proc loader asks for 731 NVN2 procs (`work/nvn2_procs.txt`) ->
  DeviceBuilder SetDefaults/SetFlags -> DeviceInitialize -> DeviceGetInteger(0), (1) -> hid init -> the game's
  renderer set-up (main+0x31bb800) reads through results the tracer left zero. Real NVN2 semantics are next.
  Finding: the real NVN2 driver is in `sdk` (exports nvnBootstrapLoader, nvnPrivate0; recompiled with the rest),
  so there are two routes (decision pending):
  A. NVN2 API on the host (TotK's route): implement the 731 procs on Vulkan; 276 names match TotK's NVN1 front
     end, the rest (ray tracing, mesh shaders, DLSS, new pool/queue options) are new. Layouts and enums of NVN2
     are not public; the recompiled driver in sdk can be called as an oracle to learn them. Fastest at run time.
  Decision (2026-11-02): route A, with the real driver as an oracle.
  Design now in `host/nx2_gfx.c`: every proc is a slot that is either `host` (host_procs[]), `forward` (the
  driver's recompiled code in sdk, looked up through the sdk's own nvnBootstrapLoader / nvnDeviceGetProcAddress)
  or `stub`. `work/nvn2_procs.txt` lists how each of the 731 procs is served. Host procs read builders through
  the driver's forwarded getters (no builder layouts reverse-engineered); host objects live in host tables keyed
  by the guest object pointer.
  - Forwarding works for pure procs: builders, state objects, nvnSamplerInitialize, nvnDeviceGetInteger (the
    driver answers all 256 device properties from constants: `work/nvn2_device_integers.txt`; API 231.39).
  - Host so far: Device Initialize/Finalize; MemoryPool (10 procs; GPU address = CPU address); Texture and
    Sampler pools (registrations kept per id); Queue, Sync, Window as a null back end (submits and presents
    counted, fences signalled, window textures cycled).
  - Reached: device, first memory pool (4.1 MiB), sampler pool (4096), texture pool (131372), queue, then the
    first render target's TextureBuilder (3840x2160, format 0x25 = RGBA8 as in NVN1).
  - Textures (`host/nx2_tex.c`): TextureBuilder storage size/alignment/class and the Texture family (31 procs)
    on the host with TotK's `nvn_tex.c` layout math; the description is read through the driver's builder
    getters. NVN2 change found: nvnTextureBuilderGetPackagedTextureLayout fills a caller structure (second
    argument) instead of returning a pointer, so every getter gets a scratch buffer there. Window builder
    getters return values directly (3 textures). Write/ReadTexels are logged, not implemented yet.
  - Device fast-clear registration (ZBC) is host and only succeeds.
  - Kernel: NCORE 6 (Switch 2 cores 0-5), GetInfo core mask 0x3f; CreateThread and the main thread no longer
    assume three cores (nn::oe's GPU error handler thread runs on core 5).
  - Reached: 3 swap-chain textures 3840x2160 RGBA8, 16 memory pools (716 MiB, incl. the 300 MiB regions),
    3 queues, events, buffers (forwarded), default textures and samplers registered, input initialized, then
    the first command buffer: AddCommandMemory, BeginRecording, and DrawElements, where the forwarded driver
    code crashes (it needs its internal GPU state).
  - Shader translator (2026-11-02, after the owner allowed GPL code; the project is GPL-3.0-or-later now):
    Eglantine's `shader_recompiler` (Ampere SM86 -> Maxwell lowering, Maxwell front end, SPIR-V back end) is
    copied to `third_party/eglantine/` and builds standalone with `shader/CMakeLists.txt` (CMake + Ninja +
    LLVM clang++) into `shader/build/shader_recompiler.lib`. Dependencies: fmt 12.1.0, sirit v1.0.5
    (`projetos/deps`), Boost 1.91.0 headers (`projetos/deps/boost_dl`), SPIR-V headers from the Vulkan SDK.
    Eden's settings/logging/assert headers are replaced by small stand-ins in `third_party/eglantine/compat`
    (assertions throw instead of aborting); `enum_format.h` lets fmt 12 print enums.
    Still to take from Eglantine: the environment adapter (`video_core/shader_environment.cpp`: program end
    detection, LowerToMaxwell, Maxwell scheduling words, address map) to translate captured programs.
    The game has created no NVN program yet when it stops: its first draw comes before program set-up.
  - Command buffers (`host/nx2_cmd.c`): all 201 nvnCommandBuffer* procs are host. 25 lifecycle procs
    (memory, callbacks, Begin/EndRecording, strides) are implemented; every other command is appended to a
    host list as {proc slot, x0..x7}; EndRecording returns a handle id; QueueSubmitCommands looks the lists up
    (null back end: counted). Pointer arguments are not copied yet (the Vulkan replay will add that per command).
  - Programs and shader capture (`host/nx2_prog.c`): NVNshaderData is 16 bytes as in NVN1 ({code address,
    control pointer}); every program's stages are written to `work/shaders/<serial>-<stage>.code/.ctrl`
    (about 20 programs captured so far). Program procs are host (InitializeFromBuilder needs the driver's GPU state).
  - Object state in place: memory pools and textures keep their host record inside the guest object (magic +
    data), because the game copies these objects by value; an uninitialized pool reads as zero (the game probes
    pools embedded in resource files with GetBufferAddress and initializes them when it returns 0).
  - Texels: Write/ReadTexels[Strided] copy straight between the caller and the block-linear storage in the
    game's pool memory (`nx2_tex_deswizzle.c`, TotK's tex_deswizzle.c without its Vulkan header).
  - Crash reports end with the last 256 NVN2 calls (proc, arguments, return, caller): `nx2_ring_dump`.
  - Virtual pools: a pool without storage gets a reserved address range; nvnMemoryPoolMapVirtual uses NVN1's
    NVNmappingRequest layout ({physical pool, physical offset, virtual offset, size, storage class}); a request
    covering the virtual pool from offset 0 gives it the physical storage's address (the game maps a 300 MiB
    physical pool into a 300 MiB virtual one this way).
  - Buffers and events (`host/nx2_buf.c`) are host: the forwarded driver code read pool fields it never wrote,
    so the game's "GpuBuffer" heap (allocator main+0x7a2640) got a wrong base/size and its first allocation
    failed ("GpuBuffer allocation failed"). Records in place (NVNbuffer stride 0x50, NVNevent 0x40).
  - Debugging aids: NX2_FNTRACE=1 records every guest function entry in a per-thread ring (FN_ENTER hook) and
    the crash report prints the crashing thread's last 80; pool queries on uninitialized pools are logged.
  - Fonts (`host/nx2_font.c`): nn::pl::GetBuiltInFontData (Ounce and Nx) and RequestLoadNxFont on the host;
    every font type gets C:\Windows\Fonts\arial.ttf (NX2_FONT=<path>) until the player's firmware fonts can be
    used. Without it the game parsed garbage font data (main+0x810540).
  - Audio: TotK's audren.c, audio_mixer.c, audio_effects.c, audio_reverb.c; `aud:u` cmd 2 (Switch 2 audio
    entry) opens TotK's IAudioRendererManager; the renderer runs (48 kHz, REV15, 96 voices) but plays nothing:
    `host/compat_sdl/SDL3/SDL.h` makes SDL audio report failure ("mixing without sound"). Unknown audio
    commands (IAudioDevice 17, 19, 20) succeed and are logged once.
  - Offline user (`host/nx2_account.c`): network service account queries report unavailable; no profile image.
  - Time (`host/nx2_time.c`): device location "UTC", time zone rules accepted, ToCalendarTime with a rule uses
    TotK's conversion, network clock unavailable (its async task finishes at once).
  - Controller applet (`host/nx2_input.c`): nn::hid::ShowControllerSupport answered as 1 player, No. 1.
  - MILESTONE (2026-11-02): the game runs stably (5-minute test, ended by the time limit): 8,640 presents
    (about 618 fps with the null back end: no GPU work, no vsync), 4.2 million NVN2 commands recorded,
    20,749 shader files captured in `work/shaders/`. Nothing is drawn yet.
  - Vulkan presenter (`host/nx2_vk.c`, 2026-11-02): Win32 window (1280x720, resizable), Vulkan 1.3 device on
    the RTX 5060, FIFO (vsync) swapchain; each nvnQueuePresentTexture converts the window texture
    (3840x2160 RGBA8, block-linear) to linear rows and blits it, scaled, to the swapchain. Window textures come
    from the driver's nvnWindowBuilderGetTexture. Closing the window exits.
    Screenshots: NX2_SHOT=<file.png> NX2_SHOT_FRAME=<n> writes present n (downscaled 3x) and exits.
    NX2_TEST_PATTERN=1 writes a gradient into the window texture's tiled storage before each present: verified
    correct end to end (`work/shot_pattern.png`).
  - Record-time copies in `nx2_cmd.c` (Rec.aux): SetRenderTargets keeps its first colour target, ClearColor its
    colour. CPU replay of RGBA8 clears exists, but frames contain none.
  - What a frame contains now (first 60 frames, histogram at present #60): exactly one draw per frame with full
    state: BindProgram, BindVertexBuffer, BindUniformBuffer (+ UpdateUniformBuffer), BindTexture, blend /
    colour / depth / polygon / channel-mask / vertex states, SetViewport/Scissor/DepthRange, SetRenderTargets,
    DrawElements, Barrier, ReportCounter: a single textured screen (likely the boot logo or loading image).
    The presented frame is black because that draw is not executed yet.
  - Offline shader translator `shader/build/xlate_tool.exe <out dir> <file.code>...` (shader/xlate_tool.cpp):
    capture -> ProgramCaptureState -> LowerToMaxwell -> Maxwell code (SPH + scheduling words + address map)
    -> TranslateProgram -> EmitSPIRV. Native instructions start at +0x80 (the Maxwell SPH struct is 0x50).
    Whole capture set (2026-11-03): 6,940 of 6,947 stages translate (99.9%), and every SPIR-V module passes
    `spirv-val --target-env vulkan1.3 --uniform-buffer-standard-layout --scalar-block-layout` (the Vulkan device
    must enable uniformBufferStandardLayout and scalarBlockLayout, as Eglantine does). Changes that got there:
    - LowerToMaxwell(native, "full"): HMMA matrix multiply expanded (this title's upscaler needs it).
    - Compute programs (no SPH; header word not a graphics type) are read from offset 0 and translated as
      compute; offline placeholders 48 KiB shared memory, 64x1x1 workgroup (the game's dispatch gives the real
      values).
    - `third_party/eglantine/.../lower_to_maxwell.h` (our change, commented): TXQ masks 4 and 8 pass through
      like 1 and 2 (226 stages queried only the depth/layer count).
    Left: 4 indirect branches the translator cannot track, 2 "uniform comparison mode" (op 0x88c), 1 op 0x8a5.
    Translating everything takes about 3 s with 10 processes (`xargs -P 10 -n 250`).
  - FIRST REAL FRAMES ON VULKAN (2026-11-03). What it took:
    - Command memory "used" (`nx2_cmd.c`): the game reads nvnCommandBufferGetCommandMemoryUsed after
      EndRecording and drops lists that used none (main+0x322d260). Reporting 0 made it submit only the final
      copy each frame: the cause of the endless black screen. Now 16 bytes per recorded command (4 of control).
    - CallCommands / CopyCommands are replayed (nested lists); record-time copies (blobs) for every pointer
      argument (uniform updates, render targets, viewports, ...); s0-s3 kept for float arguments.
    - Vertex attribute/stream and blend/color/channel-mask/depth/polygon states are decoded at record time with
      the driver's own getters (cached by raw bytes): NVN2 packs them its own way.
    - nvnProgramSetShadersExt entries are 24 bytes {code, control, 8 bytes}.
    - Constant buffer c1 = the stage's own constant data (jump tables, compiler constants) at code +
      control[0x40] .. control[0x44] (control section magic 0x98761234). Without it the translator and the GPU
      followed indirect branches to address 0: GPU hang (device lost). Mapping for this title: uniform buffer
      binding i -> c(3 + i), texture binding i -> handle at c2[0x20 + 8 i] (texture id bits 0-19, sampler above).
    - In-game translator `shader/nx2_xlate.cpp` (C API `nx2_xlate.h`, static lib linked into the host): viewport
      transform on (ReadViewportTransformState 1), no native NDC (depth -1..1 converted in the shader:
      NVN's default depth mode), device float controls passed in, render area push constant.
    - `host/nx2_vkdraw.c`: Vulkan executor. One command buffer per QueueSubmitCommands (submitted and waited),
      128 MiB staging ring, images per NVN texture (GENERAL layout, typed views for cube/array reads, ASTC decoded
      on the CPU by TotK's decoder `nx2_astc.c`), pipelines keyed by program/vertex input/targets/state,
      dynamic rendering, depth targets and clears; winding reversed (NVN y up). `nx2_vkfmt.c`: TotK's format
      table. Window textures are presented from their GPU images (`nx2_present_image`).
    - Debug aids: NX2_SHOT_EVERY=n (shots/shot_*.png), NX2_SHOT_ALL=1 (every GPU image as gpu_*.png),
      NX2_DUMP_BEFORE=<program>, NX2_DEBUG_PROG=<serial> (+ NX2_DEBUG_VS/FS=<spv> to swap a stage),
      NX2_SYNC_DRAWS=1 (find a GPU hang), NX2_SKIP_PROGRAMS, NX2_CLEAR_DEBUG=1 (clears magenta),
      NX2_DUMP_SUBMITS / NX2_DUMP_SKIP / NX2_DUMP_ENDED (command lists), validation layers via
      VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation.
    - Result: ~55 draws a frame executed, no validation errors in draws; from about frame 1950 the loading
      screen's spinning icon and dim text appear (`work/shots/`). Missing: storage buffers/images (draws using
      them are skipped), compute dispatches, NVN samplers (all linear clamp), copies/blits, the swapchain
      semaphore reuse warning, frame overlap (each submit waits).
  - MENU RENDERS CORRECTLY (2026-11-03, later): intro cloud scene, then "Select a difficulty" (Tourist / Raider /
    Survivalist) with textured buttons and full text (`work/shots/`). What it took:
    - Storage buffers: graphics {u64 address, u32 size} tables at c0[0x110 + 16 i] (vertex) / c0[0x510 + 16 i]
      (fragment); compute programs have their own compact c0 layout from 0x20 (storage buffers 16 bytes, then
      textures, then images, 8 bytes each, in binding order), so a compute descriptor's NVN index is its rank in
      its kind. c0 is synthesized per draw/dispatch with the addresses and handles. Storage buffers are staging
      copies; the ones a shader writes are copied back to guest memory when the submit completes.
    - Compute: DispatchCompute executes (workgroup size from the control section +0x58/+0x5c/+0x60), storage
      images through UNORM views of sRGB images (mutable format), image handles = image ids in the texture pool.
    - CPU-written textures are uploaded again when they change: WriteTexels marks them, and storage up to 64 MiB
      is hashed once a frame (sampled above 1 MiB). The glyph cache (2048x10240 R8, linear) needs it.
    - NVNtextureView of each RegisterTexture/RegisterImage decoded with the driver's getters (swizzle, format,
      target, levels, layers): BC4 masks are registered with swizzle RRRR (alpha = red) and the font with
      (1, 1, 1, R); without it they drew as solid blocks.
    - Samplers decoded at RegisterSampler/RegisterSamplerBuilder (filters, wrap, LOD range and bias, compare,
      anisotropy, border); one VkSampler per distinct state.
    - Lesson: a host proc that calls driver getters must read its own arguments first (the calls clobber x0-x18);
      reading them after made every texture registration land in the wrong slot.
    - Validation clean (also the swapchain semaphore: one per image).
    Open: the intro clouds show streaks (cause unknown: 3D noise written by compute, or sampling), the cursor
    icon had stray lines before the view fix (recheck), texture views' format/layer overrides are not applied yet
    (swizzle and levels are), texel buffers unsupported, frame pacing (runs at the display rate, ~156 fps).
  - CONTROLLER INPUT (2026-11-03): `host/nx2_hid.c` (replaces nx2_input.c in the build; adapted from TotK's
    ov_input.c): one full-key controller as player 1 fed by the keyboard (only while the game window has focus)
    and XInput gamepads (Nintendo button positions). Keys: W A S D left stick, I J K L right stick, arrows D-pad,
    Enter/Space A, Backspace/Esc B, C X, V Y, Q L, E R, 1 ZL, 3 ZR, P Plus, M Minus, F/H stick clicks.
    Vibration devices and the six-axis sensor are present (vibration not played, sensor at rest): with a
    controller connected the game initializes them, and the hid service path crashed (null read in
    detail::InitializeVibrationDevice). NX2_INPUT_SCRIPT="from-to:Button+Button;..." presses buttons on given
    presents (tests). Verified by script: Right moves to Survivalist, A opens "Start on this difficulty?", Right +
    A confirms; the game saves ("Game saved!", build/profile/save/.../save.dat) and starts the story intro
    (subtitles over black). The difficulty menu still appears at the next boot (the save does not skip it).
    The intro is not a movie: the romfs holds only 16 webm files, all UI gadget tutorials (UI/Movie); the story
    opening is an in-engine scene with dialogue (Big Man: "Where'd our mechanic get to?"), drawn over black so far.
    Advancing the dialogue with A reaches character creation ("How do you wanna look and sound?", frame ~9900):
    four 3D character previews drawn (with magenta dotted-line artifacts), the right half black. The 3D scene does
    render (work/dump9900: a dark tunnel with a light in a 1280x720 RGBA16F target), but does not reach the
    screen: the composition / post-processing chain is the next graphics task.
    Open: the Yes/No labels of the confirm dialog draw as black boxes; movies (VP9); rumble; motion.
  - MENU MATCHES THE REAL GAME (2026-11-03, checked against a screenshot the user gave). Fixes:
    - Switch 2 block-linear GOBs swap offset bits 5 and 6 of the Switch 1 layout (Eglantine NativeGobOffset,
      notes row 69). `nx2_tex_deswizzle.c` gob_off applies it (and tex_bl_copy's per-row segment offsets). Without
      it every texture had hatched/dotted edges (block-sized pieces swapped where content changes).
    - Render target views: SetRenderTargets' views are decoded at record time (nvnTextureViewGetFormat); the
      window textures (RGBA8) are written through RGBA8_SRGB views, so the hardware encodes linear -> sRGB.
      Colour images are created with MUTABLE_FORMAT and drawn through a view in that format. Without it every
      dark tone was crushed (black menu background, dark scenes).
    - Also: conditional rendering (SetRenderEnable / SetRenderEnableConditional comparing the two counter reports
      at the address), CopyTextureToTexture (blit / copy, regions copied at record time), BindTextures,
      ClearColor honouring the scissor and channel mask, image variants per texture object (a 2560x1440 target
      alternates colour/depth every frame: up to 4 kept instead of re-created), CPU-texture change checks only
      for uncompressed non-depth textures and every 16 frames.
    - Debug: NX2_TRACE_FRAME=<n> (one line per clear/draw/dispatch/copy with targets and inputs),
      NX2_DUMP_TEX=all|<object> (BC4 textures as PNG plus their raw storage).
    Open: 3D story scenes run at ~5 fps (each draw copies its vertex/uniform/storage ranges into the staging ring
    and every submit waits): import the memory pools as Vulkan buffers (VK_EXT_external_memory_host) next;
    NVN2 Push{Graphics,Compute}Constants (cb, ?, offset, size, data in x4) not executed yet.
  - STORY SCENES RENDER (2026-11-03; the storm intro with clouds, the plane, lights and subtitles). Fixes:
    - Compute c0 layout counted back from its end (ctrl+0xb8 >> 16): images, textures, then storage buffers; the
      push constants ((ctrl+0xa9) words) sit before the storage buffers in a 16-byte aligned block (#16: c0[0x10]).
    - #16, the final 2560x1440 -> 3840x2160 step, is a neural upscaler (fp16 weights in a 0x3000-byte buffer, 48 KiB
      of shared memory, HMMA lowered to shuffles): its output is one flat colour, so it is replaced by a linear
      blit (NX2_NN_UPSCALE=1 runs the shader). Translating it properly is open.
    - Images are keyed by memory + description, not by texture object: the game's transient render targets
      re-initialize the same objects every frame with other sizes (4 variants per object thrashed and lost the
      scene). Up to 16 descriptions per address, least recently used retired.
    - Render target views' layer and level (nvnTextureViewGetLayers/Levels at record time): the colour-grading LUT
      (8x8x8 RGBA16F) is drawn as 8 colour targets, one view per slice (3D images are 2D-array compatible and drawn
      through 2D slice views); cube-map arrays by face, mip chains by level; clears use the view's layer/level.
      Without it slices 1-7 stayed empty and the whole screen came out blue/purple.
    - ReportCounter / ResetCounter executed in command order ({counter, timestamp} at the address; timestamps
      count 0): the game clears targets "once" with SetRenderEnableConditional(not equal) + ReportCounter over the
      first report. Ignored, every conditional clear ran again and wiped the HDR scene before the composite.
    - nvnTextureWriteTexels passed its result (x0 = 1) instead of the texture to the dirty mark (fixed; it only
      surfaced as a crash once the cache read the texture).
    - Frame pacing at 60 fps (NX2_FPS=<n>, 0 = no limit); the monitor's own rate (144/165 Hz) ran the game too fast.
    - Debug: NX2_DUMP_BEFORE=<serial> also writes large images (top rows) and .f16/.f32 raw texels; gpu_*.png names
      carry the storage address; NX2_SHOT_ALL=1 dumps at the end of the screenshot frame (the mid-frame dump
      still crashes after writing).
    Open: a black object in some frames (lighting of some materials?), the upscaler shader, confirm dialog labels,
    performance at 4K.
    Working rule (the user's): never delete anything; edits go through a temporary file + os.replace with a size
    check; old test outputs are moved to work/shots_old.
  - CUTSCENES COMPLETE (2026-11-03: Frye, Shiver and Big Man in the helicopter, the newspaper, the storm). Fixes:
    - Command lists (nx2_cmd.c): lists were kept in a ring of the last 32768 by id, but the game records each mesh's
      setup (vertex attribute / stream states, buffers) once at load and calls it every frame (CallCommands): those
      lists were evicted, ~900 calls a frame went to "unknown handles", and every object was drawn with the vertex
      state of the previous full-screen pass (normals / UVs (0,0,0,1): one flat colour per object, black
      silhouettes). Now lists are kept by id until other commands are recorded over their command memory (16 bytes
      a command from where the recording began, the estimate the game is told and allocates with); the game
      re-adds the rest of its 4 MiB chunks after each recording and the whole chunks each frame. Live lists level
      off (~16k in the story, ~170k commands); "N command lists live" is logged every 4096 AddCommandMemory.
    - Viewports flipped (negative height) for NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT (the game sets it): clip y = +1 is
      row 0 there, -1 in Vulkan. Without it the scene was upside down internally and passes that sample by
      interpolated coordinates disagreed with those using gl_FragCoord; winding is no longer reversed. NX2_YFLIP=0
      restores the old behaviour.
    - Vertex attribute words on NVN2 (for reference): stream = bits 0-4, enable bit 6, offset = bits 7-20, NVN
      format = bits 21+ (mesh: RGB32F position, RGB10A2SN normal/tangent, RG16F UV, RGBA8 colour).
    - hid's shared memory (in the alias region) survives UnmapPhysicalMemory over a range around it (it crashed
      GetTouchScreenStates once; not reproduced since, the protection logs when it applies).
    - Debug: NX2_SKIP_COUNTS=<n,...> (skip draws by index count: program numbers change between runs, assets load
      asynchronously), NX2_DEBUG_COUNT=<n> (vertex input of those draws), NX2_RAW_VSTATES=1 (raw vertex state
      arrays), NX2_LOG_LISTS=<frame> (AddCommandMemory / EndRecording / calls / submits), NX2_ZEQUAL=lequal (relax
      depth "equal"), NX2_DUMP_BEFORE now exits after the dump (the replay could not go on and crashed), raw .r11
      dumps of R11G11B10 images (work/r11.py reads them), work/png.py writes PNGs.
    Open: speed (everything is drawn now: ~20 fps in this run), the upscaler shader, confirm dialog labels.
  - STORY AT 50-60 FPS (2026-11-03; was ~10). Profile first: "prof:" lines every 300 presents (frame time split into
    submits / GPU waits / pipeline builds / pacing / rest, the slowest and most called NVN procs), NX2_GPU_PROF=<present>
    (GPU time per program from timestamps), TOTK_TRACE=sched (time per guest thread every 120 presents; now called
    from our present). Periodic screenshots (NX2_SHOT_EVERY) cost ~0.8 ms a frame in these numbers. Fixes, in order:
    - Storage buffers a shader writes run in a device-local ring (copied in before, back after): the imported game
      memory is host memory and a per-pixel atomicMax (depth reduction, #8090) took 46 of 55 GPU ms over the bus.
      NX2_SSBO_HOST=1: the old way.
    - Command lists indexed by command-memory MiB (EndRecording scanned all ~16k live lists: 13 ms a frame); lists
      dropped during a replay wait in a graveyard (the replay no longer holds the list lock: EndRecording on the game's
      threads waited for it).
    - nvnQueueSubmitCommands and nvnQueuePresentTexture give the virtual core up (kern_core_away): the render thread
      held core 1 through replay + GPU wait and the thread sharing it waited (NX2_SUBMIT_KEEP_CORE=1: old way).
    - Asynchronous submission: 3 slots (command buffer, fence, staging part, device ring part, descriptor pool, dead
      objects); flush() no longer waits; nvnQueueFenceSync / nvnSyncWait / nvnQueueFinish wait for submission serials
      (they were stubs). Dumps and screenshots use flush_wait. NX2_SYNC_SUBMIT=1: every flush waits.
    - The game's frame limiter (main thread, virtual call returning to main+0x31b8e20) slept to 16.7 ms after the
      frame began, before our replay: frames took ~23 ms. That sleep is skipped; our present paces to 60 fps
      (NX2_GAME_LIMITER=1 keeps it). The cutscene timeline per frame is unchanged.
    - Swapchain MAILBOX when available (FIFO on a 144/165 Hz monitor added vblank waits; NX2_VSYNC=1 for FIFO).
    Left: the heaviest storm shots (~1000 draws, cloud particles with overdraw at 2560x1440: ~15 ms of GPU) run at
    50-57 fps; replay CPU ~8 ms there. Next ideas: device-local copies of read-only vertex / storage data (all game
    memory is read over PCIe now), a lower internal resolution (the game's dynamic resolution sees our timestamps as
    fast), replay on its own thread.
  - CHARACTER CREATION AND GAMEPLAY (2026-11-04; the beach tutorial: aiming, shards, ink on the ground, swimming,
    water danger, HUD). Rendering fixes on the way:
    - GPU texture compression: the game renders BC4 / BC5 textures (skin detail, labels) through RGBA16UI / RGBA32UI
      views whose texels are the blocks. Such a target is drawn into an uncompressed image of the view's size and
      copied into the compressed image after the draw or clear (rt_image / enc_flush in nx2_vkdraw.c).
    - Texture swizzles: the builder's swizzle (nvnTextureBuilderGetSwizzle) is the default of every view that sets none
      (label BC5 textures are R,R,R,G); image matching ignores swizzles.
    - CPU-written textures (the glyph cache, 2048x10240 R8) are checked once a frame (was every 16: letters added and
      drawn in the same frame were missing). NX2_TEX_CHECK_EVERY=<n> for the old interval.
    Saves work (build/profile/save): a boot resumes at the hangar after ~1200 presents, so gameplay tests take minutes.
    Test inputs (NX2_INPUT_SCRIPT, up to 256 steps): stick directions LsUp..RsRight and Slow (quarter deflection).
    Scripts in work/: save_script4.txt (from the save through the tutorial), play_script.txt (new game to the confirm).
  - GAMEPLAY AT 60 FPS (2026-11-04; was 33-36 fps with ~1400 draws a frame). "prof:" now splits the draw CPU time
    into phases, pipeline builds into translation / driver, and reports the pool mirrors. Fixes, in order:
    - getenv / snprintf in every draw (3 ms a frame), a linear scan over ~3000 memory pools for every vertex, index and
      storage buffer (5 ms; now a per-thread cache invalidated when the pool table changes), indices copied into
      staging (now read in place). Draw CPU time: 27 -> 7.5 ms a frame.
    - Video-memory mirrors of the memory pools read as vertex, index and read-only storage data: the GPU read them from
      host memory over PCIe (24 ms of GPU time a frame; 3 ms from video memory). The game's heap is write-watched
      (memory.c), so once per submit the pages the CPU wrote are copied into the mirrors (~0.1-0.2 MiB a frame);
      storage buffers written by shaders are copied into the mirror as well. ~100 pools, ~210 MiB. NX2_NO_MIRROR=1:
      the old way. Staging in resizable-BAR memory did not help (opt-in NX2_STAGE_BAR=1).
    Left: the first time a shader appears its pipeline is built on the spot (hitches of a few frames on a first visit;
    the driver's disk cache makes later runs smooth); the game renders at 2560x1440 (docked).
  - MIRROR CORRECTNESS (2026-11-04). Entering the water in the tutorial gave black frames (sky, far scenery and the
    player black, lit patches of rock left) only with the mirrors on (0 of 117 sampled frames with NX2_NO_MIRROR).
    Holes found: a 53 MiB storage buffer written in place by shaders (too large for the device ring) changed the pool
    behind its mirror. Pools written in place by shaders are no longer mirrored (NX2_MIRROR_FOLLOW=1 copies the
    written range into the mirror instead: black frames got more frequent); a pool overlapping a mirrored one is not
    mirrored either (one mirror per memory: a sync clears the write marks of shared pages). After that: 1 black frame
    in 184 sampled. NX2_MIRROR_VERIFY=1 hashes every mirrored page at each sync and logs pages that changed without a
    write mark.
    Debug tools: NX2_SHOT_FROM=<n> (periodic shots from present n), NX2_BLACK_CHECK_FROM=<n> (reads the output back
    each present and dumps all GPU images of a mostly black one; the wait it adds hides the race above),
    NX2_DUMP_ON_BLACK=1 (the same from the periodic shots, one present late, with a draw trace), work/blackcheck.py
    (black frames in a shots folder), work/sheet.py / sheet2.py (contact sheets), work/readpng.py.
    Tutorial route (work/seg5.txt..seg7.txt): aim at the shard, collect shards, swim in ink, look around; the scripted
    walks still end in the water often (the route to the dock with the balloon targets is not found yet).
    Root cause of the last black frame (one frame at the moment the player is splatted): not the mirrors' data but
    timing. With the mirrors the GPU got fast and the CPU ran up to three submission slots ahead of it; the game reads
    what the GPU wrote into its memory during the previous frame (finished long before on the console) and read it
    half-written. Captures on every frame (which wait for the GPU) hid it; NX2_SYNC_FROM=<present> (every flush waits
    from then on) and NX2_FRAMES_AHEAD=0 removed it (0 of 234 sampled frames). NX2_FRAMES_AHEAD=0 is now the
    default: at each present the CPU waits for that present's GPU work (n: for the work of present - n). Gameplay
    stays at 60 fps, but the present now waits ~6 ms for the GPU (pacing slack 2 ms instead of 8.7): the GPU still has
    ~6 ms of work after the last submit although its draws sum to 3 ms (full barriers between passes, copies: the
    next thing to cut if a heavier scene drops below 60).
    The verifier (NX2_MIRROR_VERIFY) also showed the write-watch to be exact for CPU writes once asynchronous GPU
    writes are excluded (NX2_MIRROR_TRAP=1 logs who writes a pool: nx2_mtrap.c). Pools whose writes are not tracked
    are refused at creation (a locked compare-exchange probe must leave a write mark).
    Input recording: NX2_INPUT_RECORD=<file> writes the played input (keyboard / XInput) as script steps, one line
    each; NX2_INPUT_SCRIPT_FILE=<file> replays a script from a file (up to 8192 steps; lines or ';' separate steps).
    Intermittent boot abort (hid result 0x826ca after "hid:IAppletResource cmd 0"): the shared memory map failed
    without a log; nx2_shmem.c now logs the address and error of a failed map. Seen once in ~25 boots.
  - PLAYER FEATURES (2026-11-04):
    - Launcher (nx2_settings.c): a settings window before the game (window size, windowed / borderless fullscreen,
      console mode TV / handheld, FPS limit, VSync, overlay, controller, button layout, rumble, show at start). Saved in
      build/splatoon.ini and applied as the NX2_* variables (the environment wins). Skipped with launcher = 0 (Shift at
      start shows it again), NX2_NO_LAUNCHER and in automated runs (input scripts, screenshots).
    - Window: NX2_WINDOW_SIZE=<w>x<h>, NX2_FULLSCREEN=1 (borderless); F11 / Alt+Enter toggle; minimized windows skip
      the present.
    - Performance overlay (nx2_overlay.c, NX2_OVERLAY=off|fps|full, F1 cycles): FPS and frame time; full mode adds
      frame min / max, replay CPU and GPU wait per frame, draws, new shaders, VRAM (VK_EXT_memory_budget), RAM, CPU use,
      image and window size, GPU name. GDI text in a BGRA bitmap copied onto the swapchain image after the game's.
    - Controllers (nx2_pad.c): XInput (empty slots asked once a second), DualShock 4 / DualSense / DualSense Edge from
      their HID reports (USB and Bluetooth, background thread, hot-plug). NX2_PAD_LAYOUT=xbox puts Switch A on the
      bottom button (default: by position, as on a Switch). Rumble: the game's SendVibrationValue(s) (it calls them)
      drive the XInput motors and the Sony motors over USB; NX2_RUMBLE=<percent>. NX2_CONTROLLER=keyboard: no pads.
    - Console mode: NX2_OPERATION_MODE=handheld reports handheld (operation and performance mode 0); the game then
      presents 1280x720 instead of 3840x2160.
    - 60 fps kept with NX2_FRAMES_AHEAD=0: work goes to the GPU every NX2_FLUSH_DRAWS draws / dispatches (default 200)
      instead of only at the game's two submissions per frame; the GPU now runs during the replay and the present waits
      ~1-3 ms instead of 7.5 (beach tutorial: 54.6 -> 60 fps, slack 2.5 ms; hangar slack 7.7 ms). Black-frame test still
      clean (0 of 117).
    - Not done: Sony rumble over Bluetooth (needs the CRC'd output report), gyro aiming from controllers.
  - FSR 1 (2026-11-04, nx2_fsr.c): AMD's EASU + RCAS shaders as the TotK base ships them (host/shaders/fsr_*.comp over
    shaders/ffx, unmodified, MIT; SPIR-V in host/fsr_*_spv.h) on a plain Vulkan device. Used where the game's neural
    upscaler ran (it comes out as a flat colour through the shader translation and was a linear blit): docked mode
    scales 2560x1440 (dynamic: 2559 / 3071 / 3839 wide outputs seen) to the 4K output; and on the present when the
    window is larger than the game's image (handheld mode in a big window). RGBA16F intermediates (reused when large
    enough, rounded to 256), a 1:1 blit into the destination (the game's RGB9E5 scene). Gradient energy of the same
    frame (virtual time): 6.55 linear -> 7.48 FSR; gameplay stays at 60 fps. NX2_UPSCALER=fsr|linear,
    NX2_SHARPNESS=0..100 (default 80); both in the launcher. Known: RCAS reads one column / row past the output at the
    right / bottom edge of a reused larger intermediate.
    Note: host/shaders/fsr_easu.comp and fsr_rcas.comp were overwritten by mistake on 2026-11-04 and restored from the
    session's file history the same minute (sizes 1780 / 1264 bytes, AMD headers untouched).
  - NVIDIA IMAGE SCALING (2026-11-04, nx2_nis.c): NIS 1.0.3's NVScaler from NVIDIA's repository
    (NVIDIAGameWorks/NVIDIAImageScaling, MIT; NIS_Main.glsl, NIS_Scaler.h, NIS_Config.h in third_party/nis, unmodified;
    the license text is at the top of each file). tools/gen_nis.py compiles the shader with glslc (blocks 32x24, 128
    threads, viewports, clamped output, SDR) into host/nis_spv.h and copies the coefficient tables into host/nis_coef.h;
    NVScalerUpdateConfig is ported to C. Same interface and places as FSR; above 2x per axis it falls back to FSR.
    NX2_UPSCALER=nis. Same frame (virtual time), gradient energy: linear 6.55, FSR 7.48, NIS 8.44; 60 fps kept.
    DLSS was looked at and left out: the game has no TAA (SMAA: its 160x560 / 64x16 lookup tables in the last passes),
    no camera jitter and no motion vector buffer, all of which every DLSS mode needs.
  - LAUNCHER, SECOND VERSION (nx2_settings.c): Splatoon Raiders colours (dark teal panel, orange / turquoise / yellow ink
    splats, slanted tags), categories in a sidebar (Tela, Imagem, Desempenho, Controles, Geral; Ctrl+Tab cycles), a
    description of the option under the mouse, owner-drawn dark option lists and JOGAR! / Sair buttons, dark title bar.
  - LAUNCHER IN ENGLISH: every text in Portuguese and English (Setting.label / names / desc pairs), a Language option
    (automatic = Windows' UI language, Portuguese, English) that switches the window at once; the choice also sets
    NX2_UI_LANG for the overlay's lines.
  - RENDER THREAD PROFILE AND OPTIMIZATION (2026-11-04). nx2_sampler.c (NX2_SAMPLE_PROF=1, NX2_SAMPLE_FROM=<present>):
    samples the thread that last called nvnQueueSubmitCommands ~1000 times a second; samples.txt gets host offsets,
    guest functions and samples outside the executable by DLL. The build now writes build/splatoon_host.pdb
    (-gcodeview, /DEBUG); tools/symbolize_samples.py names the offsets with llvm-symbolizer and sums them by function.
    Beach tutorial, busy share of the render thread 43.5% -> 24.8% of samples (the rest: GPU fence and pacing waits
    ~61%, NVIDIA driver ~12%); gameplay slack 2.5 -> 6.0 ms at 60 fps:
    - nx2_pool_range: binary search over a sorted copy of the pool table (rebuilt when it changes; overlapping pools:
      the first in table order, as before) behind the per-thread cache: 8.4% -> 0.5%.
    - CPU texture checks: storage_hash in four lanes, and large storage (>= 1 MiB) checked per 2 MiB region with
      memory.c's frozen regions: a region still frozen since its hash is unchanged (28 MiB -> 0.9 MiB hashed a frame;
      the glyph cache's one region next to file reads that keep it thawed is hashed alone). Dialogue text checked.
    - key_hash 8 bytes at a time (3.3% -> 0.6%); constant blocks copied then zero-filled only past the data; the
      per-draw image slots cleared only for the pipeline's descriptors; two getenv calls on hot paths cached.
    - NX2_ASYNC_SHADERS=1 (nx2_jobs.c: two background threads compile new graphics pipelines; draws skip until ready)
      works but is NOT for players: ink painted on the ground is gameplay in Splatoon (read back from the GPU) and a
      skipped ink draw changed the outcome of the same scripted run. Left out of the launcher.
    Next candidates: descriptor set reuse (the driver's 12%), fewer full barriers (GPU), a persistent pipeline list
    compiled at boot for after driver updates (the NVIDIA cache already covers visited areas on a machine).
  - READINESS (2026-11-04):
    - SOUND: there was none (compat_sdl's SDL3 stand-in failed on purpose: "mixing without sound"). compat_sdl/
      sdl_wasapi.c implements the ten SDL3 audio calls audio_mixer.c makes on WASAPI: shared mode, event driven,
      48 kHz stereo float with the system's rate conversion, MMCSS "Pro Audio" thread, reopens the default device
      when it goes away. Log: "audio: SDL device 'Windows default output' opened ... device period 480 frames".
    - BOOT ABORT FIXED (hid 0x826ca, ~1 boot in 15-20): the SDK maps hid's shared memory at a random address it probes
      with QueryMemory; only ~1 GiB of the ~500 GiB ASLR range read free (the rest is reported as the host's), so the
      probes sometimes all failed. memory.c now reserves a 16 GiB map region after the alias region, reported free,
      and shared memory mapped outside the alias region is reported Shared (it read free before). 0 failures in 40
      boots.
    - GAME FOLDER: the launcher's "Pasta do jogo / Game folder" (General) picks the folder with exefs/main and romfs
      (validated); NX2_GAME_DIR replaces the build-time paths; saves go to "profile" next to the executable; a missing
      game shows a message (PT/EN) saying how to choose the folder (nx2_game_paths in nx2_settings.c).
    - Still to do: combat and later areas untested; a packaged folder (exe + PDB not needed + COMO_JOGAR).
  - QUAD HD / STUTTER (2026-11-04):
    - 2560x1440 (RTX 5060, 1440p 180 Hz monitor) runs the route at 60 fps with the same per-frame numbers as 1280x720:
      docked, the game always renders 2560x1440 and upscales to 3840x2160 itself (FSR/NIS there); the window only
      changes the last scale (4K -> window, a linear blit). Slack 3-4.5 ms/frame in the heaviest gameplay.
    - WINDOW (nx2_vk.c): a window size equal to the monitor's opens borderless fullscreen (a framed 2560x1440 window
      was clipped by the taskbar to 2560x1421 and stretched); a size larger than the desktop shrinks to fit, same
      shape; per-monitor DPI awareness on the window thread (125%/150% scaling no longer blurs it); the picture keeps
      16:9 with black bars in a window of another shape (FSR/NIS on the present write the inner rectangle).
    - NX2_SLOW_LOG=<ms> (debug): each frame slower than that, with where its time went (submits, GPU waits,
      pipelines with translation/driver split, textures hashed, mirror copies).
    - The gameplay hitches (22-44 ms, a few per minute) were pipeline builds, ~85% of it the shader translation
      (~2 ms a stage; the NVIDIA driver caches its own compile on disk).
    - TRANSLATION CACHE (nx2_xcache.c, build/shader_cache.bin): key = two 64-bit hashes of the stages' code (up to
      their constant data, which follows the code) and the options; stored with the questions the translator asked
      (constant buffer words, texture types) and their answers; reused only when the same questions get the same
      answers (texture handles mapped by position, their types compared). Also an in-memory dedupe: pipeline variants
      of one program share a translation (first run: 1262 hits, 652 translated). NX2_XCACHE_VERIFY=1 retranslates
      every hit and compares: 1900 checked, 0 differ. The translator's identity (sha1 of nx2_xlate.lib +
      shader_recompiler.lib) is compiled in by build.py; another translator sets the old file aside. NX2_XCACHE=0 off.
      Warm cache: worst gameplay frame interval 44 -> 29 ms; what remains (~6 per 2 minutes, 20-29 ms) is new
      texture uploads. ~58 MiB for the route.
    - Bug found and fixed: in append mode ftell is 0 before the first write, so every session appended a second file
      header and the next one set the file aside as "damaged-end" (records recovered each time); now fseek to the end
      first. The three set-aside copies are in work/shots_old.
  - READY-TO-PLAY FOLDER (2026-11-04): tools/package.py builds dist/Splatoon Raiders Recomp: SplatoonRaiders.exe
    (build/splatoon_host.exe), COMO_JOGAR.md, HOW_TO_PLAY.md (new, English) and, when the folder has none, the shader
    cache. It never removes anything: the player's ini, profile, logs and cache stay; a differing older exe or guide
    goes to work/shots_old/dist_<time>/. Imports are Windows DLLs plus vulkan-1.dll (the GPU driver's); the C/C++
    runtime is static. 322 MiB.
    - Debug output now opt-in: shader capture (45k files in shaders/ per boot!) only with NX2_SHADER_CAPTURE=1, the
      proc list nvn2_procs.txt only with NX2_PROC_LIST=1. The log stays in logs/splatoon.log next to the program.
    - Tested as a player: started from the folder with no settings, launcher -> PLAY -> new game (difficulty screen)
      at 60 fps, sound, save in the folder's profile, cache loaded, clean exit on closing the window. The test's
      profile/ini/logs were moved to work/shots_old so the folder ships clean.
  - DUALSENSE OVER BLUETOOTH (2026-11-04, nx2_pad.c): buttons were wrong. Over Bluetooth the DualSense starts with the
    basic report 0x01 (DualShock 4 layout), and Windows pads every Bluetooth report to 78 bytes, so the "n >= 64"
    test took it for the USB report: the D-pad read "up" all the time and the face buttons came from other bytes.
    Now the transport decides the layout, and opening a Bluetooth DualSense reads its calibration (feature 0x05),
    which switches it to the full report 0x31. A read returns every report queued since the last one (312 bytes =
    4 reports): the newest is used now, not the oldest. Rumble over Bluetooth: output reports 0x31 (DualSense) and
    0x11 (DualShock 4) with the CRC-32 of 0xA2 + report (a probe's writes were accepted). NX2_PAD_LOG=1 (debug)
    logs each button change with its report. Tested idle with the user's DualSense on Bluetooth (no stuck buttons);
    pressing buttons in game not yet checked by the user.
    - nx2_draw.h had the Nx2Copy declaration twice since 11:18 (an old CopyTextureToTexture patch script run again;
      the other files it patches were untouched): the duplicate removed, the file before it kept in work/shots_old.
      Cause found later: the scratchpad held old patch scripts named copy.py and mailbox.py; an edit script run from
      there that failed imported `copy` while printing its traceback and ran the old patch. Renamed
      (old_patch_copytex.py, old_mailbox_probe.py); scratchpad scripts now run with python -I.
  - INK, SHOTS, CAMERA (2026-11-04; the user: no swimming in own ink, no refill, no wall climbing, no low-ink sign,
    invisible shots, right stick only sideways):
    - nvnCommandBufferSignalEvent was not replayed. The game polls the event (nvnEventGetValue) to know when the GPU
      results it reads (ink under the player...) are in memory: it never saw them. Now the GPU writes the value
      (vkCmdFillBuffer into the imported pool, full barriers around, then a submit) after the work before it.
      Values climb 0, 1, 2... as the game expects. WaitEvent: one in-order queue, nothing to wait for.
    - The replay resolved command names to procs once, at the first replay; procs the game asks for later
      (DrawElementsIndirect: first in gameplay) were dropped. Resolved per proc now (nx2_draw.c kind_of); the recorder's
      indices (resolve, state binds) are resolved again whenever the proc count grows.
    - DrawElementsIndirect / DrawArraysIndirect: vkCmdDrawIndexedIndirect / vkCmdDrawIndirect reading the game's
      numbers in place (imported pools now carry INDIRECT_BUFFER usage); a barrier before.
    - Base instance was dropped from DrawElementsInstanced / DrawArraysInstanced (over a million draws by the
      tutorial): per-instance data started at instance 0. Passed as firstInstance now.
    - Result: the ink tank shows beside the reticle and refills in ink, "Low ink!" shows, the shots fly as ink drops
      to the reticle. 60 fps kept in continuous shooting (slack 1.5-1.9 ms; NX2_EVENT_FLUSH=0 delays the signal to the
      next submission: worse, 54 fps).
    - Vertical aim: Splatoon aims up/down with motion controls by default (the right stick then turns only sideways)
      and the sensor read at rest. The right stick's vertical axis is now fed as rotation about x through TotK's sensor
      fusion (motion_fusion.c, now built): direction, angle, rate and gravity agree. Launcher: Controls > Vertical aim
      (NX2_GYRO_STICK 0/50/100/150).
    - REAL GYRO (nx2_pad.c): every full Sony report (DualSense 0x31 / USB 0x01: gyro at data+15, accel +21; DualShock 4
      0x11 / USB 0x01: +12, +18; all reports of a batched read, 4 ms apart) feeds motion_global(): SDL axes ->
      Switch axes as input.c (acc (-x, z, -y) / 8192 G, gyro (x, -z, y) / 16.384 deg/s). Bias learned while still
      (all axes within 32 raw of it for 60 samples; first estimate within 400): this controller 1.4 -1.0 -3.2. Held
      still the game reads gyro 0.000 and gravity -1 G on z. The combined right stick (keyboard too) still adds its
      turn about x. Launcher: Controls > Motion aiming (NX2_GYRO=0 off); rows of a category with more than four
      options are spaced to fit. NX2_MOTION_LOG=1 (debug): raw reports and the state the game reads.
      Not verified by hand here: the senses of real turns (the axis mapping is input.c's, tested there with SDL).
  - SWIMMING IN INK / CLIMBING (2026-11-04; the user: the squid stays on top of its ink, cannot climb inked walls).
    The GPU-event fix was not enough. Read traps (NX2_READ_TRAP=1: CPU storage of GPU-written images; =2: samples
    reports; nx2_mtrap.c) showed the game reads no image back but reads ~180 samples-passed reports a frame
    (ReportCounter type 1, reset -> one draw -> report) from main+0x2afe3e0. They were estimated (draws since the
    reset = 1 each). NX2_SAMPLES_FAKE=<n> (debug) showed they decide gameplay (large counts: damage everywhere).
    NX2_OCC_LOG=1 (debug) groups them: 177 a frame by one program drawing into the 6400x6400 ink map without depth
    test, 1-2 with depth test.
    - Real counts: a Vulkan occlusion query (precise) from each reset to its report; the result copied into the
      game's memory at the end of the submission (one wait for all). Queries are reset 1024 at a time at the start
      of a command buffer and begin/end inside the open rendering when they can (a query begun inside a rendering
      that ends is cut: its report keeps the estimate, 0.02 %). NX2_OCCLUSION=0: estimates only.
    - With real counts every ink-map query passed all its pixels (~2500 each): the stencil test was ignored. Stencil
      now: the depth-stencil state's stencil enable, functions and ops (front/back, nvnDepthStencilStateGetStencil*),
      SetStencilRef / ValueMask / Mask as dynamic state, stencil clears (ClearDepthStencil's stencil and mask).
      83 of 177 queries count ink now; the squid submerges in its ink (only its eyes show), swims fast, leaves the
      ink at its edge; walking into the sea now hurts and drowns the player (it went through water unharmed
      before). 60 fps kept in the tutorial with ~0.7 ms of slack (was ~2 ms): the occlusion work costs ~1 ms.
    Not verified: climbing an inked wall (the tutorial wall is beyond the scripted route).
  - WASHED-OUT COLOURS (2026-11-04; the user: make the squid in ink look like the original). The whole picture was
    pale and hazy next to the reference video (pastel ink, grey sky). Followed one ink pixel through the frame
    (NX2_TRACE_FRAME, NX2_DUMP_BEFORE, gpu3d_*.bin dumps of 3D images, NX2_DUMP_SPIRV=<serials> with the constant
    buffers of the first draw, decompiled with the Vulkan SDK's spirv-cross --flatten-ubo):
    HDR 0.03/0.45/0.33 -> tonemap #11220 (bloom^3.27 + scene x exposure 0.95, 1 - e^-x, 8x8x8 LUT: all correct)
    3/100/70 -> SMAA blend #4 encodes sRGB on purpose (28/168/143) -> the game's neural upscaler (dispatch #16, which
    FSR / NIS / the linear blit replace) -> UI -> last pass #2163 copies through an R8G8B8A8_SRGB view, which encodes
    again: 93/212/198. So the game's upscaler also turns the frame linear; ours did not. Now a compute pass
    (shaders/srgb_decode.comp -> srgb_decode_spv.h, nx2_srgb_decode in nx2_fsr.c) decodes sRGB in place in the
    upscaled RGB10A2 frame, after any of the three upscalers: the ink 20/123/106 and the sky blue, as in the video.
    NX2_UPSCALE_DECODE=0: the old washed-out look. Cost: negligible (60 fps, slack 1.7 ms).
  - WALL CLIMBING VERIFIED (2026-11-04): NX2_INPUT_LIVE=<file> (tests, nx2_hid.c) takes steps appended to a file while
    the game runs, relative to the present now ("0-60:LsUp"); the present number is written to <file>.frame. With
    periodic shots that drives the game interactively. Route to the tutorial's dock wall: work/route_wall.txt (from
    present ~15780: face the wall, ink it, ZL + forward). The squid swims up the inked wall face and comes out on top
    (shots 16020-16200 of work/wall1). An earlier attempt on a crate stopped at the bottom where the ink was low.
    - Monitor sleep: controller input is not input to Windows; after the idle time (5 min here) the display turned off
      and presents crawled (87 ms frames). SetThreadExecutionState(display required) while the game runs. Timing tests
      done with the display off are not comparable (several false slowdowns today).
    - Loading: the boot loading screen is ~7 s at 60 fps; all-thread sampling (NX2_SAMPLE_PROF=all, NX2_SAMPLE_UNTIL,
      waits attributed to ntdll calls) shows threads waiting, not computing and not reading files. With warm caches no
      area-transition hitch over 80 ms remains. First visits translate and compile new shaders (cached afterwards).
  - VIRTUAL TIME FOR TESTS (sp_main.c rt_ticks): NX2_VIRTUAL_TIME=<present> makes the guest counter follow the presents
    (1/60 s each) from that present on. Two runs of the same input script then give the same frames (mean pixel
    difference ~1/255 at frame 13700 against ~40 px of drift before), so a route found once replays. Uncapped speed
    (NX2_FPS=0) does not help: the replay and GPU limit the frame rate, not the pacing.
    Parallel test runs share build/profile/save: one run deleted the other's working copy ("cannot make the save working
    copy"). Run tests one at a time.
    Boot abort (hid 0x826ca) seen again: svc 0x13 MapSharedMemory is never reached, the SDK gives up before (likely its
    random address search for the mapping). Not reproduced on demand yet.
  Next: (1) the Vulkan back end replaying the recorded command lists (TotK's vk_*.c as the base; per-command
  argument copies at record time), (2) shader translation of the captured programs with Eglantine's
  translator (environment adapter from its video_core/shader_environment.cpp), (3) a window and present,
  (4) controller input (TotK's ov_input.c) and sound output, (5) frame pacing at 60 fps.
  Next: the CommandBuffer family on the host (201 procs; 147 exist in TotK's nvn_api_cmd.c, which records an
  own command stream), with Event/Buffer families reviewed for host ownership; then the Vulkan back end that
  replays recorded commands, shader translation (SM86 through Eglantine-style Ampere lowering to TotK's
  Maxwell translator), and the first screenshot.
  B (not chosen). Run the real NVN2 driver and emulate below it: gsv/nvsrv/syncpt/gpu services, an Ampere command processor
     (GPFIFO, 3D/compute engines, Turing MME macros, QMD 3.0) and SM86 shader translation, all documented for
     this game in Eglantine's NX2_NOTES.md (GPL: our own code). Driver-exact, but GPU-emulation cost.

Run tests with: `CC="C:\Program Files\LLVM\bin\clang.exe"` and
`--cflags "-march=x86-64-v3 -D_CRT_SECURE_NO_WARNINGS -Wno-deprecated-declarations"`.

## Stages

1. Toolchain: LLVM/Clang, CMake, Ninja, Vulkan SDK, VS Build Tools.
2. Lifter: fork TotK's `recomp/`, add PAC, LSE and LDAPR; differential tests against Unicorn.
3. Function discovery and C generation for all four modules; build and link a stub host that runs to the first SVC.
4. Host: Horizon kernel/SVCs and `nn::*` services, extended with the Switch 2 services from Eglantine's notes.
   Milestone: game boots to its first graphics call.
5. Graphics: NVN2 front end (the SDK's `nvn*` entry points, not GPU command streams) on a Vulkan back end;
   shaders through Eglantine's Ampere lowering + translator to SPIR-V. Milestone: title screen.
6. Audio, input, saves, movies (VP9).
7. Performance (60 fps) and graphical-bug pass with screenshot comparisons.
