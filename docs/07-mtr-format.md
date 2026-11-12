# 07 · MTR 1.0 归档格式

[简体中文](07-mtr-format.md) | [English](en/07-mtr-format.md)

> **版本说明**：MTR 格式版本为 **1.0**，本文对应 MoteVM **0.1.0** 的当前实现。仓库尚未发布带版本标签的正式发行版，因此本文属于尽力而为的参考；若本文与实现不一致，以 `src/include/archive/archive.cpp`、`writer.cpp`、`manifest.cpp`、`name.cpp`、`util.cpp` 为准。MTR 文件后缀为 `.mtr`。

## 1. 总览

MTR 1.0 是一个只读归档容器，布局固定为：96 字节头部、TOC、字符串表、manifest、连续排列的 payload。所有多字节整数均为**小端**。

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

基本约定：

| 项目 | 规则 |
|---|---|
| 字节序 | 所有多字节整数均为小端 |
| 偏移运算 | 所有 offset/size 使用 u64，并以 `offset <= total && length <= total - offset` 形式检查 |
| 对齐 | TOC、字符串表之后的 manifest、payload 条目起始、整个文件均要求 8 字节对齐 |
| 规范化 | 给定逻辑内容只有一种合法布局；读取器拒绝任何非规范排列 |
| 填充 | 文件内所有未使用字节必须为 0，包括对齐填充、段间空隙与 TLV 记录填充 |
| 完整性 | 整包 CRC-32C + 每个条目 CRC-32C；任一失败拒绝整个包 |
| 失败处理 | fail-closed：不执行模块、不发布半成品 |

## 2. 头部（96 字节）

| 偏移 | 大小 | 字段（代码名） | 类型 | 值 / 含义 | 校验 |
|---:|---:|---|---|---|---|
| 0x00 | 8 | `magic` | `u8[8]` | `4D 4F 54 45 41 52 43 00`（`"MOTEARC\0"`） | 必须精确匹配 |
| 0x08 | 2 | `versionMajor` | `u16` | 主版本 | 必须等于 1 |
| 0x0A | 2 | `versionMinor` | `u16` | 次版本 | 必须等于 0（当前读取器） |
| 0x0C | 4 | `flags` | `u32` | bit0 = `HAS_MANIFEST` | 必须精确等于 `0x00000001` |
| 0x10 | 4 | `entryCount` | `u32` | TOC 条目数 | ≤ 65535 |
| 0x14 | 4 | `tocEntrySize` | `u32` | TOC 条目大小 | 必须等于 40 |
| 0x18 | 8 | `tocOffset` | `u64` | TOC 起始偏移 | 必须等于 96 |
| 0x20 | 8 | `tocSize` | `u64` | TOC 字节数 | 必须等于 `entryCount × 40` |
| 0x28 | 8 | `stringsOffset` | `u64` | 字符串表起始偏移 | 必须等于 `96 + tocSize` |
| 0x30 | 8 | `stringsSize` | `u64` | 字符串表字节数 | ≤ 16 MiB；不要求 8 对齐 |
| 0x38 | 8 | `manifestOffset` | `u64` | manifest 起始偏移 | 必须等于 `align8(stringsOffset + stringsSize)` |
| 0x40 | 8 | `manifestSize` | `u64` | manifest 字节数 | ≤ 1 MiB；必须是 8 的倍数 |
| 0x48 | 8 | `dataOffset` | `u64` | 第一条 payload 的起始偏移 | 必须等于 `align8(manifestOffset + manifestSize)`；8 对齐 |
| 0x50 | 8 | `fileSize` | `u64` | 整个文件字节数 | 必须等于实际输入长度；必须是 8 的倍数 |
| 0x58 | 4 | `checksum` | `u32` | 整包 CRC-32C | 计算时把本字段视为 0 |
| 0x5C | 4 | `reserved0` | `u32` | 保留 | 必须为 0 |

补充规则：

- 头部字段、`fileSize` 与实际长度、各区间偏移/大小、8 字节对齐全部通过后，才计算整包 CRC。
- `manifestOffset`、`dataOffset` 必须严格符合上述公式；读取器不接受其他排列。
- `entryCount = 0` 在格式上合法：此时 `tocSize = 0`、`stringsSize = 0`、`manifestOffset = 96`、`dataOffset = align8(96 + manifestSize)`、`fileSize = dataOffset`。`motear pack` 仍要求至少提供一个模块或资源。
- 任何偏移/长度计算都不得回绕；越界视为结构错误。

