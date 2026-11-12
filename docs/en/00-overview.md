# 00 · Quick Start

[简体中文](../00-overview.md) | [English](00-overview.md)

## 1. What MoteVM is

MoteVM is a self-contained register-based virtual machine and toolchain:

| Tool | Input | Output |
| --- | --- | --- |
| `moteasm` | `.mta` assembly source | `.mbc` module |
| `motevm` | `.mbc` module | program output |
| `motedis` | `.mbc` module | canonical `.mta` text |
| `motear` | `.mbc`/resource files | `.mtr` archive |
| `builder` | source tree | build products without CMake |

Dataflow:

```text
hello.mta ──moteasm──▶ hello.mbc ──motevm──▶ output
                           │
                           ├──motedis──▶ canonical .mta
                           └──motear───▶ hello.mtr
```

The current ISA is a 32-bit register-based instruction set; see
[01 · ISA Reference](01-isa.md).

## 2. Requirements

- CMake 3.20 or newer
- A C++20 compiler (Clang, GCC, or MSVC)
- Ninja recommended

Full options and platform notes: [06 · Build and Release](06-build.md) §2.1.

## 3. Build

```bash
git clone https://github.com/Lights280/MoteVM.git
cd MoteVM
cmake -S . -B build -G Ninja -DMOTEVM_COMPILER=clang -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Executables are written to `build/cmake/<toolchain>-<arch>-<config>/bin/`, for example:

```text
build/cmake/clang-x64-release/bin/moteasm
build/cmake/clang-x64-release/bin/motevm
```

The rest of this page uses `B` for that directory:

```bash
B=build/cmake/clang-x64-release/bin
```

## 4. Run the first program

The repository includes [examples/hello.mta](../examples/hello.mta):

```mta
.module "hello"
.string msg "hello, mote"
.import print_str 2
.import const_addr 1
.import const_len 1

.func main 0 8
    llo r0, 0
    hostcall r1, const_addr
    llo r0, 0
    hostcall r2, const_len
    mov r0, r1
    mov r1, r2
    hostcall r3, print_str
    llo r0, 0
    ret r0
.end
```

Assemble and run:

```bash
$B/moteasm docs/examples/hello.mta -o hello.mbc
$B/motevm hello.mbc
```

Expected output:

```text
hello, mote
```

Pass `-s` to write execution statistics to stderr:

```bash
$B/motevm -s hello.mbc
```

## 5. Tool quick reference

```bash
$B/moteasm docs/examples/hello.mta -o hello.mbc   # assemble
$B/motevm hello.mbc                               # run
$B/motevm -d hello.mbc                            # debug diagnostics
$B/motedis hello.mbc                              # disassemble
$B/motear pack -o hello.mtr --module hello.mbc    # package
$B/motear list hello.mtr --verify                 # list and verify
$B/motear verify hello.mtr --strict               # strict verification
```

## 6. Next steps

- Learn `.mta` syntax: [self/moteasm.md](self/moteasm.md)
- Understand instruction semantics: [01 · ISA Reference](01-isa.md)
- See all CLI options and exit codes: [05 · Toolchain Reference](05-toolchain.md)
- Call the runtime from C++: [04 · Embedding API](04-embedding-api.md)
- Look up hard limits: [08 · Limits](08-limits.md)
