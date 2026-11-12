# MoteVM

**English** | [简体中文](README.md)

MoteVM is a self-contained virtual machine and toolchain written in C++20. It targets
**MOTEBC 1.0** bytecode (`.mbc`), and modules can be packaged into **MTR 1.0** archives
(`.mtr`).

- Version: `0.1.0`
- Bytecode format: `MOTEBC 1.0`
- License: MIT

## Components

MoteVM ships four command-line tools plus a standalone build driver:

| Tool | Purpose |
| --- | --- |
| `moteasm` | Assembler: `.mta` source -> `.mbc` |
| `motevm` | Virtual machine: loads, verifies and runs `.mbc` modules |
| `motedis` | Disassembler: `.mbc` -> canonical `.mta` text |
| `motear` | Archive tool: pack, list, inspect, verify and unpack `.mtr` |
| `builder` | Standalone build driver; builds the whole project without CMake |

## Repository layout

| Path | Description |
| --- | --- |
| `src/include/` | Shared code: `motevm_core` (module loading/verification/disassembly), `motevm_tools` (assembler and tooling infrastructure), `archive` (MTR container) |
| `src/motevm/` | VM runtime, memory management, host interface and debugger |
| `src/moteasm/` | Assembler: `.mta` source -> `.mbc` |
| `src/motedis/` | Disassembler: `.mbc` -> canonical `.mta` text |
| `src/motear/` | Archive tool: pack, list, verify and unpack `.mtr` |
| `src/builder/` | Standalone build driver; can build the whole project outside CMake |
| `CMakeLists.txt` | Root build script; auto-discovers `src/<module>/<module>.cpp` as executable targets |
| `LICENSE` | MIT license |

## Requirements

- CMake 3.20 or newer
- A compiler with C++20 support (`clang++` is preferred by default)
- Ninja is recommended (the commands below use Ninja)

## Building

```bash
git clone https://github.com/Lights280/MoteVM.git
cd MoteVM
cmake -S . -B build -G Ninja -DMOTEVM_COMPILER=clang -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Compiler selection

`MOTEVM_COMPILER=auto` (the default) prefers `clang++` for non-Visual-Studio generators.
To build with MSVC on Windows, either enter a Visual Studio Developer PowerShell
(`Enter-VsDevShell`) and pass `-DMOTEVM_COMPILER=msvc`, or use a Visual Studio generator:

```powershell
# Use the generator version installed on your machine
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

### CMake options

| Option | Value |
| --- | --- |
| `MOTEVM_COMPILER` | `auto` (default), `clang`, `gcc`, `msvc` |
| `MOTEVM_ARCH` | `native` (default), `x86`, `x64` |
| `MOTEVM_BUILD_TOOLS` | `ON` (default); build every module under `src/` |
| `MOTEVM_BUILD_TESTS` | `ON` (default); build `tests/` (skipped when the directory is absent) |
| `MOTEVM_BUILD_BENCHMARKS` | `OFF` (default); build `benchmarks/` |
| `MOTEVM_BUILD_FUZZERS` | `OFF` (default); build libFuzzer targets (Clang only) |
| `MOTEVM_WERROR` | `OFF` (default); treat warnings as errors |
| `MOTEVM_SANITIZERS` | `OFF` (default); enable ASan/UBSan |
| `MOTEVM_INSTALL` | `OFF` (default); generate install rules |

### Build output

Executables are written to `build/cmake/<toolchain>-<arch>-<config>/bin/`,
for example `build/cmake/clang-x64-release/bin/motevm`.

## Command-line tools

```bash
moteasm hello.mta -o hello.mbc                # assemble
motevm hello.mbc                              # run; -s stats, -d debug, -i interactive debugger
motedis hello.mbc                             # disassemble to canonical text
motear pack -o hello.mtr --module hello.mbc   # package
motear list hello.mtr --verify                # list and verify an archive
```

### `motevm` common options

Usage: `motevm [extern_parameters] <entry_mbc_module> [ext_mbc_modules] [intern_parameters]`

| Option | Description |
| --- | --- |
| `-s, --status` | print execution statistics after the run |
| `-r, --release` | release mode (default): errors only |
| `-d, --debug` | debug mode: verbose diagnostics, stop context and trace |
| `-i, --interactive` | stop at entry and read debugger commands from stdin |
| `--step` | stop before the first instruction (implies `--interactive`) |
| `--break SPEC` | set a breakpoint (repeatable): `#index`, `offset`, `function[:offset]` |
| `--trace LVL` | instruction trace: `off`, `call`, `exec`, `full` (default: `off`) |
| `--trace-file PATH` | trace target (default: stderr, `-` = stdout) |
| `--trace-limit N` | stop recording after N trace lines (0 = unlimited) |
| `-w, --workspace DIR` | grant read/write access inside `DIR` only (default: stdio only) |

### `motear` subcommands

| Subcommand | Description |
| --- | --- |
| `pack` | create an archive from modules (and resources) |
| `list` | list archive entries (`--verbose`, `--verify`) |
| `info` | print header and manifest information (`--json`) |
| `verify` | validate the container and its modules (`--strict`, `--no-modules`) |
| `unpack` | extract entries into a directory (`-C DIR`, `--force`, `--module NAME`) |

## Standalone builder

`src/builder/builder.cpp` is a dependency-free C++20 build driver that compiles the whole
project without CMake:

```bash
builder          # build all targets (default)
builder targets  # list targets and sources
builder doctor   # print toolchain and build configuration
builder clean    # remove builder outputs (never CMake files)
```

## Exit codes

| Tool | Codes |
| --- | --- |
| `motevm` | `0` success, `1` usage, `2` input/verify failure, `3` runtime trap, `4` internal error |
| `moteasm` | `0` success, `1` usage, `2` input/source error, `4` internal or I/O error |
| `motedis` | `0` success, `1` usage, `2` input/verify error, `4` internal or I/O error |
| `motear` | `0` success, `1` usage, `2` input/verify error, `3` I/O error, `4` internal error |

## Help

Every tool prints its full option list and exit codes with `--help`:
`motevm --help`, `moteasm --help`, `motedis --help`, `motear --help`, `builder --help`.

## License

Released under the MIT License. See [LICENSE](LICENSE) for details.
