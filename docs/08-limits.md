# 08 · 限制

[简体中文](08-limits.md) | [English](en/08-limits.md)

**注意**：下表数值来自当前源码常量，不是格式规范中的永久承诺，可能随实现调整。格式背景见 [02 字节码格式](02-bytecode-format.md) 与 [07 MTR 格式](07-mtr-format.md)；宿主函数错误码见 [05 工具链](05-toolchain.md) §8.7。

## 1. ISA

| 项目 | 值 | 源常量 | 超限/边界行为 |
| --- | --- | --- | --- |
| 寄存器数量 | 256 | `ISA::REGISTER_COUNT` | 函数的 `regcnt` 大于 256 时模块加载失败（`BAD_CONTENT`），工具退出码 2 |
| 指令大小 | 4 B | `ISA::INSTRUCTION_SIZE` | CODE 长度必须是 4 的倍数，且指令条数为偶数（8 B 对齐）；否则 `BAD_CONTENT`，退出码 2 |
| 数据字宽度 | 32 位 | `dword`（`unsigned int`） | 寄存器与整数运算按 32 位存放；溢出结果被截断/回绕 |

## 2. 核心

| 项目 | 值 | 源常量 | 超限行为 |
| --- | --- | --- | --- |
| 最大调用深度 | 1024（含根帧） | `CORE::MAX_CALL_DEPTH` | 继续 `CALL` 返回 `CORE::STACK_OVERFLOW`；`motevm` 以退出码 3 结束 |

## 3. 内存

| 项目 | 值 | 源常量 | 超限/边界行为 |
| --- | --- | --- | --- |
| 块大小 | 65536 B（64 KiB） | `MEMORY::BLOCK_SIZE` | — |
| 块数量 | 65536 | `MEMORY::BLOCK_COUNT` | 块索引超出 0..65535 的访问失败 |
| 地址空间 | 4 GiB | `MEMORY::ADDRESS_SPACE` = `BLOCK_COUNT * BLOCK_SIZE` | 地址越过 `ADDRESS_SPACE - 4` 时返回 `MEMORY_OVERFLOW`，核心报告 `CORE::MEMORY_FAULT`，`motevm` 退出码 3 |
| 未分配块 | — | `MEMORY::MEMORY_UNAVAILABLE` | 访问未分配块返回该错误，核心报告 `CORE::MEMORY_FAULT`，退出码 3 |
| 重复分配 | — | `MEMORY::BLOCK_OCCUPIED` | 分配已占用块失败；初始化阶段失败时 `motevm` 退出码 4 |

## 4. 模块（MOTEBC 1.0）

### 4.1 容量限制

| 项目 | 值 | 源常量 | 超限行为 |
| --- | --- | --- | --- |
| 模块文件大小 | 64 MiB | `PARSE::MAX_MODULE_BYTES` | 加载返回 `TOO_LARGE`，工具退出码 2 |
| 常量/函数/全局/导入/调试局部数量 | 65536 | `PARSE::MAX_STATIC_COUNT` | 解析返回 `BAD_CONTENT`，退出码 2 |
| 字符串常量长度 | 16 MiB | `PARSE::MAX_STRING_BYTES` | 解析返回 `BAD_CONTENT`，退出码 2 |
| 函数寄存器数 | 1..256 | `PARSE::Function::regcnt`、`ISA::REGISTER_COUNT` | 越界返回 `BAD_CONTENT`，退出码 2 |
| 函数元数 | ≤16 且 ≤`regcnt` | `PARSE::Function::arity` | 越界返回 `BAD_CONTENT`，退出码 2 |
| 导入元数 | ≤16 | `PARSE::Import::arity` | 越界返回 `BAD_CONTENT`，退出码 2 |
| 节数量 | ≥6，字段上限 65535 | `PARSE::Header::seccnt`（`word`） | 小于 6 或节表越过文件末尾时返回 `BAD_FILE`，退出码 2 |
| CODE 指令数 | 偶数条 | `parse.cpp` 的 8 B 对齐要求 | 不满足时返回 `BAD_CONTENT`，退出码 2 |

模块加载/校验错误中，`TRUNC`、`TOO_LARGE`、`BAD_FILE`、`BAD_CONTENT`、`BAD_CODE` 都使 `moteasm`、`motedis`、`motevm`、`motear` 以退出码 2 结束（`motevm` 的加载阶段）。

### 4.2 头部与结构尺寸

| 结构 | 大小 | 源常量 |
| --- | --- | --- |
| 文件头 | 32 B | `PARSE::HEADER_SIZE` / `sizeof(PARSE::Header)` |
| 节表项 | 24 B | `PARSE::SECBLOCK_SIZE` / `sizeof(PARSE::SecBlock)` |
| CONST 头 | 8 B | `sizeof(PARSE::ConstHeader)` |
| CONST 条目 | 8 B + `align8(length)` | `sizeof(PARSE::ConstEntry)` |
| FUNCS 头 | 4 B | `sizeof(PARSE::FuncHeader)` |
| FUNCS 条目 | 32 B | `sizeof(PARSE::Function)` |
| GLOBALS 头 | 4 B | `sizeof(PARSE::GlobalHeader)` |
| GLOBALS 条目 | 12 B | `sizeof(PARSE::Global)` |
| IMPORTS 头 | 4 B | `sizeof(PARSE::ImportHeader)` |
| IMPORTS 条目 | 8 B | `sizeof(PARSE::Import)` |
| DEBUG 头 | 8 B | `sizeof(PARSE::DebugHeader)` |
| DEBUG 块 | 16 B | `sizeof(PARSE::DebugBlock)` |
| DEBUG 行条目 | 12 B | `sizeof(PARSE::LineEntry)` |
| DEBUG 局部变量条目 | 16 B | `sizeof(PARSE::LocalEntry)` |

