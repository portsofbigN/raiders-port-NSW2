"""Names the host addresses in a samples.txt (host/nx2_sampler.c) with the PDB and sums the samples per function.
usage: python tools/symbolize_samples.py <samples.txt> [top]"""
import collections
import subprocess
import sys

EXE = r'C:/Program Files (x86)/claudinho/splatoon-recomp/build/splatoon_host.exe'
SYM = r'C:/Program Files/LLVM/bin/llvm-symbolizer.exe'
BASE = 0x140000000   # the executable's preferred base (samples are offsets from the loaded base)

path = sys.argv[1]
top = int(sys.argv[2]) if len(sys.argv) > 2 else 40
lines = open(path).read().splitlines()
print(lines[0])
host, guest = [], collections.Counter()
for ln in lines[1:]:
    if ln.startswith('#'):
        print(ln)
        continue
    n, kind, what = ln.split(' ', 2)
    if kind == 'host':
        host.append((int(n), int(what, 16)))
    else:
        guest[what.split(' (')[0] if '(' not in what else what] += int(n)
addrs = '\n'.join('0x%x' % (BASE + off) for _, off in host)
out = subprocess.run([SYM, '--obj=' + EXE, '--functions=linkage', '--no-inlines', '--output-style=GNU'],
                     input=addrs, capture_output=True, text=True).stdout.splitlines()
per = collections.Counter()
i = 0
for n, off in host:
    name = out[2 * i] if 2 * i < len(out) else '?'
    if off == 1:
        name = '<outside the executable: system / driver>'
    per[name] += n
    i += 1
total = sum(per.values()) + sum(guest.values())
print('host functions:')
for name, n in per.most_common(top):
    print('%6d %5.1f%%  %s' % (n, 100.0 * n / total, name))
print('game (lifted) functions:')
for name, n in guest.most_common(10):
    print('%6d %5.1f%%  %s' % (n, 100.0 * n / total, name))
