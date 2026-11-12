#!/usr/bin/env python3
"""Summarise the overhead of QuantLib built with Real = xad::AReal<double>
(no tape recording) against the same build with Real = double, per test.

Reads the logs that run_benchmarks.py writes into a results directory and
writes a CSV of per-test ratios plus a markdown summary.

Copyright (C) 2010-2026 Xcelerit Computing Limited
SPDX-License-Identifier: AGPL-3.0-or-later
"""

import argparse
import csv
import glob
import json
import os
import re
import sys
import statistics
import xml.etree.ElementTree as ET

NOT_PASSED = ("failure", "error", "skipped")
BENCH_ROW = re.compile(r"^(\S+/\S+)\s*:\s*([0-9.eE+-]+)s$")
BENCH_THREW = re.compile(r"caught (?:unknown )?exception in benchmark (\S+)", re.IGNORECASE)


def parse_junit(text):
    """Per-test times in seconds of the passed tests in a Boost.Test JUNIT log."""
    times = {}
    for case in ET.fromstring(text).iter("testcase"):
        if any(case.find(tag) is not None for tag in NOT_PASSED):
            continue
        if case.get("classname") is None or case.get("time") is None:
            continue
        suite = case.get("classname").split(".", 1)[-1]  # without the master suite
        times[suite + "/" + case.get("name")] = float(case.get("time"))
    return times


def parse_bench(text):
    """Per-test times in seconds from the table quantlib-benchmark --verbose=2 prints,
    without the tests that threw (their time covers only part of the work)."""
    times = {}
    for line in text.splitlines():
        row = BENCH_ROW.match(line.strip())
        if row:
            times[row.group(1)] = float(row.group(2))
    for threw in BENCH_THREW.findall(text):
        times.pop(threw, None)
    return times


def median_times(runs):
    """Median time per test over repeated runs, for the tests timed in every run."""
    common = set.intersection(*(set(run) for run in runs)) if runs else set()
    return {test: statistics.median(run[test] for run in runs) for test in common}


def overhead_rows(double, aad):
    """One row per test timed in both builds, with the AD/double time ratio."""
    return [{"test": test, "double_s": double[test], "aad_s": aad[test],
             "ratio": aad[test] / double[test] if double[test] > 0 else None}
            for test in sorted(set(double) & set(aad))]


def summarize(rows, min_time, worst_n):
    """Total time ratio; median, geometric mean and worst of the per-test ratios.

    Tests whose double time is below min_time are left out: their ratio is
    mostly timer noise.
    """
    timed = [r for r in rows if r["ratio"] is not None and r["ratio"] > 0
             and r["double_s"] >= min_time]
    ratios = [r["ratio"] for r in timed]
    total_double = sum(r["double_s"] for r in rows)
    total_aad = sum(r["aad_s"] for r in rows)
    return {"tests": len(rows),
            "min_time": min_time,
            "total_double_s": total_double,
            "total_aad_s": total_aad,
            "total_ratio": total_aad / total_double if total_double > 0 else None,
            "timed_tests": len(timed),
            "median_ratio": statistics.median(ratios) if ratios else None,
            "geomean_ratio": statistics.geometric_mean(ratios) if ratios else None,
            "worst": sorted(timed, key=lambda r: r["ratio"], reverse=True)[:worst_n]}


def _times(value):
    return "n/a" if value is None else "%.2fx" % value


def _code(text):
    """Text as inline code in a markdown table cell."""
    return "`%s`" % text.replace("|", "\\|")


def markdown(summary, title, runs_used, note=""):
    """Markdown section with the headline ratios and the worst tests."""
    lines = ["### " + title, ""] + ([note, ""] if note else []) + [
             "| AD / double | |", "|---|---|",
             "| Total run time, sum over the %d tests in both builds | %s |"
             % (summary["tests"], _times(summary["total_ratio"])),
             "| Median per-test ratio | %s |" % _times(summary["median_ratio"]),
             "| Geometric mean per-test ratio | %s |" % _times(summary["geomean_ratio"]),
             "| Tests with at least %g s in double, used for per-test ratios | %d |"
             % (summary["min_time"], summary["timed_tests"]),
             "| Runs used, double / AD | %d / %d |" % (runs_used["double"], runs_used["aad"]),
             "", "| Highest overhead | double (s) | AD (s) | AD / double |",
             "|---|---|---|---|"]
    lines += ["| %s | %.3f | %.3f | %s |"
              % (_code(r["test"]), r["double_s"], r["aad_s"], _times(r["ratio"]))
              for r in summary["worst"]]
    return "\n".join(lines) + "\n"


