#!/usr/bin/env python3
"""Choose the precompiled headers per QuantLib target and write cmake/QLAADFastBuildPCH.cmake.

    pch-select.py <QuantLib build dir> <QuantLib source dir> [-o <output>] [--prefix <dir>/]

The build dir must be configured with QLAAD_UNITY_BATCH_SIZE=0 and QLAAD_PRECOMPILE_HEADERS=OFF, and built.

A PCH may only hold headers that a file includes anyway: anything else adds declarations (another overload,
specialisation or static object) and can change the code. From the build's dependency data (ninja -t deps),
per target: greedily pick common headers maximising (files whose own includes cover the PCH's whole
closure) x (closure size). The files not covered, and the Boost.Test mains, are listed to skip the PCH.
The header closures use the build's own compiler and its -D, -I, -isystem and -std flags (from ninja -t compdb).
<ql/qldefines.hpp> comes first, so that it sets BOOST_ENABLE_ASSERT_HANDLER before any Boost header.
--prefix: QuantLib's build dir below the build dir, when QuantLib is a subproject.
Rerun after include changes; files added later get the PCH, so rerun when adding sources too.
"""
import argparse, collections, json, os, re, shlex, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

# object dir prefix -> (cmake target, max files without PCH or None)
TARGETS = {
    'ql/CMakeFiles/ql_library.dir/': ('ql_library', 30),
    'test-suite/CMakeFiles/ql_test.dir/': ('ql_test', None),
}
MAIN = re.compile(r'/quantlibglobalfixture\.cpp\.o$')
INC = re.compile(r'/(ql/[^/].*\.hpp)$')


def select(prefix, maxskip, alldeps, cc, engine):
    deps = {o: d for o, d in alldeps.items() if o.startswith(prefix)}
    mains = sorted(o for o in deps if MAIN.search(o))
    deps = {o: d for o, d in deps.items() if o not in mains}
    n = len(deps)
    freq = collections.Counter(h for d in deps.values() for h in d)
    cands = {}
    for h, c in freq.items():
        m = INC.search(h)
        if c * 4 < n:
            continue
        if m and h.startswith(engine + os.sep):   # QuantLib's own headers, not QuantLibAAD's
            cands[h] = f'<{m.group(1)}>'
        elif h.endswith('/boost/test/unit_test.hpp'):
            cands[h] = '<boost/test/unit_test.hpp>'
    cmd = shlex.split(next(e['command'] for e in cc if f' -o {prefix}' in e['command']))
    while os.path.basename(cmd[0]) in ('ccache', 'sccache') or '=' in cmd[0]:   # launcher and its settings
        cmd = cmd[1:]
    compiler = cmd[0]
    args = [a for a in cmd if a.startswith(('-D', '-I', '-std'))]
    args += [x for i, a in enumerate(cmd) if a == '-isystem' for x in (a, cmd[i + 1])]

    def closure(h):
        out = subprocess.run([compiler, *args, '-M', '-x', 'c++', h], capture_output=True, text=True).stdout
        return h, {os.path.normpath(x) for x in out.replace('\\\n', ' ').split()[2:]} | {h}

    with ThreadPoolExecutor(min(6, os.cpu_count() or 1)) as ex:
        clo = dict(ex.map(closure, cands))
    clo = {h: c for h, c in clo.items() if sum(c <= d for d in deps.values()) * 4 >= n}
    picks, cur, best = [], set(), 0
    while True:
        pick = None
        for h, c in sorted(clo.items()):
            if h in picks:
                continue
            u = cur | c
            cov = sum(u <= d for d in deps.values())
            if maxskip is not None and n - cov > maxskip:
                continue
            if cov * len(u) > best * 1.02:
                best, pick, pu = cov * len(u), h, u
        if pick is None:
            break
        picks.append(pick)
        cur = pu
    skip = sorted(o for o, d in deps.items() if not cur <= d)
    picks = [h for h in picks if not any(h in clo[o] for o in picks if o != h)]
    print(f'{prefix}: {len(cur)} headers, {n - len(skip)}/{n} files use the PCH', file=sys.stderr)
    hdrs = [cands[h] for h in sorted(picks, key=lambda h: -freq[h])]
    # in every closure anyway (generated into the build dir)
    if any(h.endswith('/ql/qldefines.hpp') for h in cur) and '<ql/qldefines.hpp>' not in hdrs:
        hdrs.insert(0, '<ql/qldefines.hpp>')
    return hdrs, skip + mains


def fail(msg):
    sys.exit(f'pch-select.py: {msg}')


def ninja(build, tool):
    r = subprocess.run(['ninja', '-C', build, '-t', tool], capture_output=True, text=True)
    if r.returncode:
        fail(f'ninja -t {tool} failed in {build}: {r.stderr.strip()}')
    return r.stdout


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('build')
    ap.add_argument('quantlib')
    ap.add_argument('--prefix', default='')
    ap.add_argument('-o', '--out', default=os.path.join(os.path.dirname(__file__), '../cmake/QLAADFastBuildPCH.cmake'))
    a = ap.parse_args()
    engine = os.path.realpath(a.quantlib)
    cc = json.loads(ninja(a.build, 'compdb'))
    if any('/Unity/' in e['file'] for e in cc):
        fail('the build has unity sources: configure it with QLAAD_UNITY_BATCH_SIZE=0 and QLAAD_PRECOMPILE_HEADERS=OFF')
    if any('cmake_pch' in e['command'] for e in cc):
        fail('the build uses precompiled headers: configure it with QLAAD_UNITY_BATCH_SIZE=0 and QLAAD_PRECOMPILE_HEADERS=OFF')
    txt = ninja(a.build, 'deps')
    alldeps = {}
    for b in txt.split('\n\n'):
        lines = b.strip().splitlines()
        if lines and lines[0].split(':')[0].endswith('.o'):
            alldeps[lines[0].split(':')[0]] = {os.path.normpath(l.strip()) for l in lines[1:]}
    src = {re.findall(r' -o (\S+)', e['command'])[-1]: e['file'] for e in cc if ' -o ' in e['command']}
    out = ['# Precompiled headers per QuantLib target (tools/pch-select.py on a build without PCH, ninja -t deps):',
           '# only headers every file of the target includes anyway, so the PCH adds no declarations.',
           '# qlaad_pch_<n>: target, headers; qlaad_pch_<n>_skip: its files (relative to QuantLib) that keep no PCH.']
    for i, (prefix, (target, maxskip)) in enumerate(TARGETS.items()):
        prefix = a.prefix + prefix
        if not any(f' -o {prefix}' in e['command'] for e in cc):
            continue   # target not in this build
        if not any(o.startswith(prefix) for o in alldeps):
            fail(f'no dependency data for {target}: build {a.build} first')
        hdrs, skip = select(prefix, maxskip, alldeps, cc, engine)
        out.append(f'set(qlaad_pch_{i} "{target}" {" ".join(hdrs)})')
        files = []
        for o in skip:
            if o not in src:
                fail(f'no compile command for {o} in ninja -t compdb')
            f = os.path.realpath(src[o])
            files.append(os.path.relpath(f, engine))
        out.append(f'set(qlaad_pch_{i}_skip\n    ' + '\n    '.join(files) + ')')
    open(a.out, 'w').write('\n'.join(out) + '\n')


if __name__ == '__main__':
    main()
