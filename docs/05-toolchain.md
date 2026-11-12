# 05 · 工具链参考

[简体中文](05-toolchain.md) | [English](en/05-toolchain.md)

本文描述 MoteVM 0.1.0 当前实现的命令行工具：`moteasm`、`motedis`、`motevm`、`motear`，
以及独立构建驱动 `builder`。所有选项与退出码以 `--help` 和当前源码为准；0.x 阶段选项可能调整。

## 1. 工具地图与数据流

| 工具 | 作用 | 输入 | 输出 |
|---|---|---|---|
| `moteasm` | 汇编器 | `.mta` 文本 | `.mbc` 字节码、listing |
| `motedis` | 规范化反汇编器 | `.mbc` | canonical `.mta` 文本 |
| `motevm` | 加载、校验并执行入口模块 | `.mbc`（入口 + 可选外部模块） | 脚本 stdout、诊断、trace、统计 |
| `motear` | MTR 1.0 打包/检查/校验/解包 | `.mbc`、资源文件、`.mtr` | `.mtr` 归档或报告 |
| `builder` | 项目构建驱动 | `src/` 源文件 | `build/<toolchain>-<arch>-<config>/bin/` |

```text
hello.mta --moteasm--> hello.mbc --motevm--> stdout / exit code
                           |
                           +--motedis--> hello.canon.mta --moteasm--> hello.mbc   (RT-1)

hello.mbc + resources --motear pack--> app.mtr
app.mtr --motear list | info | verify | unpack--> reports / unpacked directory
```

`builder` 不参与运行时数据流；它直接发现 `src/<module>` 并调用 C++ 编译器，不读取
`CMakeLists.txt`（见 [06 · 构建与发布](06-build.md)）。

## 2. 共同约定

### 2.1 选项

- 长选项支持 `--name value` 与 `--name=value`；短选项（`-o`、`-w`、`-C`、`-j`）取下一个 token。
- `--` 结束选项解析；其后 token 一律按位置参数处理。
- `--help`、`--version` 写 **stdout** 并 exit 0，命中后不再处理其余参数。
- `moteasm` 与 `motedis` 的选项可位于位置参数之前或之后；`motear` 要求子命令是第一个参数，
  选项位于子命令之后；`motevm` 的 extern 选项必须位于入口模块之前（见 §8.4）。
- 单独的 `-` 只在 `moteasm`/`motedis` 中表示 stdin 或 stdout，不作为选项。

### 2.2 诊断

所有诊断写 **stderr**；正常数据写 stdout。不使用 ANSI 颜色，重定向后输出确定。

| 工具 | 诊断格式 |
|---|---|
| `moteasm` | `moteasm: [path:line:col: ]<error|warning>: <code>: <message>`，末尾 `moteasm: N error(s), M warning(s)` |
| `motedis` | `motedis: <path>: error: <message>`；告警 `motedis: warning: <message>` |
| `motevm` | `error: <message>` / `note: <message>`（无工具前缀） |
| `motear` | `motear: <file>: <message>` 或 `motear: <具体错误>` |
| `builder` | `error: <message>`，用法错误打印到 stderr |

示例：

```text
$ moteasm bad.mta
moteasm: bad.mta:3:5: error: E003: unknown mnemonic 'foo'
moteasm: 1 error(s), 0 warning(s)

$ motedis bad.mbc
motedis: bad.mbc: error: invalid header, section table or checksum
```

### 2.3 stdout、stderr 与确定性

- 程序的正常结果与工具的数据输出写 stdout；诊断、提示、trace（未重定向时）与统计写 stderr。
- 输出不含时间戳、主机地址或随机量；同一输入产生逐字节相同的输出。
- 工具产物（`.mbc`、反汇编文本、`.mtr`、解包条目）先完整构建到内存，再通过
  “写临时文件 + 原子替换”落盘；失败不留下半成品。`motevm --trace-file` 直接截断写目标文件，不经过原子替换。
- `motedis --annotate` 是非 canonical 调试输出；canonical 文本不含 PC 注释。

