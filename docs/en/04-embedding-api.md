# 04 · Embedding API

[简体中文](../04-embedding-api.md) | [English](04-embedding-api.md)

> **Internal interface · Unstable · No ABI promise.**
>
> `Host`, `Core`, `Memory` and `Parse` are internal C++ interfaces of the current implementation, not a public API: there is no umbrella header, they are not installed as a library, and no source or binary compatibility is promised. Interfaces, layouts and behavior may change without notice. This document describes the surface that is directly usable in the current code; runtime semantics are in [03 · Runtime](03-runtime.md).

## 1. Status and scope

- Runtime sources live in `src/motevm/*.cpp` and are compiled into the `motevm` executable; the build system has no "runtime library" target.
- `motevm_core` is a static library containing `Parse`, `Verifier` and `Disasm` (sources in `src/include`).
- `motevm_tools` is a static library containing the assembler/disassembler toolchain; it depends on `motevm_core`.
- Embedders include the headers under `src/motevm/include` and `src/include` and compile the runtime sources into their own target.
- There is no `VM`, `Value`, `Result` or `RootedValue`, no GC/root set, and no public API in the sense of install rules (see §11).

## 2. Include and link

### 2.1 Headers and sources

| Path | Contents | Required |
|---|---|---|
| `src/motevm/include/host.hpp` | `Host`, `HOST` error codes and registry | Required when using `Host` |
| `src/motevm/include/core.hpp` | `Core`, `CORE` error codes, `StackFrame`, `HostSlot` | Required |
| `src/motevm/include/memory.hpp` | `Memory` and block constants | Required |
| `src/motevm/include/workspace.hpp` | `Workspace` path sandbox | Optional (CLI side) |
| `src/motevm/include/debug.hpp` | Debugger/trace support | Optional |
| `src/include/parse.hpp` | `Parse`, `PARSE` error codes, module image | Provided by `motevm_core` |
| `src/include/verifier.hpp` | `PARSE::Verifier` | Provided by `motevm_core` |
| `src/include/disasm.hpp` | Instruction text formatting | Provided by `motevm_core` |

Sources that must be compiled:

| Source | Dependencies |
|---|---|
| `src/motevm/core.cpp` | `parse.hpp`, `verifier.hpp`, `memory.hpp`, `define.hpp` |
| `src/motevm/host.cpp` | `parse.hpp`, `core.hpp`, `memory.hpp` |
| `src/motevm/memory.cpp` | `memory.hpp`, `define.hpp` |
| `src/motevm/workspace.cpp` | Optional; standard library only |
| `src/motevm/debug.cpp` | Optional; depends on `disasm.hpp` and `parse.hpp` |

`Parse`, `Verifier` and `Disasm` implementations are provided by `motevm_core` (`src/include/parse.cpp`, `verifier.cpp`, `disasm.cpp`). Do not link only `motevm_core` and omit `host.cpp`/`core.cpp`/`memory.cpp`: the runtime is not in the library.

### 2.2 CMake

Inside the MoteVM project (or after pulling it in with `add_subdirectory`), a host can be built like this:

```cmake
# Point this at the MoteVM source tree.
set(MOTEVM_SRC /path/to/MoteVM/src)

# The runtime has no library target; compile its sources into the host.
add_executable(myhost
    myhost.cpp
    ${MOTEVM_SRC}/motevm/core.cpp
    ${MOTEVM_SRC}/motevm/host.cpp
    ${MOTEVM_SRC}/motevm/memory.cpp
    ${MOTEVM_SRC}/motevm/workspace.cpp   # optional
    ${MOTEVM_SRC}/motevm/debug.cpp       # optional (uses Disasm)
)
target_include_directories(myhost PRIVATE
    ${MOTEVM_SRC}/motevm/include
    ${MOTEVM_SRC}/include)
target_compile_features(myhost PRIVATE cxx_std_20)
target_link_libraries(myhost PRIVATE motevm_core)  # Parse/Verifier/Disasm
```

### 2.3 Direct compilation

From the repository root, the equivalent command-line build:

