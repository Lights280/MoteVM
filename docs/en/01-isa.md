# 01 · ISA Reference (Current Implementation)

[简体中文](../01-isa.md) | [English](01-isa.md)

This document describes the **current implementation** of MoteVM 0.1.0 (MOTEBC 1.0): a register-based 32-bit instruction set. Instructions are fixed at 4 bytes and stored little-endian in the CODE section. Registers are raw 32-bit dwords; floating-point values are simply IEEE-754 binary32 bit patterns in those registers. The current implementation has no garbage collector, no object model, no type tags, and no separate data stack.

Related documents: [overview](00-overview.md) · [bytecode format](02-bytecode-format.md) · [runtime](03-runtime.md) · [embedding API](04-embedding-api.md) · [moteasm](self/moteasm.md) · [motedis](self/motedis.md) · [example](../examples/hello.mta)

## 1. Scope

- This document covers only instructions and runtime semantics implemented in `src/` and usable through `moteasm` / `motevm`.
- Register fields in an instruction word are 8 bits: **256 registers per frame** (`r0`..`r255`).
- The current ISA defines **58 opcodes**; all other byte values are reserved and rejected at load time.
- Numeric operations use 32-bit two's complement or IEEE-754 binary32. There is no exception-flag register.

## 2. Encoding

### 2.1 Instruction word

Each instruction is one 32-bit word stored little-endian in the CODE section (the opcode byte appears first). The word layout is fixed:

```text
[31:24] c | [23:16] b | [15:8] a | [7:0] opcode
```

| Field | Bits | Meaning |
|---|---|---|
| `opcode` | `[7:0]` | Operation code, see section 4 |
| `a` | `[15:8]` | First operand byte; usually the destination register |
| `b` | `[23:16]` | Second operand byte |
| `c` | `[31:24]` | Third operand byte |

### 2.2 Operand formats

| Format | Fields used | Purpose |
|---|---|---|
| `op` | none | No operands (`HALT`, `BREAKPOINT`) |
| `r` | `a` | One register (`ZERO`, `RET`) |
| `rr` | `a`, `b` | Two registers (`MOV`, `NEG`, `BNOT`, unary float operations) |
| `rr8` | `a`, `b`, `c` | Two registers + 8-bit unsigned immediate (`LOAD`, `STO`) |
| `rrr` | `a`, `b`, `c` | Three registers (arithmetic, logic, compare, binary float operations) |
| `ri16` | `a`, `b`+`c` | One register + 16-bit immediate (`LHI`, `LLO`, `JZ`, `JNZ`) |
| `i24` | `a`+`b`+`c` | 24-bit immediate (`JMP`, `TRAP`) |
| `call16` | `a`, `b`+`c` | Destination register + 16-bit table index (`CALL`, `HOSTCALL`) |

The third field of `rr8` is an unsigned displacement in `0..255`.

### 2.3 Immediate construction

```text
imm16 = b | (c << 8)
imm24 = a | (b << 8) | (c << 16)        ; equivalent to word >> 8
```

- `LHI` / `LLO` use `imm16` as a raw 16-bit pattern; the assembler accepts `[-32768, 65535]`.
- `JZ` / `JNZ`'s `imm16` and `JMP`'s `imm24` are **signed byte offsets** relative to the next instruction.
- The 16-bit field of `CALL` / `HOSTCALL` is a table index and is not sign-extended.
- `TRAP`'s `imm24` is a trap code; the verifier requires `1..16`.

### 2.4 Alignment

- Every instruction is 4 bytes, so every legal jump target must be a multiple of 4.
- Jumps are encoded as byte offsets; the verifier requires `offset % 4 == 0` and a target inside the **current function's** instruction range.
- The CODE section length must be a multiple of 8 bytes. If the total instruction count is odd, the assembler appends one `NOP` (`0x00000000`, decoding as `ZERO` with all fields zero) at the end of CODE. That word belongs to no function and is not counted in `FUNCS.instr`.

## 3. Execution Model

### 3.1 Frames and registers

