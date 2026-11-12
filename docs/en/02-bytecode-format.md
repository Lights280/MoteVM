# 02 · MOTEBC 1.0 Bytecode Format

[简体中文](../02-bytecode-format.md) | [English](02-bytecode-format.md)

> **Version note**: The MOTEBC format version is **1.0**, and this document corresponds to the current implementation of MoteVM **0.1.0**. The repository does not yet publish a tagged release, so this document is a best-effort reference; if it disagrees with the implementation, `src/include/parse.hpp`, `src/include/parse.cpp`, and `src/include/verifier.cpp` are authoritative. MOTEBC files use the `.mbc` suffix.

## 1. Overview

MOTEBC 1.0 is a container with a fixed header, a section table, and a sequence of 8-byte-aligned sections. All multi-byte integers are **little-endian**.

```text
┌──────────────────────────────────────────────────────────────┐
│ Header (32 bytes, offset 0)                                  │
│   magic, major/minor, flags, entry, section_count,           │
│   file_size, CRC-32C, reserved                               │
├──────────────────────────────────────────────────────────────┤
│ Section Table (section_count × 24 bytes, right after header) │
│   each: u32 id, u32 flags, u64 offset, u64 size              │
├──────────────────────────────────────────────────────────────┤
│ Section data (each 8-byte aligned, non-overlapping)          │
│   1 META | 2 CONST | 3 FUNCS | 4 CODE | 5 GLOBALS            │
│   6 IMPORTS | 7 DEBUG (optional)                             │
└──────────────────────────────────────────────────────────────┘
```

### 1.1 Basic conventions

| Item | Rule |
|---|---|
| Byte order | All multi-byte integers in the file are little-endian |
| Section alignment | Every section `offset` and `size` must be a multiple of 8 |
| Table order | Section-table entries are in **strictly ascending** `id` order (so ids are unique) |
| Physical order | The parser only requires non-overlapping ranges; the canonical writer lays sections out adjacently in ascending id order |
| File size | At most `MAX_MODULE_BYTES` = 64 MiB |
| Static table counts | `count` in `CONST`, `FUNCS`, `GLOBALS`, and `IMPORTS` must not exceed 65536 |
| String limit | A single String constant payload must not exceed 16 MiB |
| Exact coverage | Each section is exactly covered by entries or padding; padding must be zero |
| Checksum | Header `checksum` covers the whole file with the field itself treated as zero |
| Failure handling | Loading verifies immediately; any failed check rejects the whole module and no bytecode executes |

### 1.2 Section IDs

| id | Section | Required | Content |
|---:|---|---|---|
| 1 | META | yes | Metadata TLV (module name, source file, build id, comment) |
| 2 | CONST | yes | Constant pool |
| 3 | FUNCS | yes | Function table |
| 4 | CODE | yes | Sequence of 32-bit instruction words |
| 5 | GLOBALS | yes | Global slot definitions |
| 6 | IMPORTS | yes | Host import definitions |
| 7 | DEBUG | conditionally | Debug information; its presence is tied both ways to header `flags.bit0` |

Sections 1–6 must each appear exactly once. Section 7 may appear only when `flags.bit0 = 1`, and `flags.bit0 = 1` requires section 7. Unknown sections with id greater than 7 are allowed only with `flags = 0`; the parser skips their content.

A valid module has at least one function: header `entry` must name an existing FUNCS entry, and that function must have `arity = 0`.

## 2. Header (32 bytes)