```sh
clang++ -std=c++20 -O2 \
  -I src/motevm/include -I src/include \
  myhost.cpp \
  src/motevm/core.cpp src/motevm/host.cpp src/motevm/memory.cpp \
  src/include/parse.cpp src/include/verifier.cpp src/include/disasm.cpp \
  -o myhost
```

Add `src/motevm/workspace.cpp` when `Workspace` is needed; add `src/motevm/debug.cpp` when `DEBUG` is needed (it uses the already-linked `disasm.cpp`).

## 3. Minimal example

```cpp
#include <cstdio>

#include "host.hpp"

static dword add(void *, const dword *args, int)
{
    return args[0] + args[1];
}

int main()
{
    Host host;

    // Imports are resolved during load(), so register before load().
    if (host.registerFunction("add", 2, add) != HOST::OK)
        return 4;

    if (host.load("main.mbc") != HOST::OK)
        return 2; // PARSE 1..7, or HOST::UNLINKED_IMPORT
    if (host.initMemory() != HOST::OK)
        return 4; // HOST::NO_MODULE or MEMORY 1..3

    const int result = host.runModule(0);
    if (result != CORE::OK)
    {
        const Core *core = host.core();
        std::fprintf(stderr, "error %d at pc=%u\n", result,
                     core != nullptr ? static_cast<unsigned>(core->errorPc()) : 0u);
        return 3;
    }
    return 0;
}
```

Usage order:

1. Register all host functions. Imports are resolved during `load()`; later registrations do not bind retroactively (see §7.5).
2. Load all modules. `Host::load` may be called multiple times; a module that fails to load is not added to `Host`.
3. Call `Host::initMemory()` once to create memory and seed constants. It must come after all `load()` calls.
4. Call `Host::runModule(moduleIndex)`. The index is a **module index**, not a function index; each module runs the entry function recorded in its header.
5. Check the return value or `Host::error()`. Use `Host::core()` (the `Core` of the most recent run) to read the stop position and stack.

`Host::core()` is `nullptr` until the first successful creation of a `Core`; the returned pointer becomes invalid after the next `runModule()` or when `Host` is destroyed.

## 4. Lifetime and ownership

### 4.1 Ownership table

| Object | Owns | Borrows | Constraints |
|---|---|---|---|
| `Host` | module `Parse` list, paths, constant address table, per-module `HostSlot` tables, `Memory`, most recent `Core` | — | Non-copyable; must outlive the `Core`s it creates and the ranges it lends out |
| `Parse` | module byte buffer (image spans point into it) | — | Copy deleted, movable; owned by `Host`; must outlive any `Core` using it |
| `Core` | call stack, copy extents, call table | `Memory` pointer, CODE span, FUNCS span, `HostSlot` span | One per run; treat as non-copyable; borrowed objects must outlive it |
| `Memory` | block table and block allocations | — | Non-copyable, non-movable; `Host` holds it with `unique_ptr`; one per `initMemory()` |
| `HostSlot` table | — | function and `user` pointers | Owned by `Host`; bound at load and valid until `Host` is destroyed |

### 4.2 Rules

- `Host` explicitly deletes its copy constructor and copy assignment.
- `Parse` owns the module bytes; CODE/FUNCS/constant spans in the image point into that buffer. `Core` only borrows those spans, so `Parse` must not be destroyed while a `Core` is running.
- `Host` stores `std::unique_ptr<Memory>` and `std::unique_ptr<Core>` internally. There is no module unload API; loaded modules live until `Host` is destroyed.
- `Memory` explicitly deletes copy and move operations. Copying the block table would create two owners whose `allocate()`/`free()` invalidate each other's caches.
- The copy operations of `Core` are not explicitly deleted in the current header; however, a copy duplicates the frame stack while still pointing at the same borrowed `Memory`/CODE/slot table, so copying a `Core` is not a supported usage. Do not copy it.
- When embedding directly, the `std::span<const CORE::HostSlot>` passed to `Core::attachImports` must remain valid for the whole run; do not pass the span of a temporary `std::vector`.
- `Host::module(index)` and `Host::path(index)` return references to internal objects; they are valid only while `Host` is alive.