- Each call frame owns 256 32-bit registers, numbered `r0`..`r255`.
- Registers are untyped 32-bit patterns: integers are interpreted as two's complement, and float instructions reinterpret the same bits as IEEE-754 binary32.
- The root frame is zeroed when `run` starts.
- There is no dedicated stack pointer, frame pointer, or condition-flag register; programs maintain stack pointers and conditions in registers or memory themselves.

### 3.2 Program counter and execution order

- `pc` is a **byte address** inside CODE. The entry is `FUNCS[entry].entry * 4`.
- Fetch reads 4 bytes and advances `pc` by 4, then the instruction executes. Conditional jumps and `JMP` therefore use offsets relative to the **next instruction**.
- Sequential execution reaching past the end of CODE triggers `INVALID_TARGET (5)` (runtime bounds check). Verifier V62 requires every function to end in a terminator, so verified code cannot fall through into the next function; an unverified module can fall through between functions.

### 3.3 Calling convention

```text
CALL rDest, funcIndex
RET  rSrc
```

- `CALL` looks up the FUNCS table by the 16-bit index; the callee starts at `FUNCS[funcIndex].entry * 4`.
- `CALL` pushes a new frame, copies the caller's register window into it, and jumps to the callee entry.
- `RET` returns from the current frame: it writes `rSrc` into the caller's `rDest` from the `CALL` instruction, then restores the caller's `pc`.
- Changes the callee makes to other registers are not propagated back; the only return-value channel is the `CALL` destination register.
- A callee's `arity` describes how many arguments it expects in `r0..r(arity-1)`.

### 3.4 Arguments and return values

- The caller places arguments in `r0..r(arity-1)`; the callee reads them according to its own `arity`.
- The callee returns the 32-bit value of `rSrc` with `RET rSrc`.
- The `CALL` destination register may overlap argument registers; the return write happens after the caller is restored.
- Float arguments and return values are likewise just 32-bit bit patterns.

### 3.5 Call stack and depth limit

- The call stack is a set of frames, not a data stack; temporaries live in registers or flat memory.
- The maximum call depth is `MAX_CALL_DEPTH = 1024`, **including the root frame**. Exceeding it stops `CALL` with `STACK_OVERFLOW (4)`.
- Each call copies the caller's register window; the implementation may copy only the statically reachable prefix, but every register a verified function can address is copied, so the observed result is identical to copying the full window.

### 3.6 Termination

- `RET` in the root frame ends execution normally; the return value is discarded and this is not an error.
- `HALT` in any frame immediately ends the entire VM and leaves `error()` at `OK`.
- `TRAP` stops execution and reports `TRAP (14)`; the specific trap code is available from `trapCode()`.
- `BREAKPOINT` is a NOP without a debugger; a debugger hook is called before **every** instruction and may request a stop there (`DEBUG_BREAK (16)`). `BREAKPOINT` itself triggers nothing extra.

### 3.7 HOSTCALL

```text
HOSTCALL rDest, importIndex      ; 0x60, call16
```

- `importIndex` is an IMPORTS table index encoded in `b | (c << 8)`; `a` is the result register.
- At load time the loader binds each IMPORTS slot to a host callback by **name and arity**; no host address is stored in the bytecode.
- Arguments are placed in the current frame's `r0..r(arity-1)` and passed to the host function as a read-only pointer; the host function returns a 32-bit value that is written to `rDest`.
- `HOSTCALL` pushes no VM call frame and does not change the call stack; the host cannot directly modify current-frame registers other than by the returned value written to `rDest`.
- If the index is out of range, the slot is unbound, or the arity is invalid, execution stops with `INVALID_IMPORT (11)`.
- Assembly form: `HOSTCALL r1, print` or `HOSTCALL r1, #3`.
- A host callback must not throw and must not re-enter the VM.

## 4. Opcode Reference

All 58 defined opcodes follow. The “Trap” column lists the `CORE` error codes (section 5) the instruction may produce; `—` means the instruction itself cannot stop execution.

### 4.1 0x0x Immediate and Memory

