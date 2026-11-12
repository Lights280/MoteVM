# 03 · Runtime

[简体中文](../03-runtime.md) | [English](03-runtime.md)

> **Internal implementation · Unstable · No ABI promise.** `Host`, `Core` and `Memory` are the current C++ runtime interfaces. They are compiled into the `motevm` executable, are not installed as a library, and have no public umbrella header. This document records the behavior observable in the current implementation; the implementation may change without notice.

## 1. Scope

This document describes the runtime that actually exists in `src/motevm`:

| Component | Files | Responsibility |
|---|---|---|
| `Parse` | `src/include/parse.*` | Reads and validates the `.mbc` container; owns the module byte buffer |
| `PARSE::Verifier` | `src/include/verifier.*` | Validates the instruction stream, control-flow targets and FUNCS/CODE cross tables |
| `Host` | `src/motevm/host.*` | Module registry, import binding, constant seeding, run orchestration |
| `Core` | `src/motevm/core.*` | One execution: frames, pc, instructions, fuel and hooks |
| `Memory` | `src/motevm/memory.*` | 4 GiB linear address space and 64 KiB blocks |
| `Workspace` | `src/motevm/workspace.*` | Path sandbox used only by the CLI; `Host` does not know it |
| `DEBUG` | `src/motevm/debug.*` | Breakpoints, stepping, trace and REPL; read-only access to `Core` |

Linkage facts:

- The runtime sources have no separate library target; they are compiled into the `motevm` executable.
- `motevm_core` is a static library containing `Parse`, `Verifier` and `Disasm` (sources in `src/include`).
- `motevm_tools` is a static library containing the assembler/disassembler toolchain built on `motevm_core`.
- There is no public umbrella header; use the concrete headers directly (see [04 · Embedding API](04-embedding-api.md)).

This document describes only the current implementation. Embedding, linking and examples are in [04 · Embedding API](04-embedding-api.md).

## 2. Load → verify → execute

### 2.1 Pipeline

```text
(optional) CLI resolves the workspace and module paths
        │
        ▼
Host::registerFunction(...)             Register host functions; must precede load
        │
        ▼
Host::load(path)                        Parse::load -> Parse::parse
        │                               parse() runs PARSE::Verifier internally
        │                               bindImports matches by (name, arity)
        ▼
Host::initMemory()                      Create Memory, allocate constant blocks, write constants
        │
        ▼
Host::runModule(index)                  Each call creates a new Core
        │  Core::attachVerifiedModule(...)
        │  Core::attachImports(...)
        │  Core::setFuel / setInterruptHook / setDebugHook
        ▼
Core::run(entryFunction)                Synchronous execution to completion
        │
        ▼
Host::error() / Core::error()           Read the final status
```

Boundaries between the roles:

| Stage | Failure channel | Notes |
|---|---|---|
| `Parse::load` / `Parse::parse` | `PARSE` 1..7 | File, header, CRC, section table, section content or instruction-rule errors |
| Import binding in `Host::load` | `HOST::UNLINKED_IMPORT` (102) | No registered function matches `(name, arity)` |
| `Host::initMemory` | `HOST::NO_MODULE` (100) or `MEMORY` 1..3 | Constant allocation or address-space overflow |
| `Host::runModule` | `CORE` 1..16, `HOST::NO_MODULE` (100), `HOST::NO_MEMORY` (101) | Runtime error or wrong call order |
| Successful run | `CORE::OK` (0) | `halt`, root-frame `ret`, or normal completion |

Rules:

- `Host::load` may be called multiple times. Each successful load appends one module; a module that fails to load is not added to `Host`.
- All `load()` calls must complete before `runModule()`, and `initMemory()` must be called first. Every `initMemory()` creates a new `Memory` and seeds it from scratch; it does not seed constants for modules loaded later.
- `Parse::parse()` runs `PARSE::Verifier` internally; therefore every module `Host` hands to `Core` is verified. Callers that use `Core` directly are responsible for this themselves (see §4.2).
- `Host::run()` runs the entry of every loaded module in load order. The CLI runs only the entry module, so it uses `runModule(0)`.