SOURCES = (("suite", "QuantLib test suite", "suite-%s-*.xml", parse_junit),
           ("benchmark", "QuantLib benchmark", "bench-%s-*.txt", parse_bench))
NOTES = {"benchmark": "Times are totals over the benchmark's --size repetitions of each test."}


def load_runs(paths, parse):
    """Per-test times of each run, leaving out runs that can't be read or have
    no timed test (e.g. a crashed run), so that they don't empty the others.
    Times that are not positive are left out: they can't give a ratio."""
    runs = []
    for path in paths:
        try:
            with open(path, encoding="utf-8", errors="replace") as fh:
                times = parse(fh.read())
        except (OSError, ValueError, ET.ParseError) as exc:
            print("warning: skipping %s: %s" % (path, exc), file=sys.stderr)
            continue
        times = {test: t for test, t in times.items() if t > 0}
        if times:
            runs.append(times)
    return runs


def load_rows(results_dir, pattern, parse):
    """Overhead rows from the logs of both builds that match pattern, and
    the number of runs of each build they come from."""
    medians, runs_used = {}, {}
    for build in ("double", "aad"):
        runs = load_runs(sorted(glob.glob(os.path.join(results_dir, pattern % build))), parse)
        medians[build] = median_times(runs)
        runs_used[build] = len(runs)
    return overhead_rows(medians["double"], medians["aad"]), runs_used


def write_csv(path, rows_by_source):
    with open(path, "w", newline="", encoding="utf-8") as fh:
        writer = csv.writer(fh)
        writer.writerow(["source", "test", "double_s", "aad_s", "ratio"])
        for source, rows in rows_by_source.items():
            for r in rows:
                writer.writerow([source, r["test"], "%.6g" % r["double_s"], "%.6g" % r["aad_s"],
                                 "" if r["ratio"] is None else "%.4f" % r["ratio"]])


def write_summary(path, metadata, sections):
    lines = ["## AD overhead: " + metadata.get("platform", ""), "",
             "| | |", "|---|---|"]
    lines += ["| %s | %s |" % item for item in sorted(metadata.items())]
    with open(path, "a", encoding="utf-8") as fh:
        fh.write("\n".join(lines) + "\n\n" + "\n".join(sections))


def read_metadata(results_dir):
    path = os.path.join(results_dir, "metadata.json")
    if not os.path.exists(path):
        return {}
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("results_dir")
    parser.add_argument("--csv", help="default: <results_dir>/results.csv")
    parser.add_argument("--summary", help="markdown file to append to, e.g. $GITHUB_STEP_SUMMARY")
    parser.add_argument("--min-time", type=float, default=0.01,
                        help="leave tests with a shorter double time out of the per-test ratios")
    parser.add_argument("--worst", type=int, default=20)
    args = parser.parse_args(argv)
    if not os.path.isdir(args.results_dir):
        parser.error("%s is not a directory" % args.results_dir)

    loaded = {source: load_rows(args.results_dir, pattern, parse)
              for source, _, pattern, parse in SOURCES}
    write_csv(args.csv or os.path.join(args.results_dir, "results.csv"),
              {source: rows for source, (rows, _) in loaded.items()})
    sections = [markdown(summarize(loaded[source][0], args.min_time, args.worst), title,
                         loaded[source][1], NOTES.get(source, ""))
                for source, title, _, _ in SOURCES]
    print("\n".join(sections))
    metadata = read_metadata(args.results_dir)
    if args.summary:
        write_summary(args.summary, metadata, sections)
    return 0 if all_runs_present(loaded, metadata) else 1


def all_runs_present(loaded, metadata):
    """Whether every source has a usable run of each build; reports the ones that don't."""
    present = True
    for source, title, _, _ in SOURCES:
        if source == "benchmark" and metadata.get("benchmark") == "not run":
            continue
        for build, count in loaded[source][1].items():
            if not count:
                print("error: %s: no usable run of the %s build" % (title, build), file=sys.stderr)
                present = False
    return present


if __name__ == "__main__":
    sys.exit(main())
