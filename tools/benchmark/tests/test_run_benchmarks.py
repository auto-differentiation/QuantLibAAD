import json
import shutil
import sys

import pytest

import run_benchmarks


def test_schedule_interleaves_the_builds_and_alternates_which_runs_first():
    assert run_benchmarks.schedule(3) == [
        (1, "double"), (1, "aad"),
        (2, "aad"), (2, "double"),
        (3, "double"), (3, "aad"),
    ]


FAKE_EXE = """#!{python}
import os, sys
with open({calls!r}, "a") as fh:
    fh.write({build!r} + " " + os.path.basename(sys.argv[0]) + " " + " ".join(sys.argv[1:]) + "\\n")
for arg in sys.argv[1:]:
    if arg.startswith("--logger="):
        with open(arg.rsplit(",", 1)[1], "w") as fh:
            fh.write("junit of " + {build!r})
print("output of " + {build!r}, flush=True)
if sys.argv[0].endswith("quantlib-benchmark") and {table!r}:
    print("A/t : 1.5s", flush=True)
print("errors of " + {build!r}, file=sys.stderr)
"""


def fake_exe(calls, build, table=True):
    return FAKE_EXE.format(python=sys.executable, calls=str(calls), build=build, table=table)


@pytest.fixture
def builds(tmp_path):
    """Build dirs whose test-suite executables log their calls instead of running tests."""
    calls = tmp_path / "calls.log"
    for build in run_benchmarks.BUILDS:
        bin_dir = tmp_path / build / "test-suite"
        bin_dir.mkdir(parents=True)
        for exe in ("quantlib-test-suite", "quantlib-benchmark"):
            path = bin_dir / exe
            path.write_text(fake_exe(calls, build))
            path.chmod(0o755)
        write_cache(tmp_path / build, QLAAD_DISABLE_AAD="ON" if build == "double" else "OFF")
        write_compiler(tmp_path / build, "4.2.3", "GNU", "15.2.0")
    return tmp_path


def write_cache(build_dir, **entries):
    (build_dir / "CMakeCache.txt").write_text(
        "# This is the CMakeCache file.\n"
        + "".join("%s:BOOL=%s\n" % item for item in entries.items()))


def run_main(builds, *extra):
    out = builds / "results"
    run_benchmarks.main(["--build-double", str(builds / "double"), "--build-aad", str(builds / "aad"),
                         "--out", str(out), *extra])
    return out


def test_main_runs_the_suite_of_both_builds_interleaved_and_keeps_their_junit_logs(builds):
    out = run_main(builds, "--repeats", "2")

    calls = (builds / "calls.log").read_text().splitlines()
    suite_calls = [c.split()[0] for c in calls if " quantlib-test-suite " in c]
    assert suite_calls == ["double", "aad", "aad", "double"]
    assert (out / "suite-double-2.xml").read_text() == "junit of double"
    assert (out / "suite-aad-1.xml").read_text() == "junit of aad"


def test_main_passes_the_test_filter_to_the_suite(builds):
    run_main(builds, "--repeats", "1", "--run-test", "AmericanOptionTests")

    calls = (builds / "calls.log").read_text().splitlines()
    suite_calls = [c for c in calls if " quantlib-test-suite " in c]
    assert all(c.endswith(" --run_test=AmericanOptionTests") for c in suite_calls)
    assert len(suite_calls) == 2


def test_main_runs_the_benchmark_single_process_interleaved_and_keeps_its_output(builds):
    out = run_main(builds, "--repeats", "2", "--bench-size", "S")

    calls = (builds / "calls.log").read_text().splitlines()
    bench_calls = [c for c in calls if " quantlib-benchmark " in c]
    assert bench_calls == [
        "double quantlib-benchmark --verbose=2 --size=S --nProc=1",
        "aad quantlib-benchmark --verbose=2 --size=S --nProc=1",
        "aad quantlib-benchmark --verbose=2 --size=S --nProc=1",
        "double quantlib-benchmark --verbose=2 --size=S --nProc=1",
    ]
    assert (out / "bench-aad-2.txt").read_text().startswith("output of aad\n")


def test_main_can_skip_the_benchmark(builds):
    out = run_main(builds, "--repeats", "1", "--no-benchmark")

    assert " quantlib-benchmark " not in (builds / "calls.log").read_text()
    assert not list(out.glob("bench-*"))


