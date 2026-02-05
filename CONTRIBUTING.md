# How to contribute

We love pull requests from everyone. By participating in this project, you agree to abide
by our [code of conduct](CODE_OF_CONDUCT.md).

1.  Fork, then clone the repository:

```bash
git clone https://github.com/yourusername/QuantLibAAD.git
```

2.  Follow the [Build Instructions](README.md) to setup the dependencies and 
    build the software. Make sure all tests pass.

3.  Create a feature branch, typically based on main, for your change

```bash
git checkout -b feature/my-changed-main
```

4.  Make your changes, adding tests as you go, and commit. Again, make sure all 
    tests pass.

5.  Push your fork 

6.  [Submit a pull request][pr]. Not that you will have to sign the [Contributor License Agreement][cla] 
    before the PR can be merged.

At this point, you are depending on the core team to review your request. 
We may suggest changes, improvements, or alternatives. 
We strive to at least comment on a pull request within 3 business days. 
After feedback has been given, we expect a response within 2 weeks, 
after which we may close the pull request if it isn't showing activity.

Some things that will highly increase the chance that your pull request gets
accepted:

-   Discuss the change you wish to make via issue or email

-   Write good tests for all added features

-   Write good commit messages (short one-liner, followed by a blank line, 
    followed by a more detailed explanation)

## Source Code Organisation

-   [cmake](https://cmake.org): CMake modules and scripts used for the build
-   [ql](ql): Files to be added to the QuantLib build
-   [Examples](Examples): QuantLib examples with adjoints, using xad
-   [test-suite](test-suite): Unit tests suite, mimicking QuantLib's, incl. adjoint calculations

## Coding Style

This repository follows [QuantLib's coding style](https://www.quantlib.org/style.shtml).

[pr]: https://github.com/auto-differentiation/QuantLibAAD/compare/

[cla]: https://gist.github.com/auto-differentiation-dev/ee51f6c0a3365ae2fc2489e092366de2
