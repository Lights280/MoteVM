# 07 · MTR 1.0 Archive Format

[简体中文](../07-mtr-format.md) | [English](07-mtr-format.md)

> **Version note**: The MTR format version is **1.0**, and this document corresponds to the current implementation of MoteVM **0.1.0**. The repository does not yet publish a tagged release, so this document is a best-effort reference; if it disagrees with the implementation, `src/include/archive/archive.cpp`, `writer.cpp`, `manifest.cpp`, `name.cpp`, and `util.cpp` are authoritative. MTR files use the `.mtr` suffix.

## 1. Overview

MTR 1.0 is a read-only archive container with a fixed layout: a 96-byte header, a TOC, a string table, a manifest, and contiguous payloads. All multi-byte integers are **little-endian**.

```text
┌──────────────────────────────────────────────────────────────┐
│ Header (96 bytes, offset 0)                                  │
│   magic, version, flags, entry_count, toc_entry_size,        │
│   toc/strings/manifest/data/file_size offsets, CRC-32C       │
├──────────────────────────────────────────────────────────────┤
│ TOC (entry_count × 40 bytes, offset 96)                      │
├──────────────────────────────────────────────────────────────┤
│ String table (name bytes concatenated, no separators/NUL/pad)│
├──────────────────────────────────────────────────────────────┤
│ Manifest (TLV records, 8-byte aligned, zero padded)          │
├──────────────────────────────────────────────────────────────┤
│ Payload (contiguous in TOC order, each 8-byte aligned/padded)│
└──────────────────────────────────────────────────────────────┘
```

Basic conventions:

| Item | Rule |
|---|---|
| Byte order | All multi-byte integers are little-endian |
| Offset arithmetic | All offsets/sizes use u64 and are checked as `offset <= total && length <= total - offset` |
| Alignment | The TOC, the manifest after the string table, each payload entry start, and the whole file are 8-byte aligned |
| Canonical form | Given the logical contents, exactly one legal layout exists; the reader rejects any non-canonical arrangement |
| Padding | Every unused byte in the file must be zero, including alignment padding, gaps, and TLV record padding |
| Integrity | Whole-archive CRC-32C plus per-entry CRC-32C; any failure rejects the whole archive |
| Failure handling | Fail-closed: no module executes and no partial output is published |

## 2. Header (96 bytes)

| Offset | Size | Field (code name) | Type | Value / meaning | Validation |
|---:|---:|---|---|---|---|
| 0x00 | 8 | `magic` | `u8[8]` | `4D 4F 54 45 41 52 43 00` (`"MOTEARC\0"`) | Must match exactly |
| 0x08 | 2 | `versionMajor` | `u16` | Major version | Must equal 1 |
| 0x0A | 2 | `versionMinor` | `u16` | Minor version | Must equal 0 (current reader) |
| 0x0C | 4 | `flags` | `u32` | bit0 = `HAS_MANIFEST` | Must equal exactly `0x00000001` |
| 0x10 | 4 | `entryCount` | `u32` | Number of TOC entries | ≤ 65535 |
| 0x14 | 4 | `tocEntrySize` | `u32` | TOC entry size | Must equal 40 |
| 0x18 | 8 | `tocOffset` | `u64` | TOC start offset | Must equal 96 |
| 0x20 | 8 | `tocSize` | `u64` | TOC size in bytes | Must equal `entryCount × 40` |
| 0x28 | 8 | `stringsOffset` | `u64` | String table start offset | Must equal `96 + tocSize` |
| 0x30 | 8 | `stringsSize` | `u64` | String table size in bytes | ≤ 16 MiB; not required to be 8-byte aligned |
| 0x38 | 8 | `manifestOffset` | `u64` | Manifest start offset | Must equal `align8(stringsOffset + stringsSize)` |
| 0x40 | 8 | `manifestSize` | `u64` | Manifest size in bytes | ≤ 1 MiB; must be a multiple of 8 |
| 0x48 | 8 | `dataOffset` | `u64` | Start offset of the first payload | Must equal `align8(manifestOffset + manifestSize)`; 8-byte aligned |
| 0x50 | 8 | `fileSize` | `u64` | Total file size in bytes | Must equal the actual input length; must be a multiple of 8 |
| 0x58 | 4 | `checksum` | `u32` | Whole-archive CRC-32C | The field is treated as zero when computing it |
| 0x5C | 4 | `reserved0` | `u32` | Reserved | Must be 0 |

