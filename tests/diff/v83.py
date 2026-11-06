"""Differential test of the ARMv8.3 instructions Switch 2 code uses that Unicorn (a Cortex-A57) cannot run.

Each game word is lifted as is, and Unicorn runs its plain ARMv8.0 equivalent at the same address:
- LDAPR{B,H} (RCpc load-acquire) against LDAR{B,H}: same value, same zero-extension.
- RETAA/RETAB against RET; BRAA/BRAB/BRAAZ/BRABZ against BR; BLRAA/BLRAB/BLRAAZ/BLRABZ against BLR.
  The host has no PAC keys and signing is lifted as identity, so an authenticated branch must act as the plain one.
- PACIA/PACIB/AUT*/XPAC* (data-processing 1-source, opcode2 1) against NOP.

Not covered here: PACGA (no ARMv8.0 equivalent; any deterministic value is valid) and FABS/FNEG on .4H/.8H
(Unicorn has no FP16; the lifted code only flips or clears bit 15 of each lane).

Usage: v83.py --exefs DIR [--cases K] [--seed S] [--cflags ...]
"""
import argparse
import os
import platform
import random
import sys
import time
import zlib
from pathlib import Path

import numpy as np
import unicorn as uc

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import BUF, BUF_SIZE, ROOT, Emu, build_harness, diff_state, dis, run_harness  # noqa: E402
from lift import BASES, is_mem, make_case, to_test_body  # noqa: E402
from lifter import Lifter, bits  # noqa: E402
from nso import Nso  # noqa: E402

NOP, RET = 0xD503201F, 0xD65F03C0


def equivalent(w):
    """ARMv8.0 word Unicorn runs in place of w, or None when w is not one of the instructions tested here."""
    if (w & 0x3FFFFC00) == 0x38BFC000:  # LDAPR{B,H}
        return (w & 0xC00003FF) | 0x08DFFC00
    if w in (0xD65F0BFF, 0xD65F0FFF):  # RETAA, RETAB
        return RET
    if (w & 0xFE9FF800) == 0xD61F0800:  # BRA*/BLRA*, with or without Z
        rn, link = bits(w, 5, 5), bits(w, 21, 1)
        return (0xD63F0000 if link else 0xD61F0000) | (rn << 5)
    if (w & 0xFFFF0000) == 0xDAC10000 and bits(w, 10, 6) <= 0b010001:  # PAC*/AUT*/XPAC*
        return NOP
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--exefs', type=Path, required=True)
    ap.add_argument('--modules', default='main,sdk,subsdk0')
    ap.add_argument('--cases', type=int, default=8)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    ap.add_argument('--cflags', default='-march=x86-64-v3' if platform.machine().lower() in ('amd64', 'x86_64') else '')
    args = ap.parse_args()
    r = random.Random(args.seed)
    t0 = time.time()
    work = ROOT / 'build' / 'test' / 'v83'
    work.mkdir(parents=True, exist_ok=True)

    images, picks = [], []
    for name in args.modules.split(','):
        nso = Nso(str(args.exefs / name))
        base = BASES[name]
        img = work / f'{name}.img'
        img.write_bytes(bytes(nso.image))
        images.append((base, img, nso.image))
        lo, size = nso.text
        words = np.frombuffer(bytes(nso.image[lo:lo + size]), dtype='<u4')
        uniq, first = np.unique(words, return_index=True)
        lifter = Lifter(base, 'c')
        n = 0
        for w, i in zip(uniq.tolist(), first.tolist()):
            eq = equivalent(w)
            if eq is None:
                continue
            pc = base + lo + i * 4
            picks.append((name, w, eq, pc, to_test_body(lifter.lift(w, pc), pc)))
            n += 1
        print(f'{name}: {n} distinct ARMv8.3 words')

    exe = build_harness(work, [p[4] for p in picks], args.jobs, args.cflags)
    emu = Emu([(base, data) for base, _, data in images])
    mu = emu.mu
    buf0 = bytes(r.getrandbits(8) for _ in range(4096)) * (BUF_SIZE // 4096)
    mu.mem_write(BUF, buf0)
    cases, expect = [], []
    for k, (name, w, eq, pc, _) in enumerate(picks):
        orig = bytes(mu.mem_read(pc, 4))
        mu.mem_write(pc, eq.to_bytes(4, 'little'))
        mem = is_mem(w)
        for _ in range(args.cases):
            fpcr, x, sp, nzcv, v = make_case(r, w)
            try:
                ox, osp, onz, opc, ov = emu.run(pc, fpcr, x, sp, nzcv, v)
            except uc.UcError:
                if mem:
                    mu.mem_write(BUF, buf0)
                continue
            mh = 0
            if mem:
                mh = zlib.crc32(bytes(mu.mem_read(BUF, BUF_SIZE)))
                mu.mem_write(BUF, buf0)
            cases.append((k, fpcr, x, sp, nzcv, v, mem))
            expect.append((ox, osp, onz, opc, ov, mh))
        mu.mem_write(pc, orig)

    recs, err = run_harness(exe, work, buf0, cases, [(base, img) for base, img, _ in images])
    if err:
        print(f'harness failed: {err}')
    fails = {}
    for case, want, got in zip(cases, expect, recs):
        name, w, eq, pc, _ = picks[case[0]]
        d = diff_state(got, want, check_pc=got[3] != 0, check_mem=case[6])
        if d:
            fails.setdefault(w, (name, pc, eq, d[:4]))
    print(f'{len(picks)} encodings, {len(cases)} cases: {len(fails)} failing encodings ({time.time() - t0:.0f}s)')
    for w, (name, pc, eq, d) in list(fails.items())[:40]:
        print(f'  {name:8} {w:08x} {dis(w, pc):32} as {dis(eq, pc):24} {"; ".join(d)}')
    return 1 if fails or err or not cases else 0


if __name__ == '__main__':
    sys.exit(main())
