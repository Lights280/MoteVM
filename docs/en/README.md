# MoteVM Documentation

[简体中文](../README.md) | [English](README.md)

This documentation set describes the **current implementation of MoteVM 0.1.0**: a
register-based virtual machine and toolchain written in C++20, targeting MOTEBC 1.0
bytecode (`.mbc`) and MTR 1.0 archives (`.mtr`).

> Only implemented behavior is documented here. The target ISA from the design
> material (`Value`/GC/object model), the `motec` compiler, JIT, and the not-yet-
> implemented public `VM`/`Value`/`Result` API are **out of scope**.

## Choose your entry point

| You are | Start here |
| --- | --- |
| A first-time user | [00 · Quick Start](00-overview.md) → [06 · Build and Release](06-build.md) |
| Writing `.mta` modules | [self/moteasm.md](self/moteasm.md) → [01 · ISA Reference](01-isa.md) → [02 · MOTEBC Format](02-bytecode-format.md) |
| Using the command-line tools | [05 · Toolchain Reference](05-toolchain.md) → [self/cli.md](self/cli.md) → [self/motedis.md](self/motedis.md) |
| Packaging/distributing modules | [07 · MTR Format](07-mtr-format.md) → [05 · Toolchain Reference](05-toolchain.md) |
| Embedding the runtime from C++ | [03 · Runtime](03-runtime.md) → [04 · Embedding API](04-embedding-api.md) |
| Building/releasing | [06 · Build and Release](06-build.md) → [08 · Limits](08-limits.md) |

## Document map

| Document | Contents |
| --- | --- |
| [00 · Quick Start](00-overview.md) | Build, run the first program, tool dataflow |
| [01 · ISA Reference](01-isa.md) | Current instruction set, encoding, semantics, traps, memory |
| [02 · MOTEBC Format](02-bytecode-format.md) | `.mbc` header, section layout, verification rules |
| [03 · Runtime](03-runtime.md) | Load/verify/execute pipeline, memory, hooks, error codes |
| [04 · Embedding API](04-embedding-api.md) | `Host`/`Core` usage (internal, unstable) |
| [05 · Toolchain Reference](05-toolchain.md) | Tool conventions, CLI design, exit codes, host ABI |
| [06 · Build and Release](06-build.md) | CMake options, targets, warnings, version policy |
| [07 · MTR Format](07-mtr-format.md) | `.mtr` archive layout and verification |
| [08 · Limits](08-limits.md) | Authoritative table of all hard limits |
| [self/moteasm.md](self/moteasm.md) | `.mta` assembly language reference |
| [self/cli.md](self/cli.md) | Full `motevm` CLI, built-in host functions, debugger |
| [self/motedis.md](self/motedis.md) | Canonical disassembly and round-trip |

## Stability

| Surface | Status | Notes |
| --- | --- | --- |
| Command-line tools | Documented | Options may change during 0.x; `--help` is authoritative |
| MOTEBC 1.0 / MTR 1.0 | Documented | Format versions are independent of the project version; release tags are created by the maintainers on frozen commits |
| C++ `Host`/`Core`/`Parse` | Internal, unstable | No umbrella header, no installed library, no ABI promise |
| `tests/`, CI | Part of the repository | Unit tests and CLI integration checks; see `CHANGELOG.md` |
| Target ISA (`Value`/GC), `motec`, JIT | Not implemented | Out of scope for this documentation |

## Versions

| Item | Value |
| --- | --- |
| MoteVM | 0.1.0 |
| MOTEBC | 1.0 |
| MTR | 1.0 |
| License | MIT |

## Conventions

- Exit codes: `0` success; `1` usage error; `2` input/verification error; `3` runtime
  trap (`motevm` only); `4` internal or I/O error. Details per tool: [05 · Toolchain Reference](05-toolchain.md) §8.5.
- Diagnostics go to stderr; normal program/tool data goes to stdout.
- Output contains no timestamps or randomness; the same input produces the same result.
- Abbreviated references in source comments such as `docs/05`, `docs/06`, and
  `docs/self/cli.md` map to the files in this directory.
