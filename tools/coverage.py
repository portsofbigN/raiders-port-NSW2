"""Run the lifter over every 4-byte word of each module's .text and group the Unsupported reasons.

Usage: coverage.py <exefs dir> [module ...]
Prints, per module, the count of words that fail to lift, grouped by reason with one example encoding.
"""
import sys, os, collections, multiprocessing as mp, struct
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'recomp'))
from nso import Nso
import lifter
import capstone

_code = None
_base = None

def _init(code, base):
    global _code, _base, _lf, _md
    _code, _base = code, base
    _lf = lifter.Lifter(0, simd='c')
    _md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)

def _work(rng):
    lo, hi = rng
    fails = collections.Counter(); ex = {}
    for off in range(lo, hi, 4):
        w = struct.unpack_from('<I', _code, off)[0]
        try:
            _lf.lift(w, _base + off)
        except Exception as e:
            reason = f'{type(e).__name__}: {str(e).split(" 0x")[0][:60]}'
            # group by mnemonic as well, so reasons like "dp_reg <word>" split meaningfully
            m = next(_md.disasm_lite(struct.pack('<I', w), 0), (0, 0, '?', ''))
            key = (reason if 'Unsupported' in reason and not any(c.isdigit() for c in reason[-9:]) else reason.rsplit(' ', 1)[0]) + ' | ' + m[2]
            fails[key] += 1
            ex.setdefault(key, (off + _base, w, m[2] + ' ' + m[3]))
    return fails, ex

def run(path, name):
    n = Nso(path)
    t0, tl = n.text
    code = bytes(n.image[t0:t0 + tl])
    chunk = 1 << 20
    rngs = [(i, min(i + chunk, tl)) for i in range(0, tl, chunk)]
    with mp.Pool(initializer=_init, initargs=(code, t0)) as pool:
        res = pool.map(_work, rngs)
    fails = collections.Counter(); ex = {}
    for f, e in res:
        fails += f
        for k, v in e.items(): ex.setdefault(k, v)
    print(f'== {name}: {tl // 4} words, {sum(fails.values())} fail to lift, {len(fails)} groups')
    for k, cnt in fails.most_common():
        a, w, txt = ex[k]
        print(f'  {cnt:8d}  {k:70s}  e.g. +{a:x} {w:08x} {txt}')

if __name__ == '__main__':
    d = sys.argv[1]
    for name in sys.argv[2:] or ['main', 'sdk', 'subsdk0', 'rtld']:
        run(os.path.join(d, name), name)
