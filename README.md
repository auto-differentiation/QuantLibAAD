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

## Performance

Risk with XAD costs little more than pricing alone, and far less than bumping.

### Cost of AD types

QuantLib built with XAD's active type, running without computing derivatives, against the same build with `double`:

| | AD / double |
|---|---|
| QuantLib test suite (1714 cases) | 1.13× |
| QuantLib benchmark | 1.21× |
| QuantLib examples (20) | 1.10× |

### Sensitivities: AD vs bump-and-revalue

All sensitivities in one adjoint pass, against one finite-difference revaluation per input:

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

AMD EPYC (Zen 5) server, 32 cores, Linux, GCC 15, XAD 2.2.0, Release build tuned for speed; median of 3 runs.

## Getting Started

For detailed build instructions with [XAD](https://auto-differentiation.github.io) and [QuantLib](https://www.quantlib.org), please refer to the [XAD documentation site](https://auto-differentiation.github.io/quantlib-risks/cxx/).

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
