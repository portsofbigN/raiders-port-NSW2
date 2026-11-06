"""Decompress Splatoon Raiders NSO modules and report size, symbols and instruction mix."""
import sys, os, collections
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'projetos', 'totk-pc-recomp', 'recomp'))
from nso import Nso
import capstone

def scan(path, name, outdir):
    n = Nso(path)
    os.makedirs(outdir, exist_ok=True)
    open(os.path.join(outdir, name + '.bin'), 'wb').write(n.image)
    t0, tl = n.text
    print(f'== {name}: build {n.build_id[:16]}  image {n.size/2**20:.1f} MiB  text {tl/2**20:.1f} MiB '
          f'rodata {n.rodata[1]/2**20:.1f} MiB  data {n.data[1]/2**20:.1f} MiB  bss {n.bss_size/2**20:.1f} MiB')
    try:
        print(f'   syms {len(n.syms)}  imports {len(n.imports())}  exports {len(n.exports())}')
    except Exception as e:
        print('   sym parse:', e)
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    md.skipdata = True
    c = collections.Counter(); bad = 0; total = 0
    code = bytes(n.image[t0:t0 + tl])
    for ins in md.disasm_lite(code, t0):
        total += 1
        if ins[2] == '.byte': bad += 1
        else: c[ins[2]] += 1
    interesting = ['retab','retaa','pacibsp','paciasp','autibsp','braa','blraa','blrab','ldraa','ldrab',
                   'ldapr','ldaprb','cas','casal','casp','swp','swpal','ldadd','ldaddal','ldset','ldclr',
                   'svc','mrs','ldxr','stxr','ldaxr','stlxr','ldxp','stxp','bti','irg','stg','ldg']
    print(f'   instructions {total}  undecodable {bad}  distinct mnemonics {len(c)}')
    print('   ', '  '.join(f'{m}={c[m]}' for m in interesting if c[m]))
    return c

out = sys.argv[2]
allc = collections.Counter()
for name in ['main', 'sdk', 'subsdk0', 'rtld']:
    allc += scan(os.path.join(sys.argv[1], name), name, out)
print('top 40:', allc.most_common(40))
