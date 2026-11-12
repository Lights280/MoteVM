# 06 · Build and Release

[简体中文](../06-build.md) | [English](06-build.md)

This document describes building MoteVM 0.1.0, its CMake options, output layout, the standalone `builder` driver, install rules and the release checklist. The project currently supports MOTEBC 1.0 bytecode and MTR 1.0 archives.

## 1. Prerequisites

| Item | Requirement |
| --- | --- |
| CMake | 3.20 or newer |
| C++ standard | C++20 (`CMAKE_CXX_STANDARD 20`, `CMAKE_CXX_EXTENSIONS OFF`) |
| Compilers | Clang, GCC, MSVC |
| Generator | Ninja recommended; Makefiles and Visual Studio generators are also supported |
| Default build type | `Release` for single-config generators when not specified explicitly |

`clang-cl` (Clang's MSVC frontend, `CMAKE_CXX_COMPILER_FRONTEND_VARIANT` equal to `MSVC`) is not supported. Configuration fails immediately:

```text
clang-cl is not supported; use clang++ or MSVC cl
```

## 2. CMake build system

```bash
cmake -S . -B build -G Ninja -DMOTEVM_COMPILER=clang -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### 2.1 CMake options

| Option | Default | Values | Semantics |
| --- | --- | --- | --- |
| `MOTEVM_COMPILER` | `auto` | `auto`, `clang`, `gcc`, `msvc` | Compiler selection. With a non-Visual-Studio generator, `auto` prefers `clang++` (trying `clang++`, `clang++-20`, `clang++-19`, `clang++-18` in order); if none is found it warns and leaves the choice to CMake. `clang` searches the `clang++` family and `gcc` searches `g++` (`g++`, `g++-15`, `g++-14`); a missing compiler is a `FATAL_ERROR`. With Ninja/Makefiles, `msvc` requires `cl` in the developer environment, otherwise `FATAL_ERROR`; the Visual Studio generator chooses the compiler itself. The compiler ID is validated after configuration. |
| `MOTEVM_ARCH` | `native` | `native`, `x86`, `x64` | Target architecture. `native` resolves to `x64` or `x86` from the host pointer width; other values force 32/64-bit. An invalid value is a `FATAL_ERROR`. |
| `MOTEVM_BUILD_TOOLS` | `ON` | `ON`, `OFF` | Build the module executables discovered under `src/`. With `OFF` the three static libraries are still built, but no tool executables are produced. |
| `MOTEVM_BUILD_TESTS` | `ON` | `ON`, `OFF` | Build `tests/`. `add_subdirectory(tests)` and CTest are enabled only when `tests/CMakeLists.txt` exists; a missing directory prints a status message and is skipped. |
| `MOTEVM_BUILD_BENCHMARKS` | `OFF` | `ON`, `OFF` | Build `benchmarks/`. Added only when `benchmarks/CMakeLists.txt` exists; otherwise a status message is printed and the directory is skipped. |
| `MOTEVM_BUILD_FUZZERS` | `OFF` | `ON`, `OFF` | Build libFuzzer targets; Clang only. A non-Clang compiler prints a warning and skips; a missing `fuzz/CMakeLists.txt` prints a status message and skips. |
| `MOTEVM_WERROR` | `OFF` | `ON`, `OFF` | Treat warnings as errors: adds `/WX` for MSVC and `-Werror` for GCC/Clang. |
| `MOTEVM_SANITIZERS` | `OFF` | `ON`, `OFF` | Enable ASan/UBSan. GCC/Clang use `-fsanitize=address,undefined`; MSVC enables ASan only. |
| `MOTEVM_INSTALL` | `OFF` | `ON`, `OFF` | Generate install rules. See §2.6. |

`MOTEVM_COMPILER` constraints:

- The Visual Studio generator cannot be combined with `MOTEVM_COMPILER=clang` or `gcc`; configuration fails.
- With Ninja/Makefiles, `MOTEVM_COMPILER=msvc` requires entering a Visual Studio Developer PowerShell (`Enter-VsDevShell`) first.
- A compiler ID that does not match the option fails configuration, for example `MOTEVM_COMPILER=gcc` when the actual compiler is Clang.

### 2.2 Targets

Three static libraries are excluded from module discovery:

| Target | Type | Sources | Dependencies |
| --- | --- | --- | --- |
| `motevm_core` (alias `motevm::core`) | STATIC | `src/include/parse.cpp`, `verifier.cpp`, `disasm.cpp` | none |
| `motevm_archive` (alias `motevm::archive`) | STATIC | `src/include/archive/*.cpp` | PUBLIC `motevm_core` |
| `motevm_tools` | STATIC | `src/include/asm/*.cpp`, `common/*.cpp`, `dis/*.cpp` | PUBLIC `motevm_core` |

All three libraries expose `src/include` as a PUBLIC include directory and require C++20.

Executable targets are discovered from directories: every direct child of `src/` (except `include`) is a module, the directory name must match `[a-z][a-z0-9_]*`, and it must contain `<module>.cpp` as the entry point. The module's `*.cpp` files are collected recursively, excluding files under `include/`.

Current modules and link relationships:

| Executable | Entry point | Links |
| --- | --- | --- |
| `motevm` | `src/motevm/motevm.cpp` | `motevm_core` |
| `moteasm` | `src/moteasm/moteasm.cpp` | `motevm_tools` |
| `motedis` | `src/motedis/motedis.cpp` | `motevm_tools` |
| `motear` | `src/motear/motear.cpp` | `motevm_archive`, `motevm_tools` |
| `builder` | `src/builder/builder.cpp` | none (uses `src/include` headers only) |

Module names must not collide with CMake built-in targets (`all`, `clean`, `test`, `install`, and so on). An invalid directory name, a missing main file, or a module without any `.cpp` file fails configuration.

### 2.3 Output paths

CMake-built module executables are written to:

```text
<build>/cmake/<toolchain>-<arch>-<config>/bin/<module>[.exe]
```

Where:

- `<toolchain>` is `clang`, `gcc` or `msvc`;
- `<arch>` is `x86` or `x64`;
- `<config>` is the lowercase build type, for example `release` or `debug`.

For example:

```text
build/cmake/clang-x64-release/bin/motevm
build/cmake/clang-x64-release/bin/moteasm
```

The generic output directory (tests, benchmarks, and so on) is `build/bin`; libraries go to `build/lib`. Module targets override those generic directories.

### 2.4 Warnings and sanitizers

Warning flags are applied by compiler frontend.

MSVC (`cl`):

```text
/W4 /permissive- /utf-8 /Zc:__cplusplus
```

With `MOTEVM_WERROR`, `/WX` is added.

GCC / Clang:

```text
-Wall -Wextra -Wpedantic
-Wconversion -Wsign-conversion -Wshadow -Wcast-qual -Wundef
-Wnon-virtual-dtor -Wold-style-cast
-Werror=return-type
```

`-Werror=return-type` is always enabled. With `MOTEVM_WERROR`, `-Werror` is added.

Sanitizers:

| Compiler | Compile and link flags |
| --- | --- |
| GCC / Clang | `-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all` |
| MSVC | `/fsanitize=address` |
| Other | Prints the warning `MOTEVM_SANITIZERS unsupported by <id>` |

Sanitizer flags are used for both compilation and linking. On compilers other than Clang, enabling `MOTEVM_BUILD_FUZZERS` only produces a warning and is skipped.

### 2.5 Standalone builder

`src/builder/builder.cpp` is a single-translation-unit build driver that does not read `CMakeLists.txt`; it can build the whole project without CMake. Commands:

| Command | Behavior |
| --- | --- |
| `build` (default) | Compile and link the selected targets |
| `clean` | Remove builder-generated directories and state; never touches CMake files |
| `rebuild` | `clean`, then `build` |
| `self` | Rebuild `builder` itself |
| `doctor` | Print toolchain, ABI, output directories, targets and shared source groups |
| `targets` | List selectable targets, source counts and link relationships |
| `help` | Print usage |

Options:

| Option | Description |
| --- | --- |
| `-j, --jobs N` | Parallel compile jobs; accepts 1..1024, defaults to hardware concurrency (capped at 64, or 4 when unavailable) |
| `--compiler clang\|gcc\|msvc` | Compiler selection, `clang++` preferred by default; also accepts `clang++`, `g++`, `cl` aliases |
| `--arch x86\|x64` | Target architecture, host architecture by default; also accepts `host`, `auto`, `32`, `64`, `i686`, `win32`, `x86_64`, `amd64` |
| `--release` / `--debug` | Build configuration, Release by default |
| `--cxx PATH` | Explicit C++ compiler; the family is detected from the file name |
| `--build-dir DIR` | Output root, `<root>/build` by default |
| `--root DIR` | Project root; by default searches upward from the current directory and executable location for `src/motevm` and `src/builder` |
| `--werror` | Treat warnings as errors |
| `--sanitize` | ASan + UBSan; MSVC uses ASan only |
| `--force` | Ignore the cache and rebuild everything |
| `-n, --dry-run` | Print commands without running them |
| `-v, --verbose` | Print cache-hit reasons and full command lines |
| `--version` | Print builder, MoteVM and MOTEBC versions |
| `-h, --help` | Print help |

Environment variables:

| Variable | Effect |
| --- | --- |
| `MOTEVM_CXX` | Compiler candidate path; used after `--cxx` and before `CXX` and automatic search |
| `CXX` | Compiler candidate path; consulted after `MOTEVM_CXX` and before automatic search |
| `MOTEVM_BUILD_DIR` | Output root when `--build-dir` is absent; resolved relative to the project root |
| `MOTEVM_JOBS` | Parallel job count when `-j/--jobs` is absent; accepts 1..1024 |
| `VSCMD_ARG_TGT_ARCH` | Target architecture of the MSVC developer environment (`amd64`/`x86_64`/`win32`, and so on); used to decide whether to re-enter the DevShell |

Child compilers use `<build>/.builder/tmp` as their temporary directory (set through `TMP`, `TEMP`, `TMPDIR`).

Output layout:

```text
build/<toolchain>-<arch>-<config>/bin/<module>[.exe]
build/<toolchain>-<arch>-<config>/.builder/{obj,dep,state,logs,tmp}/
build/bin/builder[.exe]     # default self location
```

`builder self` rebuilds to `build/bin/builder[.exe]` by default; if the running builder is already inside the build tree, it rebuilds that executable in place.

The shared source groups `motevm_core` and `motevm_tools` are internal targets: they do not appear in `all` and cannot be selected as user targets. They are compiled once per build and reused by every executable that links them. The shared source lists are hardcoded in builder; adding a shared source requires updating both builder and the root `CMakeLists.txt`.

Exit codes: `0` success, `1` build failure, `2` usage error, `3` environment or toolchain error.

### 2.6 Install

With `MOTEVM_INSTALL=ON`, the following install rules are generated:

- module executables install to `${CMAKE_INSTALL_BINDIR}`;
- headers under `src/include/` install to `${CMAKE_INSTALL_INCLUDEDIR}`;
- each module's own `include/` installs to `${CMAKE_INSTALL_INCLUDEDIR}/<module>`.

The install rules contain no static libraries, CMake export targets or package config, and promise no ABI. C++ headers are internal interfaces; see §6.

## 3. Compiler selection and platform notes

### 3.1 Windows / MSVC

With Ninja or Makefiles, enter a Visual Studio Developer PowerShell first:

```powershell
Enter-VsDevShell
cmake -S . -B build -G Ninja -DMOTEVM_COMPILER=msvc
cmake --build build
```

Or use a Visual Studio generator directly:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

`-A Win32` targets 32-bit and `-A x64` targets 64-bit. `MOTEVM_ARCH` must match the compiler environment, otherwise configuration fails.

### 3.2 MinGW / GCC

`MOTEVM_COMPILER=gcc` uses `g++`. A 32-bit build requires that `g++` to provide `-m32` multilib, or to target a 32-bit triple (such as `i686`/MinGW32):

```bash
cmake -S . -B build -G Ninja -DMOTEVM_COMPILER=gcc -DMOTEVM_ARCH=x86
```

Without 32-bit multilib, configuration fails with a message to use a MinGW32 toolchain or switch back to `x64`.

### 3.3 32-bit builds

- MSVC: use `-A Win32`, or enter an x86 DevShell.
- GCC: use `-m32` (multilib required); see §3.2.
- Clang on Windows (MSVC ABI): the architecture option adds `--target=i686-pc-windows-msvc` or `--target=x86_64-pc-windows-msvc`.
- Other platforms: `MOTEVM_ARCH=x86/x64` adds `-m32`/`-m64`.

## 4. Test, benchmark and fuzz status

`tests/` is part of the repository and contains unit tests plus CLI integration
checks. `benchmarks/` and `fuzz/` remain local working directories.

```bash
cmake --build build
ctest --test-dir build --output-on-failure
```

| Option | Default | Behavior |
| --- | --- | --- |
| `MOTEVM_BUILD_TESTS=ON` | yes | Builds `tests/`, calls `enable_testing()` and registers the `unit` and `cli` CTest cases |
| `MOTEVM_BUILD_BENCHMARKS=ON` | no | Adds `benchmarks/CMakeLists.txt` when present; otherwise prints a status message and skips |
| `MOTEVM_BUILD_FUZZERS=ON` | no | Adds `fuzz/` with Clang; other compilers print a warning first |

CI covers Linux (Clang/GCC, x64/x86), Windows/MSVC (x64/x86), ASan/UBSan,
`MOTEVM_WERROR=ON`, and the standalone `builder`.

## 5. Version and compatibility

| Item | Current value | Source |
| --- | --- | --- |
| Project version | `0.1.0` | `MOTEVM_VERSION` in `src/include/define.hpp`; CMake `project(... VERSION 0.1.0)` |
| Bytecode format | MOTEBC 1.0 | `MOTEBC_VERSION` and `PARSE::SUPPORTED_MAJOR/MINOR` |
| Archive format | MTR 1.0 | `ARCHIVE::MTR_VERSION` and `ARCHIVE::SUPPORTED_MAJOR/MINOR` |

Each tool's `--version` prints:

```text
motevm 0.1.0 (MOTEBC 1.0)
moteasm 0.1.0 (MOTEBC 1.0)
motedis 0.1.0 (MOTEBC 1.0)
motear 0.1.0 (MTR 1.0)
```

Changes are recorded in `CHANGELOG.md`; release tags are created by the
maintainers on frozen commits. Version numbers come from the source constants.

Compatibility is defined by the current implementation:

- Loading MOTEBC requires major equal to `1` and minor no greater than `0`; any other major or a newer minor is rejected (`BAD_FILE`).
- Parsing MTR requires major equal to `1` and minor no greater than `0`; otherwise it is rejected (`BAD_HEADER`).
- No broader backward-compatibility policy is declared or tested; treat different versions as incompatible.

## 6. Stability classification

| Interface | Stability |
| --- | --- |
| Command-line options, usage and exit codes | Documented; each tool's `--help` is authoritative |
| MOTEBC 1.0 and MTR 1.0 file formats | Documented; see [02 Bytecode format](02-bytecode-format.md) and [07 MTR format](07-mtr-format.md) |
| C++ headers under `src/include/` and each module's `include/` | Internal interfaces; no stability or ABI promise, subject to change |
| Static library targets and link relationships | Internal build details, not a public API |

## 7. Release checklist

1. Reconfigure and build from a clean tree:

   ```bash
   cmake -S . -B build -G Ninja -DMOTEVM_COMPILER=clang -DCMAKE_BUILD_TYPE=Release
   cmake --build build
   ```

2. Assemble and run the example:

   ```bash
   build/cmake/clang-x64-release/bin/moteasm docs/examples/hello.mta -o hello.mbc
   build/cmake/clang-x64-release/bin/motevm hello.mbc
   ```

3. Run the tests: `ctest --test-dir build --output-on-failure`; repeat once with
   `-DMOTEVM_WERROR=ON` and once with `-DMOTEVM_SANITIZERS=ON`.

4. Disassemble/reassemble round trip: run `motedis --check-roundtrip hello.mbc -o hello.out.mta`, reassemble it and run it; the output must match the original module.

5. Archive round trip: `motear pack -o hello.mtr --module hello.mbc`, `motear verify hello.mtr`, `motear unpack hello.mtr -C out`.

6. Check version consistency: `--version` for `motevm`, `moteasm`, `motedis`, `motear` and `builder` all report `0.1.0`, with format version MOTEBC 1.0 or MTR 1.0 as appropriate.

7. Use `builder doctor` / `builder targets`, then run
   `builder --build-dir <tmp> --werror` to confirm the standalone driver builds
   every target.

8. Update the release status in README, this document and `docs/README.md` in
   the tagging commit, and record the changes in `CHANGELOG.md`.
