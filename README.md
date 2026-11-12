# MoteVM

[English](README.en.md) | **简体中文** | [完整文档](docs/README.md)

MoteVM 是一个用 C++20 实现的自包含虚拟机与工具链，目标格式为 **MOTEBC 1.0** 字节码
（`.mbc`），模块可打包为 **MTR 1.0** 归档（`.mtr`）。

- 版本：`0.1.0`
- 字节码格式：`MOTEBC 1.0`
- 许可证：MIT

## 组件概览

MoteVM 由四个命令行工具和一个独立构建驱动组成：

| 工具 | 作用 |
| --- | --- |
| `moteasm` | 汇编器：`.mta` 源文件 -> `.mbc` |
| `motevm` | 虚拟机：加载、校验并运行 `.mbc` 模块 |
| `motedis` | 反汇编器：`.mbc` -> 规范化 `.mta` 文本 |
| `motear` | 归档工具：打包、列出、查看详情、校验、解包 `.mtr` |
| `builder` | 独立构建驱动，可在 CMake 之外单独构建整个项目 |

## 仓库结构

| 路径 | 说明 |
| --- | --- |
| `src/include/` | 共享代码：`motevm_core`（模块加载/校验/反汇编）、`motevm_tools`（汇编器与工具基础设施）、`archive`（MTR 容器） |
| `src/motevm/` | 虚拟机运行时、内存管理、宿主接口与调试器 |
| `src/moteasm/` | 汇编器：`.mta` 源文件 -> `.mbc` |
| `src/motedis/` | 反汇编器：`.mbc` -> 规范化 `.mta` 文本 |
| `src/motear/` | 归档工具：打包、查看、校验、解包 `.mtr` |
| `src/builder/` | 独立构建驱动，可在 CMake 之外单独构建整个项目 |
| `docs/` | 用户文档：快速开始、ISA、字节码/归档格式、工具链、构建与限制 |
| `CMakeLists.txt` | 根构建脚本，自动发现 `src/<module>/<module>.cpp` 为可执行目标 |
| `LICENSE` | MIT 许可证 |

> 克隆仓库只包含上表文件。`tests/`、`benchmarks/`、`deepseek-cache/`
> 等是本地工作目录，不在版本库中。

## 环境要求

- CMake 3.20 或更新版本
- 支持 C++20 的编译器（默认优先使用 `clang++`）
- 推荐使用 Ninja（下文命令均以 Ninja 为例）

## 构建

```bash
git clone https://github.com/Lights280/MoteVM.git
cd MoteVM
cmake -S . -B build -G Ninja -DMOTEVM_COMPILER=clang -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### 编译器选择

`MOTEVM_COMPILER=auto`（默认）在非 Visual Studio 生成器下优先使用 `clang++`。
在 Windows 上使用 MSVC 时，可以进入 Visual Studio Developer PowerShell
（`Enter-VsDevShell`）后传入 `-DMOTEVM_COMPILER=msvc`，或者直接使用 Visual Studio 生成器：

```powershell
# 生成器版本以本机安装为准
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

### CMake 选项

| 选项 | 取值 |
| --- | --- |
| `MOTEVM_COMPILER` | `auto`（默认）、`clang`、`gcc`、`msvc` |
| `MOTEVM_ARCH` | `native`（默认）、`x86`、`x64` |
| `MOTEVM_BUILD_TOOLS` | `ON`（默认），构建 `src/` 下的所有模块 |
| `MOTEVM_BUILD_TESTS` | `ON`（默认），构建 `tests/`（该目录不存在时自动跳过） |
| `MOTEVM_BUILD_BENCHMARKS` | `OFF`（默认），构建 `benchmarks/` |
| `MOTEVM_BUILD_FUZZERS` | `OFF`（默认），构建 libFuzzer 目标（仅 Clang） |
| `MOTEVM_WERROR` | `OFF`（默认），把警告视为错误 |
| `MOTEVM_SANITIZERS` | `OFF`（默认），启用 ASan/UBSan |
| `MOTEVM_INSTALL` | `OFF`（默认），生成安装规则 |

### 产物路径

可执行文件输出到 `build/cmake/<toolchain>-<arch>-<config>/bin/`，
例如 `build/cmake/clang-x64-release/bin/motevm`。

## 命令行工具

仓库自带可运行示例 [`docs/examples/hello.mta`](docs/examples/hello.mta)：