### 2.4 退出码理念

| 码 | 含义 |
|---|---|
| `0` | 成功 |
| `1` | 用法错误（选项、参数、互斥组合、输入=输出） |
| `2` | 输入/校验错误（文件不可读、格式非法、汇编诊断、import 未链接） |
| `3` | 运行时陷阱（仅 `motevm`；`motear` 用于 I/O 错误） |
| `4` | 内部/环境错误（自检失败、写入失败、不变量错误） |

`builder` 使用另一套退出码：`0` 成功、`1` 构建失败、`2` 用法、`3` 环境/工具链。
逐工具细节见 §8.5。

## 3. moteasm

`moteasm` 把 self-ISA 的 `.mta` 文本汇编为 MOTEBC 1.0 `.mbc`。语言、指令与诊断码见
[self/moteasm.md](self/moteasm.md)、[01 · ISA 参考](01-isa.md) 与 [02 · MOTEBC 格式](02-bytecode-format.md)。

- 默认输出为输入文件扩展名替换成 `.mbc`；`-o -` 写 stdout。
- DEBUG 段默认在源包含 `.loc`/`.local` 时生成；`-g` 强制生成，`--no-debug` 强制省略。
- 成功时 stdout 无输出。失败时不创建或修改输出文件。
- `--listing FILE` 写文本 listing；`--max-errors N` 控制保留的诊断数量（默认 20，0 = 不限制
  请求但内部最多保留 1000 条，范围 0..100000）。
- `--warn-as-error` 把警告升级为失败。
- `--check-roundtrip` 在内存中反汇编并重新汇编，比较字节（见 §9）。

## 4. motedis

`motedis` 把合法 `.mbc` 渲染为 canonical 文本；输出可被 `moteasm` 重新汇编。

- 输入先经过与 `motevm` 相同的加载与校验；非法模块不产生输出。
- 默认写 stdout；`-o FILE` 原子写文件，`-o -` 写 stdout。
- `--no-debug` 省略 `.loc`/`.local`，因此不保证字节级往返。
- `--annotate` 在每条指令后追加 `; pc=<绝对字节偏移>`，仅用于调试。
- `--check-roundtrip` 在内存中重新汇编并比较字节（见 §9）。
- 详细规则见 [self/motedis.md](self/motedis.md)。

## 5. motevm

当前 `motevm` 使用位置参数形式，没有子命令：

```text
motevm [extern_parameters] <entry_mbc_module> [ext_mbc_modules] [intern_parameters]
```

- 只执行入口模块；入口之后连续的 `.mbc` token（大小写不敏感）作为外部模块加载，仅贡献常量池，不执行。
- 其余 token 是预留给脚本的 intern 参数，当前解析后忽略。
- `-s/--status` 在结束后把确定性统计写 stderr；`-d/--debug` 输出模块与停机摘要。
- `-i`/`--step`/`--break`/`--trace` 安装调试钩子；REPL 从 stdin 读命令，提示与结果写 stderr。
- `-w/--workspace DIR` 是唯一的目录授权；未给出时脚本只有 stdio 能力。
- 完整参考见 [self/cli.md](self/cli.md)。

## 6. motear

`motear` 读写 MTR 1.0 归档（`.mtr`）。容器布局、校验与 CRC 见 [07 · MTR 格式](07-mtr-format.md)。

### 6.1 子命令

```text
motear pack -o OUT.mtr [options] --module FILE[=NAME]... [--resource FILE[=NAME]]...
motear list FILE.mtr [--verbose] [--verify]
motear info FILE.mtr [--json]
motear verify FILE.mtr [--strict] [--no-modules]
motear unpack FILE.mtr -C DIR [--force] [--module NAME]... [--resource NAME]...
```

子命令必须是紧接程序名的第一个参数（选项不能放在子命令之前）；未给子命令或未知子命令 ⇒ exit 1。

### 6.2 `pack`