| Offset | Size | Field (code name) | Type | Meaning | Validation |
|---:|---:|---|---|---|---|
| 0x00 | 4 | `magic` | `u8[4]` | `4D 4F 54 45` (`"MOTE"`) | Must match exactly |
| 0x04 | 2 | `majorVer` | `u16` | Major version | Must equal 1 |
| 0x06 | 2 | `minorVer` | `u16` | Minor version | Must equal 0 (the current reader's supported minor) |
| 0x08 | 4 | `flags` | `u32` | bit0 = `DEBUG_PRESENT` (`0x1`); other bits reserved | Only bit0 may be set |
| 0x0C | 4 | `entry` | `u32` | FUNCS index of the entry function | Semantic phase: `< func_count` and `arity = 0` |
| 0x10 | 2 | `seccnt` | `u16` | Number of section-table entries | ≥ 6; the table must fit entirely in the file |
| 0x12 | 2 | `reserved0` | `u16` | Reserved | Must be 0 |
| 0x14 | 4 | `fileSize` | `u32` | Total file size in bytes | Must equal the actual input length |
| 0x18 | 4 | `checksum` | `u32` | CRC-32C of the whole file | The field is treated as zero when computing it |
| 0x1C | 4 | `reserved1` | `u32` | Reserved | Must be 0 |

Header check order (implementation order):

1. Input length ≥ 32 and ≤ `MAX_MODULE_BYTES`.
2. magic, major/minor, reserved flag bits, `reserved0`, `reserved1`, and `fileSize == input length`.
3. `seccnt ≥ 6` and `32 + 24 × seccnt ≤ fileSize`.
4. CRC-32C verification.
5. Parse and validate the section table.

The header `entry` is not checked during the header phase; it is checked after all sections are parsed by cross-table rules V76/V77.

## 3. Section table (24 bytes per entry)

The section table follows the 32-byte header and contains `seccnt` entries.

| Entry offset | Size | Field | Type | Meaning |
|---:|---:|---|---|---|
| 0x00 | 4 | `id` | `u32` | Section id |
| 0x04 | 4 | `flags` | `u32` | Section flags; the current format requires 0 |
| 0x08 | 8 | `offset` | `u64` | Start offset of the section data in the file |
| 0x10 | 8 | `size` | `u64` | Section data size in bytes |

Section-table invariants:

| ID | Rule |
|---|---|
| S1 | The table range `[32, 32 + 24 × seccnt)` lies entirely in the file (u64 arithmetic) |
| S2 | `id != 0` |
| S3 | Entries are in strictly ascending `id` order, so ids do not repeat |
| S4 | Known sections (id 1–7) must have `flags == 0`; unknown sections must also have `flags == 0`, otherwise the module is rejected |
| S5 | `offset % 8 == 0` and `size % 8 == 0` |
| S6 | `offset ≥ 32 + 24 × seccnt` (section data must not overlap the header or table) |
| S7 | `offset + size ≤ fileSize`, using u64 throughout and never wrapping |
| S8 | No two section ranges may overlap; gaps between sections are allowed (the canonical writer produces none) |
| S9 | Sections 1–6 each exist exactly once |
| S10 | Section 7 exists if and only if `flags.bit0 = 1` |

Unknown sections (id > 7) with `flags = 0` are accepted and skipped; the parser does not interpret their content. Unknown sections with non-zero `flags` are rejected.

## 4. Section formats

All offsets inside a section are relative to the section start. Trailing padding in every section must be zero.

### 4.1 META (id = 1)

META is a sequence of 8-byte-aligned TLV records; the section may be 0 bytes.

| Record offset | Size | Field | Type | Meaning |
|---:|---:|---|---|---|
| 0x00 | 2 | `key` | `u16` | Metadata key |
| 0x02 | 2 | `length` | `u16` | Payload size in bytes |
| 0x04 | `length` | `raw` | `u8[]` | Payload |

- Record size = `align8(4 + length)`, where `align8(n) = (n + 7) & ~7`.
- Records start on 8-byte boundaries within the section; the payload follows the 4-byte header and is not necessarily 8-byte aligned itself.
- The whole section must be exactly covered by records, the section size must be a multiple of 8, and record padding must be zero.
- Known keys are listed below; the same known key may appear at most once in a section, and a duplicate is rejected.
- Unknown keys are accepted and retained as raw records; they do not affect semantics.
- The parser does not validate payload UTF-8; text is handled as bytes.

| key | Name | Image field populated |
|---:|---|---|
| 1 | Module name | `moduleName` |
| 2 | Source file | `sourceFile` |
| 3 | Build id | `buildId` |
| 4 | Comment | `comment` |

### 4.2 CONST (id = 2)

| Section offset | Size | Field | Type | Meaning |
|---:|---:|---|---|---|
| 0x00 | 4 | `count` | `u32` | Number of constant entries, ≤ 65536 |
| 0x04 | 4 | `reserved0` | `u32` | Must be 0 |
| 0x08 | — | `entries` | variable | Constant entries |

CONST entry:

| Entry offset | Size | Field | Type | Meaning |
|---:|---:|---|---|---|
| 0x00 | 1 | `kind` | `u8` | Constant kind, 0–4 |
| 0x01 | 1 | `reserved` | `u8` | Must be 0 |
| 0x02 | 2 | `flags` | `u16` | Must be 0 |
| 0x04 | 4 | `length` | `u32` | Payload size in bytes |
| 0x08 | `length` | `payload` | `u8[]` | Content |

- Entry size = `8 + align8(length)`; the whole section must be exactly covered by entries, with no extra trailing record.
- The payload starts at entry offset +8; because every entry size is a multiple of 8, payloads are 8-byte aligned within the section.
- `count ≤ 65536`; `reserved0`, entry `reserved`, and entry `flags` must be 0.
- For non-String kinds, `length` must be exactly 8.
- For String, `length` is 0 to 16 MiB; the parser does not validate its UTF-8.

| kind | Name | length | Payload rule |
|---:|---|---|---|
| 0 | null | 8 | All 8 bytes must be zero |
| 1 | boolean | 8 | 8-byte little-endian integer; only 0 or 1 |
| 2 | integer | 8 | 8 raw bytes (by convention int64 two's complement little-endian; the parser does not interpret it) |
| 3 | real | 8 | 8 raw bytes (by convention IEEE-754 binary64 little-endian; the parser does not interpret it) |
| 4 | string | 0–16 MiB | Raw bytes; referenced by index from FUNCS/GLOBALS/IMPORTS |

### 4.3 FUNCS (id = 3)

| Section offset | Size | Field | Type | Meaning |
|---:|---:|---|---|---|
| 0x00 | 4 | `count` | `u32` | Number of functions, ≤ 65536 |
| 0x04 | 32 × `count` | `entries` | fixed | Function entries |
| 0x04 + 32 × `count` | 4 | `pad` | `u32` | Must be 0 |

The section size must be exactly `8 + 32 × count`. Entries start at offset 4; because `4 + 32n ≡ 4 (mod 8)`, the section always ends with 4 zero padding bytes.

Function entry (32 bytes):

| Entry offset | Size | Field | Type | Meaning and validation |
|---:|---:|---|---|---|
| 0x00 | 4 | `name` | `u32` | CONST index of the name; `0xFFFFFFFF` = anonymous |
| 0x04 | 4 | `entry` | `u32` | Instruction index of the function's first instruction in CODE |
| 0x08 | 4 | `instr` | `u32` | Instruction count, must be ≥ 1 |
| 0x0C | 2 | `regcnt` | `u16` | Register count, 1–256 |
| 0x0E | 1 | `arity` | `u8` | Parameter count, ≤ 16 and ≤ `regcnt` |
| 0x0F | 1 | `flags` | `u8` | bit0 = exported; bits 1–7 must be 0 |
| 0x10 | 2 | `local` | `u16` | Number of distinct named register slots, ≤ `regcnt` |
| 0x12 | 2 | `reserved0` | `u16` | Must be 0 |
| 0x14 | 4 | `dbgOffset` | `u32` | Offset in DEBUG; `0xFFFFFFFF` = no debug information |
| 0x18 | 4 | `reserved1` | `u32` | Must be 0 |
| 0x1C | 4 | `reserved2` | `u32` | Must be 0 |

Validation rules:

- `name = 0xFFFFFFFF`, or it references a CONST entry whose kind is String. Non-anonymous names must be unique **by String content** within FUNCS; anonymous entries do not participate in uniqueness.
- `entry + instr ≤ CODE instruction count` (u64).
- Instruction ranges `[entry, entry + instr)` of any two functions must not overlap.
- Only bit0 of `flags` may be set; `reserved0/1/2` must be 0.
- `dbgOffset` is either `0xFFFFFFFF` or an 8-byte-aligned offset of a valid function debug block inside DEBUG.
- The last instruction of a function must be a terminator (`JMP`, `RET`, `HALT`, or `TRAP`), checked by verifier rule V62.

### 4.4 CODE (id = 4)

| Section offset | Size | Field | Type | Meaning |
|---:|---:|---|---|---|
| 0x00 | 4 × N | `instructions` | `u32[]` | N 32-bit little-endian instruction words |

- The section size must be a multiple of 4 and of 8, so the instruction count **N must be even**.
- `NOP = 0x00000000`. When the total number of function instructions is odd, the canonical writer appends one `NOP` at the end of CODE; that word belongs to no function.
- Function ranges must fall within `[0, N)` and must not overlap; gap words between functions are allowed.
- Every CODE word must pass V51 (known opcode and zero unused fields); only words inside function ranges receive full instruction-level checks. Gap words are normally `NOP`.
- Instruction encoding and operand layouts are described in [01-isa.md](01-isa.md).

### 4.5 GLOBALS (id = 5)

| Section offset | Size | Field | Type | Meaning |
|---:|---:|---|---|---|
| 0x00 | 4 | `count` | `u32` | Number of global slots, ≤ 65536 |
| 0x04 | 12 × `count` | `entries` | fixed | Global slots |
| 0x04 + 12 × `count` | 0–4 | `pad` | `u8[]` | Zero padding to a multiple of 8 bytes |

The section size must be `align8(4 + 12 × count)`, and padding must be zero.

Global entry (12 bytes):

| Entry offset | Size | Field | Type | Meaning and validation |
|---:|---:|---|---|---|
| 0x00 | 4 | `name` | `u32` | CONST index of the name; `0xFFFFFFFF` = anonymous |
| 0x04 | 4 | `init` | `u32` | CONST index of the initial value; `0xFFFFFFFF` = null |
| 0x08 | 4 | `flags` | `u32` | bit0 = const; all other bits must be 0 |

- A non-anonymous name must reference a String constant and must be unique by content within GLOBALS.
- `init` is `0xFFFFFFFF` or less than the CONST entry count (any kind).
- The current instruction set has no instruction that reads or writes a global slot. GLOBALS is still fully parsed and validated, but the current interpreter does not perform reads or writes through these slots.

### 4.6 IMPORTS (id = 6)

| Section offset | Size | Field | Type | Meaning |
|---:|---:|---|---|---|
| 0x00 | 4 | `count` | `u32` | Number of imports, ≤ 65536 |
| 0x04 | 8 × `count` | `entries` | fixed | Import entries |
| 0x04 + 8 × `count` | 4 | `pad` | `u32` | Must be 0 |

The section size must be exactly `8 + 8 × count`.

Import entry (8 bytes):

| Entry offset | Size | Field | Type | Meaning and validation |
|---:|---:|---|---|---|
| 0x00 | 4 | `name` | `u32` | CONST String index of the name; anonymous is forbidden |
| 0x04 | 1 | `arity` | `u8` | Parameter count, ≤ 16 |
| 0x05 | 1 | `flags` | `u8` | Must be 0 |
| 0x06 | 2 | `reserved` | `u16` | Must be 0 |

- `name` must reference a String constant and may not be `0xFFFFFFFF`; names must be unique by content within IMPORTS.
- At load time the host binds each import by exact `(name bytes, arity)` match. If binding fails, loading fails and no bytecode executes. The file format itself stores no function pointers.
- The 16-bit index of `HOSTCALL` selects an IMPORTS entry; the verifier checks that the imported `arity` is ≤ the caller's `regcnt`.

### 4.7 DEBUG (id = 7)

The DEBUG section is optional, and its presence is tied both ways to header `flags.bit0`. A non-empty section begins with an 8-byte header.

| Section offset | Size | Field | Type | Meaning |
|---:|---:|---|---|---|
| 0x00 | 1 | `version` | `u8` | Debug format version, must be 1 |
| 0x01 | 1 | `reserved0` | `u8` | Must be 0 |
| 0x02 | 2 | `reserved1` | `u16` | Must be 0 |
| 0x04 | 4 | `flags` | `u32` | Must be 0 |
| 0x08 | — | `blocks` | variable | Function debug blocks |

Function debug blocks are addressed by FUNCS `dbgOffset`; `dbgOffset` is an 8-byte-aligned offset relative to the start of DEBUG. Functions without debug information use `0xFFFFFFFF`.

| Block offset | Size | Field | Type | Meaning and validation |
|---:|---:|---|---|---|
| 0x00 | 4 | `function` | `u32` | Must equal the FUNCS index that references the block |
| 0x04 | 4 | `lines` | `u32` | Number of line entries, ≤ the function's `instr` |
| 0x08 | 4 | `locals` | `u32` | Number of local-variable records, ≤ 65536 |
| 0x0C | 4 | `reserved0` | `u32` | Must be 0 |
| 0x10 | 12 × `lines` | `lineEntries` | fixed | Line table |
| 0x10 + 12 × `lines` | 16 × `locals` | `localEntries` | fixed | Local-variable table |
| block end | 0–7 | `pad` | `u8[]` | Zero padding to a multiple of 8 bytes |

A block occupies `align8(16 + 12 × lines + 16 × locals)` bytes and must fit entirely in the section. Referenced blocks must not overlap; unreferenced regions are not parsed.

LineEntry (12 bytes):

| Entry offset | Size | Field | Type | Meaning and validation |
|---:|---:|---|---|---|
| 0x00 | 4 | `pc` | `u32` | Instruction index within the function; must be strictly increasing; the first entry must be 0 when entries exist |
| 0x04 | 4 | `line` | `u32` | Source line, 1–0x7FFFFFFF |
| 0x08 | 4 | `column` | `u32` | Source column; 0 means unknown |

Every line entry's `pc` must be less than the function's `instr`. Column values are unrestricted.

LocalEntry (16 bytes):

| Entry offset | Size | Field | Type | Meaning and validation |
|---:|---:|---|---|---|
| 0x00 | 4 | `name` | `u32` | CONST String index of the variable name |
| 0x04 | 2 | `slot` | `u16` | Register slot, must be < `regcnt` |
| 0x06 | 1 | `flags` | `u8` | bit0 = parameter; all other bits must be 0 |
| 0x07 | 1 | `reserved` | `u8` | Must be 0 |
| 0x08 | 4 | `startPc` | `u32` | Scope start instruction index (inclusive) |
| 0x0C | 4 | `endPc` | `u32` | Scope end instruction index (exclusive) |

- `name` must be a String constant (anonymous is not allowed).
- `startPc ≤ endPc ≤` the function's `instr`.
- When `flags.bit0 = 1`, `slot` must also be less than the function's `arity`.
- The number of distinct `slot` values in a function block must equal the FUNCS entry's `local` field.

### 4.8 Section size formula summary

| Section | Header | Entry | Section size | Empty size |
|---|---|---|---:|---:|
| META | none | variable TLV | Σ `align8(4 + length)` | 0 |
| CONST | 8 (count + reserved0) | 8 + `align8(length)` | 8 + Σ(`8 + align8(length)`) | 8 |
| FUNCS | 4 (count) | 32 | `8 + 32 × count` | 8 |
| CODE | none | 4 | `4 × N`, N even | 0 |
| GLOBALS | 4 (count) | 12 | `align8(4 + 12 × count)` | 8 |
| IMPORTS | 4 (count) | 8 | `8 + 8 × count` | 8 |
| DEBUG | 8 | block header 16, line entry 12, local entry 16 | `8 + Σ align8(16 + 12 × L + 16 × V)` | 8 |

## 5. CRC-32C

| Parameter | Value |
|---|---|
| Polynomial (normal form) | `0x1EDC6F41` (Castagnoli) |
| Reflected form | `0x82F63B78` |
| Initial value | `0xFFFFFFFF` |
| Final XOR | `0xFFFFFFFF` |
| Input/output reflection | yes |
| Coverage | The whole file `[0, fileSize)` |
| Special handling | The `checksum` field (offset 0x18–0x1B) is treated as `00 00 00 00` |

- Verification steps: confirm `fileSize` equals the actual input length; treat the 4 bytes at offsets 0x18–0x1B as zero; compute CRC-32C over the whole file; the result must equal the stored little-endian u32.
- Implementations may use SSE4.2, the ARM CRC extension, or a portable table-driven path. The only observable requirement is that the same byte sequence produces the same CRC-32C value on every path.
- CRC-32C detects corruption only; it is not cryptographic authentication.
- Test vectors:

| Input | CRC-32C |
|---|---|
| Empty input | `0x00000000` |
| ASCII `"123456789"` | `0xE3069283` |
| The 272-byte minimal module in §10 (checksum field zeroed) | `0x1B5A747A` |

## 6. Canonical writer rules

A canonical writer (currently implemented by `moteasm`) should produce a unique, reproducible byte sequence:

| ID | Rule |
|---|---|
| W1 | `majorVer = 1`, `minorVer = 0`; set `flags` bit0 only when needed; `reserved0/1 = 0` |
| W2 | Write the section table in ascending id order and lay section data out adjacently in ascending id order; the first section offset is `32 + 24 × seccnt`, with no gaps afterwards |
| W3 | Every section size follows §4.8; all padding bytes are zero |
| W4 | The CODE instruction count must be even; append one `NOP` if needed, and that word belongs to no function |
| W5 | META records are sorted by key; CONST follows source order; FUNCS follows function definition order; GLOBALS follows declaration order; IMPORTS follows first-appearance order |
| W6 | Anonymous names in FUNCS/GLOBALS use `0xFFFFFFFF`; non-anonymous names are unique by String content; IMPORTS names must be Strings and may not be anonymous |
| W7 | With no debug information, omit DEBUG and clear `flags.bit0`; with debug information, set bit0, write DEBUG, and zero unused debug areas |
| W8 | `entry` is the FUNCS index of the entry function, and that function has `arity = 0` |
| W9 | All reserved fields, reserved bits, and entry flags are written as 0 per §4 |
| W10 | Compute and backfill `checksum` last; never write timestamps, random values, or host absolute paths into the file |
| W11 | The same logical module produced repeatedly by the same writer must yield identical bytes |

## 7. Load, parse, and verification pipeline

The entry point is `Parse`:

```text
Parse::load(path or span)
        ↓
Parse::parse()
        ↓
check()          header, section table, CRC
        ↓
readSections()   META → CONST → CODE → FUNCS → GLOBALS → IMPORTS → DEBUG → cross-table
        ↓
Verifier::verify()  CODE decoding + per-function instruction rules
        ↓
success: parsed = true, Image readable; failure: Image cleared
```

| Stage | Main checks | Failure code |
|---:|---|---|
| `load(path)` | File opens; stat size ≤ 64 MiB; full read | `FAIL_OPEN` / `TOO_LARGE` / `TRUNC` |
| `load(span)` | Copies input bytes; size limits deferred to `parse()` | — |
| `parse()` entry | Non-empty; length ≥ 32; length ≤ 64 MiB | `EMPTY` / `TRUNC` / `TOO_LARGE` |
| `check()` | magic, version, flags, reserved fields, `fileSize`, table bounds, CRC | `BAD_FILE` |
| `readSections()` | Section structures, entry bounds, alignment, padding, name/index references, DEBUG | `BAD_CONTENT` |
| `Verifier::verify()` | Whole-CODE decoding plus per-function register/index/jump/terminator rules | `BAD_CODE` |

Failure handling is **fail-closed**: any failed stage calls `clearImage()`, `isValid()` is `false`, no partial image is retained, and no bytecode executes.

PARSE status codes (the corresponding text is returned by `PARSE::errorText()`):

| Code | Name | Meaning |
|---:|---|---|
| 0 | `OK` | Success |
| 1 | `FAIL_OPEN` | File cannot be opened or read |
| 2 | `EMPTY` | Input is empty |
| 3 | `TRUNC` | Input is too short or the read was incomplete |
| 4 | `TOO_LARGE` | Exceeds 64 MiB |
| 5 | `BAD_FILE` | Invalid header, section table, or checksum |
| 6 | `BAD_CONTENT` | Invalid section content |
| 7 | `BAD_CODE` | Invalid instruction stream (verifier) |

## 8. Verifier rules (V51–V62, V76/V77)

The verifier first decodes every word in CODE, then performs full checks on each function's instruction range. Gap words outside function ranges receive only V51.

| ID | Rule (current implementation) |
|---|---|
| V51 | Every CODE word must decode: the opcode is defined, and operand fields unused by that instruction format must be 0 |
| V52 | Words outside function ranges receive only V51, not register/jump checks |
| V53 | Every register number actually used by an instruction must be less than the current function's `regcnt` |
| V54 | Constant indexes must be less than the CONST entry count; the current instruction set has no instruction that references constants, so this rule does not currently apply |
| V55 | Global slot indexes must be less than the GLOBALS entry count; the current instruction set has no global-access instruction, so this rule does not currently apply |
| V56 | The `CALL` FUNCS index and the `HOSTCALL` IMPORTS index must each be within the corresponding table |
| V57 | The 24-bit immediate of `TRAP` must be in `[1, 16]` |
| V58 | The `SETG` destination must not be a const global; the current instruction set has no `SETG`, so this rule does not currently apply |
| V59 | The target function's `arity` must be ≤ the caller's `regcnt` for `CALL` |
| V60 | The import's `arity` must be ≤ the caller's `regcnt` for `HOSTCALL` |
| V61 | The byte offset of `JMP`/`JZ`/`JNZ` must be a multiple of 4, and the target must fall within the same function's `[entry, entry + instr)` |
| V62 | The last instruction of every function must be a terminator: `JMP`, `RET`, `HALT`, or `TRAP` |
| V76 | Header `entry` must be less than the FUNCS entry count |
| V77 | The function named by header `entry` must have `arity = 0` |

Additional notes:

- V51's "unused fields are zero" depends on each instruction format. For example, the b/c fields of `RET` and all operand fields of `HALT` must be 0. The complete opcode set and field layouts are in [01-isa.md](01-isa.md).
- Function ranges must not overlap; this structural rule is already checked while parsing FUNCS (`BAD_CONTENT`).
- The verifier also computes register-copy extents over the CALL graph for the runtime to attach verified modules; that result is not part of the file format.

## 9. Version compatibility and rejection rules

| Input condition | Reader behavior |
|---|---|
| `majorVer != 1` | Reject (`BAD_FILE`) |
| `minorVer > 0` | Reject (`BAD_FILE`); the current reader supports only 1.0 |
| Any of `flags` bits 1–31 set | Reject (`BAD_FILE`) |
| `reserved0` or `reserved1` non-zero | Reject (`BAD_FILE`) |
| `fileSize` differs from the actual length | Reject (`BAD_FILE`) |
| Section 7 presence disagrees with `flags.bit0` | Reject (`BAD_FILE`) |
| Unknown section id > 7 with `flags = 0` | Skip; it does not affect semantics |
| Unknown section id > 7 with `flags != 0` | Reject (`BAD_FILE`) |
| Unknown META key | Accept and retain the raw record; it does not affect semantics |
| Unknown CONST kind (> 4) | Reject (`BAD_CONTENT`) |
| Unknown opcode | Reject (`BAD_CODE`) |
| DEBUG `version != 1` | Reject (`BAD_CONTENT`) |
| Any structural/semantic check fails | Reject the whole module; execute no bytecode |

The reader does not attempt best-effort parsing of a higher minor: an unconfirmed version is rejected outright.

## 10. Minimal module hexdump example

The following module was produced by `moteasm --no-debug` from this source. It is 272 bytes with 6 sections, 1 constant, 1 function, 0 globals, and 0 imports:

```text
.module mini
.func main 1
  ret r0
.end
```

### 10.1 Full hexdump

```text
00000000  4d 4f 54 45 01 00 00 00 00 00 00 00 00 00 00 00  |MOTE............|
00000010  06 00 00 00 10 01 00 00 7a 74 5a 1b 00 00 00 00  |........ztZ.....|
00000020  01 00 00 00 00 00 00 00 b0 00 00 00 00 00 00 00  |................|
00000030  08 00 00 00 00 00 00 00 02 00 00 00 00 00 00 00  |................|
00000040  b8 00 00 00 00 00 00 00 18 00 00 00 00 00 00 00  |................|
00000050  03 00 00 00 00 00 00 00 d0 00 00 00 00 00 00 00  |................|
00000060  28 00 00 00 00 00 00 00 04 00 00 00 00 00 00 00  |(...............|
00000070  f8 00 00 00 00 00 00 00 08 00 00 00 00 00 00 00  |................|
00000080  05 00 00 00 00 00 00 00 00 01 00 00 00 00 00 00  |................|
00000090  08 00 00 00 00 00 00 00 06 00 00 00 00 00 00 00  |................|
000000a0  08 01 00 00 00 00 00 00 08 00 00 00 00 00 00 00  |................|
000000b0  01 00 04 00 6d 69 6e 69 01 00 00 00 00 00 00 00  |....mini........|
000000c0  04 00 00 00 04 00 00 00 6d 61 69 6e 00 00 00 00  |........main....|
000000d0  01 00 00 00 00 00 00 00 00 00 00 00 01 00 00 00  |................|
000000e0  01 00 00 00 00 00 00 00 ff ff ff ff 00 00 00 00  |................|
000000f0  00 00 00 00 00 00 00 00 54 00 00 00 00 00 00 00  |........T.......|
00000100  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  |................|
```

### 10.2 Header (0x00–0x1F)

| Offset | Bytes | Field | Interpretation |
|---:|---|---|---|
| 0x00–0x03 | `4D 4F 54 45` | `magic` | `"MOTE"` |
| 0x04–0x05 | `01 00` | `majorVer` | 1 |
| 0x06–0x07 | `00 00` | `minorVer` | 0 |
| 0x08–0x0B | `00 00 00 00` | `flags` | 0 (no DEBUG) |
| 0x0C–0x0F | `00 00 00 00` | `entry` | 0 (FUNCS[0]) |
| 0x10–0x11 | `06 00` | `seccnt` | 6 |
| 0x12–0x13 | `00 00` | `reserved0` | 0 |
| 0x14–0x17 | `10 01 00 00` | `fileSize` | 0x110 = 272 |
| 0x18–0x1B | `7A 74 5A 1B` | `checksum` | little-endian u32 = `0x1B5A747A` |
| 0x1C–0x1F | `00 00 00 00` | `reserved1` | 0 |

### 10.3 Section table (0x20–0xAF, 6 × 24 bytes)

| Entry | Byte range | id | flags | offset | size |
|---:|---|---:|---:|---:|---:|
| [0] | 0x20–0x37 | 1 | 0 | 0xB0 | 0x08 |
| [1] | 0x38–0x4F | 2 | 0 | 0xB8 | 0x18 |
| [2] | 0x50–0x67 | 3 | 0 | 0xD0 | 0x28 |
| [3] | 0x68–0x7F | 4 | 0 | 0xF8 | 0x08 |
| [4] | 0x80–0x97 | 5 | 0 | 0x100 | 0x08 |
| [5] | 0x98–0xAF | 6 | 0 | 0x108 | 0x08 |

The table ends at 0xB0, which equals `32 + 24 × 6`. Section data is laid out adjacently; the last section ends at 0x110, matching `fileSize`.

### 10.4 Section payload walkthrough

| Offset | Section | Bytes | Interpretation |
|---:|---|---|---|
| 0xB0–0xB7 | META | `01 00 04 00 6D 69 6E 69` | key=1 (module name), length=4, `"mini"` |
| 0xB8–0xBF | CONST header | `01 00 00 00 00 00 00 00` | count=1, reserved0=0 |
| 0xC0–0xCF | CONST[0] | `04 00 00 00 04 00 00 00 6D 61 69 6E 00 00 00 00` | kind=4 (String), length=4, `"main"`, 4 zero padding bytes |
| 0xD0–0xD3 | FUNCS header | `01 00 00 00` | count=1 |
| 0xD4–0xDF | FUNCS[0] first half | `00 00 00 00 00 00 00 00 01 00 00 00` | name=0, entry=0, instr=1 |
| 0xE0–0xEF | FUNCS[0] second half | `01 00 00 00 00 00 00 00 FF FF FF FF 00 00 00 00` | regcnt=1, arity=0, flags=0, local=0, dbgOffset=NO_INDEX, reserved1=0 |
| 0xF0–0xF7 | FUNCS tail | `00 00 00 00 00 00 00 00` | reserved2=0, 4 padding bytes |
| 0xF8–0xFF | CODE | `54 00 00 00 00 00 00 00` | `RET r0` (0x54, a=0); one `NOP` makes N=2 |
| 0x100–0x107 | GLOBALS | `00 00 00 00 00 00 00 00` | count=0 + 4 padding bytes |
| 0x108–0x10F | IMPORTS | `00 00 00 00 00 00 00 00` | count=0 + 4 padding bytes |

Checksum: after zeroing 0x18–0x1B, CRC-32C over all 272 bytes is `0x1B5A747A`; writing it little-endian gives `7A 74 5A 1B`.

The module passes header, section-table, CRC, section-structure, and verifier checks: entry FUNCS[0] exists with `arity = 0`; its instruction range `[0, 1)` is within CODE; `RET` is a terminator; every CODE word decodes.

## 11. Related documents

- [01-isa.md](01-isa.md): instruction encoding, operand formats, and verifier field rules.
- [03-runtime.md](03-runtime.md): execution model after loading.
- [05-toolchain.md](05-toolchain.md): how the assembler/disassembler writes and reads MOTEBC.
- [07-mtr-format.md](07-mtr-format.md): packaging verified `.mbc` modules into `.mtr` archives.
- [08-limits.md](08-limits.md): consolidated resource limits.
- [self/moteasm.md](self/moteasm.md): `moteasm` text format and command line.
- [self/motedis.md](self/motedis.md): `motedis` disassembly output.
- [examples/hello.mta](../examples/hello.mta): an assemblable example program.
