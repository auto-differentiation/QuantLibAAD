#!/usr/bin/env python3
"""Run the QuantLib test suite and benchmark of a Real = double build and a
Real = xad::AReal<double> build, interleaved, saving the logs that
aggregate.py reads.

Copyright (C) 2010-2026 Xcelerit Computing Limited
SPDX-License-Identifier: AGPL-3.0-or-later
"""

import argparse
import glob
import json
import os
import platform
import re
import subprocess
import sys

from aggregate import parse_bench

BUILDS = ("double", "aad")
# Settings that change how fast the AD build runs, shown with the results: cache
# entries, and XAD's options from the Config.hpp it generated
BUILD_OPTIONS = ("CMAKE_BUILD_TYPE", "CMAKE_CXX_STANDARD", "QLAAD_LTO",
                 "QLAAD_FP_CONTRACT_OFF", "XAD_LINEAR_SLOTS", "XAD_USE_STRONG_INLINE",
                 "XAD_TLS_INITIAL_EXEC", "XAD_HUGE_PAGES")
EXE_SUFFIX = ".exe" if os.name == "nt" else ""


def schedule(repeats):
    """(repeat, build) pairs, interleaved so that drift in machine speed hits
    both builds alike; the order alternates so neither always runs first."""
    return [(i, build)
            for i in range(1, repeats + 1)
            for build in (BUILDS if i % 2 else BUILDS[::-1])]


def read_cpuinfo():
    try:
        with open("/proc/cpuinfo", encoding="utf-8") as fh:
            return fh.read()
    except OSError:
        return ""


def cpu_name(cpuinfo=None):
    """The CPU model from Linux's /proc/cpuinfo, else what Python reports."""
    for line in (read_cpuinfo() if cpuinfo is None else cpuinfo).splitlines():
        if line.startswith("model name"):
            return line.split(":", 1)[1].strip()
    return platform.processor()


def cmake_version(path):
    """(4, 10, 0) for .../CMakeFiles/4.10.0/CMakeCXXCompiler.cmake"""
    return tuple(int(part) for part in re.findall(r"\d+", os.path.basename(os.path.dirname(path))))


def compiler_name(build_dir):
    """Compiler id and version as CMake detected them in build_dir."""
    paths = glob.glob(os.path.join(build_dir, "CMakeFiles", "*", "CMakeCXXCompiler.cmake"))
    text = ""
    if paths:  # the newest CMake's, if the build dir was configured by several
        with open(max(paths, key=cmake_version), encoding="utf-8") as fh:
            text = fh.read()
    fields = [re.search(r'CMAKE_CXX_COMPILER_%s "([^"]*)"' % key, text) for key in ("ID", "VERSION")]
    return " ".join(m.group(1) for m in fields if m) or "unknown"


def cmake_cache(build_dir):
    """The entries of build_dir's CMakeCache.txt, by name."""
    entries = {}
    try:
        with open(os.path.join(build_dir, "CMakeCache.txt"), encoding="utf-8") as fh:
            for line in fh:
                name, sep, value = line.rstrip("\n").partition("=")
                if sep and not name.startswith(("#", "//")):
                    entries[name.split(":", 1)[0]] = value
    except OSError:
        pass
    return entries


def xad_config(build_dir):
    """XAD's options as ON/OFF from the Config.hpp it generated in build_dir."""
    paths = glob.glob(os.path.join(build_dir, "**", "XAD", "Config.hpp"), recursive=True)
    text = ""
    for path in sorted(paths)[:1]:
        with open(path, encoding="utf-8") as fh:
            text = fh.read()
    options = {name: "ON" for name in re.findall(r"^#define (XAD_\w+)\s*$", text, re.MULTILINE)}
    options.update((name, "OFF") for name in re.findall(r"#undef (XAD_\w+)", text))
    return options


def library_kind(build_dir):
    """static or shared, from the QuantLib library file in build_dir."""
    # a shared build on Windows has a .lib too, so the shared files come first
    for kind, pattern in (("shared", "libQuantLib*.so*"), ("shared", "libQuantLib*.dylib"),
                          ("shared", "QuantLib*.dll"), ("static", "libQuantLib*.a"),
                          ("static", "QuantLib*.lib")):
        if glob.glob(os.path.join(build_dir, "**", pattern), recursive=True):
            return kind
    return "n/a"


def build_options(build_dir):
    """The BUILD_OPTIONS of build_dir as NAME=VALUE (n/a when not set), and the library kind."""
    found = {**cmake_cache(build_dir), **xad_config(build_dir)}
    return " ".join(["%s=%s" % (name, found.get(name, "n/a")) for name in BUILD_OPTIONS]
                    + ["library=" + library_kind(build_dir)])


def executable(build_dir, name):
    return os.path.abspath(os.path.join(build_dir, "test-suite", name + EXE_SUFFIX))


def run(cmd, out_dir, stdout=None, stderr=None):
    """Run cmd in out_dir. A failing test must not stop the other runs: its
    test is left out of the timings by aggregate.py."""
    print("+ " + " ".join(cmd), flush=True)
    return subprocess.run(cmd, cwd=out_dir, stdout=stdout, stderr=stderr).returncode