| Byte | Mnemonic | Format | Operands | Semantics | Trap |
|---|---|---|---|---|---|
| `0x00` | `ZERO` | `r` | `rA` | `r[A] = 0` | — |
| `0x01` | `LOAD` | `rr8` | `rA, rB, imm8` | `r[A] = mem32[r[B] + imm8]` | `MEMORY_FAULT (15)` |
| `0x02` | `STO` | `rr8` | `rA, rB, imm8` | `mem32[r[B] + imm8] = r[A]` | `MEMORY_FAULT (15)` |
| `0x03` | `LHI` | `ri16` | `rA, imm16` | `r[A] = (r[A] & 0x0000FFFF) \| (imm16 << 16)` | — |
| `0x04` | `LLO` | `ri16` | `rA, imm16` | `r[A] = (r[A] & 0xFFFF0000) \| imm16` | — |

- The `LOAD` / `STO` displacement is unsigned 8-bit. The effective address is computed in 64 bits from `r[B] + imm8`, so it cannot wrap at 32 bits and alias low memory.
- `LHI` / `LLO` replace only one half of the destination register and leave the other half unchanged; the usual idiom is `LLO` then `LHI` to build a 32-bit constant.

### 4.2 0x1x Integer Operations

| Byte | Mnemonic | Format | Operands | Semantics | Trap |
|---|---|---|---|---|---|
| `0x10` | `MOV` | `rr` | `rA, rB` | `r[A] = r[B]` | — |
| `0x11` | `ADD` | `rrr` | `rA, rB, rC` | `r[A] = r[B] + r[C]` (mod 2³²) | — |
| `0x12` | `SUB` | `rrr` | `rA, rB, rC` | `r[A] = r[B] - r[C]` (mod 2³²) | — |
| `0x13` | `MUL` | `rrr` | `rA, rB, rC` | `r[A] = r[B] * r[C]` (low 32 bits) | — |
| `0x14` | `DIV` | `rrr` | `rA, rB, rC` | Signed division, truncating toward zero | `DIVIDED_BY_ZERO (2)`; `INTEGER_OVERFLOW (3)` |
| `0x15` | `MOD` | `rrr` | `rA, rB, rC` | Signed remainder; sign follows the dividend | `DIVIDED_BY_ZERO (2)` |
| `0x16` | `NEG` | `rr` | `rA, rB` | `r[A] = -r[B]` | `INTEGER_OVERFLOW (3)` |
| `0x17` | `ADDU` | `rrr` | `rA, rB, rC` | Unsigned addition; bit result identical to `ADD` | — |
| `0x18` | `SUBU` | `rrr` | `rA, rB, rC` | Unsigned subtraction; bit result identical to `SUB` | — |
| `0x19` | `MULU` | `rrr` | `rA, rB, rC` | Unsigned multiplication; bit result identical to `MUL` | — |
| `0x1A` | `DIVU` | `rrr` | `rA, rB, rC` | Unsigned division | `DIVIDED_BY_ZERO (2)` |
| `0x1B` | `MODU` | `rrr` | `rA, rB, rC` | Unsigned remainder | `DIVIDED_BY_ZERO (2)` |

- Addition, subtraction, and multiplication do not trap on signed overflow; results wrap modulo 2³².
- `DIV` traps when the divisor is zero; `INT32_MIN / -1` cannot be represented and triggers `INTEGER_OVERFLOW`.
- `MOD` traps when the divisor is zero; `INT32_MIN % -1` is defined as `0` and does not trap.
- `NEG` triggers `INTEGER_OVERFLOW` for `INT32_MIN`.

### 4.3 0x2x Bitwise Operations

| Byte | Mnemonic | Format | Operands | Semantics | Trap |
|---|---|---|---|---|---|
| `0x20` | `BAND` | `rrr` | `rA, rB, rC` | `r[A] = r[B] & r[C]` | — |
| `0x21` | `BOR` | `rrr` | `rA, rB, rC` | `r[A] = r[B] \| r[C]` | — |
| `0x22` | `BXOR` | `rrr` | `rA, rB, rC` | `r[A] = r[B] ^ r[C]` | — |
| `0x23` | `BNOT` | `rr` | `rA, rB` | `r[A] = ~r[B]` | — |
| `0x24` | `SHL` | `rrr` | `rA, rB, rC` | `r[A] = r[B] << (r[C] & 31)` | — |
| `0x25` | `SHR` | `rrr` | `rA, rB, rC` | Logical right shift: `r[A] = r[B] >> (r[C] & 31)` | — |
| `0x26` | `SAR` | `rrr` | `rA, rB, rC` | Arithmetic (sign-extending) right shift by `r[C] & 31` | — |

