# moteasm Assembler

[简体中文](../../self/moteasm.md) | [English](moteasm.md)

`moteasm` assembles UTF-8 source files (`.mta`) into MOTEBC 1.0 modules (`.mbc`). It accepts only the current implementation's own ISA instructions; instruction semantics are in [01 · ISA Reference](../01-isa.md), and the container layout is in [02 · Bytecode Format](../02-bytecode-format.md). See [05 · Toolchain](../05-toolchain.md) for the toolchain overview. Tool version 0.1.0; the project uses C++20 and the MIT license.

```text
foo.mta (UTF-8) --moteasm--> foo.mbc (MOTEBC 1.0) --motevm--> stdout/trap
```

## 1. Source File Rules

| Rule | Description |
|---|---|
| Encoding | UTF-8; optional BOM; LF or CRLF |
| Size | At most 64 MiB (`E002`) |
| Comments | `;` to end of line; no block comments |
| Separators | Spaces, tabs, and commas all separate tokens |
| Line structure | `[label:] [directive\|instruction] [; comment]`; at most one statement per line |
| Case | Mnemonics and directives are case-insensitive; identifiers, names, and labels are case-sensitive |
| Empty file | Legal, but a module with no `.func` reports `E020` |

## 2. Lexical Rules

### 2.1 Identifiers and Reserved Words

- Identifier: `[A-Za-z_][A-Za-z0-9_]*`.
- The following reserved words (case-insensitive) cannot be used as names: `auto`, `const`, `true`, `false`, `null`, `nan`, `inf`.
- Names and labels are case-sensitive: `Main` and `main` are different.

### 2.2 Integer and Float Literals

- Integers: optional `+`/`-`, decimal, `0x`/`0X` hexadecimal, or `0b`/`0B` binary; `_` may separate digits. Parsing uses signed 64-bit range.
- Floats: decimal or scientific notation, with an optional trailing `f`/`F`; also accepts `nan`, `+nan`, `-nan`, `inf`, `+inf`, `-inf`, leading-dot forms such as `.5`, and `f64=0x<16 hex digits>` for a raw 64-bit bit pattern.
- `NaN` constants are generated from a fixed bit pattern and produce warning `W005`.
- Float values in the constant pool are stored as 64-bit double payloads; the current ISA has no “load constant” instruction, so float register values must be constructed by the program.

### 2.3 Strings and Escapes

Strings are enclosed in double quotes, stored as bytes, and may contain UTF-8 directly. Supported escapes:

| Escape | Meaning |
|---|---|
| `\\` | Backslash |
| `\"` | Double quote |
| `\n` | Newline (0x0A) |
| `\r` | Carriage return (0x0D) |
| `\t` | Tab (0x09) |
| `\0` | NUL (0x00) |
| `\xHH` | Exactly two hexadecimal digits denoting one byte |
| `\u{H...}` | 1..6 hexadecimal digits denoting a Unicode scalar; surrogates and values above U+10FFFF are rejected |

An unknown escape, an unterminated string, or an invalid `\x` / `\u{}` reports `E009`, `E008`, and `E009` respectively.

### 2.4 Registers, Indexes, and Labels

- Registers are `r0`..`r255` (prefix `r` or `R`; the number is parsed with integer syntax, normally decimal). Semantically the number must be less than the enclosing function's declared `regs`, otherwise `E022`.
- A raw index is `#n` in `0..65535` (`E024`); its meaning depends on position:
  - CONST constant index: `.import` / `.global` / `.func` names, `.local` names, `.global` init (init may reference any constant entry);
  - FUNCS index: `.entry #n`, `CALL rDest, #n`;
  - IMPORTS index: `HOSTCALL rDest, #n`.
- Import names also accept dotted identifiers such as `file.write` (the built-in
  `file.*` host functions). Both `.import file.write 4` and
  `HOSTCALL rDest, file.write` are valid.
