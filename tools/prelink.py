"""Link the ExeFS modules ahead of time at the bases recomp/layout.py chose.

Horizon's rtld relocates each module and resolves its imports against the exports of every loaded module, in load
order (main, subsdk0..9, sdk; first definition wins). This does the same once, offline, and writes each module's
memory image with relocations applied, so the runtime only maps the files.

Output, under --out:
  <mod>.img          relocated memory image (text, rodata, data, zeroed bss), mapped at the module's base
  prelink.json       per module: base, size, text/rodata/data ranges, entry; unresolved imports
Unresolved non-weak imports point at UNRESOLVED_ADDR, a page the runtime leaves unmapped, so a call through one
faults and names itself.

Usage: prelink.py --exefs DIR --gen GENERATED_DIR --out DIR
"""
import argparse
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'recomp'))
from layout import read as read_layout  # noqa: E402
from nso import Nso, R_AARCH64_ABS64, R_AARCH64_GLOB_DAT, R_AARCH64_JUMP_SLOT, R_AARCH64_RELATIVE  # noqa: E402

STB_WEAK = 2
UNRESOLVED_ADDR = 0x6FFF000000


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--exefs', type=Path, required=True)
    ap.add_argument('--gen', type=Path, required=True, help='gen.py output (for layout.json)')
    ap.add_argument('--out', type=Path, required=True)
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    mods = [(name, base, size, Nso(str(args.exefs / name))) for name, base, size in read_layout(args.gen)]
    exports = {}
    for name, base, _, n in mods:
        for sname, info, shndx, value, _ in n.exports():
            exports.setdefault(sname, base + value)

    report = {}
    for name, base, size, n in mods:
        img = bytearray(n.image)
        counts, unresolved = {}, []
        for off, typ, sym, add in n.relocs:
            if typ == R_AARCH64_RELATIVE:
                val = base + add
            elif typ in (R_AARCH64_GLOB_DAT, R_AARCH64_JUMP_SLOT, R_AARCH64_ABS64):
                sname, info, shndx, value, _ = n.syms[sym]
                if shndx:
                    s = base + value
                elif sname in exports:
                    s = exports[sname]
                elif info >> 4 == STB_WEAK:
                    s = 0
                else:
                    unresolved.append(sname)
                    s = UNRESOLVED_ADDR
                val = (s + add) & (2 ** 64 - 1) if s else 0
            else:
                raise SystemExit(f'{name}: relocation type {typ} at {off:#x}')
            struct.pack_into('<Q', img, off, val)
            counts[typ] = counts.get(typ, 0) + 1
        (args.out / f'{name}.img').write_bytes(bytes(img))
        report[name] = {
            'base': f'{base:#x}', 'size': f'{len(img):#x}',
            'text': [f'{base + n.text[0]:#x}', f'{n.text[1]:#x}'],
            'rodata': [f'{base + n.rodata[0]:#x}', f'{n.rodata[1]:#x}'],
            'data': [f'{base + n.data[0]:#x}', f'{n.data[1] + n.bss_size:#x}'],
            'relocs': {str(k): v for k, v in sorted(counts.items())},
            'unresolved': sorted(set(unresolved)),
        }
        print(f'{name}: base {base:#x} size {len(img):#x}  relocs {sum(counts.values())}  '
              f'unresolved {len(set(unresolved))}')
    (args.out / 'prelink.json').write_text(json.dumps(report, indent=1) + '\n')

    # boot.h: what the runtime needs to do rtld's hand-off (runtime/rt_min.c boot): each module's DT_INIT and the
    # SDK's nn::init::Start, which the Switch 2 rtld calls after linking (as on Switch 1).
    lines = ['// Written by tools/prelink.py from the ExeFS. Do not edit.', '#pragma once', '']
    for name, base, _, n in mods:
        init = n.dyn.get(12)   # DT_INIT
        lines.append(f'#define MOD_{name.upper()}_INIT {hex(base + init) + "ull" if init else "0"}')
    start = exports.get('_ZN2nn4init5StartEmmPFvvES2_S2_')
    if not start:
        raise SystemExit('nn::init::Start is not exported')
    lines += [f'#define SDK_INIT_START {start:#x}ull',
              '#define MOD_INIT_LIST(X) ' + ' '.join(f'X({name}, {name.upper()})' for name, *_ in mods), '']
    (args.out / 'boot.h').write_text('\n'.join(lines))


if __name__ == '__main__':
    main()
