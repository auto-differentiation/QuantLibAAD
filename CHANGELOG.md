# Changelog

All notable changes to the QuantLibAAD module will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Note that we follow QuantLib's releases in this repository, with matching version numbers.

## [unreleased]


## [1.44] - 2026-11-17

Release compatible with [QuantLib v1.44](https://github.com/lballabio/QuantLib/releases/tag/v1.44) and [XAD v2.2.0](https://github.com/auto-differentiation/xad/releases/tag/v2.2.0).

-   Adds `patches/quantlib` with the fixes QuantLib v1.44 needs to build with AD, from QuantLib master (see the README)
-   Replaces the previous JIT backend option with xad-codegen (`QLAAD_ENABLE_CODEGEN`, commercial add-on)
-   Builds default to XAD's fastest tape options, a static QuantLib, link-time optimisation and no floating-point contraction (so AD and `double` results match), with faster builds of QuantLib and its tests; each can be switched off (see the README)
-   Configuring stops with an error when XAD is listed before QuantLibAAD in `QL_EXTERNAL_SUBDIRECTORIES`
-   New samples: AdjointScaling and AdjointTradeRisk
-   AdjointHestonModel computes calibration sensitivities with the implicit function theorem
-   AdjointSwap, AdjointHestonModel and the two new samples run as tests in CI; fixed the sign of the bumped sensitivities in AdjointSwap
-   Removed the forward declaration of `QuantLib::squared` from the expression overloads
-   Keeps the MSVC random-engine workaround only for STLs that need it; adds a Visual Studio 2026 CI job
-   Adds scripts and a workflow that compare AD and `double` builds
-   Restores the XAD-triggered and daily CI runs


## [1.41] - 2026-03-30

Release compatible with [QuantLib v1.41](https://github.com/lballabio/QuantLib/releases/tag/v1.41).

-   Renamed quantlib-xad -> QuantLib-Risks -> QuantLibAAD, with the `QLAAD` prefix for CMake options (old `QLRISKS` options still work)
-   Supports recording into XAD's `JITGraph` and evaluating with a JIT backend
-   Adds the `benchmark_aad` Monte Carlo benchmark (tape, split mode and JIT)
-   Adds runtime measurement to the Bermudan swaption sample
-   Compatibility fixes for Boost 1.89, Boost deprecations and the upcoming QuantLib 1.42
-   Fixed template resolution on macOS, missing includes and typos in the samples
-   CI runs when XAD changes, and on the latest macOS and Windows with C++20


## [1.33] - 2024-03-19

Release compatible with [QuantLib v1.33](https://github.com/lballabio/QuantLib/releases/tag/v1.33).

-   Boost 1.84 compatibility fixes
-   Fixed linking issues for external applications after installing
-   Adds presets for use as CMakeUserPresets.json with QuantLib


## [1.31] - 2023-07-23

Release compatible with [QuantLib v1.31](https://github.com/lballabio/QuantLib/releases/tag/v1.31).


- A new CMake option `QLXAD_DISABLE_AAD` to allow running the Examples without AAD
- A new set of Examples, partially enabled for benchmarking against double
- Additional tests in the adjoint test suite



## [1.28] - 2022-11-17

Initial open-source release, compatible with [QuantLib v1.28](https://github.com/lballabio/QuantLib/releases/tag/QuantLib-v1.28).


[unreleased]: https://github.com/auto-differentiation/QuantLibAAD/compare/v1.44...HEAD

[1.44]: https://github.com/auto-differentiation/QuantLibAAD/compare/v1.41...v1.44

[1.41]: https://github.com/auto-differentiation/QuantLibAAD/compare/v1.33...v1.41

[1.33]: https://github.com/auto-differentiation/QuantLibAAD/compare/v1.31...v1.33

[1.31]: https://github.com/auto-differentiation/QuantLibAAD/compare/v1.28...v1.31

[1.28]: https://github.com/auto-differentiation/QuantLibAAD/tree/v1.28