### 2.2 Host and Core relationship

- A `Host` owns: the module `Parse` objects and their paths, the per-module constant address table, the per-module `CORE::HostSlot` binding tables, one `Memory`, and the `Core` of the most recent run (`lastCore`).
- Every `Host::runModule()` creates a new `Core` and replaces `lastCore`. One run corresponds to one `Core`; the previous `Core` is destroyed when the next `runModule()` executes.
- `Core` owns nothing. It borrows one `Memory`, the CODE byte range, the FUNCS table range and the host slot table range; those objects must outlive the `Core`.
- Execution is synchronous: when `Core::run()` returns, the run has ended and the final state (`error()`, `errorPc()`, `instructionCount()`, `stack()`, etc.) can be read.
- Direct embedding may bypass `Host`: construct `Core` from a `Memory`, then call `attachModule()`/`attachVerifiedModule()`, `attachImports()` and `run()`. The caller is then responsible for `Parse`, verification, import binding and constant seeding.

## 3. Memory

### 3.1 Address space and blocks

| Constant | Value | Meaning |
|---|---|---|
| `MEMORY::BLOCK_SIZE` | 65536 | 64 KiB per block |
| `MEMORY::BLOCK_COUNT` | 65536 | Number of blocks |
| `MEMORY::ADDRESS_SPACE` | 4 GiB | `BLOCK_COUNT * BLOCK_SIZE` |

- The `Memory` constructor allocates no blocks. Blocks are allocated on demand by `Memory::allocate(index)` and return to an unmapped state after `free(index)`.
- Block states are `RELEASED` (unmapped) or `FREE` (allocated). Calling `allocate` again for an allocated block is idempotent and returns `MEMORY::OK`.
- A 4-byte access computes `base + displacement` in 64 bits, so a 32-bit register plus displacement cannot wrap and alias low memory. An address greater than `ADDRESS_SPACE - 4` returns `MEMORY_OVERFLOW`.
- A 4-byte access may cross a block boundary, but both blocks involved must be allocated; otherwise the access fails.
- A load/store to an unmapped address does not allocate a block automatically.

### 3.2 Constant seeding

Current behavior of `Host::initMemory()`:

1. Create an empty `Memory`.
2. Starting at address 0, walk the constants in module load order and then in table order within each module.
3. Align the cursor up to 8 bytes before each constant.
4. Reserve 4 bytes for a zero-length constant (advance the cursor by 4); write a non-zero payload and advance by `payload.size()`.
5. Call `Memory::allocate()` for every 64 KiB block touched.
6. Record the 32-bit address of each constant; retrieve it with `Host::constantAddress(module, index)`. Out-of-range module or index returns `NO_INDEX` (`0xFFFFFFFF`).

Constant addresses start at 0 and their order is fixed, so the same set of modules yields the same constant addresses. `Host::stats().constantBytes` is the final cursor value (bytes occupied by the constant pool).

### 3.3 Access and faults

The `load`/`sto` instructions in `Core` call `Memory::readAt`/`writeAt`. Any result other than `MEMORY::OK` maps to `CORE::MEMORY_FAULT`:

| Case | `Memory` returns | `Core` result |
|---|---|---|
| Block unallocated/released | `MEMORY_UNAVAILABLE` | `MEMORY_FAULT` |
| Address beyond the 4 GiB boundary | `MEMORY_OVERFLOW` | `MEMORY_FAULT` |
| Access succeeds | `OK` | No error |

`MEMORY_FAULT` is a runtime error: the faulting instruction is counted, `errorPc()` points at it, and `currentPc()` points at the next instruction.

## 4. Execution model

### 4.1 CODE and pc

- The CODE section is a byte sequence whose length must be a multiple of 4; each instruction is 4 bytes.
- `pc` is a **byte offset** into CODE, not an instruction index.
- The `entry` field in the FUNCS table is an **instruction index**; the function entry address is `entry * 4`.
- Registers are 256 raw 32-bit values `r0..r255`. Floating-point instructions interpret register contents as IEEE-754 binary32 bit patterns.
- `Core::textBytes()` returns the CODE size. `Core::codeWord(byteAddress)` returns the 32-bit instruction word at that byte address, or 0 when the address is out of range.