- A label is `name:`, allowed only inside `.func`, at most one per line. A label name may be an ordinary identifier (not a reserved word) or the local form `.L` followed by `[A-Za-z0-9_]+` (for example `.L0`, `.L_loop`). A label's scope is the **current function**.

## 3. Directives

### 3.1 Directive Table

| Directive | Syntax | Description |
|---|---|---|
| `.module` | `.module <string\|word>` | Module name; must be the first statement, at most once |
| `.source` | `.source <string\|word>` | Alias for `.meta 2, ...`; source file name; an absolute path triggers `W006` |
| `.meta` | `.meta <u16>, <string\|word>` | Metadata key/value; key 1 is the module name, 2 source file, 3 build ID, 4 comment; keys 5..65535 are reserved and trigger `W003` |
| `.const` | `.const <literal>` | Explicit constant mode; index equals declaration order |
| `.string` | `.string <name> <string>` | Named constant: string |
| `.int` | `.int <name> <integer>` | Named constant: integer |
| `.float` | `.float <name> <number>` | Named constant: float (integers converted through double) |
| `.bool` | `.bool <name> true\|false` | Named constant: boolean |
| `.null` | `.null <name>` | Named constant: null |
| `.import` | `.import <name\|#n> <arity> [, flags=0x00]` | IMPORTS entry; `name` is an identifier or a dotted identifier such as `file.write`, and must be a String constant; arity 0..16; flags must be 0 |
| `.global` | `.global <name\|#n\|anonymous> [const] [<init>]` | GLOBALS entry; init is a literal or `#CONST index` |
| `.entry` | `.entry <name\|#n>` | Entry FUNCS index; the target arity must be 0 |
| `.func` | `.func <name\|#n\|anonymous> <arity> <regs\|auto> [, flags=0xNN]` | Starts a function block; the compatibility form `.func <name> <regs>` also exists (arity=0) |
| `.end` | `.end` | Ends the current function block; required, no operands |
| `.loc` | `.loc ["<file>",] <line>[, <column>]` | DEBUG line table; the first `.loc` must precede the first instruction |
| `.local` | `.local <name\|#n>, <reg>, [param], [start=<pc\|label>], [end=<pc\|label>]` | DEBUG local variable |

A `#n` name (`.import` / `.global` / `.func` / `.local`) must reference a String constant in the Const section; a `.global` init `#n` references any constant entry. `.entry #n` is a FUNCS index, not a constant index.

### 3.2 Declaration Order

Violations report `E018`. The legal structure is:

```text
[.module]
{.source | .meta}*
( {.const}* | {.string|.int|.float|.bool|.null}* )
{.import | .global}*
[.entry]
{.func ... .end}+
```

- `.module` must precede every other statement (comments and blank lines are not statements).
- `.source` / `.meta` must precede constants.
- Constants must precede `.import` / `.global`.
- `.import` and `.global` may be interleaved, but both must precede `.entry` / `.func`.
- `.entry` precedes `.func` and appears at most once.
- Function blocks cannot nest; every `.func` needs a matching `.end`.
- Labels, instructions, `.loc`, and `.local` may appear only between `.func` and `.end`.

### 3.3 Constant Pool Modes

The two modes cannot be mixed (`E016`):

**Explicit mode** (when `.const` appears)

- Each `.const <literal>` is appended in declaration order, with indexes starting at 0.
- References may point only to declared constants; an undeclared reference reports `E017`.
- Duplicate declarations of the same value are allowed and occupy distinct indexes.
- Canonical text produced by `motedis` uses this mode.

**Named mode** (when `.string` / `.int` / `.float` / `.bool` / `.null` appears)

- Named constant declarations enter the pool in declaration order; declarations are not deduplicated by value, so identical values occupy separate entries.
- Later names and init literals are reused by content: if an entry with the same `kind + value` already exists, its index is reused.
- Constant-pool indexes are assigned by first appearance in this order:
  1. Named constant declarations (declaration order)
  2. `.import` names
  3. `.global` names
  4. `.func` names
  5. `.global` init literals
  6. `.local` names