Additional rules:

- The whole-archive CRC is computed only after header fields, `fileSize` vs. actual length, all region offsets/sizes, and 8-byte alignment have passed.
- `manifestOffset` and `dataOffset` must follow the formulas exactly; the reader accepts no other arrangement.
- `entryCount = 0` is structurally legal: then `tocSize = 0`, `stringsSize = 0`, `manifestOffset = 96`, `dataOffset = align8(96 + manifestSize)`, and `fileSize = dataOffset`. `motear pack` still requires at least one module or resource.
- No offset/length computation may wrap; an out-of-bounds value is a structural error.

## 3. TOC (40 bytes per entry)

The TOC follows the header at the fixed offset 96 and contains `entryCount` entries.

| Entry offset | Size | Field (code name) | Type | Meaning | Validation |
|---:|---:|---|---|---|---|
| 0x00 | 4 | `nameOffset` | `u32` | Offset of the name in the string table | Must equal the previous entry's end (0 for the first) |
| 0x04 | 4 | `nameLength` | `u32` | Name size in bytes | 1–1024; the name must lie inside the string table |
| 0x08 | 1 | `kind` | `u8` | 1 = module, 2 = resource | Only 1 or 2 allowed |
| 0x09 | 1 | `flags` | `u8` | Entry flags | Must be 0 |
| 0x0A | 2 | `reserved0` | `u16` | Reserved | Must be 0 |
| 0x0C | 4 | `crc32c` | `u32` | CRC-32C of the entry data | Covers exactly `dataSize` bytes |
| 0x10 | 8 | `dataOffset` | `u64` | Absolute data start offset in the file | Must equal the previous entry's aligned end (header `dataOffset` for the first) |
| 0x18 | 8 | `dataSize` | `u64` | Data size in bytes (excluding padding) | ≤ 64 MiB; module entries must be > 0 |
| 0x20 | 8 | `reserved1` | `u64` | Reserved | Must be 0 |

TOC invariants:

| ID | Rule |
|---|---|
| T1 | Entry names are in **strictly ascending unsigned byte order**: `name[i] < name[i+1]` |
| T2 | Names are contiguous in the string table: entry 0 has `nameOffset = 0`, entry i equals the previous `nameOffset + nameLength`; the sum of all name lengths must equal `stringsSize`, with no unreferenced bytes |
| T3 | Every name must pass the path rules in §5 |
| T4 | `kind` is only 1 (module) or 2 (resource); entry `flags`, `reserved0`, and `reserved1` must be 0 |
| T5 | `kind = module` names must end with `.mbc` (ASCII case-insensitive) and have `dataSize > 0` |
| T6 | Entry data is contiguous in TOC order: the first `dataOffset` equals the header `dataOffset`, and each later entry equals `align8(previous dataOffset + previous dataSize)`; after the last entry's data and alignment, the result must equal `fileSize` |
| T7 | Names must not collide after ASCII case folding |
| T8 | File/directory prefix conflicts are forbidden: if entry `A` exists, no entry may begin with `A + "/"` (compared after ASCII case folding) |

Note: TOC ordering uses the raw name bytes (case-sensitive), while duplicate detection additionally uses ASCII case folding. Thus `A.mbc` and `a.mbc` have different byte order but are still detected as a collision and rejected.

## 4. String table

- Starts at `stringsOffset` and is `stringsSize` bytes long.
- It is the concatenation of all entry-name bytes: **no separators, no NUL terminators, no padding**.
- The TOC references names with `(nameOffset, nameLength)`; names do not need to be NUL-terminated.
- `stringsSize` is at most 16 MiB; the sum of all name lengths must equal it exactly.
- Names are stored as raw bytes with no Unicode normalization and no case conversion.