| 选项 | 说明 |
|---|---|
| `-o, --output FILE` | 输出归档，**必填** |
| `--module FILE[=NAME]` | 加入 MoteBC 模块，可重复；`NAME` 缺省为文件 basename |
| `--resource FILE[=NAME]` | 加入资源，可重复；`NAME` 缺省为文件 basename |
| `--entry NAME` | 入口模块名；多模块时必填 |
| `--name NAME` | 包名，缺省为输出文件 stem |
| `--package-version V` | 包版本，缺省 `0.0.0` |
| `--description TEXT` | 描述 |
| `--author TEXT` | 作者 |
| `--license TEXT` | 许可证 |
| `--build-id TEXT` | 构建标识/来源信息 |

规则：

- 输出路径不得与任何 `--module`/`--resource` 输入相同。
- 单个模块且未给 `--entry` 时，writer 自动把该模块名写入 manifest key 3；多个模块未给 `--entry` ⇒ exit 2。
- `--entry` 必须指向一个模块条目；给出 `--entry` 但没有 `--module` ⇒ exit 2。
- 自动写入 `created-by` = `motear 0.1.0`。
- 单条输入上限 64 MiB；归档上限 256 MiB。超限 ⇒ exit 2。
- 成功时 stdout 输出 `wrote OUT.mtr (N module(s), M resource(s), S bytes)`。

### 6.3 `list`

默认只校验容器（头部、CRC、TOC、manifest、名称）；`--verify` 额外验证 MoteBC 模块
（帮助文本表述为 entry CRCs 与 modules）。`--verbose` 显示每条目的 offset 与完整 manifest。

```text
mtr 1.0 entries=2 modules=1 resources=1 size=1112
module        432  c8a557f5  hello.mbc
resource      371  6eb5bb10  src/hello.mta
```

`--verbose` 在条目后追加 `offset=...`，并追加：

```text
  package (1): demo
  version (2): 1.2.3
  entry (3): hello.mbc
  description (4): demo package
  author (5): tester
  license (6): MIT
  build-id (7): b42
  created-by (8): motear 0.1.0
```

### 6.4 `info`

输出头部与 manifest 的键值摘要；`--json` 输出固定键序的 JSON：

```json
{
  "file": "app.mtr",
  "format": "MTR 1.0",
  "size": 1112,
  "entries": 2,
  "modules": 1,
  "resources": 1,
  "package_name": "demo",
  "package_version": "1.2.3",
  "entry_module": "hello.mbc",
  "description": "demo package",
  "author": "tester",
  "license": "MIT",
  "build_id": "b42",
  "created_by": "motear 0.1.0"
}
```

`info` 不执行 MoteBC 验证；容器本身仍被完整校验。

### 6.5 `verify`

- 默认：完整容器校验 + 每个模块的 MoteBC 验证。
- `--no-modules`：只做容器校验，跳过 MoteBC 验证。
- `--strict`：在所选校验模式之外额外要求 manifest 具备包名、版本，且入口模块可唯一解析
  （可与 `--no-modules` 组合）。
- 成功输出：`ok: FILE (N entries, M modules, K resources)`。

### 6.6 `unpack`

| 选项 | 说明 |
|---|---|
| `-C, --directory DIR` | 解包目标目录，**必填**；不存在时创建 |
| `--force` | 覆盖已存在的**普通文件** |
| `--module NAME` | 只解指定模块，可重复 |
| `--resource NAME` | 只解指定资源，可重复 |

- 未给任何过滤器时解出全部条目；过滤器按归档内的完整名称精确匹配。
- 拒绝符号链接、非普通文件目标、越出目标目录的路径，以及已存在的 `<target>.tmp`。
- 成功输出：`extracted N entry(ies) to <绝对路径>`。

### 6.7 manifest 字段

manifest 是按 key 升序排列的 TLV 列表；已知 key：

| key | 名称 | 来源 |
|---|---|---|
| 1 | `package` | `--name`（缺省输出 stem） |
| 2 | `version` | `--package-version`（缺省 `0.0.0`） |
| 3 | `entry` | `--entry` 或唯一模块名 |
| 4 | `description` | `--description` |
| 5 | `author` | `--author` |
| 6 | `license` | `--license` |
| 7 | `build-id` | `--build-id` |
| 8 | `created-by` | 自动：`motear 0.1.0` |