- If neither mode appears, names default to named-mode handling.

`.const` accepts string, integer, float, `true` / `false`, `null`, `nan` / `inf`, and `f64=0x...` literals.

### 3.4 Functions and Entry

- A `.func` name may be an identifier, a `#FUNCS index`, or `anonymous`.
- arity is `0..16` (`E021`); `regs` is `1..256` (`E020`) or `auto`.
- `auto` sets `regs` to the highest register number actually used plus one (also considering `.local` slots), with a minimum of 1; values above 256 report `E020`.
- The compatibility form `.func <name> <regs>` is equivalent to arity=0 with the given regs.
- `flags=` defines only bit0 (exported); any other bit reports `E033`.
- A function body cannot be empty, and its last instruction must be `JMP`, `RET`, `TRAP`, or `HALT` (`E019`).
- When `.entry` is omitted, the entry function index is 0. An `.entry` name must correspond to a named function (the name `anonymous` itself is unusable, but `#index` can point at an anonymous function), and the target arity must be 0 (`E026`).
- Functions enter the FUNCS table in declaration order; `entry_pc` is the running sum of preceding function lengths.

### 3.5 Debug Information

- `.loc` may appear only inside `.func`. The first `.loc` must precede the function's first instruction, otherwise `E034`.
- Line numbers are `1..2^31-1`; the column is an optional 32-bit unsigned value.
- A record is emitted only when `(line, column)` changes; `pc` is the function-relative instruction index, and the first record must have `pc=0`.
- A `.local` slot must be less than `regs`; a `param` slot must be less than `arity`. `start=` / `end=` accept a numeric pc or a label in the current function; the pc is a function-relative instruction index (same as `.loc`). The defaults are start=0 and end=function length.
- The file name in `.loc` must be consistent across all functions (`E033`); an absolute path triggers `W006`.

## 4. Instruction Syntax

Mnemonics are case-insensitive. Operand formats match [ISA §2.2](../01-isa.md).

| Format | Operands | Mnemonics in this ISA |
|---|---|---|
| `op` | none | `HALT`, `BREAKPOINT` |
| `r` | `rA` | `ZERO`, `RET` |
| `rr` | `rA, rB` | `MOV`, `NEG`, `BNOT`, `FNEG`, `FABS`, `FSQRT`, `I2F`, `F2I` |
| `rr8` | `rA, rB, imm8` | `LOAD`, `STO` (imm8 is 0..255) |
| `rrr` | `rA, rB, rC` | `ADD`/`SUB`/`MUL`/`DIV`/`MOD`/`ADDU`/`SUBU`/`MULU`/`DIVU`/`MODU`, `BAND`/`BOR`/`BXOR`, `SHL`/`SHR`/`SAR`, `FADD`/`FSUB`/`FMUL`/`FDIV`, `FEQ`/`FNE`/`FLT`/`FLE`/`FGT`/`FGE`, `EQ`..`GEU` |
| `ri16` | `rA, imm16` | `LLO`, `LHI` (accepts `-32768..65535`) |
| `ri16` | `rA, target` | `JZ`, `JNZ` |
| `i24` | `target` | `JMP` |
| `i24` | `code` | `TRAP` (1..16 or a trap name) |
| `call16` | `rDest, <func\|#idx>` | `CALL` |
| `call16` | `rDest, <import\|#idx>` | `HOSTCALL` |

- A jump target may be a label in the current function or a `+n` / `-n` byte offset; the offset must be a multiple of 4, and the target must be inside the current function.
- `CALL` targets a function name or `#FUNCS index`; `HOSTCALL` targets an import name (including dotted names such as `file.write`) or `#IMPORTS index`.
- `TRAP` accepts a number `1..16` or one of these names: `TypeMismatch`, `DivByZero`, `IntegerOverflow`, `NumericConversion`, `IndexOutOfBounds`, `NullAccess`, `StackOverflow`, `InvalidBytecode`, `UnlinkedImport`, `OutOfMemory`, `HostError`, `FuelExhausted`, `Interrupted`, `NotImplemented`, `InternalError`, `InvalidArgument`.
- `LLO` / `LHI` immediates are encoded as raw 16-bit patterns; canonical output uses `0x%04X`.