## 3. TOC（每条 40 字节）

TOC 紧接头部，起始偏移固定为 96；共 `entryCount` 条。

| 条目内偏移 | 大小 | 字段（代码名） | 类型 | 含义 | 校验 |
|---:|---:|---|---|---|---|
| 0x00 | 4 | `nameOffset` | `u32` | 名称在字符串表中的偏移 | 必须等于前一条的结束位置（首条为 0） |
| 0x04 | 4 | `nameLength` | `u32` | 名称字节数 | 1–1024；名称必须落在字符串表内 |
| 0x08 | 1 | `kind` | `u8` | 1 = module，2 = resource | 仅允许 1 或 2 |
| 0x09 | 1 | `flags` | `u8` | 条目标志 | 必须为 0 |
| 0x0A | 2 | `reserved0` | `u16` | 保留 | 必须为 0 |
| 0x0C | 4 | `crc32c` | `u32` | 条目数据的 CRC-32C | 覆盖精确的 `dataSize` 字节 |
| 0x10 | 8 | `dataOffset` | `u64` | 数据在文件中的起始偏移 | 必须等于前一条数据的对齐结束位置（首条为头部 `dataOffset`） |
| 0x18 | 8 | `dataSize` | `u64` | 数据字节数（不含填充） | ≤ 64 MiB；模块条目必须 > 0 |
| 0x20 | 8 | `reserved1` | `u64` | 保留 | 必须为 0 |

TOC 不变量：

| 编号 | 规则 |
|---|---|
| T1 | 条目名称按**无符号字节序严格升序**：`name[i] < name[i+1]` |
| T2 | 名称在字符串表中连续排列：第 0 条 `nameOffset = 0`，第 i 条等于前一条 `nameOffset + nameLength`；全部名称长度之和必须等于 `stringsSize`，不得有未引用字节 |
| T3 | 每个名称必须通过 §5 的路径规则 |
| T4 | `kind` 只能是 1（module）或 2（resource）；条目的 `flags`、`reserved0`、`reserved1` 必须为 0 |
| T5 | `kind = module` 的名称必须以 `.mbc` 结尾（ASCII 大小写不敏感），且 `dataSize > 0` |
| T6 | 条目数据按 TOC 顺序连续排列：首条 `dataOffset` 等于头部 `dataOffset`，之后每条等于 `align8(前一条 dataOffset + 前一条 dataSize)`；最后一条数据结束并对齐后必须等于 `fileSize` |
| T7 | 名称不得在 ASCII 大小写折叠后重复 |
| T8 | 禁止文件/目录前缀冲突：若存在条目 `A`，则不得存在以 `A + "/"` 开头的条目（按 ASCII 大小写折叠比较） |

说明：TOC 的排序使用原始名称字节（区分大小写），而重复检查额外使用 ASCII 大小写折叠。因此 `A.mbc` 与 `a.mbc` 的字节序不同，但会被判为冲突并拒绝。

## 4. 字符串表

- 起始于 `stringsOffset`，长度为 `stringsSize`。
- 内容是所有条目名称的字节直接拼接：**无分隔符、无 NUL 终止符、无填充**。
- TOC 用 `(nameOffset, nameLength)` 引用名称；名称本身不要求以 NUL 结尾。
- `stringsSize` 上限为 16 MiB；所有名称长度之和必须精确等于该值。
- 名称按原始字节保存，不做 Unicode 归一化，也不做大小写转换。

字符串表之后到 `manifestOffset` 之间只允许 0 填充（最多 7 字节）。

## 5. 名称规则

名称是包内**逻辑相对路径**，不是宿主文件系统路径。规则实现见 `src/include/archive/name.hpp` 与 `name.cpp`。

### 5.1 编码与字符

| 规则 | 说明 |
|---|---|
| 非空 | 长度至少为 1 字节 |
| 长度上限 | 不超过 1024 字节 |
| UTF-8 | 必须是严格合法的 UTF-8：拒绝过长编码、代理项、截断序列、非法续字节及超过 U+10FFFF 的码点 |
| 控制字符 | 不得包含 `0x00`–`0x1F` 或 `0x7F` |
| 反斜杠 | 不得包含 `\` |
| 冒号 | 不得包含 `:`（阻断 Windows 盘符与 NTFS ADS） |
| 分隔符 | 只使用 `/`；首尾 `/` 或连续 `//` 会产生空段，因此被拒绝 |