### 6.8 退出码

| 码 | 场景 |
|---|---|
| `0` | 成功 |
| `1` | 用法错误 |
| `2` | 归档/模块校验失败、数据超限、目标已存在且未给 `--force`、解包路径不安全 |
| `3` | I/O 错误（读取/创建/写入失败） |
| `4` | 内部错误（当前实现预留；正常路径不产生） |

### 6.9 选项总表

| 选项 | 子命令 | 说明 |
|---|---|---|
| `-o, --output FILE` | `pack` | 输出归档（必填） |
| `--module FILE[=NAME]` | `pack` / `unpack` | pack：加入模块；unpack：按 `NAME` 过滤 |
| `--resource FILE[=NAME]` | `pack` / `unpack` | pack：加入资源；unpack：按 `NAME` 过滤 |
| `--entry NAME` | `pack` | 入口模块 |
| `--name NAME` | `pack` | 包名 |
| `--package-version V` | `pack` | 包版本 |
| `--description TEXT` | `pack` | 描述 |
| `--author TEXT` | `pack` | 作者 |
| `--license TEXT` | `pack` | 许可证 |
| `--build-id TEXT` | `pack` | 构建标识 |
| `-C, --directory DIR` | `unpack` | 解包目标目录（必填） |
| `--force` | `unpack` | 覆盖已存在的普通文件 |
| `--verbose` | `list` | 显示 offset 与 manifest |
| `--verify` | `list` | 额外验证模块 |
| `--json` | `info` | JSON 输出 |
| `--strict` | `verify` | 要求包名、版本与可解析入口 |
| `--no-modules` | `verify` | 跳过 MoteBC 验证 |
| `-h, --help`、`-v, --version` | 全部 | 帮助/版本 |
| `--` | 全部 | 结束选项解析 |

## 7. builder

`builder` 是单翻译单元的构建驱动，不读取 `CMakeLists.txt`：

```text
builder [build] [options] [target...]
builder clean | rebuild | self | doctor | targets | help
```

| 命令 | 行为 |
|---|---|
| `build`（默认） | 编译并链接选中目标 |
| `clean` | 删除 builder 生成的输出与状态，不触碰 CMake 文件 |
| `rebuild` | `clean` 后 `build` |
| `self` | 用当前 builder 重新构建 `builder` 自身 |
| `doctor` | 打印工具链与构建配置 |
| `targets` | 列出目标、源与链接关系 |
| `help` | 用法 |

| 选项 | 说明 |
|---|---|
| `-j, --jobs N` | 并行编译任务数（默认硬件并发） |
| `--compiler X` | `clang` / `gcc` / `msvc`（默认优先 clang++） |
| `--arch X` | `x86` / `x64`（默认宿主架构） |
| `--release` / `--debug` | 配置，默认 Release |
| `--cxx PATH` | 显式 C++ 编译器，按文件名识别 family |
| `--build-dir DIR` | 输出根，默认 `<root>/build` |
| `--root DIR` | 项目根，默认自动查找 `src/motevm` + `src/builder` |
| `--werror` | 警告视为错误 |
| `--sanitize` | ASan + UBSan（MSVC 仅 ASan） |
| `--force` | 忽略缓存全量重建 |
| `-n, --dry-run` | 只打印命令 |
| `-v, --verbose` | 打印缓存命中与完整命令行 |
| `--version` | 版本 |
| `-h, --help` | 帮助 |

退出码：`0` 成功、`1` 构建失败、`2` 用法、`3` 环境/工具链。构建细节见
[06 · 构建与发布](06-build.md)。

## 8. CLI 设计

### 8.1 共同约定

