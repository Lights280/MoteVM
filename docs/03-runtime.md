# 03 · 运行时

[简体中文](03-runtime.md) | [English](en/03-runtime.md)

> **内部实现 · 不稳定 · 无 ABI 承诺。** `Host`、`Core` 和 `Memory` 是当前运行时的 C++ 接口。它们被编译进 `motevm` 可执行文件，不作为库安装，也没有公开总头文件。本文记录当前实现中可观察的行为；实现可以在不通知的情况下改变。

## 1. 范围

本文描述 `src/motevm` 中实际存在的运行时：

| 组件 | 文件 | 职责 |
|---|---|---|
| `Parse` | `src/include/parse.*` | 读取并校验 `.mbc` 容器，拥有模块字节缓冲区 |
| `PARSE::Verifier` | `src/include/verifier.*` | 校验指令流、控制流目标和 FUNCS/CODE 交叉表 |
| `Host` | `src/motevm/host.*` | 模块注册、导入绑定、常量播种、运行编排 |
| `Core` | `src/motevm/core.*` | 一次执行：帧、pc、指令、燃料与钩子 |
| `Memory` | `src/motevm/memory.*` | 4 GiB 线性地址空间和 64 KiB 块 |
| `Workspace` | `src/motevm/workspace.*` | 仅 CLI 使用的路径沙箱，`Host` 不感知 |
| `DEBUG` | `src/motevm/debug.*` | 断点、单步、trace 和 REPL，只读访问 `Core` |

链接事实：

- 运行时源文件没有独立的库目标；它们被编译进 `motevm` 可执行文件。
- `motevm_core` 是静态库，包含 `Parse`、`Verifier`、`Disasm`（源文件在 `src/include`）。
- `motevm_tools` 是静态库，包含基于 `motevm_core` 的汇编/反汇编工具链。
- 没有公开总头文件；直接使用时包含具体头文件（见 [04 · 嵌入 API](04-embedding-api.md)）。

本文只描述当前实现。嵌入、链接和示例见 [04 · 嵌入 API](04-embedding-api.md)。

## 2. 加载 → 验证 → 执行

### 2.1 流水线

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

各角色的边界：

| 阶段 | 失败通道 | 说明 |
|---|---|---|
| `Parse::load` / `Parse::parse` | `PARSE` 1..7 | 文件、头部、CRC、段表、段内容或指令规则错误 |
| `Host::load` 的导入绑定 | `HOST::UNLINKED_IMPORT` (102) | 没有任何已注册函数匹配 `(name, arity)` |
| `Host::initMemory` | `HOST::NO_MODULE` (100) 或 `MEMORY` 1..3 | 常量分配或地址空间溢出 |
| `Host::runModule` | `CORE` 1..16、`HOST::NO_MODULE` (100)、`HOST::NO_MEMORY` (101) | 执行期错误或调用顺序错误 |
| 运行成功 | `CORE::OK` (0) | `halt`、根帧 `ret` 或正常结束 |

规则：

- `Host::load` 可以调用多次。每次成功加载追加一个模块；失败的模块不会加入 `Host`。
- 必须在所有 `load()` 之后、第一次 `runModule()` 之前调用 `initMemory()`。`initMemory()` 每次都会新建 `Memory` 并从头播种，不会为之后加载的模块补种常量。
- `Parse::parse()` 内部运行 `PARSE::Verifier`；因此 `Host` 交给 `Core` 的模块总是已验证。直接使用 `Core` 的调用者自行负责这一点（见 §4.2）。
- `Host::run()` 按加载顺序运行每个模块的入口。CLI 只运行入口模块，因此使用 `runModule(0)`。

### 2.2 Host 与 Core 的关系