All shift instructions mask the shift amount to the low 5 bits (0..31).

### 4.4 0x3x binary32 Floating Point

Registers hold raw IEEE-754 binary32 bit patterns. Float instructions do not check type tags; they reinterpret the register contents directly.

| Byte | Mnemonic | Format | Operands | Semantics | Trap |
|---|---|---|---|---|---|
| `0x30` | `FADD` | `rrr` | `rA, rB, rC` | `f32(rB) + f32(rC)` | — |
| `0x31` | `FSUB` | `rrr` | `rA, rB, rC` | `f32(rB) - f32(rC)` | — |
| `0x32` | `FMUL` | `rrr` | `rA, rB, rC` | `f32(rB) * f32(rC)` | — |
| `0x33` | `FDIV` | `rrr` | `rA, rB, rC` | IEEE division; division by zero yields Inf/NaN | — |
| `0x34` | `FNEG` | `rr` | `rA, rB` | Flip the sign bit (preserves NaN payload) | — |
| `0x35` | `FABS` | `rr` | `rA, rB` | Clear the sign bit | — |
| `0x36` | `FSQRT` | `rr` | `rA, rB` | `sqrt(f32(rB))`; negative yields NaN | — |
| `0x37` | `I2F` | `rr` | `rA, rB` | `f32(int32(rB))` | — |
| `0x38` | `F2I` | `rr` | `rA, rB` | `int32(f32(rB))`, truncating toward zero | `NUMERIC_CONVERSION (9)` |
| `0x39` | `FEQ` | `rrr` | `rA, rB, rC` | `f32(rB) == f32(rC) ? 1 : 0` | — |
| `0x3A` | `FNE` | `rrr` | `rA, rB, rC` | `f32(rB) != f32(rC) ? 1 : 0`; NaN yields 1 | — |
| `0x3B` | `FLT` | `rrr` | `rA, rB, rC` | `f32(rB) < f32(rC) ? 1 : 0`; NaN yields 0 | — |
| `0x3C` | `FLE` | `rrr` | `rA, rB, rC` | `f32(rB) <= f32(rC) ? 1 : 0`; NaN yields 0 | — |
| `0x3D` | `FGT` | `rrr` | `rA, rB, rC` | `f32(rB) > f32(rC) ? 1 : 0`; NaN yields 0 | — |
| `0x3E` | `FGE` | `rrr` | `rA, rB, rC` | `f32(rB) >= f32(rC) ? 1 : 0`; NaN yields 0 | — |

- `FDIV` never traps on division by zero; `0/0` yields NaN and `x/0` yields ±Inf.
- `F2I` triggers `NUMERIC_CONVERSION` for NaN, ±Inf, and values outside `[-2³¹, 2³¹)`. The boundary `-2³¹` is accepted; `+2³¹` is not.
- `FSQRT` returns NaN for negative inputs and does not trap.
- The exact payload of an arithmetic NaN is determined by the host C++ floating-point implementation; `FNEG` / `FABS` are pure bit operations.
- `0x3F` is reserved.

### 4.5 0x4x Compare

Comparison results are written to `r[A]` as 0 or 1.

| Byte | Mnemonic | Format | Operands | Semantics | Trap |
|---|---|---|---|---|---|
| `0x40` | `EQ` | `rrr` | `rA, rB, rC` | Bit equality: `r[B] == r[C]` | — |
| `0x41` | `NE` | `rrr` | `rA, rB, rC` | Bit inequality | — |
| `0x42` | `LT` | `rrr` | `rA, rB, rC` | Signed less than | — |
| `0x43` | `LE` | `rrr` | `rA, rB, rC` | Signed less than or equal | — |
| `0x44` | `GT` | `rrr` | `rA, rB, rC` | Signed greater than | — |
| `0x45` | `GE` | `rrr` | `rA, rB, rC` | Signed greater than or equal | — |
| `0x46` | `LTU` | `rrr` | `rA, rB, rC` | Unsigned less than | — |
| `0x47` | `LEU` | `rrr` | `rA, rB, rC` | Unsigned less than or equal | — |
| `0x48` | `GTU` | `rrr` | `rA, rB, rC` | Unsigned greater than | — |
| `0x49` | `GEU` | `rrr` | `rA, rB, rC` | Unsigned greater than or equal | — |