def test_main_saves_the_given_metadata_with_the_cpu_name(builds, monkeypatch):
    monkeypatch.setattr(run_benchmarks, "cpu_name", lambda: "Some CPU")

    out = run_main(builds, "--repeats", "1", "--meta", "platform=linux-gcc", "xad=v2.2.0")

    metadata = json.loads((out / "metadata.json").read_text())
    del metadata["build options"]
    assert metadata == {
        "platform": "linux-gcc", "xad": "v2.2.0", "cpu": "Some CPU", "repeats": "1",
        "compiler": "GNU 15.2.0", "run_test": "all", "benchmark": "size 3"}


def test_cpu_name_reads_the_model_name_from_linux_cpuinfo():
    cpuinfo = "processor\t: 0\nvendor_id\t: AuthenticAMD\nmodel name\t: AMD EPYC 9R45\nflags\t\t: fpu\n"

    assert run_benchmarks.cpu_name(cpuinfo) == "AMD EPYC 9R45"


def test_main_runs_everything_but_fails_when_a_run_failed(builds):
    failing = builds / "aad" / "test-suite" / "quantlib-test-suite"
    failing.write_text(failing.read_text() + "sys.exit(1)\n")

    code = run_benchmarks.main(["--build-double", str(builds / "double"), "--build-aad",
                                str(builds / "aad"), "--out", str(builds / "results"),
                                "--repeats", "2"])

    assert code == 1
    assert len((builds / "calls.log").read_text().splitlines()) == 8


def test_compiler_name_reads_the_compiler_cmake_detected(tmp_path):
    cmake_dir = tmp_path / "CMakeFiles" / "4.1.2"
    cmake_dir.mkdir(parents=True, exist_ok=True)
    (cmake_dir / "CMakeCXXCompiler.cmake").write_text(
        'set(CMAKE_CXX_COMPILER "/usr/bin/c++")\n'
        'set(CMAKE_CXX_COMPILER_ID "GNU")\n'
        'set(CMAKE_CXX_COMPILER_VERSION "15.2.0")\n')

    assert run_benchmarks.compiler_name(str(tmp_path)) == "GNU 15.2.0"


@pytest.mark.parametrize("repeats", ["0", "-1"])
def test_main_refuses_repeats_that_are_not_positive(builds, repeats, capsys):
    with pytest.raises(SystemExit):
        run_main(builds, "--repeats", repeats)

    assert "--repeats" in capsys.readouterr().err
    assert not (builds / "calls.log").exists()


def test_main_stops_with_a_clear_error_when_an_executable_is_missing(builds, capsys):
    (builds / "aad" / "test-suite" / "quantlib-benchmark").unlink()

    with pytest.raises(SystemExit):
        run_main(builds, "--repeats", "1")

    assert "quantlib-benchmark not found" in capsys.readouterr().err
    assert not (builds / "calls.log").exists()


def test_main_refuses_metadata_without_a_value(builds, capsys):
    with pytest.raises(SystemExit):
        run_main(builds, "--repeats", "1", "--meta", "platform")

    assert "KEY=VALUE" in capsys.readouterr().err
    assert not (builds / "calls.log").exists()


def test_main_refuses_an_output_dir_with_earlier_results(builds, capsys):
    (builds / "results").mkdir()
    (builds / "results" / "suite-aad-7.xml").write_text("old run")

    with pytest.raises(SystemExit):
        run_main(builds, "--repeats", "1")

    assert "not empty" in capsys.readouterr().err
    assert not (builds / "calls.log").exists()


def test_main_keeps_the_benchmark_errors_in_its_log(builds):
    out = run_main(builds, "--repeats", "1")

    assert "errors of double\n" in (out / "bench-double-1.txt").read_text()


def write_compiler(build_dir, cmake_version, compiler_id, version):
    cmake_dir = build_dir / "CMakeFiles" / cmake_version
    cmake_dir.mkdir(parents=True, exist_ok=True)
    (cmake_dir / "CMakeCXXCompiler.cmake").write_text(
        'set(CMAKE_CXX_COMPILER_ID "%s")\nset(CMAKE_CXX_COMPILER_VERSION "%s")\n'
        % (compiler_id, version))


def test_compiler_name_takes_the_newest_cmake_version_when_there_are_several(tmp_path):
    for cmake_version, version in (("4.2.3", "15.2.0"), ("3.28.3", "13.3.0"), ("4.10.0", "16.1.0")):
        write_compiler(tmp_path, cmake_version, "GNU", version)

    assert run_benchmarks.compiler_name(str(tmp_path)) == "GNU 16.1.0"


def test_main_saves_the_run_options_with_the_metadata(builds):
    out = run_main(builds, "--repeats", "1", "--run-test", "SwapTests", "--no-benchmark")

    metadata = json.loads((out / "metadata.json").read_text())
    assert (metadata["run_test"], metadata["benchmark"]) == ("SwapTests", "not run")


