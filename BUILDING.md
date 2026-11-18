# Building

## Requirements

- Windows 10 or 11
- Visual Studio 2022 with C++, Clang, CMake and Ninja
- Python 3.10 or newer
- The [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk)
- A copy of Doritos Crash Course

## Assets

TU1 and City Lights are required. TU2 isn't supported.

## Steps

Builds with CMake against the ReXGlue SDK.

```powershell
cmake --preset win-amd64-relwithdebinfo -DREXSDK_DIR=<path>
cmake --build --preset win-amd64-relwithdebinfo
tools/shaderpack/build_shaderpack.ps1
```
