# CMake User Presets for QuantLib

This folder contains CMake user preset templates for QuantLib integration with
XAD, which can be used in addition to the existing `CMakePresets.json` file
that already ships with QuantLib to add additional presets for QuantLibAAD.
(A `CMakeUserPresets.json` can be placed beside the existing `CMakePresets.json` for user-specific local settings and is typically not checked in to Git).

**Note:** If you're using QuantLib 1.31 or earlier, please use the [CMakeUserPresets-1.31.json](./CMakeUserPresets-1.31.json) file. For later versions, [CMakeUserPresets.json](./CMakeUserPresets.json) should be used.

It provides the following extra configure presets for Windows / Visual Studio 
(all using the Ninja builder for using the built-in CMake support with "Open Folder"):

- `windows-xad-msvc-release`
- `windows-xad-msvc-debug`
- `windows-xad-msvc-relwithdebinfo`
- `windows-xad-clang-release`
- `windows-xad-clang-debug`
- `windows-xad-clang-relwithdebinfo`

And the following presets for Linux (both for Make and Ninja builders):

- `linux-xad-clang-release`
- `linux-xad-clang-debug`
- `linux-xad-clang-relwithdebinfo`
- `linux-xad-gcc-release`
- `linux-xad-gcc-debug`
- `linux-xad-gcc-relwithdebinfo`
- `linux-xad-clang-ninja-debug`
- `linux-xad-clang-ninja-release`
- `linux-xad-clang-ninja-relwithdebinfo`
- `linux-xad-gcc-ninja-debug`
- `linux-xad-gcc-ninja-release`
- `linux-xad-gcc-ninja-relwithdebinfo`

Additionally, for each of the presets listed above, there is a `*-noxad-*` version
provided as well. This sets `QLAAD_DISABLE_AAD` to `ON`, reverting to the standard `double`
for QuantLib's Real but compiling the examples within the `QuantLibAAD` repository.
This enabled benchmarking / comparisons of the same examples with XAD disabled.

## Usage

**When `CMakeUserPresets.json` is not present in QuantLib folder**

If you are not already using a `CMakeUserPresets.json` file with your QuantLib installation,
you can simply copy to file in this folder into your QuantLib folder as is,
renaming it to `CMakeUserPresets.json`.

**When a `CMakeUserPresets.json` is already used**

Use the file in this folder as a template to adjust your own presets.
You can copy the presets for XAD that you want to add, along with their
inherited base presets into you `CMakeUserPresets.json` file in the QuantLib folder.

**Usage with (Visual Studio 2019/2022)**

With Visual Studio, use the "Open Folder" mode to load the QuantLib folder.
You'll find the XAD CMake settings in the dropdown for configurations
in the top toolbar.

**Usage with the command line**

On the command-line, configure presets can be selected with the CMake command
as follows (from the QuantLib folder):

```
cmake --preset linux-xad-clang-ninja-release
cd build/linux-xad-clang-ninja-release
cmake --build .
```

(the build folder is set as `build/${presetName}`)

**Adjusting Settings**

The top section in the presets file sets the base configuration for using XAD.
It has the settings for `QL_EXTERNAL_SUBDIRECTORIES`, etc.
These can be adjusted to your needs, pointing to the relevant folders
where you checked out QuantLibAAD and XAD.

You can also add settings here about the boost location (`BOOST_ROOT`) or other
QuantLib or XAD related CMake settings.