- 一个 `Host` 拥有：模块 `Parse` 对象及其路径、每个模块的常量地址表、每个模块的 `CORE::HostSlot` 绑定表、一个 `Memory`、以及最近一次执行的 `Core`（`lastCore`）。
- 每次 `Host::runModule()` 都新建一个 `Core`，并替换 `lastCore`。一次运行对应一个 `Core`；旧的 `Core` 在执行下一次 `runModule()` 时被销毁。
- `Core` 不拥有任何东西。它借用一个 `Memory`、CODE 字节区间、FUNCS 表区间和宿主槽位表区间；这些对象的生命周期必须覆盖 `Core`。
- 执行是同步的：`Core::run()` 返回时，运行已经结束，最终状态（`error()`、`errorPc()`、`instructionCount()`、`stack()` 等）可以读取。
- 直接嵌入可以绕过 `Host`：用 `Memory` 构造 `Core`，再调用 `attachModule()`/`attachVerifiedModule()`、`attachImports()` 和 `run()`。这要求调用者自己完成 `Parse`、验证、导入绑定和常量播种。

## 3. 内存

### 3.1 地址空间与块

| 常量 | 值 | 含义 |
|---|---|---|
| `MEMORY::BLOCK_SIZE` | 65536 | 每块 64 KiB |
| `MEMORY::BLOCK_COUNT` | 65536 | 块数 |
| `MEMORY::ADDRESS_SPACE` | 4 GiB | `BLOCK_COUNT * BLOCK_SIZE` |

- `Memory` 构造时不分配任何块。块在 `Memory::allocate(index)` 时按需分配，在 `free(index)` 后回到未映射状态。
- 块状态为 `RELEASED`（未映射）或 `FREE`（已分配）。对已分配的块再次调用 `allocate` 是幂等的，返回 `MEMORY::OK`。
- 4 字节访问以 64 位计算 `base + displacement`，因此 32 位寄存器加偏移不会回绕并别名到低地址。地址大于 `ADDRESS_SPACE - 4` 时返回 `MEMORY_OVERFLOW`。
- 一次 4 字节访问可以跨块边界，但涉及的两个块都必须已分配；否则访问失败。
- 未映射地址上的 load/store 不会自动分配块。

### 3.2 常量播种

`Host::initMemory()` 的当前行为：

1. 新建一个空的 `Memory`。
2. 从地址 0 开始，按模块加载顺序、模块内常量表顺序遍历常量。
3. 每个常量写入前把游标向上对齐到 8 字节。
4. 为零长度常量保留 4 字节（游标前进 4）；非零常量写入其 payload 后前进 `payload.size()` 字节。
5. 为每个被触及的 64 KiB 块调用 `Memory::allocate()`。
6. 记录每个常量的 32 位地址，可通过 `Host::constantAddress(module, index)` 取回；模块或索引越界时返回 `NO_INDEX`（`0xFFFFFFFF`）。

常量地址从 0 开始且顺序固定，因此同一组模块得到相同的常量地址。`Host::stats().constantBytes` 是最终的游标值（常量池占用的字节数）。

### 3.3 访问与故障

`Core` 的 `load`/`sto` 指令调用 `Memory::readAt`/`writeAt`。任何非 `MEMORY::OK` 结果都被映射为 `CORE::MEMORY_FAULT`：

| 情况 | `Memory` 返回 | `Core` 结果 |
|---|---|---|
| 块未分配/已释放 | `MEMORY_UNAVAILABLE` | `MEMORY_FAULT` |
| 地址超出 4 GiB 边界 | `MEMORY_OVERFLOW` | `MEMORY_FAULT` |
| 访问成功 | `OK` | 无错误 |

`MEMORY_FAULT` 是运行期错误：触发它的指令计入执行数，`errorPc()` 指向该指令，`currentPc()` 指向下一条指令。

## 4. 执行模型

### 4.1 CODE 与 pc

- CODE 段是字节序列，长度必须是 4 的倍数；每条指令 4 字节。
- `pc` 是 CODE 内的**字节偏移**，不是指令序号。
- FUNCS 表中的 `entry` 字段是**指令序号**；函数入口地址 = `entry * 4`。
- 寄存器为 256 个 32 位原始值 `r0..r255`。浮点指令按 IEEE-754 binary32 位模式解释寄存器内容。
- `Core::textBytes()` 返回 CODE 字节数；`Core::codeWord(byteAddress)` 返回该字节地址处的 32 位指令字，地址越界时返回 0。

### 4.2 已验证与未验证 attach

