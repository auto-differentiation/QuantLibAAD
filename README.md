# QuantLib-Risks: QuantLib with XAD Automatic Differentiation in C++

<p align="center" dir="auto">
    <a href="https://github.com/auto-differentiation/QuantLib-Risks-Cpp/actions/workflows/ci.yaml">
        <img src="https://img.shields.io/github/actions/workflow/status/auto-differentiation/QuantLib-Risks-Cpp/ci.yaml?label=Build%20%28XAD%20main%20vs%20QuantLib%20master%29" alt="GitHub Workflow Status" style="max-width: 100%;margin-right:20px">
    </a>
    <a href="https://github.com/auto-differentiation/QuantLib-Risks-Cpp/blob/main/CONTRIBUTING.md">
        <img src="https://img.shields.io/badge/PRs%20-welcome-brightgreen.svg" alt="PRs Welcome" style="max-width: 100%;">
    </a>
</p>

As a demonstrator of integration of the [XAD automatic differentiation tool](https://auto-differentiation.github.io) with real-world code, 
the latest release of QuantLib can calculate risks with the help of XAD. 
The performance achieved on sample applications is many-fold superior to what has been reported previously with other tools. 
This demonstrates production quality use of the XAD library in a code-base of several hundred thousand lines.

This repository contains integration headers, examples, and tests required
for this integration.
It is not usable stand-alone.

## JIT Compilation Support

XAD is optimized for computing sensitivities efficiently in a single evaluation pass using adjoint mode. For workflows that require repeated evaluation across many scenarios—such as Monte Carlo simulations, XVA calculations, regulatory stress testing, or scenario-based risk analysis—XAD also supports recording computations into a [`JITGraph`](https://auto-differentiation.github.io/xad/ref/jit/) that can be compiled and re-evaluated efficiently using a JIT backend. The [xad-forge](https://github.com/da-roth/xad-forge) library provides Forge-based backends for this purpose, including vectorized AVX execution. See the [xad-forge README](https://github.com/da-roth/xad-forge#when-to-use-jit) for guidance on when JIT compilation is beneficial.

The repository includes a [swaption benchmark](.github/workflows/ql-benchmarks.yaml) that demonstrates a hybrid workflow: curve bootstrapping with XAD's tape, followed by Monte Carlo pricing with JIT-compiled evaluation, comparing tape-based, JIT, and JIT-AVX performance.

## Getting Started

For detailed build instructions with [XAD](https://auto-differentiation.github.io) and [QuantLib](https://www.quantlib.org), please refer to the [XAD documentation site](https://auto-differentiation.github.io/quantlib-risks/cxx/).

## Getting Help

If you have found an issue, want to report a bug, or have a feature request, please raise a [GitHub issue](https://github.com/auto-differentiation/QuantLib-Risks-Cpp/issues).

For general questions about XAD, sharing ideas, engaging with community members, etc, please use [GitHub Discussions](https://github.com/auto-differentiation/QuantLib-Risks-Cpp/discussions).


## Contributing

Please read [CONTRIBUTING](CONTRIBUTING.md) for the process of contributing to this project.
Please also obey our [Code of Conduct](CODE_OF_CONDUCT.md) in all communication.

## Related Projects

- XAD Comprehensive automatic differentiation in [Python](https://github.com/auto-differentiation/xad-py) and [C++](https://github.com/auto-differentiation/xad)
- QuantLib-Risks: Fast risk evaluations in [Python](https://github.com/auto-differentiation/QuantLib-Risks-Py) and [C++](https://github.com/auto-differentiation/QuantLib-Risks-Cpp)

## Planned Features

- Gradually port more of the QuantLib tests and add AAD-based sensitivity calculation
- Add more Examples

## Authors

-   Various contributors from Xcelerit
-   See also the list of [contributors](https://github.com/auto-differentiation/QuantLib-Risks-Cpp/contributors) who participated in the project.


## License

Due to the nature of this repository, two different licenses have to be used for 
different part of the code-base.
The [tests](test-suite/) and [examples](Examples/) folders are containing code taken
and modified from QuantLib where the [QuantLib license](test-suite/LICENSE.TXT) applies.
The [ql](ql/) folder contains adaptor modules for XAD,
where the [GNU AGPL](ql/LICENSE.md) applies.
This is clearly indicated by having separate license files in each folder.