Only zero padding (at most 7 bytes) is allowed between the end of the string table and `manifestOffset`.

## 5. Name rules

Names are logical **relative paths** inside the package, not host filesystem paths. The rules are implemented in `src/include/archive/name.hpp` and `name.cpp`.

### 5.1 Encoding and characters

| Rule | Description |
|---|---|
| Non-empty | At least 1 byte |
| Length limit | At most 1024 bytes |
| UTF-8 | Must be strictly valid UTF-8: reject overlong encodings, surrogates, truncated sequences, invalid continuation bytes, and code points above U+10FFFF |
| Control characters | No `0x00`–`0x1F` and no `0x7F` |
| Backslash | No `\` |
| Colon | No `:` (blocks Windows drive letters and NTFS ADS) |
| Separator | Only `/`; a leading/trailing `/` or `//` creates an empty segment and is therefore rejected |

### 5.2 Segment rules

After splitting on `/`, every segment must satisfy:

| Rule | Description |
|---|---|
| Length | 1–255 bytes |
| Not `.` / `..` | Relative path segments are forbidden |
| Ending | Must not end with a space (`0x20`) or a dot (`.`) |
| Reserved device name | Must not be a Windows reserved device name (basename before the first `.`, ASCII case-insensitive): `CON`, `PRN`, `AUX`, `NUL`, `COM0`–`COM9`, `LPT0`–`LPT9` |

Consequently a name cannot contain an absolute path, a drive letter, a `..` segment, or backslash separators; unpacking tools must still perform an additional canonical containment check inside the destination directory.

### 5.3 Uniqueness and folding

| Rule | Description |
|---|---|
| Exact duplicate | Rejected; strict TOC ordering already prevents it, but the reader and writer still check explicitly |
| ASCII case-folded duplicate | Rejected, e.g. `A.mbc` and `a.mbc` cannot coexist |
| Non-ASCII case/normalization | v1 does not perform full folding; this is a known residual risk, and publishers should avoid distinguishing names only by Unicode variants |
| File/directory prefix conflict | Rejected: `a` and `a/b` cannot coexist (compared after folding) |

`foldAscii()` only maps ASCII `A`–`Z` to lowercase; sorting and string-table storage keep the original bytes.

### 5.4 Module suffix

A `kind = module` name must end with `.mbc` (ASCII case-insensitive), and the final path segment must have at least one character before `.mbc`. A directory part is allowed, for example `modules/main.mbc`. Module data must be a complete MOTEBC file, not a headerless code blob.

## 6. Manifest (TLV)

The manifest follows the string table (8-byte aligned) and consists of TLV records with no extra separators.

| Record offset | Size | Field | Type | Meaning |
|---:|---:|---|---|---|
| 0x00 | 2 | `key` | `u16` | Metadata key |
| 0x02 | 2 | `len` | `u16` | Payload size in bytes, 0–65535 |
| 0x04 | `len` | `payload` | `u8[]` | Content |
| 0x04 + `len` | 0–7 | `padding` | `u8[]` | Zero padding to a multiple of 8 bytes |

- Record size = `align8(4 + len)`.
- Records are in **strictly ascending** `key` order; a key may not repeat; key 0 is reserved and rejected.
- All padding bytes must be zero; `manifestSize` must be a multiple of 8 and ≤ 1 MiB.
- Known-key payloads are validated per §6.1; unknown-key payloads are accepted as opaque bytes but must still satisfy bounds, ordering, length, and padding rules.
- Known text payloads must be valid UTF-8 and must not contain NUL, C0 control characters, or DEL.

### 6.1 Known keys

| key | Name | Payload | Length range | Validation |
|---:|---|---|---:|---|
| 1 | package name | UTF-8 text | 1–255 | Non-empty, valid text |
| 2 | package version | UTF-8 text (SemVer not enforced) | 1–64 | Non-empty, valid text |
| 3 | entry module | UTF-8 text | 1–1024 | Must exactly match the name of a `kind = module` entry |
| 4 | description | UTF-8 text | 0–4096 | Valid text |
| 5 | author / organization | UTF-8 text | 0–255 | Valid text |
| 6 | license | UTF-8 text (SPDX expression recommended) | 0–255 | Valid text |
| 7 | build id / provenance | UTF-8 text | 0–255 | Valid text |
| 8 | created-by | UTF-8 text (tool name + version) | 0–255 | Valid text |