## 5. Error model

### 5.1 Return codes

All ordinary errors are reported through `int` return codes; C++ exceptions are not used. Only `std::bad_alloc` from an allocation point can escape.

| Operation | Success | Failure |
|---|---|---|
| `Host::registerFunction` | `HOST::OK` | `HOST::BAD_ARGUMENT` (103) |
| `Host::load` | `HOST::OK` | `PARSE` 1..7, `HOST::UNLINKED_IMPORT` (102) |
| `Host::initMemory` | `HOST::OK` | `HOST::NO_MODULE` (100), `MEMORY` 1..3 |
| `Host::runModule` / `Host::run` | `CORE::OK` | `CORE` 1..16, `HOST::NO_MODULE` (100), `HOST::NO_MEMORY` (101) |
| `Memory::readAt` / `writeAt` / `reads` / `writes` | `MEMORY::OK` | `MEMORY` 1..3 |

Full code tables are in [03 · Runtime §6](03-runtime.md#6-error-and-status-model). `Host::error()` holds the most recent error code set by `Host`; use the return value of a given call to decide whether that call succeeded.

`CORE::NO_CALLER` (8) is an internal normal-completion signal and is never returned as a failure; after a root-frame `ret`, `error()` is `CORE::OK`.

### 5.2 CLI exit-code mapping

The following mapping belongs to the `motevm` CLI, not to the `Host` API itself:

| Exit code | Trigger |
|---|---|
| 0 | Load, verification and execution all succeeded; or the debugger executed `quit` at `CORE::DEBUG_BREAK` |
| 1 | Command-line usage error |
| 2 | Input/load failure: `PARSE` 1..7, `HOST::UNLINKED_IMPORT`, workspace or path errors, breakpoint/trace file errors |
| 3 | Runtime fault: `CORE` 1..16 (`DEBUG_BREAK` when the user executes `abort`); `TRAP`, `FUEL_EXHAUSTED`, `INTERRUPTED`, `MEMORY_FAULT`, etc. |
| 4 | Internal/environment error: `HOST::NO_MODULE`, `HOST::NO_MEMORY`, `HOST::BAD_ARGUMENT`, `MEMORY` errors from `initMemory`, allocation failure |

`CORE::DEBUG_BREAK` is not a fault: the CLI maps it to exit code 0 when the debugger executes `quit`, and to 3 on `abort`.

### 5.3 Diagnostics

- After `Host::load` returns `HOST::UNLINKED_IMPORT`, `unlinkedImportIndex()`, `unlinkedImportName()` and `unlinkedImportArity()` provide the index, name and arity of the unlinked import. These values are meaningful only for the most recent failed load.
- `Host::error()` may return a `PARSE` code after a failed load, a `MEMORY` code after a failed `initMemory`, or a `CORE` code after a failed run.
- `Host::core()` points at the `Core` created by the most recent run; `Core::error()`, `errorPc()`, `trapCode()` and `stack()` can be used to produce host-specific diagnostics.
- Successfully loaded modules can be inspected with `Host::moduleCount()`, `module(index)` and `path(index)`.

## 6. Memory API

### 6.1 Block management

| Method | Behavior |
|---|---|
| `int allocate(int block)` | Block index `0..65535`; on `RELEASED`, allocate 64 KiB and set `FREE`; on `FREE`, return `OK` idempotently; on `OCCUPIED`, return `BLOCK_OCCUPIED`; out of range returns `MEMORY_UNAVAILABLE` |
| `void free(int block)` | Release the mapping and set `RELEASED`; an invalid index is a no-op |
| `std::size_t allocatedBlocks() const` | Number of blocks that are not `RELEASED` |

The current allocator never sets a block to `OCCUPIED`, so `BLOCK_OCCUPIED` does not occur in the current runtime.

### 6.2 Access

| Method | Meaning |
|---|---|
| `readAt(dword &value, dword base, dword displacement)` | Read 4 bytes at `base + displacement` |
| `writeAt(dword value, dword base, dword displacement)` | Write 4 bytes at `base + displacement` |
| `read(dword &value, dword offset)` / `write(const dword &value, dword offset)` | 4-byte access with base 0 |
| `reads(std::span<byte> buffer, dword offset, dword size)` | Read `size` bytes from `offset` into the buffer |
| `writes(std::span<const byte> buffer, dword offset, dword size)` | Write `size` bytes from the buffer at `offset` |

### 6.3 Limits and errors

- The address space is 4 GiB, made of 65536 blocks of 64 KiB.
- Addresses are computed in 64 bits; an address greater than `ADDRESS_SPACE - 4` returns `MEMORY_OVERFLOW`.
- The accessed block must be allocated (`FREE`); an unmapped block returns `MEMORY_UNAVAILABLE`.
- 4-byte accesses and `reads`/`writes` may cross blocks, but every block involved must be allocated; `reads`/`writes` process the range block by block.
- If `size` passed to `reads`/`writes` is larger than the supplied buffer, the call returns `MEMORY_UNAVAILABLE`.
- Access never allocates or grows memory automatically. `Host::initMemory()` allocates only the blocks touched by constants; heap allocation is performed by host code calling `Memory::allocate()` itself (the CLI's `mem_alloc` does exactly that).
- Error codes: `OK` (0), `BLOCK_OCCUPIED` (1), `MEMORY_UNAVAILABLE` (2), `MEMORY_OVERFLOW` (3).

## 7. Host functions

### 7.1 Signature

```cpp
using HOST::Function = dword (*)(void *user, const dword *args, int arity);
```

`CORE::HostSlot` stores the same signature. `user` is the opaque pointer supplied at registration; `args` and `arity` are provided by the runtime at call time.

### 7.2 Arguments and return value

- `args` points at the caller's register array; `args[i]` corresponds to the caller's `r_i`. Only `args[0..arity-1]` belong to this call's argument window.
- The return value is written to the destination register of the `hostcall` instruction.
- All values are raw 32-bit values; floats are binary32 bit patterns. The runtime has no `Value`/string object; conventions such as `(address, length)` are defined by the host function itself.
- `args` is valid only during the call; the host function must not retain it.
- A host function **must not throw**: the runtime does not catch C++ exceptions at the call site.
- A host function **must not re-enter the VM**: it must not call `Host::run`/`runModule` or `Core::run`/`step`, nor destroy the host/memory objects during the call.
- A host function may read and write the `Memory` passed through `user`; the VM continues after it returns.

### 7.3 Registration API

```cpp
int Host::registerFunction(const std::string &name, int arity,
                           HOST::Function function, void *user = nullptr);
```

- Returns `HOST::OK` on success.
- An empty name, null function pointer, `arity < 0` or `arity > 256` returns `HOST::BAD_ARGUMENT`.
- A duplicate name returns `HOST::BAD_ARGUMENT` (even when the arity differs).
- Registration is a `Host`-level operation; all modules share one registry.

### 7.4 Link timing

Imports are resolved inside `Host::load()`: after `Parse::parse()` succeeds, `bindImports()` walks the module's IMPORTS table, searches the registry for a match, and binds each import to a `CORE::HostSlot`.

- Registration must be complete before the first `load()`.
- Modules loaded later use the registry as it exists at that time; already loaded modules are never rebound by later registrations.
- There is no deferred binding, runtime lookup or relinking API.

### 7.5 Host-function registration and linking

| Rule | Behavior |
|---|---|
| Name match | Exact byte-for-byte match of the string constant in IMPORTS (case-sensitive, no trimming) |
| Arity match | Must exactly equal the import arity recorded in the module |
| Duplicate registration | A second registration with the same name (regardless of arity) returns `HOST::BAD_ARGUMENT` |
| Invalid registration | Empty name, null function pointer, `arity < 0` or `arity > 256` returns `HOST::BAD_ARGUMENT` |
| Unlinked import | `Host::load()` returns `HOST::UNLINKED_IMPORT` (102) and the module is not added to `Host` |
| Binding granularity | One slot per IMPORTS entry; slot order matches the IMPORTS table |
| Call window | `Core` passes the caller's `r0..r(arity-1)` as `args` to the host function and writes the return value to the destination register |
| Lifetime | A binding stays valid after module load until `Host` is destroyed; there is no rebinding interface |

Diagnostics for `HOST::UNLINKED_IMPORT` are available through `Host::unlinkedImportIndex()`, `unlinkedImportName()` and `unlinkedImportArity()`. The CLI maps it to exit code 2 during load.

## 8. Fuel, interrupt and debug hooks

| Capability | `Host` interface | `Core` interface | Semantics |
|---|---|---|---|
| Fuel | `Host::setFuel(dword)` | `Core::setFuel(dword)` | 0 means unlimited; reset at the start of every run; checked before the next instruction executes; exhaustion returns `CORE::FUEL_EXHAUSTED` |
| Interrupt | `Host::setInterruptHook(bool (*)(void *), void *)` | `Core::setInterruptHook(...)` | Called before each instruction; returning `true` stops with `CORE::INTERRUPTED` |
| Debug | `Host::runModule(index, CORE::DebugHook, void *)` | `Core::setDebugHook(...)` | Called after fetch and before execution; returning `true` stops with `CORE::DEBUG_BREAK`; read-only, no re-entry |

- `Host` forwards its fuel and interrupt settings to every `Core` it creates.
- The debug hook passed to `Host::runModule(index, hook, user)` applies only to that call; `Host` clears it after the run and does not retain the `user` pointer.
- With fuel, an interrupt hook or a debug hook enabled, execution uses the guarded loop; the guarded loop always keeps the bounds check, even for verified modules.
- Hooks run synchronously on the VM thread and must return quickly. A debug hook may observe state only through the read-only accessors (see [03 · Runtime §4.6](03-runtime.md#46-debug-hook)).

## 9. Workspace

`Workspace` is a CLI-side path sandbox that `Host` does not know about:

- `Host` has no `Workspace` member and does not resolve or validate paths; it opens modules at the path it is given.
- The CLI creates a `Workspace` before execution, resolves each module path, and passes the resolved path to `Host::load(path, displayPath)`.
- `src/motevm/workspace.cpp` is independent of the runtime; compile and call it only if the embedder needs a path sandbox.
- File host functions are registered by the CLI and are not part of the runtime; the CLI enforces their path authorization with `Workspace`.

CLI workspace rules are in [`self/cli.md`](self/cli.md).

## 10. Threading and reentrancy

- `Host`, `Core`, `Memory` and `Parse` are not thread-safe and contain no locks.
- A VM instance is usable by one thread only; hooks and host functions are called synchronously on the executing thread.
- Host functions must not re-enter the VM. Calling `runModule` or `run` while the VM is executing conflicts with the current execution stack and state.
- Multiple threads may each create an independent `Host` and `Memory` and run in parallel; there is no shared runtime global state between them.
- `Memory` blocks are managed with `shared_ptr` but there is no cross-thread synchronization; do not access the same `Memory` concurrently.

## 11. Not implemented

The following **do not exist in the current implementation** and must not be treated as available interfaces in code or documentation:

- the `VM` class and the `Value`/`Result`/`RootedValue` types;
- a public umbrella header (for example `motevm.hpp`);
- an installed library, CMake package, pkg-config file or ABI promise;
- a runtime library target (runtime sources are compiled directly into the `motevm` executable);
- garbage collection, root sets/`RootedValue` mechanisms, a string/object model.

## 12. Related documentation

- Runtime semantics, error codes, memory and execution model: [03 · Runtime](03-runtime.md)
- Instruction set: [01 · ISA](01-isa.md)
- `.mbc` container and section layout: [02 · Bytecode format](02-bytecode-format.md)
- Toolchain and relocation: [05 · Toolchain](05-toolchain.md)
- Build: [06 · Build](06-build.md)
- Limits and bounds: [08 · Limits](08-limits.md)
- CLI workspace and exit codes: [`self/cli.md`](self/cli.md)
- Assembly example: [`examples/hello.mta`](../examples/hello.mta)
