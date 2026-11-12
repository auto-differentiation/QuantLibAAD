# QuantLibAAD: QuantLib with XAD Automatic Differentiation in C++

<p align="center" dir="auto">
    <a href="https://github.com/auto-differentiation/QuantLibAAD/actions/workflows/ci.yaml">
        <img src="https://img.shields.io/github/actions/workflow/status/auto-differentiation/QuantLibAAD/ci.yaml?label=Build%20%28XAD%20main%20vs%20QuantLib%20master%29" alt="GitHub Workflow Status" style="max-width: 100%;margin-right:20px">
    </a>
    <a href="https://github.com/auto-differentiation/QuantLibAAD/blob/main/CONTRIBUTING.md">
        <img src="https://img.shields.io/badge/PRs%20-welcome-brightgreen.svg" alt="PRs Welcome" style="max-width: 100%;">
    </a>
</p>

> Part of the [XAD ecosystem](https://github.com/auto-differentiation) - the fastest automatic differentiation library for C++.
> If this is useful to you, please star the [core XAD repository](https://github.com/auto-differentiation/xad).

QuantLibAAD builds QuantLib with [XAD](https://github.com/auto-differentiation/xad), so a pricing can compute all its
risks (sensitivities) in one adjoint pass. QuantLib's library code is unchanged; your code registers the inputs, records
the pricing and runs the adjoint, as the samples show. That is 4× to 29× faster than bumping on the QuantLib samples
below, and up to 270× faster on Monte Carlo with XAD's commercial code generation add-on. It shows XAD in production-quality use on a code base of
several hundred thousand lines.

This repository contains integration headers, examples, and tests required
for this integration.
It is not usable stand-alone.

## Performance

Risk with XAD costs little more than pricing alone, and far less than bumping.

### Cost of AD types

About 10% overhead on QuantLib's examples and test suite: QuantLib built with XAD's active type, running without
computing derivatives, against the same build with `double`:

| | AD / double |
|---|---|
| QuantLib test suite (1,714 cases) | 1.13× |
| QuantLib benchmark | 1.21× |
| QuantLib examples (20) | 1.10× |

### Sensitivities: AD vs bump-and-revalue

All sensitivities in one adjoint pass, against one-sided bumping, one revaluation per input:

| Sample | Inputs | XAD | Bumping |
|---|---|---|---|
| AdjointEuropeanEquityOption | 98 | **29×** faster · 0.24 s | 6.90 s |
| AdjointMulticurveBootstrapping | 69 | **27×** faster · 1.85 s | 50.5 s |
| AdjointSwap | 55 | **26×** faster · 0.45 s | 11.5 s |
| AdjointReplication | 13 | **4.4×** faster · 1.33 s | 5.81 s |

A full set of sensitivities costs 2–3.5 valuations.

### Monte Carlo: tape, split mode and JIT

`benchmark_aad`, 100,000 paths:

| | Swaption (45 inputs) | CVA (90 inputs) |
|---|---|---|
| Bumping | 24.5 s | 48.9 s |
| XAD tape | **6.8×** faster · 3.62 s | **13×** faster · 3.63 s |
| XAD split mode | **11×** faster · 2.18 s | **22×** faster · 2.21 s |
| JIT with xad-codegen ¹ | **158×** faster · 0.15 s | **270×** faster · 0.18 s |

¹ xad-codegen is XAD's native code generation backend, available under a commercial license.

### Setup

AMD EPYC (Zen 5), one core, Linux, GCC 15, XAD 2.2.0, release build; median of 3 runs.

Using QuantLibAAD in production under commercial terms, with support, the XAD validation pack or code generation: see [XAD subscriptions](https://www.xcelerit.com/xad/) or [contact us](https://www.xcelerit.com/xad-enterprise-support).

## Getting Started

For detailed build instructions with [XAD](https://auto-differentiation.github.io) and [QuantLib](https://www.quantlib.org), please refer to the [XAD documentation site](https://auto-differentiation.github.io/quantlib-risks/cxx/).

### Build options

The defaults are tuned for speed: fast AD code, and fast builds of QuantLib and its tests.
They need QuantLibAAD listed before XAD in `QL_EXTERNAL_SUBDIRECTORIES`, e.g. `-DQL_EXTERNAL_SUBDIRECTORIES="<path>/QuantLibAAD;<path>/xad"`; with XAD first, CMake warns which settings it misses.

Any of them can be switched off on the QuantLib command line:
XAD's tape options with `-DXAD_<option>=OFF` (`XAD_USE_STRONG_INLINE`, `XAD_LINEAR_SLOTS`, `XAD_TLS_INITIAL_EXEC`, `XAD_HUGE_PAGES`),
a shared QuantLib with `-DBUILD_SHARED_LIBS=ON`, and `-DQLAAD_<option>=OFF` for `QLAAD_LTO`, `QLAAD_FP_CONTRACT_OFF`, `QLAAD_FAST_BUILD`
(or only `QLAAD_PRECOMPILE_HEADERS`, with `QLAAD_UNITY_BATCH_SIZE=0` for no unity build), `QLAAD_USE_CCACHE` and `QLAAD_CTEST_PER_SUITE`.
Options that don't apply to a platform, compiler or Debug build (with single-config generators) are left off there; link-time optimisation is on by default with GCC, except on Windows.

- Set XAD's options with `-D` on the command line: other cache entries for them (from an old-order configure, ccmake, cmake-gui or `-C`) are replaced by the defaults (for `XAD_NO_THREADLOCAL`, XAD's own default, OFF), with a warning naming each entry that changes.
- Linear slots take more tape memory, and stay off with `XAD_COPY_ELISION`, `XAD_TAPE_REUSE_SLOTS` or `XAD_TAPE_TRACK_LIVE_SLOTS`.
- Bindings and other modules loaded with `dlopen` need `-DXAD_TLS_INITIAL_EXEC=OFF` (it is off by default for a shared QuantLib).
- When QuantLib is built inside another project, that project's settings (`BUILD_SHARED_LIBS`, XAD's options) decide; with an installed XAD package, XAD's own settings apply.
- A static QuantLib needs `-lxad` too for users that link through `pkg-config` or `quantlib-config` (or build with `-DBUILD_SHARED_LIBS=ON`); remove an older shared QuantLib from the install prefix.
- One CTest test per suite needs CMake 3.28 or newer; older versions keep QuantLib's single test.
- An installed LTO build links only with the same toolchain: installing for other toolchains needs `-DQLAAD_LTO_FAT_OBJECTS=ON` (GCC; it covers XAD's and QuantLib's libraries, not xad-codegen's), or `-DQLAAD_LTO=OFF`.

## Code Generation

XAD is optimized for computing sensitivities efficiently in a single evaluation pass using adjoint mode. For workflows that require repeated evaluation across many scenarios — such as Monte Carlo simulations, XVA calculations, regulatory stress testing, or scenario-based risk analysis — XAD also supports recording computations into a [`JITGraph`](https://auto-differentiation.github.io/ref/jit/) that can be compiled and re-evaluated efficiently using a JIT backend.

**xad-codegen** compiles a recorded graph to native code and evaluates it vectorised across paths. It is a commercial add-on, available with XAD Enterprise subscriptions: see [XAD editions](https://www.xcelerit.com/xad/).

## Getting Help

If you have found an issue, want to report a bug, or have a feature request, please raise a [GitHub issue](https://github.com/auto-differentiation/QuantLibAAD/issues).

For general questions about QuantLibAAD, sharing ideas, engaging with community members, etc, please use [GitHub Discussions](https://github.com/auto-differentiation/QuantLibAAD/discussions).


## Contributing

Please read [CONTRIBUTING](CONTRIBUTING.md) for the process of contributing to this project.
Please also obey our [Code of Conduct](CODE_OF_CONDUCT.md) in all communication.

Help wanted: more QuantLib tests ported to AAD, and more examples.

## Related Projects

- XAD Comprehensive automatic differentiation in [Python](https://github.com/auto-differentiation/xad-py) and [C++](https://github.com/auto-differentiation/xad)
- QuantLibAAD: Fast risk evaluations in [Python](https://github.com/auto-differentiation/QuantLib-Risks-Py) and [C++](https://github.com/auto-differentiation/QuantLibAAD)

## License

Due to the nature of this repository, two different licenses have to be used for 
different part of the code-base.
The [tests](test-suite/) and [examples](Examples/) folders are containing code taken
and modified from QuantLib where the [QuantLib license](test-suite/LICENSE.TXT) applies.
The [ql](ql/) folder contains adaptor modules for XAD,
where the [GNU AGPL](ql/LICENSE.md) applies.
This is clearly indicated by having separate license files in each folder.

## Authors

-   Various contributors from Xcelerit
-   See also the list of [contributors](https://github.com/auto-differentiation/QuantLibAAD/contributors) who participated in the project.