Rules:

- If key 3 exists, it must reference an actually existing module entry; referencing a resource or a missing name rejects the whole archive. Key 3's payload may not be empty.
- Key 3 may be omitted. When omitted, entry resolution requires exactly one module entry in the package.
- Unknown keys (> 8) are accepted by the reader and ignored semantically; writers must not generate unknown keys that change execution semantics.
- The manifest may be empty (`manifestSize = 0`); in that case all entry selection relies on module entries in the TOC.

## 7. Payload

- Starts at the header `dataOffset` and is stored contiguously in TOC order.
- Every entry's data start must be 8-byte aligned; data is followed by zero padding to an 8-byte boundary.
- `kind = module`: the data must be a complete 1.0 module accepted by MOTEBC `Parse::load(span)` + `Parse::parse()`; it must not be empty and is at most 64 MiB.
- `kind = resource`: arbitrary bytes, not interpreted or executed; at most 64 MiB, and may be 0 bytes.
- Only zero padding is allowed after the last entry up to `fileSize`; `fileSize` must be a multiple of 8.
- Module/resource data may contain any bytes (including NUL) but must not change the declared `dataSize`.

When validating a module, the reader uses the same parser and verifier as for a standalone `.mbc` file; failure messages include the entry name.

## 8. CRC-32C

MTR uses the same CRC-32C parameters as MOTEBC:

| Parameter | Value |
|---|---|
| Polynomial (normal form) | `0x1EDC6F41` (Castagnoli) |
| Reflected form | `0x82F63B78` |
| Initial value | `0xFFFFFFFF` |
| Final XOR | `0xFFFFFFFF` |
| Input/output reflection | yes |

### 8.1 Whole-archive checksum

- Compute CRC-32C over the whole file `[0, fileSize)` with the `checksum` field (offsets 0x58–0x5B) treated as 0.
- All other header fields and all file content participate.
- The reader validates basic header fields and `fileSize` before computing the whole-archive CRC; failure returns `BAD_CHECKSUM` and the TOC is not parsed further.

### 8.2 Entry checksums

- Each TOC entry's `crc32c` is the CRC-32C of exactly its `dataSize` bytes, excluding alignment padding.
- When `dataSize = 0`, the CRC is `0x00000000`.
- After the whole-archive CRC passes, the reader still verifies every entry; any failure rejects the whole archive, and a corrupt entry may not be skipped.
- Entry CRCs primarily locate corruption; they do not replace the whole-archive CRC and provide no cryptographic authenticity.

## 9. Canonical writer rules

A canonical writer (`motear pack` / `ARCHIVE::Writer`) produces a unique byte sequence as follows:

1. Validate and sort entries: ascending unsigned byte order of names.
2. Validate names (§5), kind, dataSize, module suffix, and MOTEBC validity; reject exact duplicates, ASCII case-folded duplicates, and file/directory prefix conflicts.
3. Build the string table: concatenate names and record each `nameOffset` / `nameLength`.
4. Build the manifest: encode TLV records in ascending key order, each padded to 8 bytes.
5. Compute the layout: `tocOffset = 96`; `tocSize = entryCount × 40`; `stringsOffset = 96 + tocSize`; `stringsSize = Σ nameLength`; `manifestOffset = align8(stringsOffset + stringsSize)`; `manifestSize = manifest byte count`; `dataOffset = align8(manifestOffset + manifestSize)`.
6. Write payloads in TOC order: `cursor = dataOffset`; for each entry, `cursor = align8(cursor)`, `dataOffset = cursor`, `crc32c = CRC32C(exactly dataSize bytes)`, `cursor += dataSize`; finally `fileSize = align8(cursor)`.
7. Write the header (checksum as 0 first), TOC, string table, manifest, and payloads; write zero to every unused byte.
8. Compute the whole-archive CRC-32C (checksum field treated as 0) and backfill it.