- 工具名即可执行文件名；Windows 下带 `.exe`。
- `--help`/`--version` 写 stdout、exit 0。
- 诊断写 stderr，数据写 stdout；无颜色控制。
- 路径按命令行原样回显，不做绝对化（`motevm -d` 的工作区 note 例外，显示规范化根）。
- 数值参数按十进制解析，`0` 的含义由各选项定义；`motevm --trace-limit` 额外接受 `0x` 前缀。
- 共享机械层位于 `src/include/common/`（`TOOL` 命名空间），只处理选项匹配/取值、版本与
  usage 头；各工具的选项、帮助文本和位置参数规则保持独立。

### 8.2 `moteasm`

```text
moteasm [options] <input.mta>
```

| 选项 | 说明 |
|---|---|
| `-o, --output FILE` | 输出 `.mbc`；缺省为输入扩展名替换 `.mbc`；`-` = stdout |
| `-g, --debug` | 强制生成 DEBUG 段 |
| `--no-debug` | 省略 DEBUG 段；与 `-g` 同给 ⇒ exit 1 |
| `--listing FILE` | 写文本 listing |
| `--max-errors N` | 最多保留 N 条错误（默认 20，范围 0..100000；0 = 不限请求，内部上限 1000 条） |
| `--warn-as-error` | 警告视为失败 |
| `--check-roundtrip` | 汇编后反汇编再汇编，比较字节（RT-1） |
| `--version`, `--help` | 版本/帮助；写 stdout，exit 0 |

- `input` 为 `-` 时必须显式给出 `-o`。
- `input` 与 `-o`（均非 stdio token）为同一路径 ⇒ exit 1。
- 成功时 stdout 无输出。

### 8.3 `motedis`

```text
motedis [options] <input.mbc>
```

| 选项 | 说明 |
|---|---|
| `-o, --output FILE` | canonical 文本输出；缺省 stdout；`-` = stdout |
| `--no-debug` | 省略 `.loc`/`.local` |
| `--annotate` | 每条指令追加 `; pc=<8 位十六进制>`（非 canonical） |
| `--check-roundtrip` | 内存中重新汇编并比较字节（RT-1） |
| `--version`, `--help` | 版本/帮助；写 stdout，exit 0 |

- `--annotate` 或 `--no-debug` 与 `--check-roundtrip` 互斥 ⇒ exit 1。
- 输入为 `-` 时从 stdin 读取。
- 输入与输出为同一路径（均非 stdio token）⇒ exit 1。
- 非法模块 ⇒ exit 2，不写输出。

### 8.4 `motevm`

```text
motevm [extern_parameters] <entry_mbc_module> [ext_mbc_modules] [intern_parameters]
```

| 选项 | 说明 |
|---|---|
| `-v, --version` | 打印版本，exit 0 |
| `-h, --help` | 打印用法到 stdout，exit 0 |
| `-d, --debug` | 输出 `note:` 摘要与停机上下文 |
| `-r, --release` | 仅错误（默认）；与 `-d` 同给时后者生效 |
| `-s, --status` | 结束后把统计写 stderr（见 [self/cli.md](self/cli.md) §6） |
| `-i, --interactive` | 在入口第一条指令前暂停并进入 REPL |
| `--step` | 在第一条指令前暂停（隐含 `--interactive`） |
| `--break SPEC` | 设置断点，可重复；语法见 [self/cli.md](self/cli.md) §11.1 |
| `--trace LVL` | `off`（默认）/`call`/`exec`/`full` |
| `--trace-file PATH` | trace 目标；缺省 stderr，`-` = stdout |
| `--trace-limit N` | 最多记录 N 行；0 = 不限 |
| `-w, --workspace DIR` | 唯一目录授权；缺省仅 stdio；重复给出时后者生效 |

位置规则：

- extern 选项必须位于入口模块之前；缺失入口模块或未知选项 ⇒ exit 1。
- 入口模块是第一个非选项 token；其后的连续 `.mbc` token（后缀大小写不敏感）是外部模块。
- 其余 token 是 intern 参数，原样保存、当前忽略；入口之后的 `-` 开头 token 不会被当作选项。
- `--` 结束 extern 段，其后的第一个 token 即使以 `-` 开头也作为入口模块。
- `--break`、`--trace-file` 缺值/空值，`--trace`、`--trace-limit` 取值非法 ⇒ exit 1。
- `-w DIR` 原样取下一个 token；缺值或 `--workspace=` 空值 ⇒ exit 1。

