# 04 · 嵌入 API

[简体中文](04-embedding-api.md) | [English](en/04-embedding-api.md)

> **内部接口 · 不稳定 · 无 ABI 承诺。**
>
> `Host`、`Core`、`Memory` 和 `Parse` 是当前实现内部的 C++ 接口，不是公开 API：没有总头文件，没有作为库安装，不承诺源码或二进制兼容。接口、布局和行为都可能在不通知的情况下改变。本文描述当前代码中可直接使用的表面；运行语义见 [03 · 运行时](03-runtime.md)。

## 1. 现状与范围

- 运行时源文件位于 `src/motevm/*.cpp`，被编译进 `motevm` 可执行文件；构建系统没有“运行时库”目标。
- `motevm_core` 是静态库，包含 `Parse`、`Verifier`、`Disasm`（源文件在 `src/include`）。
- `motevm_tools` 是静态库，包含汇编/反汇编工具链，依赖 `motevm_core`。
- 嵌入方直接包含 `src/motevm/include` 和 `src/include` 下的头文件，并把运行时源文件编译进自己的目标。
- 没有 `VM`、`Value`、`Result`、`RootedValue`，没有 GC/根集，也没有安装规则意义上的公开 API（见 §11）。

## 2. 包含与链接

### 2.1 头文件与源文件

| 路径 | 内容 | 是否必需 |
|---|---|---|
| `src/motevm/include/host.hpp` | `Host`、`HOST` 错误码和注册表 | 使用 `Host` 时必需 |
| `src/motevm/include/core.hpp` | `Core`、`CORE` 错误码、`StackFrame`、`HostSlot` | 必需 |
| `src/motevm/include/memory.hpp` | `Memory` 和块常量 | 必需 |
| `src/motevm/include/workspace.hpp` | `Workspace` 路径沙箱 | 可选（CLI 侧） |
| `src/motevm/include/debug.hpp` | 调试器/trace 支持 | 可选 |
| `src/include/parse.hpp` | `Parse`、`PARSE` 错误码、模块镜像 | 由 `motevm_core` 提供 |
| `src/include/verifier.hpp` | `PARSE::Verifier` | 由 `motevm_core` 提供 |
| `src/include/disasm.hpp` | 指令文本格式化 | 由 `motevm_core` 提供 |

必须编译的源文件：

| 源文件 | 依赖 |
|---|---|
| `src/motevm/core.cpp` | `parse.hpp`、`verifier.hpp`、`memory.hpp`、`define.hpp` |
| `src/motevm/host.cpp` | `parse.hpp`、`core.hpp`、`memory.hpp` |
| `src/motevm/memory.cpp` | `memory.hpp`、`define.hpp` |
| `src/motevm/workspace.cpp` | 可选；只依赖标准库 |
| `src/motevm/debug.cpp` | 可选；依赖 `disasm.hpp` 和 `parse.hpp` |

`Parse`、`Verifier`、`Disasm` 的实现由 `motevm_core` 提供（`src/include/parse.cpp`、`verifier.cpp`、`disasm.cpp`）。不要只链接 `motevm_core` 而漏掉 `host.cpp`/`core.cpp`/`memory.cpp`：运行时不在库里。

### 2.2 CMake

在 MoteVM 项目内（或通过 `add_subdirectory` 引入后）可以这样构建宿主：

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

### 2.3 直接编译

在仓库根目录下，等价的命令行构建：

```sh
clang++ -std=c++20 -O2 \
  -I src/motevm/include -I src/include \
  myhost.cpp \
  src/motevm/core.cpp src/motevm/host.cpp src/motevm/memory.cpp \
  src/include/parse.cpp src/include/verifier.cpp src/include/disasm.cpp \
  -o myhost
```

需要 `Workspace` 时追加 `src/motevm/workspace.cpp`；需要 `DEBUG` 时追加 `src/motevm/debug.cpp`（它使用已链接的 `disasm.cpp`）。

## 3. 最小示例

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

