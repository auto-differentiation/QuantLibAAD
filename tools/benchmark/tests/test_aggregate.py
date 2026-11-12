import csv
import json
import pytest

import aggregate

JUNIT = """<?xml version="1.0" encoding="UTF-8"?>
<testsuite tests="3" skipped="1" errors="0" failures="1" id="0" name="QuantLibTests" time="1.5">
<testcase assertions="9" classname="QuantLibTests.AmericanOptionTests" name="testBinomialTheta" time="0.062173">
<system-out><![CDATA[MESSAGE: Testing binomial theta...]]></system-out>
</testcase>
<testcase assertions="1" classname="QuantLibTests.SwapTests" name="testFairRate" time="0.5">
<failure message="check failed" type="assertion error"><![CDATA[ASSERTION FAILURE]]></failure>
</testcase>
<testcase assertions="0" classname="QuantLibTests.SwapTests" name="testCachedValue" time="0">
<skipped/>
</testcase>
</testsuite>
"""


def test_parse_junit_returns_times_of_passed_tests_keyed_by_suite_and_name():
    times = aggregate.parse_junit(JUNIT)

    assert times == {"AmericanOptionTests/testBinomialTheta": 0.062173}


def test_parse_junit_skips_tests_that_threw():
    log = """<testsuite name="QuantLibTests">
<testcase classname="QuantLibTests.SwapTests" name="testThrows" time="0.2">
<error message="uncaught exception" type="uncaught exception"><![CDATA[boom]]></error>
</testcase>
</testsuite>"""

    assert aggregate.parse_junit(log) == {}

BENCH = (
    "Running 1 test case...\n"
    "\x1b[1;32;49m*** No errors detected\n"
    "Benchmark Runtime     = 335.137s\n"
    "------------------------------------------------------------------\n"
    "                       Total Runtime spent in each test \n"
    "------------------------------------------------------------------\n"
    "NthToDefaultTests/testGauss                         : 24.0252s\n"
    "FunctionsTests/testFactorial                        : 0.008927s\n"
    "------------------------------------------------------------------\n"
)


def test_parse_bench_returns_the_per_test_runtime_table():
    times = aggregate.parse_bench(BENCH)

    assert times == {"NthToDefaultTests/testGauss": 24.0252,
                     "FunctionsTests/testFactorial": 0.008927}


def test_median_times_takes_the_median_over_runs_of_tests_timed_in_every_run():
    runs = [{"A/t": 1.0, "B/t": 5.0},
            {"A/t": 3.0},
            {"A/t": 2.5, "B/t": 6.0}]

    assert aggregate.median_times(runs) == {"A/t": 2.5}


def test_overhead_rows_pair_tests_timed_in_both_builds_with_their_ratio():
    double = {"B/t": 2.0, "A/t": 0.5, "OnlyDouble/t": 1.0}
    aad = {"A/t": 0.6, "B/t": 3.0, "OnlyAad/t": 1.0}

    rows = aggregate.overhead_rows(double, aad)

    assert rows == [
        {"test": "A/t", "double_s": 0.5, "aad_s": 0.6, "ratio": 0.6 / 0.5},
        {"test": "B/t", "double_s": 2.0, "aad_s": 3.0, "ratio": 1.5},
    ]



def test_overhead_rows_leave_the_ratio_empty_when_the_double_time_is_zero():
    rows = aggregate.overhead_rows({"A/t": 0.0}, {"A/t": 0.001})

    assert rows[0]["ratio"] is None


def row(test, double_s, aad_s):
    return {"test": test, "double_s": double_s, "aad_s": aad_s, "ratio": aad_s / double_s}


def test_summarize_gives_median_and_geometric_mean_of_the_ratios():
    rows = [row("A/t", 1.0, 1.0), row("B/t", 1.0, 2.0), row("C/t", 1.0, 4.0), row("D/t", 1.0, 8.0)]

    summary = aggregate.summarize(rows, min_time=0.0, worst_n=20)

    assert summary["median_ratio"] == pytest.approx(3.0)
    assert summary["geomean_ratio"] == pytest.approx(8 ** 0.5)