Determinism requirements:

| Rule | Description |
|---|---|
| Ordering | Entries by name byte order; manifest by ascending key |
| No timestamps/randomness | The file stores no mtime, random values, or host absolute paths |
| No permission bits | No file permissions, symlinks, or ownership are stored |
| Reproducibility | The same inputs (names, kinds, data, manifest, tool version) produce the same bytes |
| Entry | With multiple modules, entry must be specified explicitly; with exactly one module and no key 3, key 3 is written automatically |
| Checksum | Computed and backfilled last; all padding is zero |

## 10. Reader and verification pipeline

### 10.1 Pipeline

```text
Archive::load(path or span)
        ↓
Archive::parse(verifyModules = true)
        ↓
1. Input size: ≥ 96, ≤ 256 MiB, multiple of 8
2. Header fields: magic, version, flags, reserved, counts, fixed offsets/sizes
3. Whole-archive CRC-32C (checksum field treated as 0)
4. Each TOC entry: name offset/length, name rules, kind, flags, reserved,
   entry CRC, data offset/size, ascending names, case-fold uniqueness, prefix conflicts
5. Regions: header/TOC/string table/manifest/payloads do not overlap; all gaps are zero
6. Manifest: TLV bounds, ascending keys, padding, known-key length/text, key 3 reference
7. Modules: run MOTEBC Parse/Verifier on every module entry
        ↓
success: valid() = true; failure: entries/manifest cleared, error code returned
```

Failure handling is **fail-closed**: any failed step clears parsed entries and the manifest, publishes no partial result, executes no module, and writes no file.

### 10.2 ARCHIVE error codes

| Code | Name | Meaning |
|---:|---|---|
| 0 | `OK` | Success |
| 1 | `FAIL_OPEN` | Cannot open or read the archive |
| 2 | `EMPTY` | Input is empty |
| 3 | `TRUNC` | Input is shorter than 96 bytes or the read was incomplete |
| 4 | `TOO_LARGE` | Exceeds 256 MiB |
| 5 | `BAD_HEADER` | Invalid magic/version/flags/reserved/size/alignment/fixed offset |
| 6 | `BAD_CHECKSUM` | Whole-archive CRC-32C mismatch |
| 7 | `BAD_TOC` | Invalid TOC fields, order, continuity, or layout |
| 8 | `BAD_NAME` | Invalid entry name or prefix conflict |
| 9 | `DUPLICATE` | Duplicate name or ASCII case-fold collision |
| 10 | `BAD_MANIFEST` | Invalid TLV, key, length, text, or key 3 reference |
| 11 | `BAD_MODULE` | A module entry failed MOTEBC parsing/verification |
| 12 | `BAD_DATA` | Invalid entry offset/size/CRC/padding/region |
| 13 | `BAD_ENTRY` | Cannot select an entry module (missing, ambiguous, or invalid reference) |

`Archive::parse()` returns 0 or 2–12; code 1 comes from `load()`; code 13 belongs to entry-resolution semantics (`resolveEntry` reports the specific reason), and callers such as `motear verify --strict` map it to an input error.

### 10.3 `motear verify` and `--strict`

| Invocation | Behavior |
|---|---|
| `motear verify FILE.mtr` | Fully parse the archive and validate every module entry (default `verifyModules = true`) |
| `motear verify FILE.mtr --no-modules` | Container-level checks only; skip MOTEBC parsing/verification |
| `motear verify FILE.mtr --strict` | On top of full validation, require non-empty manifest key 1 (package name), non-empty key 2 (package version), and a resolvable entry module |

Entry-module resolution:

1. If manifest key 3 exists, it must name an existing module entry.
2. If key 3 is absent, the package must contain exactly one module entry.
3. No module, multiple modules without key 3, or key 3 pointing at a resource/missing name all fail.

`--strict` does not additionally require key 3 to exist; if there is exactly one module, omitting key 3 still passes. Exit codes: 0 success, 1 usage error, 2 input/verification error, 3 I/O error, 4 internal error.

## 11. Limits and where they surface