Example:

```asm
.func main 0 8
    llo r0, 0x1234
    lhi r0, 0x0001          ; r0 = 0x00011234
    load r1, r0, 4
    sto r1, r0, 8
    add r2, r0, r1
    jz r2, done
    llo r2, 0
done:
    trap DivByZero          ; stop with trap code 2
.end
```

## 5. Labels and Two-Pass Assembly

1. **Pass 1**: Tokenize and parse line by line, building the module, constants, imports, globals, functions, labels, and instruction model; assign function `entry_pc` and per-function pc; record `.loc` / `.local`; check structure, terminators, arity/regs/flags, duplicate labels, and more.
2. **Pass 2**: Resolve names and constants; validate table indexes, registers, entry, and debug ranges; encode instructions and backpatch jump/call targets; assemble sections, compute the CRC, and write atomically.
3. **Self-check**: The produced image must pass `Parse` / `Verifier`; failure reports `E031` and exits with an internal error.

Labels are resolved in pass 2, so forward references are legal. A label's scope is its containing function; referencing a label in another function reports `E025`, and a label that does not exist at all reports `E011`. Relative offsets `+n` / `-n` are in bytes and must be multiples of 4 (`E012`).

## 6. Output, CODE Alignment, and DEBUG

- The CODE section must be a multiple of 8 bytes. If the total instruction count across all functions is odd, one `NOP` (`0x00000000`) is appended at the end of CODE. That `NOP` belongs to no function and is not counted in `FUNCS.instr`.
- The default output path replaces the input extension with `.mbc`; `-o -` writes to stdout.
- `--listing FILE` writes each instruction's function name, FUNCS index, function-relative pc, encoded word, and source text, plus constant/function/global/import statistics.
- DEBUG section:
  - No DEBUG section is generated when there are no `.loc` / `.local` records.
  - `-g, --debug` forces a DEBUG section (an 8-byte header is written even with no records).
  - `--no-debug` discards all records and generates no DEBUG section; it cannot be combined with `-g`.
  - A `.local`'s `FUNCS.local_count` is the number of distinct slots; each function's debug block offset is 8-byte aligned.

## 7. Command Line

```text
usage:
  moteasm [options] <input.mta>

options:
  -o, --output FILE   output .mbc (default: input with .mbc; '-' = stdout)
  -g, --debug         force a DEBUG section
      --no-debug      omit the DEBUG section
      --listing FILE  write a listing
      --max-errors N  stop storing after N errors (0 = unlimited, default 20)
      --warn-as-error warnings are failures
      --check-roundtrip  re-assemble the canonical text and compare bytes
      --version | --help

exit codes:
  0 success   1 usage   2 input/source error   4 internal or I/O error
```

Additional notes:

- `-h` is also accepted for `--help`, and `-v` for `--version`.
- `--max-errors 0` means no user limit; the diagnostic collector stores at most 1000 records.
- Input `-` means stdin; in that case an output must be specified explicitly, unless the output is also `-`.
- Input and output cannot be the same path (usage error).
- `--check-roundtrip` failures are internal errors with exit code 4.

## 8. Diagnostic Codes

Format:

```text
moteasm: <path>:<line>:<col>: error: E012: message
moteasm: <path>:<line>:<col>: warning: W001: message
moteasm: N error(s), M warning(s)
```

Diagnostics without a source position omit `path:line:col`. Diagnostics are sorted by line, column, and code, then deduplicated.

### 8.1 Errors

