# Changelog

All notable changes to the QuantLibAAD module will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Note that we follow QuantLib's releases in this repository, with matching version numbers.

## [unreleased]

-   Fixed typos in samples
-   Updated CI scripts
-   Fixed missing includes in some samples
-   Renamed quantlib-xad -> QuantLib-Risks
-   Renamed QuantLib-Risks-Cpp -> QuantLibAAD


## [1.33] - 2024-03-19

Release compatible with [QuantLib v1.33](https://github.com/lballabio/QuantLib/releases/tag/QuantLib-v1.33).

-   Boost 1.84 compatibility fixes
-   Fixed linking issues for external applications after installing
-   Adds presets for use as CMakeUserPresets.json with QuantLib


## [1.31] - 2023-07-23

Release compatible with [QuantLib v1.31](https://github.com/lballabio/QuantLib/releases/tag/QuantLib-v1.31).


- A new CMake option `QLXAD_DISABLE_AAD` to allow running the Examples without AAD
- A new set of Examples, partially enabled for benchmarking against double
- Additional tests in the adjoint test suite



## [1.28] - 2022-11-17

Initial open-source release, compatible with [QuantLib v1.28](https://github.com/lballabio/QuantLib/releases/tag/QuantLib-v1.28).


[unreleased]: https://github.com/auto-differentiation/QuantLibAAD/compare/v1.33...HEAD

[1.33]: https://github.com/auto-differentiation/QuantLibAAD/compare/v1.31...v1.33

[1.31]: https://github.com/auto-differentiation/QuantLibAAD/compare/v1.28...v1.31

[1.28]: https://github.com/auto-differentiation/QuantLibAAD/tree/v1.28


