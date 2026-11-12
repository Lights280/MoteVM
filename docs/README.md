# MoteVM 文档

[简体中文](README.md) | [English](en/README.md)

本文档集描述 **MoteVM 0.1.0 的当前实现**：一个 C++20 编写的寄存器式虚拟机与工具链，
目标格式为 MOTEBC 1.0 字节码（`.mbc`）和 MTR 1.0 归档（`.mtr`）。

> 这里只记录已实现的行为。设计文档中的目标 ISA（`Value`/GC/对象模型）、`motec`
> 编译器、JIT，以及尚未实现的公开 `VM`/`Value`/`Result` API **不在本文档范围内**。

## 按读者选择入口

| 你是 | 从这里开始 |
| --- | --- |
| 第一次使用 | [00 · 快速开始](00-overview.md) → [06 · 构建与发布](06-build.md) |
| 编写 `.mta` 模块 | [self/moteasm.md](self/moteasm.md) → [01 · ISA 参考](01-isa.md) → [02 · MOTEBC 格式](02-bytecode-format.md) |
| 使用命令行工具 | [05 · 工具链参考](05-toolchain.md) → [self/cli.md](self/cli.md) → [self/motedis.md](self/motedis.md) |
| 打包/分发模块 | [07 · MTR 格式](07-mtr-format.md) → [05 · 工具链参考](05-toolchain.md) |
| 从 C++ 嵌入运行时 | [03 · 运行时](03-runtime.md) → [04 · 嵌入 API](04-embedding-api.md) |
| 构建/发布维护 | [06 · 构建与发布](06-build.md) → [08 · 限制](08-limits.md) |

## 文档地图

| 文档 | 内容 |
| --- | --- |
| [00 · 快速开始](00-overview.md) | 构建、运行第一个程序、工具数据流 |
| [01 · ISA 参考](01-isa.md) | 当前指令集、编码、语义、陷阱、内存模型 |
| [02 · MOTEBC 格式](02-bytecode-format.md) | `.mbc` 头部、段布局、校验规则 |
| [03 · 运行时](03-runtime.md) | 加载/校验/执行流程、内存、钩子、错误码 |
| [04 · 嵌入 API](04-embedding-api.md) | `Host`/`Core` 用法（内部、不稳定） |
| [05 · 工具链参考](05-toolchain.md) | 各工具约定、CLI 设计、退出码、宿主 ABI |
| [06 · 构建与发布](06-build.md) | CMake 选项、目标、警告、版本策略 |
| [07 · MTR 格式](07-mtr-format.md) | `.mtr` 归档布局与校验 |
| [08 · 限制](08-limits.md) | 所有硬限制的权威表 |
| [self/moteasm.md](self/moteasm.md) | `.mta` 汇编语言参考 |
| [self/cli.md](self/cli.md) | `motevm` 完整 CLI、内置宿主函数、调试器 |
| [self/motedis.md](self/motedis.md) | 规范化反汇编与 round-trip |

## 稳定性

| 接口 | 状态 | 说明 |
| --- | --- | --- |
| 命令行工具 | 已文档化 | 0.x 阶段选项可能调整；以 `--help` 为准 |
| MOTEBC 1.0 / MTR 1.0 | 已文档化 | 格式版本独立于项目版本；发布 tag 由维护者在冻结提交上创建 |
| C++ `Host`/`Core`/`Parse` | 内部、不稳定 | 无 umbrella header、无安装库、无 ABI 承诺 |
| `tests/`、CI | 随版本库发布 | 单元测试与 CLI 集成检查；变更记录见 `CHANGELOG.md` |
| 目标 ISA（`Value`/GC）、`motec`、JIT | 未实现 | 不属于当前文档范围 |

## 版本

| 项目 | 值 |
| --- | --- |
| MoteVM | 0.1.0 |
| MOTEBC | 1.0 |
| MTR | 1.0 |
| 许可证 | MIT |

## 约定

- 退出码：`0` 成功；`1` 用法错误；`2` 输入/校验错误；`3` 运行时陷阱（仅 `motevm`）；
  `4` 内部或 I/O 错误。各工具细节见 [05 · 工具链参考](05-toolchain.md) §8.5。
- 诊断信息写 stderr，程序/工具的正常数据写 stdout。
- 所有输出不含时间戳或随机量，同一输入产生相同结果。
- 源码中的 `docs/05`、`docs/06`、`docs/self/cli.md` 等简写引用对应本目录文件。