| Limit | Maximum | Primary location |
|---|---:|---|
| Whole-archive file size | 256 MiB | `load()` / `parse()`: `TOO_LARGE` |
| Single module data | 64 MiB | TOC parsing: `BAD_DATA`; writer rejects |
| Single resource data | 64 MiB | TOC parsing: `BAD_DATA`; writer rejects |
| Number of entries | 65535 | Header: `BAD_HEADER` |
| Name length | 1024 bytes | TOC: `BAD_TOC`; name validation: `BAD_NAME` |
| Single path segment | 255 bytes | Name validation: `BAD_NAME` |
| String table | 16 MiB | Header: `BAD_HEADER` |
| Manifest total size | 1 MiB | Header: `BAD_HEADER`; the decoder has an additional defensive check |
| Single manifest payload | 65535 bytes | TLV u16 length field; the encoder rejects larger values |
| Header size | fixed 96 bytes | Shorter than 96: `TRUNC` |
| TOC entry size | fixed 40 bytes | Not equal to 40: `BAD_HEADER` |

See [08-limits.md](08-limits.md) for the complete consolidated limit list.

## 12. Version rules

| Input condition | Reader behavior |
|---|---|
| `versionMajor != 1` | Reject (`BAD_HEADER`) |
| `versionMinor > 0` | Reject (`BAD_HEADER`); the current reader supports only 1.0 |
| `flags != 0x00000001` | Reject (`BAD_HEADER`) |
| `reserved0 != 0` | Reject (`BAD_HEADER`) |
| Unknown TOC `kind` | Reject (`BAD_TOC`) |
| Non-zero TOC `flags`/`reserved0`/`reserved1` | Reject (`BAD_TOC`) |
| Unknown manifest key (> 8) | Accepted as opaque metadata (still requires ascending keys, valid length, and valid padding) |
| Manifest key 0 | Reject (`BAD_MANIFEST`) |
| Unsupported layout/alignment/offset | Reject (`BAD_HEADER` / `BAD_TOC` / `BAD_DATA`) |

The reader does not attempt best-effort parsing of a higher minor; an unconfirmed version is rejected outright.

## 13. Minimal archive layout example

The following 544-byte archive contains one module and one resource; all offsets and CRCs were produced by the current writer and independently recomputed.

### 13.1 Logical contents

| Entry | kind | nameLength | dataSize | CRC-32C | dataOffset |
|---|---:|---:|---:|---|---:|
| `assets/greeting.txt` | 2 (resource) | 19 | 6 | `0x353DD8BE` | 0x108 |
| `doc_mini.mbc` | 1 (module) | 12 | 272 | `0xA642C27C` | 0x110 |

Manifest:

| key | payload | Encoded length |
|---:|---|---:|
| 1 | `demo` | 8 |
| 2 | `1.0.0` | 16 |
| 3 | `doc_mini.mbc` | 16 |
| 8 | `motear 0.1.0` | 16 |

### 13.2 Header and TOC hexdump (0x00–0xAF)