使用顺序：

1. 注册所有宿主函数。导入在 `load()` 时解析，之后注册不会补绑（见 §7.5）。
2. 加载所有模块。`Host::load` 可以多次调用；失败的模块不会加入 `Host`。
3. 调用一次 `Host::initMemory()`，创建内存并播种常量。必须在所有 `load()` 之后调用。
4. 调用 `Host::runModule(moduleIndex)`。索引是**模块索引**，不是函数索引；每个模块执行其头部记录的入口函数。
5. 用返回值或 `Host::error()` 判断结果；通过 `Host::core()`（最近一次运行的 `Core`）读取停止位置和栈。

`Host::core()` 在第一次成功创建 `Core` 之前为 `nullptr`；返回的指针在下一次 `runModule()` 或 `Host` 析构后失效。

## 4. 生命周期与所有权

### 4.1 所有权表

| 对象 | 拥有 | 借用 | 约束 |
|---|---|---|---|
| `Host` | 模块 `Parse` 列表、路径、常量地址表、每模块 `HostSlot` 表、`Memory`、最近的 `Core` | — | 不可拷贝；必须比它创建的 `Core` 和借出的区间活得更久 |
| `Parse` | 模块字节缓冲区（镜像 span 指向其中） | — | 拷贝被删除、可移动；由 `Host` 拥有；必须比使用它的 `Core` 活得更久 |
| `Core` | 调用栈、copy extent、调用表 | `Memory` 指针、CODE span、FUNCS span、`HostSlot` span | 每次运行一个；不可拷贝；借用的对象必须比它活得更久 |
| `Memory` | 块表和块分配 | — | 不可拷贝、不可移动；`Host` 用 `unique_ptr` 持有；一次 `initMemory()` 一个 |
| `HostSlot` 表 | — | 函数指针和 `user` 指针 | 由 `Host` 拥有；加载时绑定，`Host` 析构前保持有效 |

### 4.2 规则

- `Host` 显式删除了拷贝构造和拷贝赋值。
- `Parse` 拥有模块字节，镜像中的 CODE/FUNCS/常量 span 指向该缓冲区。`Core` 只借用这些 span，因此不能让 `Parse` 在 `Core` 运行期间销毁。
- `Host` 在内部保存 `std::unique_ptr<Memory>` 和 `std::unique_ptr<Core>`；模块没有卸载 API，已加载模块一直存活到 `Host` 析构。
- `Memory` 显式删除了拷贝和移动操作。复制块表会产生两个所有者，使 `allocate()`/`free()` 互相使对方的缓存失效。
- `Core` 的拷贝构造和拷贝赋值已显式删除；它借用的 `Memory`/CODE/槽位表不能被两个实例共享。
- 直接嵌入时，`Core::attachImports` 的 `std::span<const CORE::HostSlot>` 必须在整个运行期间有效；不要把临时 `std::vector` 的 span 传进去。
- `Host::module(index)` 和 `Host::path(index)` 返回内部对象的引用；它们只在 `Host` 存活期间有效。

## 5. 错误模型

### 5.1 返回码

所有常规错误通过 `int` 返回码报告，不使用 C++ 异常；只有在分配点出现的 `std::bad_alloc` 可以逃逸。

| 操作 | 成功 | 失败 |
|---|---|---|
| `Host::registerFunction` | `HOST::OK` | `HOST::BAD_ARGUMENT` (103) |
| `Host::load` | `HOST::OK` | `PARSE` 1..7、`HOST::UNLINKED_IMPORT` (102) |
| `Host::initMemory` | `HOST::OK` | `HOST::NO_MODULE` (100)、`MEMORY` 1..3 |
| `Host::runModule` / `Host::run` | `CORE::OK` | `CORE` 1..16、`HOST::NO_MODULE` (100)、`HOST::NO_MEMORY` (101) |
| `Memory::readAt` / `writeAt` / `reads` / `writes` | `MEMORY::OK` | `MEMORY` 1..3 |