def test_summarize_leaves_out_tests_shorter_than_min_time_or_without_ratio():
    rows = [row("A/t", 1.0, 2.0), row("B/t", 1.0, 2.0), row("Short/t", 0.001, 0.1),
            {"test": "Zero/t", "double_s": 0.0, "aad_s": 0.1, "ratio": None}]

    summary = aggregate.summarize(rows, min_time=0.01, worst_n=20)

    assert summary["timed_tests"] == 2
    assert summary["median_ratio"] == pytest.approx(2.0)
    assert summary["geomean_ratio"] == pytest.approx(2.0)


def test_summarize_gives_the_total_time_ratio_over_all_tests_in_both_builds():
    rows = [row("Long/t", 10.0, 11.0), row("Short/t", 0.001, 0.009),
            {"test": "Zero/t", "double_s": 0.0, "aad_s": 0.5, "ratio": None}]

    summary = aggregate.summarize(rows, min_time=0.01, worst_n=20)

    assert summary["tests"] == 3
    assert summary["total_double_s"] == pytest.approx(10.001)
    assert summary["total_aad_s"] == pytest.approx(11.509)
    assert summary["total_ratio"] == pytest.approx(11.509 / 10.001)


def test_summarize_lists_the_worst_tests_by_ratio_highest_first():
    rows = [row("A/t", 1.0, 1.1), row("B/t", 1.0, 3.0), row("C/t", 1.0, 2.0),
            row("Short/t", 0.001, 0.1)]

    summary = aggregate.summarize(rows, min_time=0.01, worst_n=2)

    assert [r["test"] for r in summary["worst"]] == ["B/t", "C/t"]


def test_summarize_without_timed_tests_has_no_ratio_statistics():
    summary = aggregate.summarize([row("Short/t", 0.001, 0.002)], min_time=0.01, worst_n=20)

    assert summary["median_ratio"] is None
    assert summary["geomean_ratio"] is None
    assert summary["worst"] == []


def test_markdown_summary_shows_the_ratios_and_a_worst_tests_table():
    rows = [row("A/t", 10.0, 11.0), row("B/t", 1.0, 2.5)]
    summary = aggregate.summarize(rows, min_time=0.01, worst_n=20)

    text = aggregate.markdown(summary, "Test suite", {"double": 3, "aad": 3})

    assert "### Test suite" in text
    assert "| Total run time, sum over the 2 tests in both builds | 1.23x |" in text
    assert "| Median per-test ratio | 1.80x |" in text
    assert "| Geometric mean per-test ratio | 1.66x |" in text
    assert "| `B/t` | 1.000 | 2.500 | 2.50x |" in text
    assert text.index("| `B/t` |") < text.index("| `A/t` |")


def junit(times):
    cases = "".join('<testcase classname="QuantLibTests.%s" name="%s" time="%s"/>'
                    % (*test.split("/"), t) for test, t in times.items())
    return '<testsuite name="QuantLibTests">%s</testsuite>' % cases


def bench(times):
    return "".join("%s : %ss\n" % (test, t) for test, t in times.items())


@pytest.fixture
def results(tmp_path):
    (tmp_path / "suite-double-1.xml").write_text(junit({"A/t": 1.0, "B/t": 2.0}))
    (tmp_path / "suite-aad-1.xml").write_text(junit({"A/t": 1.5, "B/t": 2.0}))
    (tmp_path / "suite-double-2.xml").write_text(junit({"A/t": 1.2, "B/t": 2.2}))
    (tmp_path / "suite-aad-2.xml").write_text(junit({"A/t": 1.5, "B/t": 2.2}))
    (tmp_path / "bench-double-1.txt").write_text(bench({"C/t": 4.0}))
    (tmp_path / "bench-aad-1.txt").write_text(bench({"C/t": 5.0}))
    return tmp_path