### 5.2 段规则

按 `/` 分段后，每个段必须满足：

| 规则 | 说明 |
|---|---|
| 长度 | 1–255 字节 |
| 非 `.` / `..` | 禁止相对路径段 |
| 结尾 | 不得以空格（`0x20`）或点（`.`）结尾 |
| 保留设备名 | 不得是 Windows 保留设备名（取第一个 `.` 前的基名，ASCII 大小写不敏感）：`CON`、`PRN`、`AUX`、`NUL`、`COM0`–`COM9`、`LPT0`–`LPT9` |

因此名称不可能包含绝对路径、盘符、`..` 段或反斜杠分隔符；解包工具仍必须在目标目录内做额外的规范化包含检查。

### 5.3 唯一性与折叠

| 规则 | 说明 |
|---|---|
| 精确重复 | 拒绝；TOC 严格升序已保证，但读取器与写入器仍显式检查 |
| ASCII 大小写折叠重复 | 拒绝，例如 `A.mbc` 与 `a.mbc` 不能同时存在 |
| 非 ASCII 大小写/归一化 | v1 不做完整折叠；这是已知的残余风险，发布者应避免仅靠 Unicode 变体区分名称 |
| 文件/目录前缀冲突 | 拒绝：`a` 与 `a/b` 不能同时存在（折叠后比较） |

`foldAscii()` 只把 ASCII `A`–`Z` 转为小写；排序与字符串表存储保持原始字节。

### 5.4 模块后缀

`kind = module` 的名称必须以 `.mbc` 结尾（ASCII 大小写不敏感），并且最后一个路径段在 `.mbc` 前至少有一个字符。目录部分可以存在，例如 `modules/main.mbc`。模块数据必须是完整的 MOTEBC 文件，而不是去掉头部的代码段。

## 6. Manifest（TLV）

Manifest 紧跟字符串表（8 字节对齐），由若干 TLV 记录组成；记录之间无额外分隔。

| 记录内偏移 | 大小 | 字段 | 类型 | 含义 |
|---:|---:|---|---|---|
| 0x00 | 2 | `key` | `u16` | 元数据键 |
| 0x02 | 2 | `len` | `u16` | payload 字节数，0–65535 |
| 0x04 | `len` | `payload` | `u8[]` | 内容 |
| 0x04 + `len` | 0–7 | `padding` | `u8[]` | 补 0 至记录总长为 8 的倍数 |

- 记录总长 = `align8(4 + len)`。
- 记录按 `key` **严格升序**排列；同一 key 不得重复；key 0 保留并拒绝。
- 所有 padding 字节必须为 0；`manifestSize` 必须是 8 的倍数且 ≤ 1 MiB。
- 已知 key 的 payload 按 §6.1 校验；未知 key 的 payload 作为不透明字节接受，但仍必须满足边界、升序、长度与填充规则。
- 已知文本 payload 必须是合法 UTF-8，且不得包含 NUL、C0 控制字符或 DEL。

### 6.1 已知键

| key | 名称 | payload | 长度范围 | 校验 |
|---:|---|---|---:|---|
| 1 | package name | UTF-8 文本 | 1–255 | 非空、合法文本 |
| 2 | package version | UTF-8 文本（不强制 SemVer） | 1–64 | 非空、合法文本 |
| 3 | entry module | UTF-8 文本 | 1–1024 | 必须精确匹配某个 `kind = module` 的条目名 |
| 4 | description | UTF-8 文本 | 0–4096 | 合法文本 |
| 5 | author / organization | UTF-8 文本 | 0–255 | 合法文本 |
| 6 | license | UTF-8 文本（建议 SPDX 表达式） | 0–255 | 合法文本 |
| 7 | build id / provenance | UTF-8 文本 | 0–255 | 合法文本 |
| 8 | created-by | UTF-8 文本（工具名 + 版本） | 0–255 | 合法文本 |

规则：

- key 3 若存在，必须引用一个实际存在的 module 条目；引用 resource 或不存在的名称均拒绝整个包。key 3 的 payload 不能为空。
- key 3 可以省略。省略时，入口解析要求包内**恰好有一个** module 条目。
- 未知 key（> 8）被读取器接受并忽略其语义；写入器不得生成会改变执行语义的未知键。
- Manifest 可以为空（`manifestSize = 0`）；此时所有入口选择依赖 TOC 中的 module 条目。