### 4.6 0x5x Control Flow

| Byte | Mnemonic | Format | Operands | Semantics | Trap |
|---|---|---|---|---|---|
| `0x50` | `JMP` | `i24` | `offset` | `pc = next_pc + int24` | `INVALID_TARGET (5)` |
| `0x51` | `JZ` | `ri16` | `rA, offset` | If `r[A] == 0`: `pc = next_pc + int16` | `INVALID_TARGET (5)` |
| `0x52` | `JNZ` | `ri16` | `rA, offset` | If `r[A] != 0`: `pc = next_pc + int16` | `INVALID_TARGET (5)` |
| `0x53` | `CALL` | `call16` | `rDest, funcIndex` | Push a frame, copy registers, jump to the FUNCS entry | `INVALID_FUNCTION (10)`, `INVALID_TARGET (5)`, `STACK_OVERFLOW (4)` |
| `0x54` | `RET` | `r` | `rSrc` | Write back to the caller's `rDest` and return; normal end in the root frame | — |
| `0x55` | `HALT` | `op` | — | Immediately stop the entire VM | — |
| `0x56` | `BREAKPOINT` | `op` | — | NOP without a debugger | — |
| `0x57` | `TRAP` | `i24` | `code` | Stop with trap code `code` | `TRAP (14)` |

- Branch offsets are signed byte offsets relative to the next instruction, must be multiples of 4, and must target the current function (verifier V61).
- An unverified module encoding a misaligned offset may stop with `INVALID_TARGET` before jumping; verified modules cannot.
- `CALL`'s index is a FUNCS index; an entry outside CODE yields `INVALID_TARGET`.
- `RET` in the root frame is a normal termination and produces no error.
- `TRAP`'s `code` is restricted to `1..16` by the verifier; the runtime stores it in `trapCode()` and reports `TRAP (14)`.
- `DEBUG_BREAK (16)` comes from a debug hook's stop request on any instruction, not from `BREAKPOINT` specifically.
- Trap names accepted by the assembler: `TypeMismatch`, `DivByZero`, `IntegerOverflow`, `NumericConversion`, `IndexOutOfBounds`, `NullAccess`, `StackOverflow`, `InvalidBytecode`, `UnlinkedImport`, `OutOfMemory`, `HostError`, `FuelExhausted`, `Interrupted`, `NotImplemented`, `InternalError`, `InvalidArgument`.

### 4.7 0x6x System

| Byte | Mnemonic | Format | Operands | Semantics | Trap |
|---|---|---|---|---|---|
| `0x60` | `HOSTCALL` | `call16` | `rDest, importIndex` | Call the host function bound to the IMPORTS slot and write its result to `rDest` | `INVALID_IMPORT (11)` |

See §3.7 HOSTCALL.

## 5. Traps and Errors

`CORE` error codes are engine execution status, not the 1..16 trap-code space carried by the `TRAP` instruction. After execution stops:

- `error()` returns a nonzero code from the table below; after a normal `HALT` or root-frame `RET` it remains `OK (0)`.
- `errorPc()` points at the instruction that reported the error; for `DEBUG_BREAK` it points at the instruction that has not yet executed. Errors raised at entry/attach time (such as `INVALID_FUNCTION` or `INVALID_TEXT`) may have no corresponding instruction and report 0.
- `currentPc()` is the execution loop's current/next `pc`.
- `trapCode()` holds the 1..16 immediate only after a `TRAP` instruction.