def test_main_writes_a_csv_row_per_test_and_source(results):
    csv_path = results / "results.csv"

    aggregate.main([str(results), "--csv", str(csv_path)])

    with open(csv_path, newline="") as fh:
        rows = list(csv.DictReader(fh))
    assert [(r["source"], r["test"], r["double_s"], r["aad_s"], r["ratio"]) for r in rows] == [
        ("suite", "A/t", "1.1", "1.5", "1.3636"),
        ("suite", "B/t", "2.1", "2.1", "1.0000"),
        ("benchmark", "C/t", "4", "5", "1.2500"),
    ]


def test_main_appends_a_summary_of_both_sources_headed_by_the_metadata(results):
    (results / "metadata.json").write_text(json.dumps({"platform": "linux-gcc", "cpu": "Some CPU"}))
    summary_path = results / "summary.md"
    summary_path.write_text("earlier steps\n")

    aggregate.main([str(results), "--summary", str(summary_path), "--min-time", "0"])

    text = summary_path.read_text()
    assert text.startswith("earlier steps\n## AD overhead: linux-gcc\n")
    assert "| cpu | Some CPU |" in text
    assert text.index("### QuantLib test suite") < text.index("### QuantLib benchmark")
    assert "| `C/t` | 4.000 | 5.000 | 1.25x |" in text


def test_summarize_without_tests_has_no_total_ratio():
    summary = aggregate.summarize([], min_time=0.01, worst_n=20)

    assert summary["tests"] == 0
    assert summary["total_ratio"] is None


def test_main_prints_the_summary(results, capsys):
    aggregate.main([str(results), "--min-time", "0"])

    out = capsys.readouterr().out
    assert "### QuantLib test suite" in out
    assert "| Median per-test ratio | 1.18x |" in out


def read_csv(path):
    with open(path, newline="") as fh:
        return [(r["source"], r["test"], r["ratio"]) for r in csv.DictReader(fh)]


def test_main_skips_a_benchmark_run_without_a_table(results):
    (results / "bench-aad-2.txt").write_text("benchmark crashed\n")

    aggregate.main([str(results)])

    assert ("benchmark", "C/t", "1.2500") in read_csv(results / "results.csv")


def test_main_skips_a_suite_log_that_is_not_valid_xml(results):
    (results / "suite-aad-3.xml").write_text('<testsuite name="QuantLibTests"><testcase classname=')

    aggregate.main([str(results)])

    assert ("suite", "A/t", "1.3636") in read_csv(results / "results.csv")


def test_main_leaves_out_tests_with_a_time_that_is_not_positive(results):
    (results / "suite-double-2.xml").write_text(junit({"A/t": 1.2, "B/t": -1.0}))
    (results / "suite-aad-2.xml").write_text(junit({"A/t": 0.0, "B/t": 2.2}))

    aggregate.main([str(results)])

    assert [t for s, t, _ in read_csv(results / "results.csv") if s == "suite"] == []


def test_summarize_leaves_out_ratios_that_are_not_positive():
    rows = [row("A/t", 1.0, 2.0), row("B/t", 1.0, 8.0), row("Zero/t", 1.0, 0.0)]

    summary = aggregate.summarize(rows, min_time=0.01, worst_n=20)

    assert summary["timed_tests"] == 2
    assert summary["geomean_ratio"] == pytest.approx(4.0)


def test_main_shows_how_many_runs_of_each_build_were_used(results, capsys):
    (results / "bench-double-2.txt").write_text("")
    (results / "bench-aad-2.txt").write_text(bench({"C/t": 5.1}))

    aggregate.main([str(results)])

    suite, benchmark = capsys.readouterr().out.split("### QuantLib benchmark")
    assert "| Runs used, double / AD | 2 / 2 |" in suite
    assert "| Runs used, double / AD | 1 / 2 |" in benchmark