完整代码表见 [03 · 运行时 §6](03-runtime.md#6-错误与状态模型)。`Host::error()` 保存最近一次由 `Host` 设置的错误码；判断单次调用是否成功应使用该调用的返回值。

`CORE::NO_CALLER` (8) 是内部正常结束信号，不会作为失败返回；根帧 `ret` 后 `error()` 为 `CORE::OK`。

### 5.2 CLI 退出码映射

以下是 `motevm` CLI 的映射，不属于 `Host` API 本身：

| 退出码 | 触发场景 |
|---|---|
| 0 | 加载、验证、执行全部成功；或调试器在 `CORE::DEBUG_BREAK` 处执行 `quit` |
| 1 | 命令行用法错误 |
| 2 | 输入/加载失败：`PARSE` 1..7、`HOST::UNLINKED_IMPORT`、工作区或路径错误、断点/trace 文件错误 |
| 3 | 运行期故障：`CORE` 1..16（`DEBUG_BREAK` 且用户 `abort` 时）；`TRAP`、`FUEL_EXHAUSTED`、`INTERRUPTED`、`MEMORY_FAULT` 等 |
| 4 | 内部/环境错误：`HOST::NO_MODULE`、`HOST::NO_MEMORY`、`HOST::BAD_ARGUMENT`、`initMemory` 的 `MEMORY` 错误、分配失败 |

`CORE::DEBUG_BREAK` 不是故障：CLI 在调试器 `quit` 时把它映射为退出码 0，在 `abort` 时映射为 3。

### 5.3 诊断

- `Host::load` 返回 `HOST::UNLINKED_IMPORT` 后，`unlinkedImportIndex()`、`unlinkedImportName()`、`unlinkedImportArity()` 给出未绑定导入的索引、名字和 arity。这些值只在最近一次加载失败时有意义。
- `Host::error()` 可能在加载失败后返回 `PARSE` 码，在 `initMemory` 失败后返回 `MEMORY` 码，在运行失败后返回 `CORE` 码。
- `Host::core()` 指向最近一次运行创建的 `Core`；`Core::error()`、`errorPc()`、`trapCode()`、`stack()` 可用于生成宿主自己的诊断。
- 加载成功的模块可以通过 `Host::moduleCount()`、`module(index)`、`path(index)` 检查。

## 6. 内存 API

### 6.1 块管理

| 方法 | 行为 |
|---|---|
| `int allocate(int block)` | 块索引 `0..65535`；`RELEASED` 时分配 64 KiB 并置为 `FREE`；`FREE` 时幂等返回 `OK`；越界返回 `MEMORY_UNAVAILABLE` |
| `void free(int block)` | 释放映射并置为 `RELEASED`；非法索引是无操作 |
| `std::size_t allocatedBlocks() const` | 返回非 `RELEASED` 的块数 |

### 6.2 访问

| 方法 | 含义 |
|---|---|
| `readAt(dword &value, dword base, dword displacement)` | 读取地址 `base + displacement` 处的 4 字节 |
| `writeAt(dword value, dword base, dword displacement)` | 写入地址 `base + displacement` 处的 4 字节 |
| `read(dword &value, dword offset)` / `write(const dword &value, dword offset)` | 以 0 为基址的 4 字节访问 |
| `reads(std::span<byte> buffer, dword offset, dword size)` | 从 `offset` 读取 `size` 字节到 buffer |
| `writes(std::span<const byte> buffer, dword offset, dword size)` | 从 buffer 向 `offset` 写入 `size` 字节 |

### 6.3 限制与错误

- 地址空间 4 GiB，由 65536 个 64 KiB 块组成。
- 地址以 64 位计算；地址大于 `ADDRESS_SPACE - 4` 时返回 `MEMORY_OVERFLOW`。
- 访问的块必须已分配（`FREE`）；未映射块返回 `MEMORY_UNAVAILABLE`。
- 4 字节访问和 `reads`/`writes` 可以跨块，但涉及的所有块都必须已分配；`reads`/`writes` 按块分段处理。
- `reads`/`writes` 的 `size` 大于传入 buffer 的大小时返回 `MEMORY_UNAVAILABLE`。
- 访问不会自动分配或增长内存。`Host::initMemory()` 只分配常量触及的块；堆分配由宿主代码自行调用 `Memory::allocate()`（CLI 的 `mem_alloc` 就是这样实现的）。
- 错误码：`OK` (0)、`MEMORY_UNAVAILABLE` (2)、`MEMORY_OVERFLOW` (3)。

## 7. 宿主函数

### 7.1 签名

```cpp
using HOST::Function = dword (*)(void *user, const dword *args, int arity);
```

`CORE::HostSlot` 保存相同的签名。`user` 是注册时传入的不透明指针；`args` 和 `arity` 由运行时在调用时提供。

### 7.2 参数与返回值

- `args` 指向调用者的寄存器数组；`args[i]` 对应调用者的 `r_i`。只有 `args[0..arity-1]` 属于本次调用的参数窗口。
- 返回值写回 `hostcall` 指令的目的寄存器。
- 所有值都是 32 位原始值；浮点是 binary32 位模式。运行时没有 `Value`/字符串对象；`(address, length)` 之类的约定由宿主函数自己定义。
- `args` 只在调用期间有效，宿主函数不得保存该指针。
- 宿主函数**不得抛出异常**：运行时没有在调用点捕获 C++ 异常。
- 宿主函数**不得重入 VM**：不得调用 `Host::run`/`runModule` 或 `Core::run`/`step`，也不得在调用期间销毁宿主/内存对象。
- 宿主函数可以读取和写入通过 `user` 传入的 `Memory`；返回后 VM 继续执行。

### 7.3 注册 API

```cpp
int Host::registerFunction(const std::string &name, int arity,
                           HOST::Function function, void *user = nullptr);
```

- 成功返回 `HOST::OK`。
- 空名字、空函数指针、`arity < 0` 或 `arity > 256` 返回 `HOST::BAD_ARGUMENT`。
- 同名函数已经注册时返回 `HOST::BAD_ARGUMENT`（即使 arity 不同）。
- 注册是 `Host` 级操作；所有模块共享同一个注册表。

### 7.4 链接时机

导入在 `Host::load()` 内部解析：`Parse::parse()` 成功后，`bindImports()` 遍历模块的 IMPORTS 表，在注册表中查找匹配项，并把每个 import 绑定到一个 `CORE::HostSlot`。

- 注册必须在第一次 `load()` 之前完成。
- 之后加载的模块使用当时的注册表；已经加载的模块不会因为后续注册而重新绑定。
- 没有延迟绑定、运行时查找或重新链接 API。

### 7.5 宿主函数注册与链接

| 规则 | 行为 |
|---|---|
| 名字匹配 | 与 IMPORTS 中字符串常量的字节序列**完全一致**（区分大小写，不裁剪） |
| arity 匹配 | 必须与模块记录的 import arity **完全相等** |
| 重复注册 | 同名（无论 arity）的第二次注册返回 `HOST::BAD_ARGUMENT` |
| 非法注册 | 空名字、空函数指针、`arity < 0` 或 `arity > 256` 返回 `HOST::BAD_ARGUMENT` |
| 未链接导入 | `Host::load()` 返回 `HOST::UNLINKED_IMPORT` (102)，模块不加入 `Host` |
| 绑定粒度 | 每个 IMPORTS 条目对应一个槽位；槽位顺序与 IMPORTS 表一致 |
| 调用窗口 | `Core` 把调用者 `r0..r(arity-1)` 作为 `args` 传给宿主函数，并把返回值写入目的寄存器 |
| 生命周期 | 绑定在模块加载后保持有效，直到 `Host` 析构；没有重新绑定接口 |

`HOST::UNLINKED_IMPORT` 的诊断信息通过 `Host::unlinkedImportIndex()`、`unlinkedImportName()`、`unlinkedImportArity()` 读取。CLI 在加载期把它映射为退出码 2。

## 8. 燃料、中断与调试钩子

| 能力 | `Host` 接口 | `Core` 接口 | 语义 |
|---|---|---|---|
| 燃料 | `Host::setFuel(dword)` | `Core::setFuel(dword)` | 0 表示不限；每次运行开始重置；在下一条指令执行前检查，耗尽返回 `CORE::FUEL_EXHAUSTED` |
| 中断 | `Host::setInterruptHook(bool (*)(void *), void *)` | `Core::setInterruptHook(...)` | 每条指令执行前调用；返回 `true` 停止，错误码 `CORE::INTERRUPTED` |
| 调试 | `Host::runModule(index, CORE::DebugHook, void *)` | `Core::setDebugHook(...)` | 取指后、执行前调用；返回 `true` 停止，错误码 `CORE::DEBUG_BREAK`；只读、不可重入 |

- `Host` 的燃料和中断设置会转发给它创建的每个 `Core`。
- `Host::runModule(index, hook, user)` 的调试钩子只对本次调用有效；运行结束后 `Host` 清除它，不保留 `user` 指针。
- 启用燃料、中断钩子或调试钩子后，执行走守卫循环；守卫循环始终保留边界检查，即使模块已验证。
- 钩子在 VM 线程内同步执行，必须快速返回。调试钩子只能通过只读访问器观察状态（见 [03 · 运行时 §4.6](03-runtime.md#46-调试钩子)）。

## 9. 工作区

`Workspace` 是 CLI 侧的路径沙箱，`Host` 不感知它：

- `Host` 没有 `Workspace` 成员，也不解析或校验路径；它按传入的路径打开模块。
- CLI 在执行前创建 `Workspace`，解析每个模块路径，再把解析后的路径传给 `Host::load(path, displayPath)`。
- `src/motevm/workspace.cpp` 与运行时相互独立，只有嵌入方需要路径沙箱时才编译和调用它。
- 文件类宿主函数由 CLI 注册，不属于运行时；它们的路径授权由 CLI 使用 `Workspace` 完成。

CLI 的工作区规则见 [`self/cli.md`](self/cli.md)。

## 10. 线程与可重入性

- `Host`、`Core`、`Memory`、`Parse` 都不是线程安全的，内部没有锁。
- 一个 VM 实例只能由一个线程使用；钩子和宿主函数都在执行线程上同步调用。
- 宿主函数不得重入 VM。VM 正在执行时调用 `runModule` 或 `run` 会与当前执行栈和状态冲突。
- 多个线程可以各自创建独立的 `Host` 和 `Memory` 并行运行；它们之间没有共享的运行时全局状态。
- `Memory` 的块使用 `shared_ptr` 管理，但没有跨线程同步；不要并发访问同一 `Memory`。

## 11. 尚未实现

以下内容**不存在于当前实现中**，不应在代码或文档中当作可用接口：

- `VM` 类、`Value`/`Result`/`RootedValue` 类型；
- 公开总头文件（例如 `motevm.hpp`）；
- 安装的库、CMake 包、pkg-config 文件或 ABI 承诺；
- 运行库目标（运行时源文件直接编译进 `motevm` 可执行文件）；
- 垃圾回收、根集/`RootedValue` 机制、字符串/对象模型。

## 12. 相关文档

- 运行语义、错误码、内存与执行模型：[03 · 运行时](03-runtime.md)
- 指令集：[01 · ISA](01-isa.md)
- `.mbc` 容器与段布局：[02 · 字节码格式](02-bytecode-format.md)
- 工具链与重定位：[05 · 工具链](05-toolchain.md)
- 构建：[06 · 构建](06-build.md)
- 限制与上限：[08 · 限制](08-limits.md)
- CLI 工作区与退出码：[`self/cli.md`](self/cli.md)
- 汇编示例：[`examples/hello.mta`](examples/hello.mta)