### 8.5 输出与退出码

| 工具 | stdout | stderr |
|---|---|---|
| `moteasm` | 成功无输出；`-o -` 时输出 `.mbc` 字节 | 诊断、I/O 错误 |
| `motedis` | canonical 文本；或 `-o -` | 警告、错误 |
| `motevm` | 脚本输出（`print`/`print_str`）；`--trace-file -` 时 trace；帮助/版本 | `note:`/`error:`、REPL、默认 trace、`--status` |
| `motear` | pack/list/info/verify/unpack 报告 | 错误 |
| `builder` | 构建进度/报告、`targets`/`doctor`/help | 错误与用法 |

| 工具 | `0` | `1` | `2` | `3` | `4` |
|---|---|---|---|---|---|
| `moteasm` | 成功 | 用法 | 源/输入错误 | — | 内部、I/O、round-trip 自检失败 |
| `motedis` | 成功 | 用法 | 读取/解析/校验失败 | — | 写入、round-trip 或内部错误 |
| `motevm` | 成功（含调试器 `quit`） | 用法 | 加载/校验/工作区/断点/trace 文件错误 | 运行时 trap | 内部错误（注册失败、内存初始化失败等） |
| `motear` | 成功 | 用法 | 归档/校验/数据错误 | I/O 错误 | 内部错误（声明为保留） |
| `builder` | 成功 | 构建失败 | 用法 | 环境/工具链 | — |

`motevm` 的 trap 诊断形如：

```text
error: trap 3 (TRAP) at pc=8
```

其中 `pc` 是执行统计的 `lastPc`；`-d` 的停机上下文使用出错指令地址 `errorPc`（见
[self/cli.md](self/cli.md) §7）。

### 8.6 内置宿主函数

CLI 在加载模块前注册以下宿主函数。名字按字节精确匹配，arity 必须与 `.import` 声明一致。

始终注册：

| 名字 | arity | 行为 |
|---|---|---|
| `print` | 1 | 按有符号 32-bit 十进制写参数 + `\n` 到 stdout |
| `print_str` | 2 | 写 `(addr, len)` 指定的原始字节 + `\n` 到 stdout |
| `eprint_str` | 2 | 同 `print_str`，写 stderr |
| `total_prints` | 0 | 返回 `print`/`print_str`/`eprint_str` 累计行数 |
| `const_addr` | 1 | 返回当前模块常量 payload 的地址，失败 `0xFFFFFFFF` |
| `const_len` | 1 | 返回当前模块常量 payload 长度，失败 `0xFFFFFFFF` |
| `mem_alloc` | 1 | 8 字节对齐 bump 分配，返回地址，失败 `0xFFFFFFFF` |
| `mem_free` | 1 | no-op，返回 `0`；非法参数返回 `-5` |

仅在给出 `-w/--workspace` 时注册：

| 名字 | arity | 行为 |
|---|---|---|
| `file.read` | 4 | `(pathAddr, pathLen, bufAddr, maxLen)` → 读取字节数（0 = EOF） |
| `file.write` | 4 | `(pathAddr, pathLen, dataAddr, dataLen)` → 写入字节数（截断写） |
| `file.append` | 4 | 同上，追加 |
| `file.exists` | 2 | `(pathAddr, pathLen)` → `0`/`1` |
| `file.size` | 2 | `(pathAddr, pathLen)` → 字节数；非普通文件 ⇒ `-3` |
| `file.remove` | 2 | `(pathAddr, pathLen)` → `0`；仅普通文件 |

未给出 `-w` 时引用 `file.*` 的模块在加载期失败（import 未链接，exit 2）。完整参数语义、
副作用与错误码见 [self/cli.md](self/cli.md) §10；stdout 行缓冲并在每次 `print*` 后 flush。