def test_main_saves_the_benchmark_size_with_the_metadata(builds):
    out = run_main(builds, "--repeats", "1", "--bench-size", "S")

    metadata = json.loads((out / "metadata.json").read_text())
    assert (metadata["run_test"], metadata["benchmark"]) == ("all", "size S")


def test_main_refuses_an_ad_build_dir_configured_without_ad(builds, capsys):
    write_cache(builds / "aad", QLAAD_DISABLE_AAD="ON")

    with pytest.raises(SystemExit):
        run_main(builds, "--repeats", "1")

    assert "QLAAD_DISABLE_AAD" in capsys.readouterr().err
    assert not (builds / "calls.log").exists()


def test_main_refuses_builds_made_with_different_compilers(builds, capsys):
    write_compiler(builds / "double", "4.2.3", "GNU", "15.2.0")
    write_compiler(builds / "aad", "4.2.3", "Clang", "21.1.8")

    with pytest.raises(SystemExit):
        run_main(builds, "--repeats", "1")

    assert "GNU 15.2.0" in capsys.readouterr().err
    assert not (builds / "calls.log").exists()


def test_main_saves_the_build_options_from_the_ad_cache(builds):
    for build, disable_aad in (("double", "ON"), ("aad", "OFF")):
        write_cache(builds / build, QLAAD_DISABLE_AAD=disable_aad, XAD_LINEAR_SLOTS="ON",
                    QLAAD_LTO="ON", QL_NULL_AS_FUNCTIONS="ON")

    options = build_options(run_main(builds, "--repeats", "1"))

    assert (options["QLAAD_LTO"], options["XAD_LINEAR_SLOTS"]) == ("ON", "ON")
    assert options["CMAKE_BUILD_TYPE"] == "n/a"
    assert "QL_NULL_AS_FUNCTIONS" not in options


def test_main_saves_the_xad_options_from_the_config_header_xad_generated(builds):
    for build in run_benchmarks.BUILDS:
        config = builds / build / "xad" / "src" / "XAD" / "Config.hpp"
        config.parent.mkdir(parents=True)
        config.write_text("#define XAD_LINEAR_SLOTS\n/* #undef XAD_HUGE_PAGES */\n"
                          "#define XAD_ALLOW_INT_CONVERSION\n")

    options = build_options(run_main(builds, "--repeats", "1"))

    assert (options["XAD_LINEAR_SLOTS"], options["XAD_HUGE_PAGES"]) == ("ON", "OFF")


def test_main_succeeds_when_every_run_passed(builds):
    assert run_main_code(builds, "--repeats", "1") == 0


def test_main_fails_a_benchmark_run_without_its_timing_table(builds):
    bench = builds / "aad" / "test-suite" / "quantlib-benchmark"
    bench.write_text(fake_exe(builds / "calls.log", "aad", table=False))

    assert run_main_code(builds, "--repeats", "1") == 1


def run_main_code(builds, *extra):
    return run_benchmarks.main(["--build-double", str(builds / "double"), "--build-aad",
                                str(builds / "aad"), "--out", str(builds / "results"), *extra])


def test_main_refuses_builds_whose_compiler_it_cannot_tell(builds, capsys):
    for build in run_benchmarks.BUILDS:
        shutil.rmtree(builds / build / "CMakeFiles")

    with pytest.raises(SystemExit):
        run_main(builds, "--repeats", "1")

    assert "compiler" in capsys.readouterr().err
    assert not (builds / "calls.log").exists()


def build_options(out):
    text = json.loads((out / "metadata.json").read_text())["build options"]
    return dict(item.split("=", 1) for item in text.split())


@pytest.mark.parametrize("library, kind", [
    ("libQuantLib.a", "static"), ("libQuantLib.so", "shared"), ("QuantLib-x64-mt.lib", "static"),
    ("QuantLib-x64-mt.dll", "shared"), (None, "n/a")])
def test_main_saves_whether_quantlib_was_built_static_or_shared(builds, library, kind):
    for build in run_benchmarks.BUILDS if library else ():
        (builds / build / "ql").mkdir()
        (builds / build / "ql" / library).write_text("")

    assert build_options(run_main(builds, "--repeats", "1"))["library"] == kind


def test_main_refuses_builds_with_different_build_options(builds, capsys):
    write_cache(builds / "double", QLAAD_DISABLE_AAD="ON", QLAAD_LTO="OFF")
    write_cache(builds / "aad", QLAAD_DISABLE_AAD="OFF", QLAAD_LTO="ON")

    with pytest.raises(SystemExit):
        run_main(builds, "--repeats", "1")

    assert "QLAAD_LTO" in capsys.readouterr().err
    assert not (builds / "calls.log").exists()