| Code | Name | Trigger |
|---|---|---|
| 0 | `OK` | Normal end or no error |
| 1 | `UNKNOWN_INSTRUCTION` | Undefined opcode encountered; possible only in an unverified module |
| 2 | `DIVIDED_BY_ZERO` | `DIV` / `DIVU` / `MOD` / `MODU` with a zero divisor |
| 3 | `INTEGER_OVERFLOW` | `INT32_MIN / -1`, or `NEG INT32_MIN` |
| 4 | `STACK_OVERFLOW` | Call depth exceeds `MAX_CALL_DEPTH` (1024, including the root frame) |
| 5 | `INVALID_TARGET` | Jump target not 4-byte aligned or out of range; function entry or `pc` outside CODE |
| 6 | `INVALID_TEXT` | Attached CODE is empty or not a multiple of 4 bytes |
| 7 | `NO_ACTIVE_FRAME` | Step/save/restore without an active frame (engine API level) |
| 8 | `NO_CALLER` | Internal signal after a root-frame `RET` pops; handled as a normal end |
| 9 | `NUMERIC_CONVERSION` | `F2I` on NaN, ±Inf, or a value outside `[-2³¹, 2³¹)` |
| 10 | `INVALID_FUNCTION` | FUNCS index out of range |
| 11 | `INVALID_IMPORT` | IMPORTS index out of range, unbound, or invalid arity |
| 12 | `FUEL_EXHAUSTED` | Host instruction budget exhausted |
| 13 | `INTERRUPTED` | Interrupt hook requested a stop |
| 14 | `TRAP` | A `TRAP` instruction executed; see `trapCode()` for the code |
| 15 | `MEMORY_FAULT` | `LOAD` / `STO` accessed an unallocated, occupied, or beyond-4-GiB address |
| 16 | `DEBUG_BREAK` | Debug hook requested a stop before an instruction executed |

- A load-time verification failure produces no `CORE` code; the module is rejected before execution starts.
- Codes 6, 7, 12, 13, and 16 are mainly host/debugger controls, not ISA faults raised by ordinary programs.

## 6. Memory Model

- The address space is a **flat 4 GiB space** (`0x00000000`..`0xFFFFFFFF`) managed in **64 KiB blocks**: 65536 blocks.
- Each block has one of two states:

| State | Meaning |
|---|---|
| `RELEASED` | Not allocated; any access faults |
| `FREE` | Allocated and readable/writable |

- Blocks are allocated on demand by the host through `Memory::allocate`; `LOAD` / `STO` **never** allocate a block automatically. Accessing a `RELEASED` block produces `MEMORY_FAULT (15)`; calling `allocate` again for an allocated block is idempotent.
- The effective address is computed in 64 bits: `address = r[B] + imm8`. When `address > 4 GiB - 4`, the access produces `MEMORY_FAULT` instead of wrapping at 32 bits.
- Accesses are always 4 bytes wide and little-endian. Unaligned accesses are allowed.
- A 4-byte access may cross a 64 KiB block boundary: the low and high parts land in adjacent blocks, and both blocks must be `FREE`; if either is unavailable, the access faults.
- A cross-block write may already have written part of the first block before the second block faults, so the destination memory must not be assumed unchanged after such a fault.
- Memory and registers are separate; there are no implicit object headers and no GC metadata.

## 7. Load-Time Verification Rules

Modules are verified at load time. The rules below are the ones most easily triggered by hand-written bytecode or a manually modified `.mbc`. A failed rule rejects the whole module; nothing executes partially.

| Rule | Requirement (author-facing) |
|---|---|
| V51 | Every 4-byte word in CODE must decode to a defined opcode, and unused fields must be zero (for example `MOV`'s `c` and all three fields of `HALT`). The trailing `NOP` in CODE also satisfies this rule. |
| V53 | Every register number actually used by an instruction must be less than the function's declared `regs`. |
| V56 / V59 | `CALL`'s FUNCS index must be less than the function count; the callee's `arity` must not exceed the caller's `regs`. |
| V56 / V60 | `HOSTCALL`'s IMPORTS index must be less than the import count; the import's `arity` must not exceed the current function's `regs`. |
| V57 | `TRAP`'s trap code must be in `1..16`. |
| V61 | `JMP` / `JZ` / `JNZ` byte offsets must be multiples of 4, and targets must lie inside the same function. |
| V62 | The last instruction of each function must be a terminator: `JMP`, `RET`, `TRAP`, or `HALT`. |
| V76 | The module header's entry function index must be less than the FUNCS count. |
| V77 | The entry function's `arity` must be 0. |

Before writing a `.mbc`, the assembler also self-checks the image with the same `Parse` / `Verifier` implementation; failure reports `E031` and exits with an internal error.