| 入口 | 前提 | 边界检查 |
|---|---|---|
| `Core::attachModule(text, functions)` | 无；适用于手工构造或未验证的字节码 | 保守：每次取指、每个计算出的控制流目标都检查范围和 4 字节对齐 |
| `Core::attachVerifiedModule(text, functions)` | 字节码已通过 `PARSE::Verifier` | 非守卫快速循环可以依赖验证器保证，省略逐指令边界检查 |
| `Core::attachVerifiedModule(text, functions, copyExtents)` | 同上，并复用验证器算出的寄存器复制范围 | 同上 |

可观察差异：

- 保守模式在运行期把越界或未对齐的目标报告为 `CORE::INVALID_TARGET`，因此不会执行非法指令。
- 已验证模式在被直接用于未验证字节码时不做这项保证；非法控制流的行为是未定义的。
- 启用燃料、中断钩子或调试钩子时，执行始终走守卫循环，守卫循环保留边界检查，即使模块是已验证的。上述差异只出现在未启用任何守卫的快速路径上。
- attach 失败：空 CODE 或长度不是 4 的倍数返回 `CORE::INVALID_TEXT`；函数入口落在 CODE 之外返回 `CORE::INVALID_TARGET`。

`Core::attachImports(span<const CORE::HostSlot>)` 绑定导入槽位表。`Core` 只借用该区间，调用者必须保证它在 `Core` 生命期内有效。

### 4.3 调用、帧与返回

- 每个栈帧有 256 个 32 位寄存器。`Core::run()` 在入口地址创建根帧，根帧寄存器全部清零。
- `call rA, #index`：
  - `index` 是 FUNCS 索引；越界返回 `CORE::INVALID_FUNCTION`。
  - 目标地址 = `functions[index].entry * 4`；目标必须在 CODE 内，否则返回 `CORE::INVALID_TARGET`。
  - 压入新帧，保存返回地址（调用后的下一条指令）和返回寄存器 `A`。
  - 把调用者寄存器窗口的一个前缀复制到新帧；随后 `pc` 跳到目标。
- 参数传递：调用者在 `call` 前把参数放在自己的 `r0..r(arity-1)`。被调用者看到的 `r0..arity-1` 就是这些值。
- 复制范围（copy extent）：被调用者入口处，新帧的前 N 个寄存器等于调用者的前 N 个寄存器。N 至少覆盖被调用者声明的 `regcnt`，并至少覆盖它能静态到达的函数的 `regcnt`，上限为 256。**可观察规则**：当前函数只能读取自己 `r0..regcnt-1` 范围内的寄存器；高于复制前缀的槽位没有定义。不要依赖声明范围之外的寄存器。
- `ret rX`：弹出当前帧，把 `rX` 写入调用者保存的返回寄存器，`pc` 回到返回地址。
- 根帧执行 `ret` 表示正常结束：没有调用者，返回值被丢弃，`Core::error()` 保持 `CORE::OK`。`halt` 也正常结束。
- 调用深度上限为 1024，**包含根帧**。在已有 1024 帧时再 `call` 返回 `CORE::STACK_OVERFLOW`，该 `call` 不执行。`peakCallDepth()` 报告含根帧的峰值，最小为 1。

### 4.4 燃料

- `Core::setFuel(maxInstructions)` 设置本轮运行的指令预算；0 表示不限制。
- 预算在每次 `Core::run()` 开始时重置为设定值；`Host::setFuel()` 会把值转发给它创建的每个 `Core`。
- 检查发生在**下一条指令执行之前**。预算为 N 时最多可执行 N 条指令；第 N+1 条指令被拒绝，返回 `CORE::FUEL_EXHAUSTED`，该指令未执行。
- `FUEL_EXHAUSTED` 是边界停止，不是指令故障：`errorPc()` 不指向该指令（保持运行开始时的值 0）；使用 `currentPc()` 读取停止位置。

### 4.5 中断钩子

- `Core::setInterruptHook(bool (*hook)(void *), void *user)` 设置钩子；`Host::setInterruptHook()` 转发到每次运行。
- 钩子在每条指令执行前调用，顺序在燃料检查之后、取指和边界检查之前。
- 返回 `true` 表示停止：当前指令不执行，`Core::error()` 为 `CORE::INTERRUPTED`，`currentPc()` 指向该指令。
- `INTERRUPTED` 是边界停止：`errorPc()` 保持 0；使用 `currentPc()`。
- 钩子在 VM 线程内同步调用，必须快速返回；不得重入 `Core`（不得调用 `run`/`step`）。

