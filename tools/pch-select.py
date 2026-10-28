#!/usr/bin/env python3
"""Choose the precompiled headers per QuantLib target and write cmake/QLAADFastBuildPCH.cmake.

    pch-select.py <QuantLib build dir configured with -DQLAAD_PRECOMPILE_HEADERS=OFF, built> <QuantLib source dir>

A PCH may only hold headers that a file includes anyway: anything else adds declarations (another overload,
specialisation or static object) and can change the code. From the build's dependency data (ninja -t deps),
per target: greedily pick common headers maximising (files whose own includes cover the PCH's whole
closure) x (closure size). The files not covered, and the Boost.Test mains, are listed to skip the PCH.
Rerun after include changes; files added later get the PCH, so rerun when adding sources too.
"""
import argparse, collections, json, os, re, shlex, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

# object dir prefix -> (cmake target, max files without PCH or None)
TARGETS = {
    'ql/CMakeFiles/ql_library.dir/': ('ql_library', 30),
    'test-suite/CMakeFiles/ql_test.dir/': ('ql_test', None),
}
MAIN = re.compile(r'/(testsuite|quantlibtestsuite_xad|quantlibglobalfixture)\.cpp\.o$')
INC = re.compile(r'/(ql/[^/].*\.hpp)$')


def select(prefix, maxskip, alldeps, cc):
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
        if m and '/qlaad' not in h:
            cands[h] = f'<{m.group(1)}>'
        elif h.endswith('/boost/test/unit_test.hpp'):
            cands[h] = '<boost/test/unit_test.hpp>'
    cmd = shlex.split(next(e['command'] for e in cc if f' -o {prefix}' in e['command']))
    args = [a for a in cmd if a.startswith(('-D', '-I', '-std'))]
    args += [x for i, a in enumerate(cmd) if a == '-isystem' for x in (a, cmd[i + 1])]

    def closure(h):
        out = subprocess.run(['g++', *args, '-M', '-x', 'c++', h], capture_output=True, text=True).stdout
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
    return [cands[h] for h in sorted(picks, key=lambda h: -freq[h])], skip + mains


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('build')
    ap.add_argument('quantlib')
    ap.add_argument('-o', '--out', default=os.path.join(os.path.dirname(__file__), '../cmake/QLAADFastBuildPCH.cmake'))
    a = ap.parse_args()
    engine = os.path.realpath(a.quantlib)
    txt = subprocess.run(['ninja', '-C', a.build, '-t', 'deps'], capture_output=True, text=True, check=True).stdout
    alldeps = {}
    for b in txt.split('\n\n'):
        lines = b.strip().splitlines()
        if lines and lines[0].split(':')[0].endswith('.o'):
            alldeps[lines[0].split(':')[0]] = {os.path.normpath(l.strip()) for l in lines[1:]}
    cc = json.loads(subprocess.run(['ninja', '-C', a.build, '-t', 'compdb'], capture_output=True, text=True, check=True).stdout)
    src = {re.findall(r' -o (\S+)', e['command'])[-1]: e['file'] for e in cc if ' -o ' in e['command']}
    out = ['# Precompiled headers per QuantLib target (tools/pch-select.py on a build without PCH, ninja -t deps):',
           '# only headers every file of the target includes anyway, so the PCH adds no declarations.',
           '# qlaad_pch_<n>: target, headers; qlaad_pch_<n>_skip: its files (relative to QuantLib) that keep no PCH.']
    for i, (prefix, (target, maxskip)) in enumerate(TARGETS.items()):
        if not any(o.startswith(prefix) for o in alldeps):
            continue
        hdrs, skip = select(prefix, maxskip, alldeps, cc)
        out.append(f'set(qlaad_pch_{i} "{target}" {" ".join(hdrs)})')
        files = []
        for o in skip:
            f = os.path.realpath(src[o])
            files.append(os.path.relpath(f, engine))
        out.append(f'set(qlaad_pch_{i}_skip\n    ' + '\n    '.join(files) + ')')
    open(a.out, 'w').write('\n'.join(out) + '\n')


if __name__ == '__main__':
    main()
