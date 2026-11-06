"""Assemble the ready-to-play folder: dist/Splatoon Raiders Recomp.

Copies the built program (build/splatoon_host.exe, as SplatoonRaiders.exe), the guides (PT and EN) and, when the
folder has none yet, the shader translation cache (build/shader_cache.bin). Never removes anything: the player's
splatoon.ini, profile (saves), logs and shader cache are left as they are; a previous program or guide that differs
is moved to work/shots_old/dist_<time>/ before the new one is copied.

Usage: package.py [--out DIR] [--no-cache]
"""
import argparse
import filecmp
import os
import shutil
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FILES = [  # (source, name in the folder)
    (ROOT / 'build' / 'splatoon_host.exe', 'SplatoonRaiders.exe'),
    (ROOT / 'COMO_JOGAR.md', 'COMO_JOGAR.md'),
    (ROOT / 'HOW_TO_PLAY.md', 'HOW_TO_PLAY.md'),
]


def copy_in(src: Path, dst: Path, aside: Path) -> str:
    if dst.exists():
        if filecmp.cmp(src, dst, shallow=False):
            return 'unchanged'
        aside.mkdir(parents=True, exist_ok=True)
        shutil.move(str(dst), str(aside / dst.name))
    tmp = dst.with_name(dst.name + '.new')
    shutil.copyfile(src, tmp)
    if tmp.stat().st_size != src.stat().st_size:
        raise SystemExit(f'copy of {src} is incomplete')
    os.replace(tmp, dst)
    return 'copied'


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--out', type=Path, default=ROOT / 'dist' / 'Splatoon Raiders Recomp')
    ap.add_argument('--no-cache', action='store_true', help='do not seed shader_cache.bin')
    args = ap.parse_args()
    out = args.out
    out.mkdir(parents=True, exist_ok=True)
    aside = ROOT / 'work' / 'shots_old' / time.strftime('dist_%Y%m%d_%H%M%S')
    for src, name in FILES:
        if not src.exists():
            raise SystemExit(f'missing {src} (build first: tools/build.py --host)')
        print(f'{name}: {copy_in(src, out / name, aside)}')
    cache = ROOT / 'build' / 'shader_cache.bin'
    if not args.no_cache and cache.exists() and not (out / 'shader_cache.bin').exists():
        print(f'shader_cache.bin: {copy_in(cache, out / "shader_cache.bin", aside)}')
    total = sum(f.stat().st_size for f in out.rglob('*') if f.is_file())
    print(f'{out}: {total / 1048576:.0f} MiB')


if __name__ == '__main__':
    main()