```bash
moteasm docs/examples/hello.mta -o hello.mbc  # 汇编
motevm hello.mbc                              # 运行；-s 统计，-d 调试，-i 交互调试
# 输出：hello, mote
motedis hello.mbc                             # 反汇编为规范文本
motear pack -o hello.mtr --module hello.mbc   # 打包
motear list hello.mtr --verify                # 查看并校验归档
```

`.mta` 语法见 [docs/self/moteasm.md](docs/self/moteasm.md)，指令语义见
[docs/01-isa.md](docs/01-isa.md)，完整工具说明见 [docs/05-toolchain.md](docs/05-toolchain.md)。

### `motevm` 常用选项

用法：`motevm [extern_parameters] <entry_mbc_module> [ext_mbc_modules] [intern_parameters]`

| 选项 | 说明 |
| --- | --- |
| `-s, --status` | 运行结束后打印执行统计 |
| `-r, --release` | 发布模式（默认）：只输出错误 |
| `-d, --debug` | 调试模式：详细诊断、停机上下文与跟踪 |
| `-i, --interactive` | 在入口处停下，从标准输入读取调试器命令 |
| `--step` | 在第一条指令前停下（隐含 `--interactive`） |
| `--break SPEC` | 设置断点（可重复）：`#index`、`offset`、`function[:offset]` |
| `--trace LVL` | 指令跟踪：`off`、`call`、`exec`、`full`（默认 `off`） |
| `--trace-file PATH` | 跟踪输出目标（默认 stderr，`-` 表示 stdout） |
| `--trace-limit N` | 记录 N 行跟踪后停止（0 表示不限制） |
| `-w, --workspace DIR` | 仅授予 `DIR` 内的读写权限（默认只允许 stdio） |

内置宿主函数（`print`、`print_str`、`const_addr` 等）见
[docs/self/cli.md](docs/self/cli.md) §10。

### `motear` 子命令

| 子命令 | 说明 |
| --- | --- |
| `pack` | 由模块（及资源）创建归档 |
| `list` | 列出归档条目（`--verbose`、`--verify`） |
| `info` | 打印头部与清单信息（`--json`） |
| `verify` | 校验容器与模块（`--strict`、`--no-modules`） |
| `unpack` | 解包到指定目录（`-C DIR`、`--force`、`--module NAME`） |

归档格式与清单字段见 [docs/07-mtr-format.md](docs/07-mtr-format.md)。

## 独立构建驱动 builder

`src/builder/builder.cpp` 是一个不依赖外部构建系统的 C++20 构建驱动，可在不安装 CMake 的
情况下编译整个项目：

```bash
builder          # 构建全部目标（默认）
builder targets  # 列出目标与源文件
builder doctor   # 打印工具链与构建配置
builder clean    # 清理 builder 产物（不会删除 CMake 文件）
```

## 退出码

| 工具 | 退出码 |
| --- | --- |
| `motevm` | `0` 成功，`1` 用法错误，`2` 输入/校验失败，`3` 运行时陷阱，`4` 内部错误 |
| `moteasm` | `0` 成功，`1` 用法错误，`2` 输入/源文件错误，`4` 内部或 I/O 错误 |
| `motedis` | `0` 成功，`1` 用法错误，`2` 输入/校验错误，`4` 内部或 I/O 错误 |
| `motear` | `0` 成功，`1` 用法错误，`2` 输入/校验错误，`3` I/O 错误，`4` 内部错误 |

## 稳定性与版本

- 版本 `0.1.0`；字节码 `MOTEBC 1.0`；归档 `MTR 1.0`。
- 命令行工具与 MOTEBC/MTR 格式有文档；0.x 阶段选项仍可能调整，以 `--help` 为准。
- `src/motevm/include/` 下的 C++ `Host`/`Core` 接口是内部接口：没有 umbrella
  header、不作为库安装、不承诺 ABI 稳定，详见
  [docs/04-embedding-api.md](docs/04-embedding-api.md)。
- 当前没有 git tag 或 CHANGELOG。
- 所有硬限制见 [docs/08-limits.md](docs/08-limits.md)。

## 帮助

完整文档索引见 [docs/README.md](docs/README.md)。各工具的完整参数与退出码见 `--help`：
`motevm --help`、`moteasm --help`、`motedis --help`、`motear --help`、`builder --help`。

## 许可证

本项目基于 MIT 许可证发布，详见 [LICENSE](LICENSE)。
