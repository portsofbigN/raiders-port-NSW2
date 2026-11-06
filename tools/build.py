"""Compile the generated C and the runtime into build/splatoon.exe with LLVM Clang and Ninja.

Writes build/build.ninja and runs Ninja on it, so a rerun only recompiles what changed.

Usage: build.py [--gen work/gen] [--out build] [--jobs N] [--compile-only] [--host] [--game-dir DIR]
"""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CLANG = Path(r'C:\Program Files\LLVM\bin\clang.exe')
NINJA = shutil.which('ninja') or str(Path(os.environ.get('LOCALAPPDATA', '')) / 'Microsoft/WinGet/Packages/'
                                     'Ninja-build.Ninja_Microsoft.Winget.Source_8wekyb3d8bbwe/ninja.exe')
# Generated code: TotK Recomp's flags (from LAS Recomp). Runtime: optimised, with warnings.
GEN_FLAGS = '-O1 -fno-strict-aliasing -Wno-return-type -Wno-unused-label -Wno-unused-variable ' \
            '-Wno-unused-but-set-variable -march=x86-64-v3'
RT_FLAGS = '-O2 -g -gcodeview -Wall -fno-strict-aliasing -march=x86-64-v3 -D_CRT_SECURE_NO_WARNINGS'
RUNTIME = ['rt_min.c', 'fma.c']
# --host: TotK Recomp's host core (kernel, memory, loader, dispatch, SVCs, IPC) with sp_main.c in place of its
# main.c/boot.c. Graphics, audio, input and fs are not ported yet; sp_stubs.c holds what the core calls of them.
HOST = ['sp_main.c', 'sp_stubs.c', 'nx2_services.c', 'nx2_shmem.c', 'fs_host.c', 'romfs_dir.c', 'ov_system.c', 'amiibo.c', 'nx2_gfx.c', 'nx2_tex.c', 'nx2_cmd.c', 'nx2_draw.c', 'nx2_vkdraw.c', 'nx2_vkfmt.c', 'nx2_astc.c', 'nx2_prog.c', 'nx2_buf.c', 'nx2_font.c', 'nx2_account.c', 'nx2_time.c', 'nx2_hid.c', 'nx2_vk.c', 'nx2_mtrap.c', 'nx2_settings.c', 'nx2_overlay.c', 'nx2_pad.c', 'nx2_fsr.c', 'nx2_nis.c', 'nx2_sampler.c', 'nx2_jobs.c', 'nx2_xcache.c', 'motion_fusion.c', 'compat_sdl/sdl_wasapi.c', 'audren.c', 'audio_mixer.c', 'audio_effects.c', 'audio_reverb.c', 'nx2_tex_deswizzle.c', 'nvn_tex.c', 'dispatch.c', 'hostfn.c', 'kernel.c', 'memory.c', 'loader.c', 'svc.c', 'ipc.c',
        'cfg.c', 'sha256.c', 'fma.c', 'platform/win32.c', 'platform/common.c']
XLATE_LIBS = ' '.join(f'"{ROOT / "shader" / "build" / p}"' for p in ('nx2_xlate.lib', 'shader_recompiler.lib', 'fmt/fmt.lib', 'sirit/src/sirit.lib'))
BUILTINS_DEFAULT = Path(r'C:/Program Files/LLVM/lib/clang/23/lib/windows/clang_rt.builtins-x86_64.lib')
DEFAULT_EXEFS = Path(r'C:/Program Files (x86)/nintendo_s_2games/splatoon_raiders/updates/65536/exefs')
DEFAULT_ROMFS = Path(r'C:/Program Files (x86)/nintendo_s_2games/splatoon_raiders/updates/65536/romfs')   # merged tree


def find_builtins():
    if BUILTINS_DEFAULT.exists():
        return BUILTINS_DEFAULT
    llvm_base = Path(r'C:/Program Files/LLVM/lib/clang')
    if llvm_base.exists():
        matches = list(llvm_base.glob('*/lib/windows/clang_rt.builtins-x86_64.lib'))
        if matches:
            return matches[-1]
    return BUILTINS_DEFAULT


def c_string(s):
    return '"' + s.replace('\\', '/').replace('"', '\\"') + '"'


def modules_c(gen, out, exefs, romfs):
    """build/modules.c for the host core (TotK Recomp tools/build_game.py module_glue): layout, linked function
    tables, the ExeFS file hashes the loader checks, default paths."""
    import hashlib, json
    layout = json.loads((gen / 'layout.json').read_text())
    o = ['// Module glue written by tools/build.py; do not edit.', '#include "dispatch.h"', '#include "host.h"',
         '#include "layout.h"', '', 'const RtLayoutEntry rt_layout[] = {']
    o += [f'    {{"{m["name"]}", MOD_{m["name"].upper()}_BASE, MOD_{m["name"].upper()}_SIZE}},' for m in layout]
    o += ['};', f'const int rt_layout_count = {len(layout)};', '']
    for m in layout:
        n = m['name']
        o += [f'extern const uint64_t {n}_fn_addrs[];', f'extern const GuestFn {n}_fn_ptrs[];',
              f'extern const uint64_t {n}_fn_count;']
    o += ['int rt_linked_modules(RtModule *out, int max) {', '    int n = 0;']
    for m in layout:
        n, u = m['name'], m['name'].upper()
        o.append(f'    if (n < max) out[n++] = (RtModule){{"{n}", MOD_{u}_BASE, MOD_{u}_SIZE, {n}_fn_addrs, {n}_fn_ptrs, '
                 f'{n}_fn_count}};')
    o += ['    return n;', '}', '', 'const RtFileHash rt_exefs_sha256[] = {']
    files = sorted(f.name for f in exefs.iterdir() if f.is_file()) if exefs.exists() else []
    for f in files:
        o.append(f'    {{{c_string(f)}, {c_string(hashlib.sha256((exefs / f).read_bytes()).hexdigest())}}},')
    o += ['};', f'const int rt_exefs_sha256_count = {len(files)};',
          f'const char rt_default_exefs[] = {c_string(exefs.as_posix())};',
          f'const char rt_default_romfs[] = {c_string(romfs.as_posix())};',
          f'const char rt_default_profile[] = {c_string((out / "profile").as_posix())};',
          'const int rt_game_version = 0;', 'const char rt_port_version[] = "0.0.1";', '']
    text = '\n'.join(o)
    path = out / 'modules.c'
    if not path.exists() or path.read_text() != text:
        path.write_text(text)
    return path


