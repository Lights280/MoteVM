# 00 · 快速开始

[简体中文](00-overview.md) | [English](en/00-overview.md)

## 1. MoteVM 是什么

MoteVM 是一个自包含的寄存器式虚拟机与工具链：

| 工具 | 输入 | 输出 |
| --- | --- | --- |
| `moteasm` | `.mta` 汇编源文件 | `.mbc` 模块 |
| `motevm` | `.mbc` 模块 | 程序输出 |
| `motedis` | `.mbc` 模块 | 规范化 `.mta` 文本 |
| `motear` | `.mbc`/资源文件 | `.mtr` 归档 |
| `builder` | 源码树 | 独立于 CMake 的构建产物 |

数据流：

```text
hello.mta ──moteasm──▶ hello.mbc ──motevm──▶ 输出
                           │
                           ├──motedis──▶ 规范化 .mta
                           └──motear───▶ hello.mtr
```

当前实现的指令集是 32 位寄存器式 ISA，详见 [01 · ISA 参考](01-isa.md)。

## 2. 环境要求

- CMake 3.20 或更新版本
- C++20 编译器（Clang、GCC 或 MSVC）
- 推荐 Ninja

完整的选项与平台说明见 [06 · 构建与发布](06-build.md) §2.1。

## 3. 构建

```bash
git clone https://github.com/Lights280/MoteVM.git
cd MoteVM
cmake -S . -B build -G Ninja -DMOTEVM_COMPILER=clang -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

可执行文件位于 `build/cmake/<toolchain>-<arch>-<config>/bin/`，例如：

```text
build/cmake/clang-x64-release/bin/moteasm
build/cmake/clang-x64-release/bin/motevm
```

下文用 `B` 表示该目录：

```bash
B=build/cmake/clang-x64-release/bin
```

## 4. 运行第一个程序

仓库自带示例 [examples/hello.mta](examples/hello.mta)：

```mta
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

汇编并运行：

```bash
$B/moteasm docs/examples/hello.mta -o hello.mbc
$B/motevm hello.mbc
```

期望输出：

```text
hello, mote
```

加上 `-s` 可以把执行统计写到 stderr：

```bash
$B/motevm -s hello.mbc
```

## 5. 工具速览

```bash
$B/moteasm docs/examples/hello.mta -o hello.mbc   # 汇编
$B/motevm hello.mbc                               # 运行
$B/motevm -d hello.mbc                            # 调试模式诊断
$B/motedis hello.mbc                              # 反汇编
$B/motear pack -o hello.mtr --module hello.mbc    # 打包
$B/motear list hello.mtr --verify                 # 列出并校验
$B/motear verify hello.mtr --strict               # 严格校验
```

## 6. 下一步

- 学习 `.mta` 语法：[self/moteasm.md](self/moteasm.md)
- 了解指令语义：[01 · ISA 参考](01-isa.md)
- 查看所有 CLI 选项与退出码：[05 · 工具链参考](05-toolchain.md)
- 从 C++ 调用运行时：[04 · 嵌入 API](04-embedding-api.md)
- 查询硬限制：[08 · 限制](08-limits.md)