### 4.2 Verified versus unverified attach

| Entry point | Precondition | Bounds checking |
|---|---|---|
| `Core::attachModule(text, functions)` | None; intended for hand-built or unverified bytecode | Conservative: every fetch and every computed control-flow target is checked for range and 4-byte alignment |
| `Core::attachVerifiedModule(text, functions)` | Bytecode passed `PARSE::Verifier` | The unguarded fast loop may rely on verifier guarantees and omit the per-instruction bounds check |
| `Core::attachVerifiedModule(text, functions, copyExtents)` | As above, and reuses verifier-computed register copy extents | As above |

Observable differences:

- In conservative mode, an out-of-range or unaligned target is reported as `CORE::INVALID_TARGET` at runtime, so no illegal instruction is executed.
- Verified mode offers no such guarantee when used directly on unverified bytecode; illegal control flow has undefined behavior.
- When fuel, an interrupt hook or a debug hook is enabled, execution always uses the guarded loop, and the guarded loop keeps the bounds check even for verified modules. The difference above appears only on the unguarded fast path with no guard enabled.
- Attach failures: empty CODE or a length that is not a multiple of 4 returns `CORE::INVALID_TEXT`; a function entry outside CODE returns `CORE::INVALID_TARGET`.

`Core::attachImports(span<const CORE::HostSlot>)` binds the import slot table. `Core` only borrows the range; the caller must keep it valid for the lifetime of the `Core`.

### 4.3 Calls, frames and returns

- Each stack frame has 256 32-bit registers. `Core::run()` creates the root frame at the entry address and zeroes all root-frame registers.
- `call rA, #index`:
  - `index` is a FUNCS index; out of range returns `CORE::INVALID_FUNCTION`.
  - The target address is `functions[index].entry * 4`; it must be inside CODE, otherwise `CORE::INVALID_TARGET` is returned.
  - A new frame is pushed with the return address (the instruction after the call) and return register `A`.
  - A prefix of the caller's register window is copied into the new frame; then `pc` jumps to the target.
- Argument passing: the caller places arguments in its own `r0..r(arity-1)` before `call`. The callee sees those values in `r0..arity-1`.
- Copy extent: at callee entry, the first N registers of the new frame equal the first N registers of the caller. N covers at least the callee's declared `regcnt` and at least the `regcnt` of every function statically reachable from it, capped at 256. **Observable rule**: the current function may read only its own `r0..regcnt-1`; slots above the copied prefix are undefined. Do not rely on registers outside the declared range.
- `ret rX`: pops the current frame, writes `rX` into the caller's saved return register, and sets `pc` to the return address.
- A `ret` in the root frame means normal completion: there is no caller, the return value is discarded, and `Core::error()` remains `CORE::OK`. `halt` also completes normally.
- The call-depth limit is 1024, **including the root frame**. A `call` while 1024 frames already exist returns `CORE::STACK_OVERFLOW` and is not executed. `peakCallDepth()` reports the peak including the root frame, with a minimum of 1.

### 4.4 Fuel

- `Core::setFuel(maxInstructions)` sets the instruction budget for the next run; 0 means unlimited.
- The budget is reset to the configured value at the start of every `Core::run()`; `Host::setFuel()` forwards the value to every `Core` it creates.
- The check happens **before the next instruction executes**. With a budget of N, at most N instructions execute; the N+1-th instruction is rejected with `CORE::FUEL_EXHAUSTED` and is not executed.
- `FUEL_EXHAUSTED` is a boundary stop, not an instruction fault: `errorPc()` does not point at the instruction (it stays at the run-start value 0). Use `currentPc()` to read the stop position.

### 4.5 Interrupt hook