def esc(p):
    return str(p).replace('\\', '/').replace(':', '$:').replace(' ', '$ ')


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--gen', type=Path, default=ROOT / 'work' / 'gen')
    ap.add_argument('--out', type=Path, default=ROOT / 'build')
    ap.add_argument('--game-dir', type=Path, default=None, help='Game directory containing exefs and romfs')
    ap.add_argument('--exefs', type=Path, default=None, help='Path to game exefs directory')
    ap.add_argument('--romfs', type=Path, default=None, help='Path to game romfs directory')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    ap.add_argument('--compile-only', action='store_true')
    ap.add_argument('--host', action='store_true', help="TotK Recomp's host core instead of runtime/rt_min.c")
    args = ap.parse_args()
    gen, out = args.gen.resolve(), args.out.resolve()
    (out / 'obj').mkdir(parents=True, exist_ok=True)

    env_game_dir = os.environ.get('NX2_GAME_DIR')
    game_dir = args.game_dir or (Path(env_game_dir) if env_game_dir else None)
    if game_dir:
        exefs = (game_dir / 'exefs').resolve()
        romfs = (game_dir / 'romfs').resolve()
    else:
        exefs = (args.exefs or DEFAULT_EXEFS).resolve()
        romfs = (args.romfs or DEFAULT_ROMFS).resolve()

    builtins = find_builtins()

    inc = f'-I "{ROOT / "runtime"}" -I "{gen}" -I "{ROOT / "work" / "prelinked"}"'
    # the shader translator's identity: the on-disk translation cache (nx2_xcache.c) starts over when it changes
    import hashlib
    xlate_files = [ROOT / 'shader' / 'build' / p for p in ('nx2_xlate.lib', 'shader_recompiler.lib')]
    if all(p.exists() for p in xlate_files):
        xid = hashlib.sha1(b''.join(p.read_bytes() for p in xlate_files)).hexdigest()[:16]
    else:
        xid = '0000000000000000'
    host_inc = f'-DNX2_XLATE_ID=0x{xid}ull -I "{ROOT / "host"}" -I "{ROOT / "host" / "compat_sdl"}" -I "C:/VulkanSDK/1.4.363.0/Include" -I "{ROOT / "shader"}" -I "{gen}" -include "{ROOT / "host" / "sp_compat.h"}"'
    lines = [
        f'genflags = {GEN_FLAGS} {inc}',
        f'rtflags = {RT_FLAGS} {host_inc if args.host else inc}',
        'libs = ' + (f'"C:/VulkanSDK/1.4.363.0/Lib/vulkan-1.lib" -lwinmm -luser32 -ladvapi32 -lbcrypt -lgdi32 -lshell32 -lole32 -lsetupapi -lhid "{builtins}" {XLATE_LIBS}' if args.host else ''),
        f'rule cc_gen\n  command = "{CLANG}" $genflags -MD -MF $out.d -c $in -o $out\n  deps = gcc\n  depfile = $out.d\n'
        f'  description = gen $in',
        f'rule cc_rt\n  command = "{CLANG}" $rtflags -MD -MF $out.d -c $in -o $out\n  deps = gcc\n  depfile = $out.d\n'
        f'  description = rt $in',
        f'rule link\n  command = "{CLANG}" -fuse-ld=lld -Wl,/STACK:16777216 -Wl,/LARGEADDRESSAWARE -Wl,/errorlimit:0 -Wl,/DEBUG -o $out @$out.rsp $libs\n'
        f'  rspfile = $out.rsp\n  rspfile_content = $in_newline\n  description = link $out',
    ]
    objs = []
    for src in sorted(gen.glob('*/*.c')):
        o = out / 'obj' / f'{src.stem}.o'
        lines.append(f'build {esc(o)}: cc_gen {esc(src)}')
        objs.append(o)
    if args.host:
        srcs = [ROOT / 'host' / n for n in HOST] + [modules_c(gen, out, exefs, romfs)]
        prefix, exe = 'host_', out / 'splatoon_host.exe'
    else:
        srcs, prefix, exe = [ROOT / 'runtime' / n for n in RUNTIME], 'rt_', out / 'splatoon.exe'
    for src in srcs:
        o = out / 'obj' / f'{prefix}{src.stem}.o'
        lines.append(f'build {esc(o)}: cc_rt {esc(src)}')
        objs.append(o)
    lines.append(f'build {esc(exe)}: link ' + ' '.join(esc(o) for o in objs))
    lines.append(f'default {" ".join(esc(o) for o in objs) if args.compile_only else esc(exe)}')
    (out / ('host.ninja' if args.host else 'build.ninja')).write_text('\n'.join(lines) + '\n')
    print(f'{len(objs)} objects -> {exe}')
    return subprocess.call([NINJA, '-C', str(out), '-f', 'host.ninja' if args.host else 'build.ninja', f'-j{args.jobs}'])


if __name__ == '__main__':
    sys.exit(main())
