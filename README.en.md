# MoteVM

**English** | [简体中文](README.md) | [Documentation](docs/en/README.md)

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
| `tests/` | Self-contained CTest suite: parser, verifier, round-trip, archive, runtime, CLI |
| `docs/` | User documentation: quick start, ISA, bytecode/archive formats, toolchain, build and limits |
| `CMakeLists.txt` | Root build script; auto-discovers `src/<module>/<module>.cpp` as executable targets |
| `LICENSE` | MIT license |

> `benchmarks/`, `plan/`, `deepseek-cache/` and similar are local working directories and
> are not tracked; `tests/` and the CI configuration are part of the repository.
> See [CHANGELOG.md](CHANGELOG.md) for changes.

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
| `MOTEVM_BUILD_TESTS` | `ON` (default); build the `tests/` CTest suite |
| `MOTEVM_BUILD_BENCHMARKS` | `OFF` (default); build `benchmarks/` |
| `MOTEVM_BUILD_FUZZERS` | `OFF` (default); build libFuzzer targets (Clang only) |
| `MOTEVM_WERROR` | `OFF` (default); treat warnings as errors |
| `MOTEVM_SANITIZERS` | `OFF` (default); enable ASan/UBSan |
| `MOTEVM_INSTALL` | `OFF` (default); generate install rules |

### Build output

Executables are written to `build/cmake/<toolchain>-<arch>-<config>/bin/`,
for example `build/cmake/clang-x64-release/bin/motevm`.

### Tests

```bash
cmake --build build
ctest --test-dir build --output-on-failure
```

`tests/` contains unit tests and CLI integration checks (exit codes,
`--check-roundtrip`, archive round-trips, `--warn-as-error`, and so on).
`MOTEVM_WERROR=ON` and `MOTEVM_SANITIZERS=ON` can be combined with the suite.

## Command-line tools

A runnable example is included at [`docs/examples/hello.mta`](docs/examples/hello.mta):

```bash
moteasm docs/examples/hello.mta -o hello.mbc  # assemble
motevm hello.mbc                              # run; -s stats, -d debug, -i interactive debugger
# output: hello, mote
motedis hello.mbc                             # disassemble to canonical text
motear pack -o hello.mtr --module hello.mbc   # package
motear list hello.mtr --verify                # list and verify an archive
```

`.mta` syntax is documented in [docs/en/self/moteasm.md](docs/en/self/moteasm.md),
instruction semantics in [docs/en/01-isa.md](docs/en/01-isa.md), and the full tool
reference in [docs/en/05-toolchain.md](docs/en/05-toolchain.md).

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

Built-in host functions (`print`, `print_str`, `const_addr`, ...) are documented in
[docs/en/self/cli.md](docs/en/self/cli.md) §10.

### `motear` subcommands

| Subcommand | Description |
| --- | --- |
| `pack` | create an archive from modules (and resources) |
| `list` | list archive entries (`--verbose`, `--verify`) |
| `info` | print header and manifest information (`--json`) |
| `verify` | validate the container and its modules (`--strict`, `--no-modules`) |
| `unpack` | extract entries into a directory (`-C DIR`, `--force`, `--module NAME`) |

The archive format and manifest fields are specified in
[docs/en/07-mtr-format.md](docs/en/07-mtr-format.md).

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

## Stability and version

- Version `0.1.0`; bytecode `MOTEBC 1.0`; archive `MTR 1.0`.
- The command-line tools and the MOTEBC/MTR formats are documented; options may still
  change during 0.x, and `--help` is authoritative.
- The C++ `Host`/`Core` interfaces under `src/motevm/include/` are internal: no umbrella
  header, not installed as a library, and no ABI stability promise. See
  [docs/en/04-embedding-api.md](docs/en/04-embedding-api.md).
- Changes are recorded in [CHANGELOG.md](CHANGELOG.md); release tags are created
  by the maintainers on frozen commits.
- All hard limits: [docs/en/08-limits.md](docs/en/08-limits.md).

## Help

The documentation index is [docs/en/README.md](docs/en/README.md). Every tool prints its
full option list and exit codes with `--help`:
`motevm --help`, `moteasm --help`, `motedis --help`, `motear --help`, `builder --help`.

## License

Released under the MIT License. See [LICENSE](LICENSE) for details.