def run_suite(exe, repeat, build, out_dir, run_test):
    # Boost.Test splits --logger at ':' and ',', so the JUNIT file name must be
    # relative (no Windows drive letter); the run's cwd is out_dir.
    sink = "suite-%s-%d.xml" % (build, repeat)
    cmd = [exe, "--logger=HRF,message,stdout:JUNIT,message," + sink]
    if run_test:
        cmd.append("--run_test=" + run_test)
    return run(cmd, out_dir)


def run_bench(exe, repeat, build, out_dir, size):
    """Runs the benchmark; a run without its timing table counts as failed."""
    log = os.path.join(out_dir, "bench-%s-%d.txt" % (build, repeat))
    with open(log, "w", encoding="utf-8") as fh:
        # stderr names the tests that threw, which aggregate.py leaves out
        code = run([exe, "--verbose=2", "--size=" + size, "--nProc=1"], out_dir, stdout=fh,
                   stderr=subprocess.STDOUT)
    with open(log, encoding="utf-8", errors="replace") as fh:
        if not parse_bench(fh.read()):
            print("error: %s has no timing table" % log, file=sys.stderr)
            return code or 1
    return code


def positive_int(text):
    value = int(text)
    if value < 1:
        raise argparse.ArgumentTypeError("must be at least 1: %s" % text)
    return value


def key_value(text):
    key, sep, value = text.partition("=")
    if not key or not sep:
        raise argparse.ArgumentTypeError("expected KEY=VALUE: %s" % text)
    return key, value


def write_metadata(out_dir, args):
    metadata = dict(args.meta)
    metadata.update(cpu=cpu_name(), compiler=compiler_name(args.build_aad),
                    repeats=str(args.repeats), run_test=args.run_test or "all",
                    benchmark="not run" if args.no_benchmark else "size " + args.bench_size)
    metadata["build options"] = build_options(args.build_aad)
    with open(os.path.join(out_dir, "metadata.json"), "w", encoding="utf-8") as fh:
        json.dump(metadata, fh, indent=2)


def check_builds(parser, build_dirs):
    """Stops unless the builds are a double and an AD build that differ in nothing else."""
    for build, disable_aad in (("double", "ON"), ("aad", "OFF")):
        if cmake_cache(build_dirs[build]).get("QLAAD_DISABLE_AAD") != disable_aad:
            parser.error("%s: the %s build needs QLAAD_DISABLE_AAD=%s in its CMakeCache.txt"
                         % (build_dirs[build], build, disable_aad))
    compilers = {build: compiler_name(build_dirs[build]) for build in BUILDS}
    if "unknown" in compilers.values():
        parser.error("can't tell the compiler of the builds: double %(double)s, AD %(aad)s"
                     % compilers)
    if compilers["double"] != compilers["aad"]:
        parser.error("the builds use different compilers: double %(double)s, AD %(aad)s"
                     % compilers)
    options = {build: build_options(build_dirs[build]) for build in BUILDS}
    if options["double"] != options["aad"]:
        parser.error("the builds differ in more than QLAAD_DISABLE_AAD: double %(double)s; "
                     "AD %(aad)s" % options)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-double", required=True, help="build dir with QLAAD_DISABLE_AAD=ON")
    parser.add_argument("--build-aad", required=True, help="build dir with QLAAD_DISABLE_AAD=OFF")
    parser.add_argument("--repeats", type=positive_int, default=3)
    parser.add_argument("--out", default="results")
    parser.add_argument("--bench-size", default="3", help="quantlib-benchmark --size")
    parser.add_argument("--no-benchmark", action="store_true", help="run the test suite only")
    parser.add_argument("--run-test", help="Boost.Test filter, e.g. AmericanOptionTests")
    parser.add_argument("--meta", nargs="*", default=[], type=key_value, metavar="KEY=VALUE",
                        help="facts about the run to show in the summary")
    args = parser.parse_args(argv)

    programs = ["quantlib-test-suite"] + ([] if args.no_benchmark else ["quantlib-benchmark"])
    build_dirs = {"double": args.build_double, "aad": args.build_aad}
    exes = {(build, name): executable(build_dirs[build], name)
            for build in BUILDS for name in programs}
    for path in exes.values():
        if not os.path.isfile(path):
            parser.error("%s not found; build the ql_test_suite and ql_benchmark targets" % path)
    check_builds(parser, build_dirs)

    out_dir = os.path.abspath(args.out)
    if os.path.isdir(out_dir) and os.listdir(out_dir):
        parser.error("%s is not empty; earlier results would mix with this run" % out_dir)
    os.makedirs(out_dir, exist_ok=True)
    write_metadata(out_dir, args)
    codes = [run_suite(exes[build, "quantlib-test-suite"], repeat, build, out_dir, args.run_test)
             for repeat, build in schedule(args.repeats)]
    if not args.no_benchmark:
        codes += [run_bench(exes[build, "quantlib-benchmark"], repeat, build, out_dir,
                            args.bench_size)
                  for repeat, build in schedule(args.repeats)]
    return 1 if any(codes) else 0


if __name__ == "__main__":
    sys.exit(main())