| Code | Meaning |
|---|---|
| `E001` | Lexical/syntax error: invalid UTF-8, invalid name or label, unexpected string, multiple labels on one line |
| `E002` | Source file exceeds 64 MiB |
| `E003` | Unknown/reserved mnemonic, or an illegal `i24` instruction |
| `E004` | Unknown directive |
| `E005` | Invalid literal (float, boolean, etc.) |
| `E006` | Invalid integer/numeric literal |
| `E007` | Register syntax error or number outside `0..255` |
| `E008` | Unterminated string |
| `E009` | Invalid string escape |
| `E010` | Immediate outside the instruction's allowed range |
| `E011` | Name/label reference error: unknown label, function, or import; wrong operand kind |
| `E012` | Jump target error: offset not a multiple of 4, target outside the function, or offset does not fit int16/int24 |
| `E013` | Wrong operand count or form; duplicate `flags=` / `start=` / `end=` |
| `E014` | Invalid name: reserved word, constant name not an identifier, or name given as a string |
| `E015` | Constant pool, function table, global table, import table or local table exceeds 65536 entries |
| `E016` | Explicit `.const` and named constant declarations mixed |
| `E017` | Explicit mode references an undeclared constant |
| `E018` | Declaration order or location error |
| `E019` | Function body is empty, or the last instruction is not a terminator |
| `E020` | Module has no functions; or regs is invalid, or `auto` exceeds 256 |
| `E021` | arity outside `0..16` |
| `E022` | Register/arity constraint violated: operand number >= regs, arity > regs, callee/import arity exceeds caller regs, or local slot >= regs |
| `E023` | Numeric/index/range error: trap code, meta key, name length, raw index, local pc range, line/column, init index, etc. |
| `E024` | A `#n` raw index is required, or the index is outside `0..65535` |
| `E025` | Label exists only in another function (cross-function reference) |
| `E026` | `.entry` target problem: index out of range, not found, anonymous, or arity not 0 |
| `E027` | `.func` / `.end` structure error: nested `.func`, missing `.end`, or stray `.end` |
| `E030` | Duplicate declaration: module name, meta key, entry, label, constant, function, import, or global name |
| `E031` | Internal consistency/emission failure: self-check failure or section alignment/size error |
| `E033` | Flags or source-file conflict: invalid flags, FUNCS only bit0, IMPORTS flags nonzero, or multiple different source file names |
| `E034` | DEBUG line table error: first `.loc` after the first instruction, table not starting at pc 0, non-increasing pc, or invalid entry |
| `E035` | Emitted module image exceeds 64 MiB |

The current implementation does not use `E028`, `E029`, or `E032`.

### 8.2 Warnings

| Code | Meaning |
|---|---|
| `W001` | Unreachable instruction according to control flow (including JMP/JZ/JNZ targets) |
| `W003` | Unknown meta key, preserved verbatim |
| `W005` | NaN constant generated from a fixed bit pattern |
| `W006` | Absolute source path used, reducing reproducibility |
| `W007` | `.global` explicit `null` init differs from the omitted-init sentinel |

The current implementation does not produce `W002` or `W004`. `--warn-as-error` treats any warning as a failure.

## 9. Complete Example

The following program matches [hello.mta](../../examples/hello.mta) in the repository. The three imports are bound to `motevm`'s built-in host functions.

```asm
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

```text
$ moteasm hello.mta -o hello.mbc
$ motevm hello.mbc
hello, mote
```

`const_addr` and `const_len` obtain the address and length of a string constant, and `print_str` prints by “address, length”; the 0 in `r0` selects string constant index 0, `"hello, mote"`.

## 10. Round-Trip Verification

```text
motedis hello.mbc -o hello.canon.mta
moteasm hello.canon.mta -o hello2.mbc
```

After a successful assembly, `moteasm --check-roundtrip` automatically performs the equivalent “disassemble to canonical text, re-assemble, compare bytes” flow; any difference reports the first differing byte and fails with exit code 4. See [motedis](motedis.md) for the canonical text format and [02 · Bytecode Format](../02-bytecode-format.md) for the container and section layout.
