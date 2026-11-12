# 06 · 构建与发布

[简体中文](06-build.md) | [English](en/06-build.md)

本文描述 MoteVM 0.1.0 的构建方式、CMake 选项、产物布局、独立构建驱动 `builder`、安装规则与发布检查。项目当前支持 MOTEBC 1.0 字节码与 MTR 1.0 归档。

## 1. 前置要求

| 项目 | 要求 |
| --- | --- |
| CMake | 3.20 或更新 |
| C++ 标准 | C++20（`CMAKE_CXX_STANDARD 20`，`CMAKE_CXX_EXTENSIONS OFF`） |
| 编译器 | Clang、GCC、MSVC |
| 生成器 | 推荐 Ninja；也支持 Makefiles 与 Visual Studio 生成器 |
| 默认构建类型 | 单配置生成器在未显式指定时使用 `Release` |

`clang-cl`（Clang 的 MSVC 前端，`CMAKE_CXX_COMPILER_FRONTEND_VARIANT` 为 `MSVC`）不受支持。配置阶段会直接报错：

```text
clang-cl is not supported; use clang++ or MSVC cl
```

## 2. CMake 构建系统

```bash
cmake -S . -B build -G Ninja -DMOTEVM_COMPILER=clang -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### 2.1 CMake 选项

| 选项 | 默认值 | 取值 | 语义 |
| --- | --- | --- | --- |
| `MOTEVM_COMPILER` | `auto` | `auto`、`clang`、`gcc`、`msvc` | 编译器选择。`auto` 在非 Visual Studio 生成器下优先查找 `clang++`（依次尝试 `clang++`、`clang++-20`、`clang++-19`、`clang++-18`）；找不到时给出警告并交给 CMake 默认编译器。`clang` 查找 `clang++` 系列，`gcc` 查找 `g++`（`g++`、`g++-15`、`g++-14`），找不到即 `FATAL_ERROR`。`msvc` 在 Ninja/Makefiles 下要求 `cl` 已在开发环境中，否则 `FATAL_ERROR`；Visual Studio 生成器由生成器决定编译器。配置后会校验编译器 ID。 |
| `MOTEVM_ARCH` | `native` | `native`、`x86`、`x64` | 目标架构。`native` 按宿主机指针宽度解析为 `x64` 或 `x86`；其它值直接指定 32/64 位。非法值触发 `FATAL_ERROR`。 |
| `MOTEVM_BUILD_TOOLS` | `ON` | `ON`、`OFF` | 构建 `src/` 下自动发现的模块可执行文件。`OFF` 时仍会构建三个静态库，但不生成工具可执行文件。 |
| `MOTEVM_BUILD_TESTS` | `ON` | `ON`、`OFF` | 构建 `tests/`。仅当 `tests/CMakeLists.txt` 存在时调用 `add_subdirectory(tests)` 并启用 CTest；目录不存在时打印状态消息并跳过。 |
| `MOTEVM_BUILD_BENCHMARKS` | `OFF` | `ON`、`OFF` | 构建 `benchmarks/`。仅当 `benchmarks/CMakeLists.txt` 存在时加入；否则打印状态消息并跳过。 |
| `MOTEVM_BUILD_FUZZERS` | `OFF` | `ON`、`OFF` | 构建 libFuzzer 目标，仅支持 Clang。非 Clang 编译器下给出警告并跳过；`fuzz/CMakeLists.txt` 不存在时打印状态消息并跳过。 |
| `MOTEVM_WERROR` | `OFF` | `ON`、`OFF` | 把警告视为错误：MSVC 追加 `/WX`，GCC/Clang 追加 `-Werror`。 |
| `MOTEVM_SANITIZERS` | `OFF` | `ON`、`OFF` | 启用 ASan/UBSan。GCC/Clang 使用 `-fsanitize=address,undefined`，MSVC 仅启用 ASan。 |
| `MOTEVM_INSTALL` | `OFF` | `ON`、`OFF` | 生成安装规则。见 §2.6。 |

`MOTEVM_COMPILER` 的约束：

- Visual Studio 生成器不能配合 `MOTEVM_COMPILER=clang` 或 `gcc`，否则配置失败。
- `MOTEVM_COMPILER=msvc` 在 Ninja/Makefiles 下需要先进入 Visual Studio Developer PowerShell（`Enter-VsDevShell`）。
- 编译器 ID 与选项不符时配置失败，例如 `MOTEVM_COMPILER=gcc` 但实际编译器是 Clang。

### 2.2 目标

三个静态库不参与模块自动发现：

| 目标 | 类型 | 源文件 | 依赖 |
| --- | --- | --- | --- |
| `motevm_core`（别名 `motevm::core`） | STATIC | `src/include/parse.cpp`、`verifier.cpp`、`disasm.cpp` | 无 |
| `motevm_archive`（别名 `motevm::archive`） | STATIC | `src/include/archive/*.cpp` | PUBLIC `motevm_core` |
| `motevm_tools` | STATIC | `src/include/asm/*.cpp`、`common/*.cpp`、`dis/*.cpp` | PUBLIC `motevm_core` |

三个库都以 `src/include` 作为 PUBLIC include 目录，并要求 C++20。

可执行目标由目录自动发现：`src/` 的每个直接子目录（`include` 除外）都是一个模块，模块目录名必须匹配 `[a-z][a-z0-9_]*`，且必须包含 `<module>.cpp` 作为入口。模块内的 `*.cpp` 递归收集，但 `include/` 下的文件被排除。

当前模块与链接关系：

| 可执行目标 | 入口 | 链接 |
| --- | --- | --- |
| `motevm` | `src/motevm/motevm.cpp` | `motevm_core` |
| `moteasm` | `src/moteasm/moteasm.cpp` | `motevm_tools` |
| `motedis` | `src/motedis/motedis.cpp` | `motevm_tools` |
| `motear` | `src/motear/motear.cpp` | `motevm_archive`、`motevm_tools` |
| `builder` | `src/builder/builder.cpp` | 无（仅使用 `src/include` 头文件） |

模块名不能与 CMake 内置目标冲突（`all`、`clean`、`test`、`install` 等）。目录名不合法、缺少主文件或没有任何 `.cpp` 都会导致配置失败。

### 2.3 输出路径

CMake 构建的模块可执行文件输出到：

```text
<build>/cmake/<toolchain>-<arch>-<config>/bin/<module>[.exe]
```

其中：

- `<toolchain>` 为 `clang`、`gcc` 或 `msvc`；
- `<arch>` 为 `x86` 或 `x64`；
- `<config>` 为构建类型的小写形式，例如 `release`、`debug`。

例如：

```text
build/cmake/clang-x64-release/bin/motevm
build/cmake/clang-x64-release/bin/moteasm
```

通用输出目录（测试、基准等）为 `build/bin`，库输出目录为 `build/lib`。模块目标会覆盖这些通用目录。

### 2.4 警告与消毒器

警告选项按编译器前端分组应用。

MSVC（`cl`）：

```text
/W4 /permissive- /utf-8 /Zc:__cplusplus
```

启用 `MOTEVM_WERROR` 时追加 `/WX`。

GCC / Clang：

```text
-Wall -Wextra -Wpedantic
-Wconversion -Wsign-conversion -Wshadow -Wcast-qual -Wundef
-Wnon-virtual-dtor -Wold-style-cast
-Werror=return-type
```

其中 `-Werror=return-type` 始终启用。启用 `MOTEVM_WERROR` 时追加 `-Werror`。

消毒器：

| 编译器 | 编译与链接选项 |
| --- | --- |
| GCC / Clang | `-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all` |
| MSVC | `/fsanitize=address` |
| 其它 | 打印 `MOTEVM_SANITIZERS unsupported by <id>` 警告 |

消毒器选项同时用于编译和链接。除 Clang 外的编译器启用 `MOTEVM_BUILD_FUZZERS` 只会得到警告并跳过。

### 2.5 独立构建驱动 builder

`src/builder/builder.cpp` 是不读取 `CMakeLists.txt` 的单翻译单元构建驱动，可在没有 CMake 的环境中编译整个项目。命令：

| 命令 | 行为 |
| --- | --- |
| `build`（默认） | 编译并链接选中目标 |
| `clean` | 删除 builder 生成的目录与状态，不触碰 CMake 文件 |
| `rebuild` | 先 `clean` 再 `build` |
| `self` | 重新构建 `builder` 自身 |
| `doctor` | 打印工具链、ABI、输出目录、目标与共享源组 |
| `targets` | 列出可选目标、源文件数量与链接关系 |
| `help` | 打印用法 |

选项：

| 选项 | 说明 |
| --- | --- |
| `-j, --jobs N` | 并行编译任务数；接受 1..1024，默认取硬件并发数（上限 64，取不到时为 4） |
| `--compiler clang\|gcc\|msvc` | 编译器选择，默认优先 `clang++`；也接受 `clang++`、`g++`、`cl` 别名 |
| `--arch x86\|x64` | 目标架构，默认宿主机架构；也接受 `host`、`auto`、`32`、`64`、`i686`、`win32`、`x86_64`、`amd64` |
| `--release` / `--debug` | 构建配置，默认 Release |
| `--cxx PATH` | 显式 C++ 编译器，按文件名识别编译器家族 |
| `--build-dir DIR` | 输出根目录，默认 `<root>/build` |
| `--root DIR` | 项目根目录，默认从当前目录与可执行文件位置向上查找 `src/motevm` 与 `src/builder` |
| `--werror` | 警告视为错误 |
| `--sanitize` | ASan + UBSan；MSVC 仅 ASan |
| `--force` | 忽略缓存全量重建 |
| `-n, --dry-run` | 只打印命令，不执行 |
| `-v, --verbose` | 打印缓存命中原因与完整命令行 |
| `--version` | 打印 builder、MoteVM 与 MOTEBC 版本 |
| `-h, --help` | 打印帮助 |

环境变量：

| 变量 | 作用 |
| --- | --- |
| `MOTEVM_CXX` | 编译器候选路径；在 `--cxx` 之后、`CXX` 与自动查找之前使用 |
| `CXX` | 编译器候选路径，在 `MOTEVM_CXX` 之后、自动查找之前 |
| `MOTEVM_BUILD_DIR` | 未给出 `--build-dir` 时的输出根目录，相对项目根解析 |
| `MOTEVM_JOBS` | 未给出 `-j/--jobs` 时的并行任务数，接受 1..1024 |
| `VSCMD_ARG_TGT_ARCH` | MSVC 开发环境的目标架构（`amd64`/`x86_64`/`win32` 等），用于判断是否需要重新进入 DevShell |

子编译器使用的临时目录被设置为 `<build>/.builder/tmp`（通过 `TMP`、`TEMP`、`TMPDIR`）。

输出布局：

```text
build/<toolchain>-<arch>-<config>/bin/<module>[.exe]
build/<toolchain>-<arch>-<config>/.builder/{obj,dep,state,logs,tmp}/
build/bin/builder[.exe]     # self 的默认位置
```

`builder self` 默认重建到 `build/bin/builder[.exe]`；如果当前运行的 builder 本身已位于 build 树内，则原位重建该可执行文件。

共享源组 `motevm_core` 与 `motevm_tools` 是内部目标，不出现在 `all` 中，也不能作为用户目标选择；它们在一次构建中只编译一次，并被链接它们的可执行目标复用。共享源清单硬编码在 builder 中，新增共享源时必须同时更新根 `CMakeLists.txt`。

退出码：`0` 成功、`1` 构建失败、`2` 用法错误、`3` 环境或工具链错误。

### 2.6 安装

`MOTEVM_INSTALL=ON` 时生成以下安装规则：

- 模块可执行文件安装到 `${CMAKE_INSTALL_BINDIR}`；
- `src/include/` 下的头文件安装到 `${CMAKE_INSTALL_INCLUDEDIR}`；
- 各模块自己的 `include/` 安装到 `${CMAKE_INSTALL_INCLUDEDIR}/<module>`。

安装规则不包含静态库、CMake 导出目标或 package config，也不承诺 ABI。C++ 头文件属于内部接口，见 §6。

## 3. 编译器选择与平台说明

### 3.1 Windows / MSVC

使用 Ninja 或 Makefiles 时，先进入 Visual Studio Developer PowerShell：

```powershell
Enter-VsDevShell
cmake -S . -B build -G Ninja -DMOTEVM_COMPILER=msvc
cmake --build build
```

或直接使用 Visual Studio 生成器：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

`-A Win32` 生成 32 位目标；`-A x64` 生成 64 位目标。`MOTEVM_ARCH` 必须与编译器环境一致，否则配置失败。

### 3.2 MinGW / GCC

`MOTEVM_COMPILER=gcc` 使用 `g++`。32 位构建要求该 `g++` 提供 `-m32` multilib，或本身是 32 位目标三元组（如 `i686`/MinGW32）：

```bash
cmake -S . -B build -G Ninja -DMOTEVM_COMPILER=gcc -DMOTEVM_ARCH=x86
```

没有 32 位 multilib 时配置失败，提示使用 MinGW32 工具链或改回 `x64`。

### 3.3 32 位构建

- MSVC：使用 `-A Win32`，或进入 x86 DevShell。
- GCC：使用 `-m32`（需要 multilib），见 §3.2。
- Windows 上的 Clang（MSVC ABI）：架构选项会追加 `--target=i686-pc-windows-msvc` 或 `--target=x86_64-pc-windows-msvc`。
- 其它平台：`MOTEVM_ARCH=x86/x64` 分别追加 `-m32`/`-m64`。

## 4. 测试、基准与模糊测试状态

当前仓库不包含 `tests/`、`benchmarks/` 与 `fuzz/` 目录。

| 选项 | 默认 | 目录缺失时的行为 |
| --- | --- | --- |
| `MOTEVM_BUILD_TESTS=ON` | 是 | 打印 `MOTEVM_BUILD_TESTS=ON but tests/ is not present; skipping`，不产生测试目标 |
| `MOTEVM_BUILD_BENCHMARKS=ON` | 否 | 打印 `MOTEVM_BUILD_BENCHMARKS=ON but benchmarks/ is not present; skipping` |
| `MOTEVM_BUILD_FUZZERS=ON` | 否 | Clang 下打印 `MOTEVM_BUILD_FUZZERS=ON but fuzz/ is not present; skipping`；非 Clang 下先打印警告 |

默认配置不会因为这些目录缺失而失败。

## 5. 版本与兼容性

| 项目 | 当前值 | 来源 |
| --- | --- | --- |
| 项目版本 | `0.1.0` | `src/include/define.hpp` 的 `MOTEVM_VERSION`；CMake `project(... VERSION 0.1.0)` |
| 字节码格式 | MOTEBC 1.0 | `MOTEBC_VERSION` 与 `PARSE::SUPPORTED_MAJOR/MINOR` |
| 归档格式 | MTR 1.0 | `ARCHIVE::MTR_VERSION` 与 `ARCHIVE::SUPPORTED_MAJOR/MINOR` |

各工具 `--version` 输出形如：

```text
motevm 0.1.0 (MOTEBC 1.0)
moteasm 0.1.0 (MOTEBC 1.0)
motedis 0.1.0 (MOTEBC 1.0)
motear 0.1.0 (MTR 1.0)
```

仓库当前没有 git 标签，也没有 `CHANGELOG`。版本号以源码常量为准。

兼容性以当前实现为准：

- MOTEBC 加载要求 major 等于 `1` 且 minor 不大于 `0`；其它 major 或更新的 minor 都会被拒绝（`BAD_FILE`）。
- MTR 解析要求 major 等于 `1` 且 minor 不大于 `0`；否则拒绝（`BAD_HEADER`）。
- 当前没有声明或测试更宽泛的向后兼容策略；不同版本之间按“不兼容”处理。

## 6. 稳定性分类

| 接口 | 稳定性 |
| --- | --- |
| 命令行选项、用法与退出码 | 已文档化；以各工具 `--help` 为准 |
| MOTEBC 1.0 与 MTR 1.0 文件格式 | 已文档化；见 [02 字节码格式](02-bytecode-format.md) 与 [07 MTR 格式](07-mtr-format.md) |
| `src/include/` 与各模块 `include/` 下的 C++ 头文件 | 内部接口，不承诺稳定性或 ABI，可随时更改 |
| 静态库目标与链接关系 | 内部构建细节，不构成对外 API |

## 7. 发布检查清单

1. 清理后重新配置与构建：

   ```bash
   cmake -S . -B build -G Ninja -DMOTEVM_COMPILER=clang -DCMAKE_BUILD_TYPE=Release
   cmake --build build
   ```

2. 汇编并运行示例：

   ```bash
   build/cmake/clang-x64-release/bin/moteasm docs/examples/hello.mta -o hello.mbc
   build/cmake/clang-x64-release/bin/motevm hello.mbc
   ```

3. 反汇编/再汇编往返：`motedis hello.mbc -o hello.out.mta` 后重新汇编并运行，输出应与原模块一致。

4. 归档往返：`motear pack -o hello.mtr --module hello.mbc`、`motear verify hello.mtr`、`motear unpack hello.mtr -C out`。

5. 检查版本一致性：`motevm`、`moteasm`、`motedis`、`motear`、`builder` 的 `--version` 均报告 `0.1.0`，格式版本分别为 MOTEBC 1.0 或 MTR 1.0。

6. 可选：用 `builder doctor` 确认工具链、架构与输出目录，用 `builder targets` 确认目标清单。
