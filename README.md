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

As a demonstrator of integration of the [XAD automatic differentiation tool](https://auto-differentiation.github.io) with real-world code, 
the latest release of QuantLib can calculate risks (sensitivities) with the help of XAD. 
The performance achieved on sample applications is many-fold superior to what has been reported previously with other tools. 
This demonstrates production quality use of the XAD library in a code-base of several hundred thousand lines.

This repository contains integration headers, examples, and tests required
for this integration.
It is not usable stand-alone.

## JIT Compilation Support

XAD is optimized for computing sensitivities efficiently in a single evaluation pass using adjoint mode. For workflows that require repeated evaluation across many scenarios — such as Monte Carlo simulations, XVA calculations, regulatory stress testing, or scenario-based risk analysis — XAD also supports recording computations into a [`JITGraph`](https://auto-differentiation.github.io/xad/ref/jit/) that can be compiled and re-evaluated efficiently using a JIT backend.

**xad-codegen** is a high-performance native code generation backend that compiles recorded computation graphs to optimized x86-64 machine code, with support for AVX2 SIMD vectorization (4 paths per instruction). xad-codegen is available under a separate commercial license — [contact us](https://www.xcelerit.com/xad-enterprise-support) for more information.

## Getting Started

For detailed build instructions with [XAD](https://auto-differentiation.github.io) and [QuantLib](https://www.quantlib.org), please refer to the [XAD documentation site](https://auto-differentiation.github.io/quantlib-risks/cxx/).

### Build options

The defaults are the recommended settings; each can be switched off on the QuantLib command line.

- `QLAAD_XAD_RECOMMENDED` (default `ON`): builds QuantLib and its users with XAD's `XAD_USE_STRONG_INLINE` and `XAD_OUTLINE_RECORDING`
  (no strong inlining with MSVC's `cl.exe`, where it makes compiles about 2.4x slower).
- `QLAAD_TLS_INITIAL_EXEC` (default `ON`, Linux with GCC/Clang): initial-exec TLS model for XAD's active tape.
  Together with the above, AD run time on QuantLib's test suite drops from about 2.8x to 1.6x that of `double`.
- `QLAAD_FP_CONTRACT_OFF` (default `ON`, GCC/Clang): `-ffp-contract=off`, so that results don't depend on inlining or unity builds and AD values match `double` exactly.
- `QLAAD_PRECOMPILE_HEADERS` (default `ON`, `OFF` on Windows) and `QLAAD_UNITY_BATCH_SIZE` (default `8`, `0` for off):
  precompiled headers and unity build for QuantLib's library and test suite (about 40% less compile CPU time).
- `QLAAD_USE_CCACHE` (default `ON`): uses `ccache`, if installed, when no compiler launcher is set.
- `QLAAD_CTEST_PER_SUITE` (default `ON`): registers each QuantLib test suite as its own CTest test, so `ctest -j <N> -R "^QuantLibTests/"` runs them in parallel.

## Getting Help

If you have found an issue, want to report a bug, or have a feature request, please raise a [GitHub issue](https://github.com/auto-differentiation/QuantLibAAD/issues).

For general questions about XAD, sharing ideas, engaging with community members, etc, please use [GitHub Discussions](https://github.com/auto-differentiation/QuantLibAAD/discussions).


## Contributing

Please read [CONTRIBUTING](CONTRIBUTING.md) for the process of contributing to this project.
Please also obey our [Code of Conduct](CODE_OF_CONDUCT.md) in all communication.

## Related Projects

- XAD Comprehensive automatic differentiation in [Python](https://github.com/auto-differentiation/xad-py) and [C++](https://github.com/auto-differentiation/xad)
- QuantLibAAD: Fast risk evaluations in [Python](https://github.com/auto-differentiation/QuantLib-Risks-Py) and [C++](https://github.com/auto-differentiation/QuantLibAAD)

## Planned Features

- Gradually port more of the QuantLib tests and add AAD-based sensitivity calculation
- Add more Examples

## Authors

-   Various contributors from Xcelerit
-   See also the list of [contributors](https://github.com/auto-differentiation/QuantLibAAD/contributors) who participated in the project.


## License

Due to the nature of this repository, two different licenses have to be used for 
different part of the code-base.
The [tests](test-suite/) and [examples](Examples/) folders are containing code taken
and modified from QuantLib where the [QuantLib license](test-suite/LICENSE.TXT) applies.
The [ql](ql/) folder contains adaptor modules for XAD,
where the [GNU AGPL](ql/LICENSE.md) applies.
This is clearly indicated by having separate license files in each folder.
