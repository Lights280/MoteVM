# 08 · Limits

[简体中文](../08-limits.md) | [English](08-limits.md)

**Note**: The values below come from current source constants. They are not permanent guarantees of the format specification and may change with the implementation. For format background see [02 Bytecode format](02-bytecode-format.md) and [07 MTR format](07-mtr-format.md); for host-function error codes see [05 Toolchain](05-toolchain.md) §8.7.

## 1. ISA

| Item | Value | Source constant | Behavior when exceeded |
| --- | --- | --- | --- |
| Register count | 256 | `ISA::REGISTER_COUNT` | A function `regcnt` greater than 256 fails module loading (`BAD_CONTENT`); tools exit with code 2 |
| Instruction size | 4 B | `ISA::INSTRUCTION_SIZE` | CODE length must be a multiple of 4 and the instruction count must be even (8 B aligned); otherwise `BAD_CONTENT`, exit code 2 |
| Data word width | 32-bit | `dword` (`unsigned int`) | Registers and integer operations hold 32-bit values; overflowing results are truncated/wrapped |

## 2. Core

| Item | Value | Source constant | Behavior when exceeded |
| --- | --- | --- | --- |
| Maximum call depth | 1024 (including the root frame) | `CORE::MAX_CALL_DEPTH` | A further `CALL` returns `CORE::STACK_OVERFLOW`; `motevm` exits with code 3 |

## 3. Memory

| Item | Value | Source constant | Behavior when exceeded |
| --- | --- | --- | --- |
| Block size | 65536 B (64 KiB) | `MEMORY::BLOCK_SIZE` | — |
| Block count | 65536 | `MEMORY::BLOCK_COUNT` | Accesses with a block index outside 0..65535 fail |
| Address space | 4 GiB | `MEMORY::ADDRESS_SPACE` = `BLOCK_COUNT * BLOCK_SIZE` | An address past `ADDRESS_SPACE - 4` returns `MEMORY_OVERFLOW`; the core reports `CORE::MEMORY_FAULT`; `motevm` exits with code 3 |
| Unallocated block | — | `MEMORY::MEMORY_UNAVAILABLE` | Accessing an unallocated block returns this error; the core reports `CORE::MEMORY_FAULT`; exit code 3 |
| Double allocation | — | — | Calling `Memory::allocate` again for an allocated block is idempotent and returns `MEMORY::OK` |

## 4. Module (MOTEBC 1.0)

### 4.1 Capacity limits

| Item | Value | Source constant | Behavior when exceeded |
| --- | --- | --- | --- |
| Module file size | 64 MiB | `PARSE::MAX_MODULE_BYTES` | Loading returns `TOO_LARGE`; tools exit with code 2 |
| Constant/function/global/import/debug-local count | 65536 | `PARSE::MAX_STATIC_COUNT` | Parsing returns `BAD_CONTENT`; exit code 2 |
| String constant length | 16 MiB | `PARSE::MAX_STRING_BYTES` | Parsing returns `BAD_CONTENT`; exit code 2 |
| Function register count | 1..256 | `PARSE::Function::regcnt`, `ISA::REGISTER_COUNT` | Out of range returns `BAD_CONTENT`; exit code 2 |
| Function arity | ≤16 and ≤`regcnt` | `PARSE::Function::arity` | Out of range returns `BAD_CONTENT`; exit code 2 |
| Import arity | ≤16 | `PARSE::Import::arity` | Out of range returns `BAD_CONTENT`; exit code 2 |
| Section count | ≥6, field maximum 65535 | `PARSE::Header::seccnt` (`word`) | Less than 6, or a section table past end of file, returns `BAD_FILE`; exit code 2 |
| CODE instruction count | Even | 8 B alignment requirement in `parse.cpp` | Otherwise returns `BAD_CONTENT`; exit code 2 |

Among module load/verification errors, `TRUNC`, `TOO_LARGE`, `BAD_FILE`, `BAD_CONTENT` and `BAD_CODE` all make `moteasm`, `motedis`, `motevm` and `motear` exit with code 2 (for `motevm`, during the load stage).

### 4.2 Header and structure sizes

| Structure | Size | Source constant |
| --- | --- | --- |
| File header | 32 B | `PARSE::HEADER_SIZE` / `sizeof(PARSE::Header)` |
| Section table entry | 24 B | `PARSE::SECBLOCK_SIZE` / `sizeof(PARSE::SecBlock)` |
| CONST header | 8 B | `sizeof(PARSE::ConstHeader)` |
| CONST entry | 8 B + `align8(length)` | `sizeof(PARSE::ConstEntry)` |
| FUNCS header | 4 B | `sizeof(PARSE::FuncHeader)` |
| FUNCS entry | 32 B | `sizeof(PARSE::Function)` |
| GLOBALS header | 4 B | `sizeof(PARSE::GlobalHeader)` |
| GLOBALS entry | 12 B | `sizeof(PARSE::Global)` |
| IMPORTS header | 4 B | `sizeof(PARSE::ImportHeader)` |
| IMPORTS entry | 8 B | `sizeof(PARSE::Import)` |
| DEBUG header | 8 B | `sizeof(PARSE::DebugHeader)` |
| DEBUG block | 16 B | `sizeof(PARSE::DebugBlock)` |
| DEBUG line entry | 12 B | `sizeof(PARSE::LineEntry)` |
| DEBUG local entry | 16 B | `sizeof(PARSE::LocalEntry)` |