- `Core::setInterruptHook(bool (*hook)(void *), void *user)` installs the hook; `Host::setInterruptHook()` forwards it to every run.
- The hook is called before each instruction, after the fuel check and before the fetch and bounds check.
- Returning `true` stops execution: the current instruction is not executed, `Core::error()` is `CORE::INTERRUPTED`, and `currentPc()` points at that instruction.
- `INTERRUPTED` is a boundary stop: `errorPc()` stays 0; use `currentPc()`.
- The hook is called synchronously on the VM thread and must return quickly; it must not re-enter `Core` (no `run`/`step`).

### 4.6 Debug hook

- Type: `bool (*)(void *user, const Core &core, dword pc, dword word)`.
- Install with `Core::setDebugHook(hook, user)` or `Host::runModule(index, hook, user)`.
- Timing: after each instruction is fetched and before it is executed. `pc` points at the not-yet-executed instruction and `word` is its raw 32-bit encoding.
- Returning `true` stops execution: the instruction is not executed, `Core::error()` is `CORE::DEBUG_BREAK`, and both `errorPc()` and `currentPc()` point at that instruction.
- Read-only contract: the hook may read state through the read-only `Core` accessors (`stack()`, `codeWord()`, `errorPc()`, etc.), but it **must not re-enter** `Core` (no `run`/`step`) and must not rely on internal fields.
- `Host::runModule(index, hook, user)` clears the debug hook after the run and does not retain the `user` pointer.
- The debug hook may be combined with fuel and the interrupt hook; the check order is fuel → instruction count → interrupt → bounds → fetch → debug.

## 5. Determinism and observable state

Given the same module, the same initial memory contents and the same host-function behavior, the runtime guarantees:

- The same instruction execution sequence, final register/memory state, instruction count, peak call depth, error code and stop position.
- Constant addresses are allocated from 0 in a fixed order and do not depend on host pointers or address-space layout.
- Statistics contain no timestamps or host addresses.

Inputs/outputs that may vary:

- Host-function return values, I/O, time and randomness; the embedder implements these, and they are not part of the VM determinism contract.
- Commands read from stdin by the debugger, the breakpoint set, and the trace target.
- Decisions made by the interrupt hook.

## 6. Error and status model

All APIs report ordinary errors through return codes and do not use C++ exceptions. Only an allocation failure (`std::bad_alloc`) can escape from an allocation point; the CLI classifies that as an internal error.

### 6.1 CORE codes

| Value | Name | Meaning |
|---|---|---|
| 0 | `OK` | No error |
| 1 | `UNKNOWN_INSTRUCTION` | Reserved/unknown opcode |
| 2 | `DIVIDED_BY_ZERO` | Integer division or modulo by zero |
| 3 | `INTEGER_OVERFLOW` | `INT32_MIN / -1` or `-INT32_MIN` |
| 4 | `STACK_OVERFLOW` | Call depth exceeds 1024 (including the root frame) |
| 5 | `INVALID_TARGET` | pc or function entry outside CODE, or an unaligned control-flow target |
| 6 | `INVALID_TEXT` | CODE is empty or its length is not a multiple of 4 |
| 7 | `NO_ACTIVE_FRAME` | `step`/save/take without an active frame |
| 8 | `NO_CALLER` | Internal normal-completion signal for a root-frame `ret` |
| 9 | `NUMERIC_CONVERSION` | NaN/Inf/out-of-range in `f2i` |
| 10 | `INVALID_FUNCTION` | FUNCS index out of range |
| 11 | `INVALID_IMPORT` | IMPORTS index out of range or slot not bound |
| 12 | `FUEL_EXHAUSTED` | Instruction budget exhausted |
| 13 | `INTERRUPTED` | Interrupt hook requested a stop |
| 14 | `TRAP` | Raised by a `trap` instruction |
| 15 | `MEMORY_FAULT` | load/store failed |
| 16 | `DEBUG_BREAK` | Debug hook requested a stop before instruction execution |

`NO_CALLER` (8) is an internal signal; `Core::run()` converts a root-frame `ret` into normal completion, and `error()` remains `OK`. `DEBUG_BREAK` (16) is a debugger pause, not a fault.

### 6.2 HOST codes