def test_markdown_shows_test_names_as_code_with_pipes_escaped():
    summary = aggregate.summarize([row("A/t<int>|x", 1.0, 2.0)], min_time=0.01, worst_n=20)

    text = aggregate.markdown(summary, "Test suite", {"double": 1, "aad": 1})

    assert "| `A/t<int>\\|x` | 1.000 | 2.000 | 2.00x |" in text


def test_parse_junit_skips_testcases_without_classname_or_time():
    log = """<testsuite name="QuantLibTests">
<testcase name="noClass" time="0.2"/>
<testcase classname="QuantLibTests.SwapTests" name="noTime"/>
<testcase classname="QuantLibTests.SwapTests" name="testFairRate" time="0.5"/>
</testsuite>"""

    assert aggregate.parse_junit(log) == {"SwapTests/testFairRate": 0.5}


def test_parse_junit_keeps_nested_suite_names_below_the_master_suite():
    log = """<testsuite name="QuantLibTests">
<testcase classname="QuantLibTests.Outer.Inner" name="testX" time="0.5"/>
</testsuite>"""

    assert aggregate.parse_junit(log) == {"Outer.Inner/testX": 0.5}


def test_main_skips_a_suite_log_with_a_time_that_is_not_a_number(results, capsys):
    (results / "suite-aad-3.xml").write_text(
        '<testsuite><testcase classname="QuantLibTests.A" name="t" time="abc"/></testsuite>')

    aggregate.main([str(results)])

    assert ("suite", "A/t", "1.3636") in read_csv(results / "results.csv")
    assert "suite-aad-3.xml" in capsys.readouterr().err


def test_parse_bench_drops_tests_that_threw_in_any_repetition():
    text = (BENCH
            + "Error: caught exception in benchmark NthToDefaultTests/testGauss\n"
            + "Message: boom\n"
            + "error: caught unknown exception in benchmark Other/testX\n")

    assert aggregate.parse_bench(text) == {"FunctionsTests/testFactorial": 0.008927}


def test_main_stops_with_a_clear_error_when_the_results_dir_is_missing(tmp_path, capsys):
    with pytest.raises(SystemExit):
        aggregate.main([str(tmp_path / "missing")])

    assert "is not a directory" in capsys.readouterr().err


def test_markdown_says_what_the_total_covers_and_the_minimum_time_for_per_test_ratios():
    rows = [row("A/t", 10.0, 11.0), row("Short/t", 0.001, 0.002)]
    summary = aggregate.summarize(rows, min_time=0.05, worst_n=20)

    text = aggregate.markdown(summary, "Test suite", {"double": 1, "aad": 1})

    assert "| Total run time, sum over the 2 tests in both builds | 1.10x |" in text
    assert "| Tests with at least 0.05 s in double, used for per-test ratios | 1 |" in text


def test_main_succeeds_when_every_source_has_runs_of_both_builds(results):
    assert aggregate.main([str(results)]) == 0


def test_main_fails_when_a_source_has_no_usable_run_of_a_build(results, capsys):
    (results / "bench-aad-1.txt").write_text("benchmark crashed\n")

    code = aggregate.main([str(results), "--summary", str(results / "summary.md")])

    assert code == 1
    assert "QuantLib benchmark" in capsys.readouterr().err
    assert "### QuantLib benchmark" in (results / "summary.md").read_text()


def test_main_accepts_no_benchmark_runs_when_the_benchmark_was_not_run(results):
    for path in results.glob("bench-*"):
        path.unlink()
    (results / "metadata.json").write_text(json.dumps({"benchmark": "not run"}))

    assert aggregate.main([str(results)]) == 0


def test_main_says_the_benchmark_times_are_totals_over_its_repetitions(results, capsys):
    aggregate.main([str(results)])

    suite, benchmark = capsys.readouterr().out.split("### QuantLib benchmark")
    assert "totals over the benchmark's --size repetitions" in benchmark
    assert "repetitions" not in suite