A file shorter than the 32 B header returns `TRUNC`; header fields, CRC, section table or version not conforming to MOTEBC 1.0 return `BAD_FILE`.

## 5. Archive (MTR 1.0)

### 5.1 Capacity limits

| Item | Value | Source constant | Behavior when exceeded |
| --- | --- | --- | --- |
| Archive file size | 256 MiB | `ARCHIVE::MAX_ARCHIVE_BYTES` | Load/parse returns `TOO_LARGE`; `motear` exits with code 2 |
| Single entry data size | 64 MiB | `ARCHIVE::MAX_ENTRY_BYTES` | Writing is rejected; reading returns `BAD_DATA`; exit code 2 |
| String table size | 16 MiB | `ARCHIVE::MAX_STRING_BYTES` | Parsing returns `BAD_HEADER`; exit code 2 |
| Manifest size | 1 MiB | `ARCHIVE::MAX_MANIFEST_BYTES` | Parsing returns `BAD_HEADER`/`BAD_MANIFEST`; exit code 2 |
| Entry count | 65535 | `ARCHIVE::MAX_ENTRY_COUNT` | Parsing returns `BAD_HEADER`; writing is rejected; exit code 2 |
| Entry name length | 1024 B | `ARCHIVE::MAX_NAME_BYTES` | Parsing returns `BAD_TOC`/`BAD_NAME`; writing is rejected; exit code 2 |
| Path segment length | 255 B | `ARCHIVE::MAX_SEGMENT_BYTES` | Name validation fails (`BAD_NAME`); writing is rejected; exit code 2 |
| File header | 96 B | `ARCHIVE::HEADER_SIZE` | Shorter than 96 B returns `TRUNC`; exit code 2 |
| TOC entry | 40 B | `ARCHIVE::TOC_ENTRY_SIZE` | A `toc_entry_size` other than 40 returns `BAD_HEADER`; exit code 2 |

Archive parsing is fail-closed by default: any invalid field, layout, manifest or name rejects the entire archive; embedded modules are also verified by default (`motear verify --no-modules` disables that).

### 5.2 Manifest field lengths

| Manifest key | Length range | Source |
| --- | --- | --- |
| Package name (key 1) | 1..255 B | `manifest.cpp` |
| Package version (key 2) | 1..64 B | `manifest.cpp` |
| Entry module (key 3) | 1..1024 B | `manifest.cpp` |
| Description (key 4) | 0..4096 B | `manifest.cpp` |
| Author/license/build ID/created by | 0..255 B | `manifest.cpp` |
| Any manifest value payload | ≤65535 B | `manifest.cpp` |
| Whole manifest | ≤1 MiB | `ARCHIVE::MAX_MANIFEST_BYTES` |

An out-of-range value fails manifest encoding or decoding, which makes archive writing or parsing fail; `motear` exits with code 2.

## 6. CLI built-in host I/O

The following limits come from the transitional host ABI in `src/motevm/cli.cpp`. Host functions do not raise VM traps; they return negative `dword` result codes (converted to unsigned values for the script). See [05 Toolchain](05-toolchain.md) §8.7 for error-code semantics.

| Item | Value | Source constant | Behavior when exceeded |
| --- | --- | --- | --- |
| Host path length | 4096 B | `MAX_HOST_PATH_BYTES` | A length of 0 or greater than 4096 returns `-5` (`HOST_BAD_ARGUMENT`) |
| Single file I/O size | 16 MiB | `MAX_HOST_IO_BYTES` | `file.read`/`file.write`/`file.append` return `-5` |
| Heap allocation limit | 256 MiB | `MAX_HOST_HEAP_BYTES` | `mem_alloc` returns `NO_INDEX` (`0xFFFFFFFF`) |
| Host address limit | 4 GiB | `HOST_ADDRESS_LIMIT` | An allocation range past this address returns `NO_INDEX` |
| File size return value | 32-bit | `0xFFFFFFFF` check in `file.size` | A file larger than 4 GiB returns `-3` (`HOST_SYSTEM_ERROR`) |
| Stored diagnostics | default 20; errors and warnings are counted separately; `0` means unlimited (1000-record storage cap) | `TOOL::Diagnostics::limit`, `cap()` | Diagnostics past the storage limit are no longer stored, but error/warning counts keep increasing; `moteasm --max-errors N` adjusts the storage limit, and `N=0` uses the 1000-record cap |

Host error codes: `-3` system error (open/read/write/remove failure), `-4` workspace denied, `-5` bad argument or range. `mem_free` is a no-op; memory is released only at process exit.
