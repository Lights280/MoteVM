# MoteVM

MoteVM 是一个用 C++20 实现的自包含虚拟机与工具链，目标格式为 MOTEBC 1.0
字节码（`.mbc`），模块可打包为 MTR 1.0 归档（`.mtr`）。

- 版本：`0.1.0`
- 字节码格式：`MOTEBC 1.0`
- 许可证：MIT

## 仓库结构

| 路径 | 说明 |
| --- | --- |
| `src/include/` | 共享代码：`motevm_core`（模块加载/校验/反汇编）、`motevm_tools`（汇编器与工具基础设施）、`archive`（MTR 容器） |
| `src/motevm/` | 虚拟机运行时、内存管理、宿主接口与调试器 |
| `src/moteasm/` | 汇编器：`.mta` 源文件 -> `.mbc` |
| `src/motedis/` | 反汇编器：`.mbc` -> 规范化 `.mta` 文本 |
| `src/motear/` | 归档工具：打包、查看、校验、解包 `.mtr` |
| `src/builder/` | 独立构建驱动，可在 CMake 之外单独构建整个项目 |
| `CMakeLists.txt` | 根构建脚本，自动发现 `src/<module>/<module>.cpp` 为可执行目标 |
| `LICENSE` | MIT 许可证 |

## 构建

依赖 CMake 3.20+ 和支持 C++20 的编译器（默认优先使用 clang++）。

```bash
cmake -S . -B build -G Ninja -DMOTEVM_COMPILER=clang -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

常用选项：

| 选项 | 取值 |
| --- | --- |
| `MOTEVM_COMPILER` | `auto`（默认）、`clang`、`gcc`、`msvc` |
| `MOTEVM_ARCH` | `native`（默认）、`x86`、`x64` |
| `MOTEVM_BUILD_TOOLS` | `ON`（默认），构建 `src/` 下的所有模块 |
| `MOTEVM_WERROR` | `OFF`（默认），把警告视为错误 |
| `MOTEVM_SANITIZERS` | `OFF`（默认），启用 ASan/UBSan |

可执行文件输出到 `build/cmake/<toolchain>-<arch>-<config>/bin/`。

## 命令行工具

```bash
moteasm hello.mta -o hello.mbc                # 汇编
motevm hello.mbc                              # 运行；-s 统计，-d 调试，-i 交互调试
motedis hello.mbc                             # 反汇编为规范文本
motear pack -o hello.mtr --module hello.mbc   # 打包
motear list hello.mtr --verify                # 查看并校验归档
```

各工具的完整参数与退出码见 `--help`：
`motevm --help`、`moteasm --help`、`motedis --help`、`motear --help`。