## 7. Payload

- 起始于头部 `dataOffset`，按 TOC 顺序连续存放。
- 每个条目的数据起始必须 8 字节对齐；数据之后补 0 至 8 字节边界。
- `kind = module`：数据必须是完整、可被 MOTEBC `Parse::load(span)` + `Parse::parse()` 接受的 1.0 模块；数据不得为空，大小 ≤ 64 MiB。
- `kind = resource`：任意字节，不解释、不执行；大小 ≤ 64 MiB，可以为 0 字节。
- 最后一个条目之后到 `fileSize` 只能是 0 填充；`fileSize` 必须是 8 的倍数。
- 模块/资源数据内部可以包含任意字节（包括 NUL），但不得改变声明的 `dataSize`。

读取器在验证模块时使用与独立 `.mbc` 文件相同的解析器和验证器；失败信息包含条目名称。

## 8. CRC-32C

与 MOTEBC 使用相同的 CRC-32C 参数：

| 参数 | 值 |
|---|---|
| 多项式（正常形式） | `0x1EDC6F41`（Castagnoli） |
| 反射形式 | `0x82F63B78` |
| 初始值 | `0xFFFFFFFF` |
| 结果异或 | `0xFFFFFFFF` |
| 输入/输出反射 | 是 |

### 8.1 整包校验

- 计算整个文件 `[0, fileSize)` 的 CRC-32C，计算时把 `checksum` 字段（偏移 0x58–0x5B）视为 0。
- 头部其余字段与文件全部内容都参与计算。
- 读取器先验证头部基本字段与 `fileSize`，再计算整包 CRC；失败返回 `BAD_CHECKSUM`，不继续解析 TOC。

### 8.2 条目校验

- 每个 TOC 条目的 `crc32c` 是该条目**精确 `dataSize` 字节**的 CRC-32C，不包含对齐填充。
- `dataSize = 0` 时 CRC 为 `0x00000000`。
- 整包 CRC 通过后，读取器仍逐条校验；任一条目失败都会拒绝整个包，不允许跳过损坏条目继续。
- 条目 CRC 主要用于定位损坏；它不能替代整包校验，也不提供密码学真实性。

## 9. 规范写入规则

规范写入端（`motear pack` / `ARCHIVE::Writer`）按以下步骤生成唯一字节序列：

1. 校验并排序条目：按名称无符号字节序升序。
2. 校验名称（§5）、kind、dataSize、模块后缀与 MOTEBC 有效性；拒绝精确重复、ASCII 大小写折叠重复与文件/目录前缀冲突。
3. 构造字符串表：名称依次拼接，记录每条 `nameOffset` / `nameLength`。
4. 构造 manifest：按 key 升序编码 TLV，每条补 0 至 8 字节。
5. 计算布局：`tocOffset = 96`；`tocSize = entryCount × 40`；`stringsOffset = 96 + tocSize`；`stringsSize = Σ nameLength`；`manifestOffset = align8(stringsOffset + stringsSize)`；`manifestSize = manifest 字节数`；`dataOffset = align8(manifestOffset + manifestSize)`。
6. 按 TOC 顺序写 payload：`cursor = dataOffset`；对每条条目执行 `cursor = align8(cursor)`、`dataOffset = cursor`、`crc32c = CRC32C(精确 dataSize 字节)`、`cursor += dataSize`；最后 `fileSize = align8(cursor)`。
7. 写头部（checksum 先写 0）、TOC、字符串表、manifest、payload；所有未使用字节写 0。
8. 计算整包 CRC-32C（checksum 字段视为 0）并回填。

确定性要求：

| 规则 | 说明 |
|---|---|
| 排序 | 条目按名称字节序；manifest 按 key 升序 |
| 无时间戳/随机数 | 文件不存储 mtime、随机数或宿主绝对路径 |
| 无权限位 | 不存储文件权限、符号链接或所有者 |
| 可复现 | 相同输入（名称、kind、数据、manifest、工具版本）产生相同字节 |
| 入口 | 有多个 module 时必须显式指定 entry；恰好一个 module 且没有 key 3 时自动写入 key 3 |
| 校验和 | 最后计算并回填；填充全部为 0 |

## 10. 读取与验证流程

### 10.1 流程

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

失败处理是 **fail-closed**：任一步失败都会清除已解析的条目与 manifest，不发布半成品，不执行模块，不写文件。