### 4.6 调试钩子

- 类型：`bool (*)(void *user, const Core &core, dword pc, dword word)`。
- 通过 `Core::setDebugHook(hook, user)` 或 `Host::runModule(index, hook, user)` 安装。
- 调用时机：每条指令取指之后、执行之前。`pc` 指向尚未执行的指令，`word` 是该指令的原始 32 位编码。
- 返回 `true` 表示停止：该指令不执行，`Core::error()` 为 `CORE::DEBUG_BREAK`，`errorPc()` 和 `currentPc()` 都指向该指令。
- 只读契约：钩子可以通过 `Core` 的只读访问器读取状态（`stack()`、`codeWord()`、`errorPc()` 等），但**不得重入** `Core`（不得调用 `run`/`step`），也不得依赖内部字段。
- `Host::runModule(index, hook, user)` 在运行结束后清除调试钩子，不保留 `user` 指针。
- 调试钩子与燃料、中断钩子可以同时启用；检查顺序为燃料 → 指令计数 → 中断 → 边界 → 取指 → 调试。

## 5. 确定性与可观察状态

在同一模块、相同内存初始内容、相同宿主函数行为的前提下，运行时保证：

- 相同的指令执行序列、最终寄存器/内存状态、指令数、峰值调用深度、错误码和停止位置。
- 常量地址从 0 开始按固定顺序分配，不依赖主机指针或地址空间布局。
- 统计信息不含时间戳或主机地址。

可能变化的输入/输出：

- 宿主函数的返回值、I/O、时间和随机数；它们由嵌入方实现，不属于 VM 的确定性契约。
- 调试器从 stdin 读取的命令、断点集合、trace 目标。
- 中断钩子的决策。

## 6. 错误与状态模型

所有 API 用返回码报告常规错误，不使用 C++ 异常。只有内存分配失败（`std::bad_alloc`）可以从分配点逃逸；CLI 将该情况归为内部错误。

### 6.1 CORE 代码

| 值 | 名称 | 含义 |
|---|---|---|
| 0 | `OK` | 无错误 |
| 1 | `UNKNOWN_INSTRUCTION` | 保留/未知 opcode |
| 2 | `DIVIDED_BY_ZERO` | 整数除零或模零 |
| 3 | `INTEGER_OVERFLOW` | `INT32_MIN / -1` 或 `-INT32_MIN` |
| 4 | `STACK_OVERFLOW` | 调用深度超过 1024（含根帧） |
| 5 | `INVALID_TARGET` | pc 或函数入口落在 CODE 之外，或控制流目标未对齐 |
| 6 | `INVALID_TEXT` | CODE 为空或长度不是 4 的倍数 |
| 7 | `NO_ACTIVE_FRAME` | 没有活动帧时执行 `step`/save/take |
| 8 | `NO_CALLER` | 根帧 `ret` 的内部正常结束信号 |
| 9 | `NUMERIC_CONVERSION` | `f2i` 的 NaN/Inf/越界 |
| 10 | `INVALID_FUNCTION` | FUNCS 索引越界 |
| 11 | `INVALID_IMPORT` | IMPORTS 索引越界或槽位未绑定 |
| 12 | `FUEL_EXHAUSTED` | 指令预算耗尽 |
| 13 | `INTERRUPTED` | 中断钩子请求停止 |
| 14 | `TRAP` | `trap` 指令触发 |
| 15 | `MEMORY_FAULT` | load/store 失败 |
| 16 | `DEBUG_BREAK` | 调试钩子在指令执行前请求停止 |

`NO_CALLER` (8) 是内部信号；`Core::run()` 把根帧 `ret` 转换为正常结束，`error()` 仍为 `OK`。`DEBUG_BREAK` (16) 表示调试暂停，不是故障。

### 6.2 HOST 代码

| 值 | 名称 | 含义 |
|---|---|---|
| 0 | `OK` | 成功 |
| 100 | `NO_MODULE` | 没有已加载模块，或模块索引越界 |
| 101 | `NO_MEMORY` | 尚未调用 `initMemory()` |
| 102 | `UNLINKED_IMPORT` | 加载期没有匹配 `(name, arity)` 的宿主函数 |
| 103 | `BAD_ARGUMENT` | 非法注册请求 |

