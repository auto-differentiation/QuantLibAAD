# Overhead benchmark

How much slower is QuantLib when `Real` is XAD's active type `xad::AReal<double>`
and no derivatives are computed? These scripts time every QuantLib test-suite
case and the QuantLib benchmark in two builds that differ only in
`QLAAD_DISABLE_AAD` (`ON` gives `Real = double`), and report AD/double time
ratios. Ratios are stable on noisy machines where absolute times are not.

## On GitHub Actions

Run the **Overhead benchmark** workflow from the Actions tab, or:

```sh
gh workflow run overhead.yaml -f xad_ref=v2.2.0 -f repeats=3
```

The default QuantLib is a commit known to build with AD (see `ql_ref` in the
workflow). QuantLib master, or a release, can need upstream fixes before it
builds with `Real = xad::AReal<double>`.

It builds both flavours on Linux with GCC and Clang and on Windows with MSVC,
with the CI build's options and QuantLibAAD's build defaults (so link-time
optimisation only with GCC). Each job writes its summary to the run page and uploads
`overhead-<platform>` with `results.csv` and the raw logs. A test that fails in
either build makes the job fail, but the summary and the upload still happen;
the failed test is left out of the ratios. A source without any usable run of
either build also makes the job fail.

Other repositories can start it with a `repository_dispatch` of type
`xad-release` and `client_payload` `{"xad_ref": "<tag>"}`, optionally with a
`"ql_ref"`; without `xad_ref` the job fails.

## Locally

Build both flavours as the workflow does, with QuantLibAAD before xad in
`QL_EXTERNAL_SUBDIRECTORIES`, then:

```sh
python3 QuantLibAAD/tools/benchmark/run_benchmarks.py \
  --build-double build-double --build-aad build-aad --repeats 3 --out results
python3 QuantLibAAD/tools/benchmark/aggregate.py results --summary summary.md
```

`--run-test AmericanOptionTests --no-benchmark` gives a quick run on a part of
the suite. `run_benchmarks.py` checks that the two build dirs have
`QLAAD_DISABLE_AAD` `ON` and `OFF`, a compiler it can identify, the same in
both, and the same build options, and it records those options (from
`CMakeCache.txt`, XAD's generated `Config.hpp` and the QuantLib library file)
with the results.

## What is reported

- The runs alternate between the builds (double, AD, AD, double, ...), so drift
  in machine speed hits both alike.
- Per test, the median time over the runs. Left out: tests that failed in any
  run, benchmark tests that threw (the benchmark names them on stderr, which
  goes into its log), and runs whose log is empty or can't be read. The summary
  says how many runs of each build were used.
- A benchmark test whose check fails stops the whole benchmark run: it prints
  no timing table, so that run is lost and counts as failed.
- The benchmark's time for a test is the total over the `--size` repetitions it
  runs (3 by default), not one run; the `--min-time` threshold below applies to
  that total.
- **Total run time**: the per-test median times summed over all tests timed in
  both builds, AD over double. This leaves out the test framework's own time.
- **Median** and **geometric mean** of the per-test ratios, and the 20 tests
  with the highest ratio, over the tests that take at least `--min-time`
  (0.01 s) in the double build: shorter ones mostly measure timer noise.

Unit tests: `python3 -m pytest tools/benchmark/tests`.