| Value | Name | Meaning |
|---|---|---|
| 0 | `OK` | Success |
| 100 | `NO_MODULE` | No module loaded, or module index out of range |
| 101 | `NO_MEMORY` | `initMemory()` has not been called |
| 102 | `UNLINKED_IMPORT` | No host function matched `(name, arity)` during load |
| 103 | `BAD_ARGUMENT` | Invalid registration request |

### 6.3 PARSE codes

| Value | Name | Meaning |
|---|---|---|
| 0 | `OK` | Success |
| 1 | `FAIL_OPEN` | Failed to open/read the file |
| 2 | `EMPTY` | No input |
| 3 | `TRUNC` | File too short |
| 4 | `TOO_LARGE` | Exceeds the module size limit |
| 5 | `BAD_FILE` | Header, CRC or section table error |
| 6 | `BAD_CONTENT` | Section content error |
| 7 | `BAD_CODE` | Instruction-rule error (verifier) |

### 6.4 MEMORY codes

| Value | Name | Meaning |
|---|---|---|
| 0 | `OK` | Success |
| 1 | (reserved, unused) | — |
| 2 | `MEMORY_UNAVAILABLE` | Block unallocated, invalid range, or buffer too small |
| 3 | `MEMORY_OVERFLOW` | Address computation exceeds the 4 GiB address space (including cross-block/buffer bounds) |

### 6.5 Status accessors

| Method | Returns |
|---|---|
| `Core::error()` | Error code of the most recent run (`CORE::OK` on success); attach failures are reported by the attach method's return value |
| `Core::errorPc()` | Address of the instruction that reported `error()`; boundary stops (`FUEL_EXHAUSTED`, `INTERRUPTED`) do not set it and leave it at 0 |
| `Core::trapCode()` | The 24-bit immediate of the most recent `TRAP` |
| `Core::instructionCount()` | Instructions accounted for in the most recent run (reset at the start of each `run()`) |
| `Core::peakCallDepth()` | Peak call depth of the most recent run, including the root frame |
| `Core::stack()` | Read-only frame range; `frames[0]` is the root and `back()` is the active frame |
| `Core::currentPc()` | Current/stop pc |
| `Host::error()` | Most recent error code set by Host; use the return value of a given call to decide whether that call succeeded |
| `Host::stats()` | Cumulative runs, instruction count, peak depth, last pc, trap code and result fields |

Current semantics of `errorPc()`:

| Stop reason | Faulting instruction executed | `errorPc()` |
|---|---|---|
| Arithmetic error, `TRAP`, `MEMORY_FAULT`, `INVALID_FUNCTION`, `INVALID_IMPORT`, `STACK_OVERFLOW`, `NUMERIC_CONVERSION` | No (but counted) | Address of that instruction |
| `INVALID_TARGET` (fetch out of range) | No | The invalid target address |
| `INVALID_TARGET` (unaligned jump offset) | No | The jump instruction that computed the target |
| `DEBUG_BREAK` | No | The not-yet-executed instruction |
| `FUEL_EXHAUSTED`, `INTERRUPTED` | No | Stays 0; use `currentPc()` |
| Normal completion (root-frame `ret` / `halt`) | — | The last executed instruction |

## 7. Threading model

- `Host`, `Core`, `Memory` and `Parse` are not thread-safe and contain no mutexes.
- A VM instance is usable by one thread only; hooks and host functions are called synchronously on the executing thread.
- Do not share `Memory`, `Core` or `Host` between threads, and do not access these objects concurrently from a host function.
- Multiple threads may each create an independent `Host` and run in parallel; there is no shared runtime global state between them.

## 8. Related documentation

- Embedding, linking, ownership and host functions: [04 · Embedding API](04-embedding-api.md)
- Instruction set: [01 · ISA](01-isa.md)
- `.mbc` container and section layout: [02 · Bytecode format](02-bytecode-format.md)
- Limits and bounds: [08 · Limits](08-limits.md)
- CLI execution pipeline, exit codes and debugger usage: [`self/cli.md`](self/cli.md)
- Assembly example: [`examples/hello.mta`](../examples/hello.mta)
