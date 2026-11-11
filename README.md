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

QuantLibAAD lets QuantLib compute all its risks (sensitivities) in one pass with [XAD](https://github.com/auto-differentiation/xad),
using QuantLib's own code unchanged: 4× to 29× faster than bumping on the QuantLib samples below, and up to 270× faster
on Monte Carlo with XAD's commercial code generation add-on. It shows XAD in production-quality use on a code base of
several hundred thousand lines.

This repository contains integration headers, examples, and tests required
for this integration.
It is not usable stand-alone.

## Performance

Risk with XAD costs little more than pricing alone, and far less than bumping.

### Cost of AD types

QuantLib built with XAD's active type, running without computing derivatives, against the same build with `double`:

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