### 6.3 PARSE 代码

| 值 | 名称 | 含义 |
|---|---|---|
| 0 | `OK` | 成功 |
| 1 | `FAIL_OPEN` | 打开/读取文件失败 |
| 2 | `EMPTY` | 没有输入 |
| 3 | `TRUNC` | 文件过短 |
| 4 | `TOO_LARGE` | 超过模块大小上限 |
| 5 | `BAD_FILE` | 头部、CRC 或段表错误 |
| 6 | `BAD_CONTENT` | 段内容错误 |
| 7 | `BAD_CODE` | 指令规则错误（验证器） |

### 6.4 MEMORY 代码

| 值 | 名称 | 含义 |
|---|---|---|
| 0 | `OK` | 成功 |
| 1 | （保留，未使用） | — |
| 2 | `MEMORY_UNAVAILABLE` | 块未分配、范围非法或缓冲区太小 |
| 3 | `MEMORY_OVERFLOW` | 地址运算超出 4 GiB 地址空间（含跨块/缓冲区越界） |

### 6.5 状态访问器

| 方法 | 返回 |
|---|---|
| `Core::error()` | 最近一次运行的错误码（`CORE::OK` 表示成功）；attach 失败由 attach 方法的返回值报告 |
| `Core::errorPc()` | 报告 `error()` 的指令地址；边界停止（`FUEL_EXHAUSTED`、`INTERRUPTED`）不设置它，保持 0 |
| `Core::trapCode()` | 最近一次 `TRAP` 的 24 位立即数 |
| `Core::instructionCount()` | 最近一次运行计入的指令数（在每次 `run()` 开始时重置） |
| `Core::peakCallDepth()` | 最近一次运行的峰值调用深度，含根帧 |
| `Core::stack()` | 只读帧区间；`frames[0]` 是根帧，`back()` 是活动帧 |
| `Core::currentPc()` | 当前/停止时的 pc |
| `Host::error()` | 最近一次由 Host 设置的错误码；判断单次调用是否成功应使用该调用的返回值 |
| `Host::stats()` | 累计运行数、指令数、峰值深度、最后 pc、trap 码和结果字段 |

`errorPc()` 的当前语义：

| 停止原因 | 出错指令是否执行 | `errorPc()` |
|---|---|---|
| 算术错误、`TRAP`、`MEMORY_FAULT`、`INVALID_FUNCTION`、`INVALID_IMPORT`、`STACK_OVERFLOW`、`NUMERIC_CONVERSION` | 否（但已计入执行数） | 该指令地址 |
| `INVALID_TARGET`（取指越界） | 否 | 非法目标地址 |
| `INVALID_TARGET`（跳转偏移未对齐） | 否 | 计算该目标的跳转指令 |
| `DEBUG_BREAK` | 否 | 尚未执行的指令 |
| `FUEL_EXHAUSTED`、`INTERRUPTED` | 否 | 保持 0；使用 `currentPc()` |
| 正常结束（根帧 `ret` / `halt`） | — | 最后执行的指令 |

## 7. 线程模型

- `Host`、`Core`、`Memory` 和 `Parse` 都不是线程安全的，内部没有互斥锁。
- 一个 VM 实例只能由一个线程使用；同一线程内的钩子和宿主函数都在执行线程上同步调用。
- 不要在线程之间共享 `Memory`、`Core` 或 `Host`，也不要在宿主函数中并发访问这些对象。
- 多个线程可以各自创建独立的 `Host` 实例并行运行；它们之间没有共享的运行时全局状态。

## 8. 相关文档

- 嵌入、链接、所有权与宿主函数：[04 · 嵌入 API](04-embedding-api.md)
- 指令集：[01 · ISA](01-isa.md)
- `.mbc` 容器与段布局：[02 · 字节码格式](02-bytecode-format.md)
- 限制与上限：[08 · 限制](08-limits.md)
- CLI 执行流水线和退出码、调试器用法：[`self/cli.md`](self/cli.md)
- 汇编示例：[`examples/hello.mta`](examples/hello.mta)