```text
00000000  4d 4f 54 45 41 52 43 00 01 00 00 00 01 00 00 00  |MOTEARC.........|
00000010  02 00 00 00 28 00 00 00 60 00 00 00 00 00 00 00  |....(...`.......|
00000020  50 00 00 00 00 00 00 00 b0 00 00 00 00 00 00 00  |P...............|
00000030  1f 00 00 00 00 00 00 00 d0 00 00 00 00 00 00 00  |................|
00000040  38 00 00 00 00 00 00 00 08 01 00 00 00 00 00 00  |8...............|
00000050  20 02 00 00 00 00 00 00 19 11 47 3c 00 00 00 00  | .........G<....|
00000060  00 00 00 00 13 00 00 00 02 00 00 00 be d8 3d 35  |..............=5|
00000070  08 01 00 00 00 00 00 00 06 00 00 00 00 00 00 00  |................|
00000080  00 00 00 00 00 00 00 00 13 00 00 00 0c 00 00 00  |................|
00000090  01 00 00 00 7c c2 42 a6 10 01 00 00 00 00 00 00  |....|.B.........|
000000a0  10 01 00 00 00 00 00 00 00 00 00 00 00 00 00 00  |................|
```

Header interpretation:

| Offset | Value | Field | Interpretation |
|---:|---|---|---|
| 0x00–0x07 | `4D 4F 54 45 41 52 43 00` | `magic` | `"MOTEARC\0"` |
| 0x08–0x09 | `01 00` | `versionMajor` | 1 |
| 0x0A–0x0B | `00 00` | `versionMinor` | 0 |
| 0x0C–0x0F | `01 00 00 00` | `flags` | `0x00000001` (HAS_MANIFEST) |
| 0x10–0x13 | `02 00 00 00` | `entryCount` | 2 |
| 0x14–0x17 | `28 00 00 00` | `tocEntrySize` | 40 |
| 0x18–0x1F | `60 00 00 00 00 00 00 00` | `tocOffset` | 96 |
| 0x20–0x27 | `50 00 00 00 00 00 00 00` | `tocSize` | 80 |
| 0x28–0x2F | `B0 00 00 00 00 00 00 00` | `stringsOffset` | 176 |
| 0x30–0x37 | `1F 00 00 00 00 00 00 00` | `stringsSize` | 31 |
| 0x38–0x3F | `D0 00 00 00 00 00 00 00` | `manifestOffset` | 208 |
| 0x40–0x47 | `38 00 00 00 00 00 00 00` | `manifestSize` | 56 |
| 0x48–0x4F | `08 01 00 00 00 00 00 00` | `dataOffset` | 264 |
| 0x50–0x57 | `20 02 00 00 00 00 00 00` | `fileSize` | 544 |
| 0x58–0x5B | `19 11 47 3C` | `checksum` | little-endian u32 = `0x3C471119` |
| 0x5C–0x5F | `00 00 00 00` | `reserved0` | 0 |

TOC interpretation:

| Entry | Byte range | Field | Value |
|---:|---|---|---|
| [0] | 0x60–0x87 | `nameOffset` / `nameLength` | 0 / 19 |
| | | `kind` / `flags` / `reserved0` | 2 / 0 / 0 |
| | | `crc32c` | `0x353DD8BE` |
| | | `dataOffset` / `dataSize` / `reserved1` | 264 / 6 / 0 |
| [1] | 0x88–0xAF | `nameOffset` / `nameLength` | 19 / 12 |
| | | `kind` / `flags` / `reserved0` | 1 / 0 / 0 |
| | | `crc32c` | `0xA642C27C` |
| | | `dataOffset` / `dataSize` / `reserved1` | 272 / 272 / 0 |

### 13.3 Layout regions

| Offset range | Size | Region | Description |
|---|---:|---|---|
| 0x000–0x05F | 96 | Header | Fixed header |
| 0x060–0x0AF | 80 | TOC | 2 × 40 |
| 0x0B0–0x0CE | 31 | String table | `"assets/greeting.txt"` + `"doc_mini.mbc"` |
| 0x0CF–0x0CF | 1 | Alignment padding | 0 |
| 0x0D0–0x107 | 56 | Manifest | TLV records for keys 1, 2, 3, 8 |
| 0x108–0x10D | 6 | data[0] | `"hello\n"` (resource) |
| 0x10E–0x10F | 2 | Alignment padding | 0 |
| 0x110–0x21F | 272 | data[1] | `doc_mini.mbc` (module) |
| 0x220 | 0 | End of file | `fileSize = 544` |

Checksum: after zeroing offsets 0x58–0x5B, CRC-32C over all 544 bytes is `0x3C471119`; writing it little-endian gives `19 11 47 3C`. The two payload CRCs cover exactly the 6 bytes and 272 bytes respectively, excluding alignment padding.

## 14. Related documents

- [02-bytecode-format.md](02-bytecode-format.md): the MOTEBC 1.0 format and verification rules that module entries must satisfy.
- [05-toolchain.md](05-toolchain.md): assembler/disassembler and toolchain.
- [08-limits.md](08-limits.md): consolidated resource limits.
- [self/cli.md](self/cli.md): `motear` command-line usage.