### 8.7 stdio ABI 与负错误码

当前 ABI 是**过渡实现**：寄存器是 32-bit 原始值，没有 `Value`/`String`，字节串以
`(address, length)` 指向 4 GiB 线性内存。地址来自 `const_addr`（常量池）或 `mem_alloc`
（堆）。返回值按 `dword` 解释：非负值成功，负值（补码）是错误码。

| 值 | 含义 |
|---|---|
| `0` | 成功 |
| `-3` | 系统 I/O 错误（打开/读/写/删除失败、父目录不存在、目标不是普通文件） |
| `-4` | 未授权（路径越出工作区，或没有工作区） |
| `-5` | 参数非法（path 长度为 0 或 > 4096、含 NUL、缓冲区越界、单次 I/O > 16 MiB；适用函数的 arity 不符） |

查询类函数（`const_addr`、`const_len`、`mem_alloc`）在失败时返回 `0xFFFFFFFF`（即
`NO_INDEX`），而不是负错误码。

限制：

| 项 | 值 |
|---|---|
| 线性内存 | 4 GiB |
| 内存块 | 64 KiB |
| 单次读写 | ≤ 16 MiB |
| 路径字节 | 1..4096，且不含 NUL |
| `mem_alloc` 单次 | 1..256 MiB；总量受 4 GiB 限制 |
| 文件打开模式 | 二进制，不做换行转换 |

当前 CLI **没有** `io.*` 导入；stdio 能力即上表中的 `print`/`print_str`/`eprint_str`。
该 ABI 会随值模型演进调整；文档与 `.import` 以当前 `--help`/源码为准。

### 8.8 工作区与能力模型

`-w/--workspace DIR` 是 `motevm` 唯一的目录授权：

| 状态 | 模块文件 | 脚本能力 |
|---|---|---|
| 未给出 `-w`（默认） | 按命令行 token 原样打开（相对 cwd） | 仅 stdio；不授予任何目录 |
| 给出 `-w DIR` | 相对路径以 `DIR` 为基准；绝对路径原样；两者规范化后必须落在 `DIR` 内 | 读写都只能落在 `DIR` 内 |

- 根在加载任何模块之前规范化并校验为存在的目录；失败 ⇒ exit 2。
- 越界按路径组件比较（Windows 大小写不敏感），`..` 规范化后仍在根内则允许。
- 空路径、含 NUL，以及 Windows 上的盘符相对（`C:foo`）与根相对（`\foo`）形式 ⇒ `invalid path`。
- 写侧要求目标父目录已存在且仍在根内。当前 `motevm` 的写路径是 `--trace-file` 与
  `file.*`。
- 每个模块路径在打开前解析；越界 ⇒ exit 2，且不执行任何字节码。

## 9. 往返保证

| 级别 | 条件 | 验证方式 |
|---|---|---|
| RT-1 字节一致 | 规范布局的 `.mbc`（`moteasm` 产物） | `moteasm --check-roundtrip`、`motedis --check-roundtrip` |
| 幂等 | canonical 文本是不动点 | `dis(asm(dis(bc))) == dis(bc)` |
| RT-2 可执行 | 任意合法模块重新汇编后可执行 | 反汇编 → 汇编 → `motevm` |
| RT-3 结构 | 名称/索引重编号后结构等价 | `#n` 回退 + 结构断言 |

- `moteasm --check-roundtrip`：汇编 → canonical 反汇编 → 再汇编 → 比较字节；失败 exit 4。
- `motedis --check-roundtrip`：反汇编 → 内存中重新汇编 → 比较字节；失败 exit 4，并报告首个差异。
- `--no-debug` 丢弃 DEBUG 段，因此只保证语义（RT-2），不保证字节一致。
- 非规范布局（FUNCS 顺序不同于 CODE 顺序）或名称回退（非标识符/重复字符串常量）会在
  stderr 告警；此时 RT-1 不保证。
- `--annotate` 不是 canonical 文本，不能参与往返。