### 10.2 ARCHIVE 错误码

| 码 | 名称 | 含义 |
|---:|---|---|
| 0 | `OK` | 成功 |
| 1 | `FAIL_OPEN` | 无法打开或读取归档 |
| 2 | `EMPTY` | 输入为空 |
| 3 | `TRUNC` | 输入短于 96 字节或读取不完整 |
| 4 | `TOO_LARGE` | 超过 256 MiB |
| 5 | `BAD_HEADER` | magic/版本/flags/reserved/尺寸/对齐/固定偏移错误 |
| 6 | `BAD_CHECKSUM` | 整包 CRC-32C 失败 |
| 7 | `BAD_TOC` | TOC 字段、顺序、连续性或布局错误 |
| 8 | `BAD_NAME` | 条目名称不合法或前缀冲突 |
| 9 | `DUPLICATE` | 名称重复或 ASCII 大小写折叠碰撞 |
| 10 | `BAD_MANIFEST` | TLV、键、长度、文本或 key 3 引用错误 |
| 11 | `BAD_MODULE` | module 条目未通过 MOTEBC 解析/验证 |
| 12 | `BAD_DATA` | 条目偏移/大小/CRC/填充/区间错误 |
| 13 | `BAD_ENTRY` | 无法选择入口模块（入口缺失、歧义或引用错误） |

`Archive::parse()` 返回 0 或 2–12；1 由 `load()` 返回；13 用于入口解析语义（`resolveEntry` 报告具体原因），由调用方（如 `motear verify --strict`）转换为输入错误。

### 10.3 `motear verify` 与 `--strict`

| 调用 | 行为 |
|---|---|
| `motear verify FILE.mtr` | 完整解析归档，并验证每个 module 条目（默认 `verifyModules = true`） |
| `motear verify FILE.mtr --no-modules` | 只做容器级检查，跳过 MOTEBC 解析/验证 |
| `motear verify FILE.mtr --strict` | 在完整验证之上，要求 manifest key 1（package name）非空、key 2（package version）非空，并且入口模块可解析 |

入口模块的解析规则：

1. 若 manifest 存在 key 3，则 key 3 必须指向一个存在的 module 条目；
2. 若 key 3 不存在，则包内必须恰好有一个 module 条目；
3. 无 module，或有多个 module 且无 key 3，或 key 3 指向 resource/不存在的名称，均失败。

`--strict` 不额外要求 key 3 必须存在；若只有一个 module，省略 key 3 仍可通过。退出码：0 成功，1 用法错误，2 输入/验证错误，3 I/O 错误，4 内部错误。

## 11. 限制与出现位置

| 限制项 | 上限 | 主要出现位置 |
|---|---:|---|
| 整包文件大小 | 256 MiB | `load()` / `parse()`：`TOO_LARGE` |
| 单个 module 数据 | 64 MiB | TOC 解析：`BAD_DATA`；写入端拒绝 |
| 单个 resource 数据 | 64 MiB | TOC 解析：`BAD_DATA`；写入端拒绝 |
| 条目数 | 65535 | 头部：`BAD_HEADER` |
| 名称长度 | 1024 字节 | TOC：`BAD_TOC`；名称校验：`BAD_NAME` |
| 单个路径段 | 255 字节 | 名称校验：`BAD_NAME` |
| 字符串表 | 16 MiB | 头部：`BAD_HEADER` |
| Manifest 总大小 | 1 MiB | 头部：`BAD_HEADER`；解码器另有防御检查 |
| Manifest 单条 payload | 65535 字节 | TLV 的 u16 长度字段；编码器拒绝更大值 |
| 头部大小 | 96 字节固定 | 小于 96：`TRUNC` |
| TOC 条目大小 | 40 字节固定 | 不等于 40：`BAD_HEADER` |

完整的上限汇总见 [08-limits.md](08-limits.md)。

## 12. 版本规则

| 输入情况 | 读取器行为 |
|---|---|
| `versionMajor != 1` | 拒绝（`BAD_HEADER`） |
| `versionMinor > 0` | 拒绝（`BAD_HEADER`）；当前读取器只支持 1.0 |
| `flags != 0x00000001` | 拒绝（`BAD_HEADER`） |
| `reserved0 != 0` | 拒绝（`BAD_HEADER`） |
| 未知 TOC `kind` | 拒绝（`BAD_TOC`） |
| TOC `flags`/`reserved0`/`reserved1` 非 0 | 拒绝（`BAD_TOC`） |
| 未知 manifest key（> 8） | 接受为不透明元数据（仍需 key 升序、长度与填充合法） |
| Manifest key 0 | 拒绝（`BAD_MANIFEST`） |
| 不支持的布局/对齐/偏移 | 拒绝（`BAD_HEADER` / `BAD_TOC` / `BAD_DATA`） |