文件短于 32 B 的头部返回 `TRUNC`；头部字段、CRC、节表或版本不符合 MOTEBC 1.0 时返回 `BAD_FILE`。

## 5. 归档（MTR 1.0）

### 5.1 容量限制

| 项目 | 值 | 源常量 | 超限行为 |
| --- | --- | --- | --- |
| 归档文件大小 | 256 MiB | `ARCHIVE::MAX_ARCHIVE_BYTES` | 加载/解析返回 `TOO_LARGE`；`motear` 退出码 2 |
| 单条目数据大小 | 64 MiB | `ARCHIVE::MAX_ENTRY_BYTES` | 写入被拒绝；读取返回 `BAD_DATA`；退出码 2 |
| 字符串表大小 | 16 MiB | `ARCHIVE::MAX_STRING_BYTES` | 解析返回 `BAD_HEADER`，退出码 2 |
| 清单大小 | 1 MiB | `ARCHIVE::MAX_MANIFEST_BYTES` | 解析返回 `BAD_HEADER`/`BAD_MANIFEST`，退出码 2 |
| 条目数量 | 65535 | `ARCHIVE::MAX_ENTRY_COUNT` | 解析返回 `BAD_HEADER`；写入被拒绝；退出码 2 |
| 条目名长度 | 1024 B | `ARCHIVE::MAX_NAME_BYTES` | 解析返回 `BAD_TOC`/`BAD_NAME`；写入被拒绝；退出码 2 |
| 路径段长度 | 255 B | `ARCHIVE::MAX_SEGMENT_BYTES` | 名称校验失败（`BAD_NAME`）；写入被拒绝；退出码 2 |
| 文件头 | 96 B | `ARCHIVE::HEADER_SIZE` | 短于 96 B 返回 `TRUNC`，退出码 2 |
| TOC 条目 | 40 B | `ARCHIVE::TOC_ENTRY_SIZE` | `toc_entry_size` 不等于 40 时返回 `BAD_HEADER`，退出码 2 |

归档解析默认是 fail-closed：任一字段、布局、清单或名称不合法都会拒绝整个归档；内嵌模块默认也参与校验（`motear verify --no-modules` 可关闭）。

### 5.2 清单字段长度

| 清单键 | 长度范围 | 来源 |
| --- | --- | --- |
| 包名（key 1） | 1..255 B | `manifest.cpp` |
| 包版本（key 2） | 1..64 B | `manifest.cpp` |
| 入口模块（key 3） | 1..1024 B | `manifest.cpp` |
| 描述（key 4） | 0..4096 B | `manifest.cpp` |
| 作者/许可证/构建 ID/创建者 | 0..255 B | `manifest.cpp` |
| 任意清单值负载 | ≤65535 B | `manifest.cpp` |
| 整个清单 | ≤1 MiB | `ARCHIVE::MAX_MANIFEST_BYTES` |

超出范围的值使清单编码或解码失败，进而使归档写入或解析失败；`motear` 退出码 2。

## 6. CLI 内置宿主 I/O

以下限制来自 `src/motevm/cli.cpp` 的过渡宿主 ABI。宿主函数不触发 VM 陷阱，而是返回负的 `dword` 结果码（转换为无符号值传给脚本）。错误码含义见 [05 工具链](05-toolchain.md) §8.7。

| 项目 | 值 | 源常量 | 超限行为 |
| --- | --- | --- | --- |
| 宿主路径长度 | 4096 B | `MAX_HOST_PATH_BYTES` | 长度为 0 或超过 4096 时返回 `-5`（`HOST_BAD_ARGUMENT`） |
| 单次文件 I/O 大小 | 16 MiB | `MAX_HOST_IO_BYTES` | `file.read`/`file.write`/`file.append` 返回 `-5` |
| 堆分配上限 | 256 MiB | `MAX_HOST_HEAP_BYTES` | `mem_alloc` 返回 `NO_INDEX`（`0xFFFFFFFF`） |
| 宿主地址上限 | 4 GiB | `HOST_ADDRESS_LIMIT` | 分配区间越过该地址返回 `NO_INDEX` |
| 文件大小返回值 | 32 位 | `file.size` 的 `0xFFFFFFFF` 检查 | 文件大于 4 GiB 时返回 `-3`（`HOST_SYSTEM_ERROR`） |
| 诊断存储条数 | 默认 20；错误与警告各自计数，`0` 表示不限制（存储上限 1000） | `TOOL::Diagnostics::limit`、`cap()` | 超过存储上限的诊断不再存储，但错误/警告计数继续累加；`moteasm --max-errors N` 可调整存储上限，`N=0` 时使用 1000 条上限 |

宿主错误码：`-3` 系统错误（打开/读写/删除失败）、`-4` 工作区拒绝、`-5` 参数或范围错误。`mem_free` 为无操作，内存只在进程退出时释放。