读取器不对更高 minor 做“尽力解析”；无法确认的版本直接拒绝。

## 13. 最小归档布局示例

下面是一个 544 字节的归档，包含一个 module 和一个 resource；所有偏移与 CRC 均由当前写入端生成并独立复算。

### 13.1 逻辑内容

| 条目 | kind | nameLength | dataSize | CRC-32C | dataOffset |
|---|---:|---:|---:|---|---:|
| `assets/greeting.txt` | 2（resource） | 19 | 6 | `0x353DD8BE` | 0x108 |
| `doc_mini.mbc` | 1（module） | 12 | 272 | `0xA642C27C` | 0x110 |

Manifest：

| key | payload | 编码长度 |
|---:|---|---:|
| 1 | `demo` | 8 |
| 2 | `1.0.0` | 16 |
| 3 | `doc_mini.mbc` | 16 |
| 8 | `motear 0.1.0` | 16 |

### 13.2 头部与 TOC hexdump（0x00–0xAF）

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

头部解释：

| 偏移 | 值 | 字段 | 解释 |
|---:|---|---|---|
| 0x00–0x07 | `4D 4F 54 45 41 52 43 00` | `magic` | `"MOTEARC\0"` |
| 0x08–0x09 | `01 00` | `versionMajor` | 1 |
| 0x0A–0x0B | `00 00` | `versionMinor` | 0 |
| 0x0C–0x0F | `01 00 00 00` | `flags` | `0x00000001`（HAS_MANIFEST） |
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
| 0x58–0x5B | `19 11 47 3C` | `checksum` | 小端 u32 = `0x3C471119` |
| 0x5C–0x5F | `00 00 00 00` | `reserved0` | 0 |

TOC 解释：

| 条目 | 字节范围 | 字段 | 值 |
|---:|---|---|---|
| [0] | 0x60–0x87 | `nameOffset` / `nameLength` | 0 / 19 |
| | | `kind` / `flags` / `reserved0` | 2 / 0 / 0 |
| | | `crc32c` | `0x353DD8BE` |
| | | `dataOffset` / `dataSize` / `reserved1` | 264 / 6 / 0 |
| [1] | 0x88–0xAF | `nameOffset` / `nameLength` | 19 / 12 |
| | | `kind` / `flags` / `reserved0` | 1 / 0 / 0 |
| | | `crc32c` | `0xA642C27C` |
| | | `dataOffset` / `dataSize` / `reserved1` | 272 / 272 / 0 |

### 13.3 布局区域

| 偏移范围 | 大小 | 区域 | 说明 |
|---|---:|---|---|
| 0x000–0x05F | 96 | Header | 固定头部 |
| 0x060–0x0AF | 80 | TOC | 2 × 40 |
| 0x0B0–0x0CE | 31 | 字符串表 | `"assets/greeting.txt"` + `"doc_mini.mbc"` |
| 0x0CF–0x0CF | 1 | 对齐填充 | 0 |
| 0x0D0–0x107 | 56 | Manifest | key 1、2、3、8 的 TLV |
| 0x108–0x10D | 6 | data[0] | `"hello\n"`（resource） |
| 0x10E–0x10F | 2 | 对齐填充 | 0 |
| 0x110–0x21F | 272 | data[1] | `doc_mini.mbc`（module） |
| 0x220 | 0 | 文件结束 | `fileSize = 544` |

校验：把偏移 0x58–0x5B 置零后对整个 544 字节计算 CRC-32C，结果为 `0x3C471119`，小端写回即 `19 11 47 3C`。两条 payload 的 CRC 分别覆盖精确的 6 字节与 272 字节，不含对齐填充。

## 14. 相关文档

- [02-bytecode-format.md](02-bytecode-format.md)：module 条目必须满足的 MOTEBC 1.0 格式与验证规则。
- [05-toolchain.md](05-toolchain.md)：汇编器/反汇编器与工具链。
- [08-limits.md](08-limits.md)：所有资源上限的汇总。
- [self/cli.md](self/cli.md)：`motear` 命令行用法。
