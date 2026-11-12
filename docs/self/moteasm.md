# moteasm 汇编器

[简体中文](moteasm.md) | [English](../en/self/moteasm.md)

`moteasm` 把 UTF-8 源文件（`.mta`）汇编为 MOTEBC 1.0 模块（`.mbc`）。它只接受当前实现的自有 ISA 指令，指令语义见 [01 · ISA 参考](../01-isa.md)，容器布局见 [02 · 字节码格式](../02-bytecode-format.md)。工具链总览见 [05 · 工具链](../05-toolchain.md)。工具版本 0.1.0；项目使用 C++20，MIT 许可。

```text
foo.mta (UTF-8) --moteasm--> foo.mbc (MOTEBC 1.0) --motevm--> stdout/trap
```

## 1. 源文件规则

| 规则 | 说明 |
|---|---|
| 编码 | UTF-8；可选 BOM；LF 或 CRLF |
| 大小 | 不超过 64 MiB（`E002`） |
| 注释 | `;` 到行尾；没有块注释 |
| 分隔 | 空格、制表符和逗号都作为分隔符 |
| 行结构 | `[label:] [directive\|instruction] [; comment]`；每行最多一条语句 |
| 大小写 | 助记符和指示符大小写不敏感；标识符、名称、标签大小写敏感 |
| 空文件 | 合法，但没有任何 `.func` 会报 `E020` |

## 2. 词法

### 2.1 标识符与保留字

- 标识符：`[A-Za-z_][A-Za-z0-9_]*`。
- 以下保留字（大小写不敏感）不能作为名称：`auto`、`const`、`true`、`false`、`null`、`nan`、`inf`。
- 名称和标签区分大小写：`Main` 与 `main` 不同。

### 2.2 整数与浮点字面量

- 整数：可选 `+`/`-`，十进制、`0x`/`0X` 十六进制、`0b`/`0B` 二进制；可在数字之间使用 `_` 分隔。解析范围为 64 位有符号整数。
- 浮点：十进制或科学计数法，可带结尾 `f`/`F`；也接受 `nan`、`+nan`、`-nan`、`inf`、`+inf`、`-inf`、前导小数点形式（如 `.5`），以及 `f64=0x<16 位十六进制>` 表示的原始 64 位位模式。
- `NaN` 常量由固定位模式生成，会触发警告 `W005`。
- 常量池中的浮点值是 64 位 double payload；当前 ISA 没有“加载常量”指令，浮点寄存器值需要由程序自行构造。

### 2.3 字符串与转义

字符串用双引号包围，内容按字节保存，可以直接包含 UTF-8。支持的转义：

| 转义 | 含义 |
|---|---|
| `\\` | 反斜杠 |
| `\"` | 双引号 |
| `\n` | 换行（0x0A） |
| `\r` | 回车（0x0D） |
| `\t` | 制表符（0x09） |
| `\0` | NUL（0x00） |
| `\xHH` | 恰好两个十六进制数字表示的字节 |
| `\u{H...}` | 1..6 个十六进制数字表示的 Unicode 标量；禁止代理项和大于 U+10FFFF 的值 |

未知转义、未闭合字符串、非法 `\x` / `\u{}` 分别报 `E009`、`E008`、`E009`。

### 2.4 寄存器、索引与标签

- 寄存器写作 `r0`..`r255`（前缀 `r` 或 `R`，编号按整数语法解析；通常写十进制）。语义上编号必须小于所在函数声明的 `regs`，否则 `E022`。
- 原始索引写作 `#n`，范围 `0..65535`（`E024`），含义取决于位置：
  - CONST 常量索引：`.import` / `.global` / `.func` 的名称、`.local` 名称、`.global` init（init 可指向任意常量项）；
  - FUNCS 索引：`.entry #n`、`CALL rDest, #n`；
  - IMPORTS 索引：`HOSTCALL rDest, #n`。
- 标签写作 `name:`，只能出现在 `.func` 内，每行至多一个。标签名可以是普通标识符（不能是保留字），也可以是 `.L` 后跟 `[A-Za-z0-9_]+` 的局部形式（如 `.L0`、`.L_loop`）。标签作用域是**当前函数**。

## 3. 指示符

### 3.1 指示符表

| 指示符 | 语法 | 说明 |
|---|---|---|
| `.module` | `.module <string\|word>` | 模块名；必须是第一条语句，最多一次 |
| `.source` | `.source <string\|word>` | `.meta 2, ...` 的别名；源文件名；绝对路径触发 `W006` |
| `.meta` | `.meta <u16>, <string\|word>` | 元数据键值；键 1 为模块名，2 源文件，3 build ID，4 注释；键 5..65535 保留并触发 `W003` |
| `.const` | `.const <literal>` | 显式常量模式；索引等于声明顺序 |
| `.string` | `.string <name> <string>` | 命名常量：字符串 |
| `.int` | `.int <name> <integer>` | 命名常量：整数 |
| `.float` | `.float <name> <number>` | 命名常量：浮点（整数按 double 转换） |
| `.bool` | `.bool <name> true\|false` | 命名常量：布尔 |
| `.null` | `.null <name>` | 命名常量：null |
| `.import` | `.import <name\|#n> <arity> [, flags=0x00]` | IMPORTS 表项；名称必须是 String 常量；arity 0..16；flags 必须为 0 |
| `.global` | `.global <name\|#n\|anonymous> [const] [<init>]` | GLOBALS 表项；init 为字面量或 `#CONST索引` |
| `.entry` | `.entry <name\|#n>` | 入口 FUNCS 索引；目标 arity 必须为 0 |
| `.func` | `.func <name\|#n\|anonymous> <arity> <regs\|auto> [, flags=0xNN]` | 开始函数块；另有 `.func <name> <regs>` 兼容形式（arity=0） |
| `.end` | `.end` | 结束当前函数块；必需，无操作数 |
| `.loc` | `.loc ["<file>",] <line>[, <column>]` | DEBUG 行号表；首个 `.loc` 必须在首条指令前 |
| `.local` | `.local <name\|#n>, <reg>, [param], [start=<pc\|label>], [end=<pc\|label>]` | DEBUG 局部变量 |

`#n` 形式的名称（`.import` / `.global` / `.func` / `.local`）必须引用 Const 区中的 String 常量；`.global` 的 init `#n` 则引用任意常量项。`.entry #n` 是 FUNCS 索引，不是常量索引。

### 3.2 声明顺序

违反顺序报 `E018`。合法结构：

```text
[.module]
{.source | .meta}*
( {.const}* | {.string|.int|.float|.bool|.null}* )
{.import | .global}*
[.entry]
{.func ... .end}+
```

- `.module` 必须在所有其它语句之前（注释和空行不算语句）。
- `.source` / `.meta` 必须在常量之前。
- 常量必须在 `.import` / `.global` 之前。
- `.import` 与 `.global` 可交错，但都必须在 `.entry` / `.func` 之前。
- `.entry` 在 `.func` 之前，最多一次。
- 函数块不能嵌套；每个 `.func` 必须有对应的 `.end`。
- 标签、指令、`.loc`、`.local` 只能位于 `.func` 与 `.end` 之间。

### 3.3 常量池模式

两种模式不能混用（`E016`）：

**显式模式**（出现 `.const` 时）

- 每个 `.const <literal>` 按声明顺序追加，索引从 0 开始。
- 引用只能指向已声明的常量；未声明报 `E017`。
- 允许重复声明相同值，它们占据不同索引。
- `motedis` 生成的 canonical 文本使用此模式。

**命名模式**（出现 `.string` / `.int` / `.float` / `.bool` / `.null` 时）

- 命名常量声明按声明顺序进入常量池；声明之间不按值去重，相同值也各占一项。
- 之后的名称和 init 字面量按内容复用：若池中已有 `kind + 值` 相同的项，则复用它的索引。
- 常量池索引按以下顺序首次出现分配：
  1. 命名常量声明（声明顺序）
  2. `.import` 名称
  3. `.global` 名称
  4. `.func` 名称
  5. `.global` init 字面量
  6. `.local` 名称
- 两种模式都没出现时，默认按命名模式处理名称。

`.const` 接受的 literal 包括字符串、整数、浮点、`true` / `false`、`null`、`nan` / `inf` 以及 `f64=0x...`。

### 3.4 函数与入口

- `.func` 的名称可以是标识符、`#FUNCS索引` 或 `anonymous`。
- arity 范围 `0..16`（`E021`）；`regs` 范围 `1..256`（`E020`），或写 `auto`。
- `auto` 把 `regs` 设为函数中实际使用的最大寄存器编号 + 1（也考虑 `.local` 的 slot），最小为 1；超过 256 报 `E020`。
- 兼容形式 `.func <name> <regs>` 等价于 arity=0、regs 指定值。
- `flags=` 只定义 bit0（exported）；其它位报 `E033`。
- 函数体不能为空，最后一条指令必须是 `JMP`、`RET`、`TRAP` 或 `HALT`（`E019`）。
- 省略 `.entry` 时入口函数索引为 0。`.entry` 的名称必须对应一个命名函数（`anonymous` 这个名称本身不可用，但可用 `#索引` 指向匿名函数），且目标 arity 必须为 0（`E026`）。
- 函数按声明顺序进入 FUNCS 表；`entry_pc` 由前序函数长度累加得到。

### 3.5 调试信息

- `.loc` 只能出现在 `.func` 内。首个 `.loc` 必须早于函数的第一条指令，否则 `E034`。
- 行号 `1..2^31-1`；列号是可选的 32 位无符号数。
- 只在 `(line, column)` 发生变化时生成记录；`pc` 是函数内相对指令索引，第一条记录必须是 `pc=0`。
- `.local` 的 slot 必须小于 `regs`；`param` 的 slot 必须小于 `arity`。`start=` / `end=` 接受数字 pc 或当前函数内的标签；这里的 pc 是函数内相对指令索引（与 `.loc` 相同）。默认 start=0、end=函数长度。
- `.loc` 里的文件名在所有函数中必须一致（`E033`）；绝对路径触发 `W006`。

## 4. 指令语法

助记符大小写不敏感。操作数格式与 [ISA §2.2](../01-isa.md) 一致。

| 格式 | 操作数 | 本 ISA 中的助记符 |
|---|---|---|
| `op` | 无 | `HALT`, `BREAKPOINT` |
| `r` | `rA` | `ZERO`, `RET` |
| `rr` | `rA, rB` | `MOV`, `NEG`, `BNOT`, `FNEG`, `FABS`, `FSQRT`, `I2F`, `F2I` |
| `rr8` | `rA, rB, imm8` | `LOAD`, `STO`（imm8 为 0..255） |
| `rrr` | `rA, rB, rC` | `ADD`/`SUB`/`MUL`/`DIV`/`MOD`/`ADDU`/`SUBU`/`MULU`/`DIVU`/`MODU`, `BAND`/`BOR`/`BXOR`, `SHL`/`SHR`/`SAR`, `FADD`/`FSUB`/`FMUL`/`FDIV`, `FEQ`/`FNE`/`FLT`/`FLE`/`FGT`/`FGE`, `EQ`..`GEU` |
| `ri16` | `rA, imm16` | `LLO`, `LHI`（接受 `-32768..65535`） |
| `ri16` | `rA, target` | `JZ`, `JNZ` |
| `i24` | `target` | `JMP` |
| `i24` | `code` | `TRAP`（1..16 或陷阱名） |
| `call16` | `rDest, <func\|#idx>` | `CALL` |
| `call16` | `rDest, <import\|#idx>` | `HOSTCALL` |

- 跳转目标可以是当前函数内的标签，也可以是 `+n` / `-n` 字节偏移；偏移必须是 4 的倍数，且目标必须在当前函数内。
- `CALL` 的目标是函数名或 `#FUNCS索引`；`HOSTCALL` 的目标是导入名或 `#IMPORTS索引`。
- `TRAP` 接受数字 `1..16` 或名称：`TypeMismatch`、`DivByZero`、`IntegerOverflow`、`NumericConversion`、`IndexOutOfBounds`、`NullAccess`、`StackOverflow`、`InvalidBytecode`、`UnlinkedImport`、`OutOfMemory`、`HostError`、`FuelExhausted`、`Interrupted`、`NotImplemented`、`InternalError`、`InvalidArgument`。
- `LLO` / `LHI` 的立即数按原始 16 位编码；canonical 输出为 `0x%04X`。

示例：

```asm
.func main 0 8
    llo r0, 0x1234
    lhi r0, 0x0001          ; r0 = 0x00011234
    load r1, r0, 4
    sto r1, r0, 8
    add r2, r0, r1
    jz r2, done
    llo r2, 0
done:
    trap DivByZero          ; stop with trap code 2
.end
```

## 5. 标签与两遍汇编

1. **Pass 1**：逐行分词和解析，建立模块、常量、导入、全局、函数、标签和指令模型；分配函数 `entry_pc` 和函数内 pc；记录 `.loc` / `.local`；检查结构、终止指令、arity/regs/flags、重复标签等。
2. **Pass 2**：解析名称和常量，校验表索引、寄存器、入口和调试区间；编码指令并回填跳转/调用目标；拼装各段、计算 CRC、原子写出。
3. **自检**：生成的镜像必须通过 `Parse` / `Verifier`；失败报 `E031` 并以内部错误退出。

标签在 Pass 2 解析，因此前向引用合法。标签作用域是所在函数；引用其它函数的标签报 `E025`，完全不存在的标签报 `E011`。相对偏移 `+n` / `-n` 以字节计，必须是 4 的倍数（`E012`）。

## 6. 输出、CODE 对齐与 DEBUG

- CODE 段必须为 8 字节的倍数。若所有函数的指令总数为奇数，在 CODE 末尾追加一条 `NOP`（`0x00000000`）。该 `NOP` 不属于任何函数，不计入 `FUNCS.instr`。
- 默认输出路径为输入文件名替换扩展名为 `.mbc`；`-o -` 写 stdout。
- `--listing FILE` 输出每条指令的函数名、FUNCS 索引、函数内 pc、编码字和源文本，以及常量/函数/全局/导入统计。
- DEBUG 段：
  - 没有 `.loc` / `.local` 时不生成 DEBUG 段。
  - `-g, --debug` 强制生成 DEBUG 段（即使没有记录，也写 8 字节头）。
  - `--no-debug` 丢弃所有记录且不生成 DEBUG 段；不能与 `-g` 同时使用。
  - `.local` 的 `FUNCS.local_count` 是不同 slot 的数量；每个函数调试块的偏移按 8 字节对齐。

## 7. 命令行

```text
usage:
  moteasm [options] <input.mta>

options:
  -o, --output FILE   output .mbc (default: input with .mbc; '-' = stdout)
  -g, --debug         force a DEBUG section
      --no-debug      omit the DEBUG section
      --listing FILE  write a listing
      --max-errors N  stop storing after N errors (0 = unlimited, default 20)
      --warn-as-error warnings are failures
      --check-roundtrip  re-assemble the canonical text and compare bytes
      --version | --help

exit codes:
  0 success   1 usage   2 input/source error   4 internal or I/O error
```

补充说明：

- 也接受 `-h` 作为 `--help`、`-v` 作为 `--version`。
- `--max-errors 0` 表示不设用户上限；诊断收集器最多保存 1000 条记录。
- 输入 `-` 表示 stdin；此时必须显式指定输出，除非输出也是 `-`。
- 输入和输出不能是同一路径（usage 错误）。
- `--check-roundtrip` 失败归入内部错误，退出码 4。

## 8. 诊断码

格式：

```text
moteasm: <path>:<line>:<col>: error: E012: message
moteasm: <path>:<line>:<col>: warning: W001: message
moteasm: N error(s), M warning(s)
```

没有源位置的诊断省略 `path:line:col`。诊断按行、列、码排序并去重。

### 8.1 错误

| 码 | 含义 |
|---|---|
| `E001` | 词法/语法错误：非法 UTF-8、非法名称或标签、意外字符串、单行多个标签 |
| `E002` | 源文件超过 64 MiB |
| `E003` | 未知/保留助记符，或非法的 `i24` 指令 |
| `E004` | 未知指示符 |
| `E005` | 非法字面量（浮点、布尔等） |
| `E006` | 非法整数/数值字面量 |
| `E007` | 寄存器语法错误或编号不在 `0..255` |
| `E008` | 字符串未闭合 |
| `E009` | 非法字符串转义 |
| `E010` | 立即数超出该指令允许范围 |
| `E011` | 名称/标签引用错误：未知标签、函数或导入，操作数类型不符 |
| `E012` | 跳转目标错误：偏移非 4 的倍数、目标超出函数、偏移放不进 int16/int24 |
| `E013` | 操作数个数或形式错误；重复 `flags=` / `start=` / `end=` |
| `E014` | 名称非法：保留字、常量名不是标识符、名称不能是字符串 |
| `E015` | 常量池超过 65536 项 |
| `E016` | 显式 `.const` 与命名常量声明混用 |
| `E017` | 显式模式引用未声明的常量 |
| `E018` | 声明顺序或位置错误 |
| `E019` | 函数体为空，或最后一条指令不是终止指令 |
| `E020` | 模块没有函数；或 regs 非法、`auto` 超过 256 |
| `E021` | arity 超出 `0..16` |
| `E022` | 寄存器/arity 约束不满足：操作数编号 >= regs、arity > regs、被调方/导入 arity 超出调用者 regs、局部变量 slot >= regs |
| `E023` | 数值/索引/范围错误：陷阱码、meta 键、名称长度、原始索引、局部变量区间、行列、init 索引等 |
| `E024` | 需要 `#n` 原始索引，或索引超出 `0..65535` |
| `E025` | 标签只存在于其它函数（跨函数引用） |
| `E026` | `.entry` 目标问题：索引越界、找不到、anonymous 或 arity 非 0 |
| `E027` | `.func` / `.end` 结构错误：嵌套 `.func`、缺少 `.end`、孤立 `.end` |
| `E030` | 重复声明：模块名、meta 键、entry、标签、常量、函数、导入或全局名 |
| `E031` | 内部一致性/发射失败：自检失败、段对齐/大小错误、模块超过 4 GiB |
| `E033` | flags 或源文件名冲突：非法 flags、FUNCS 仅 bit0、IMPORTS flags 非 0、多个不同源文件名 |
| `E034` | DEBUG 行号表错误：首个 `.loc` 晚于首条指令、未从 pc 0 开始、pc 不递增、条目非法 |

当前实现不使用 `E028`、`E029`、`E032`。

### 8.2 警告

| 码 | 含义 |
|---|---|
| `W001` | 终止指令之后存在不可达指令 |
| `W003` | 未知 meta key，原样保留 |
| `W005` | NaN 常量由固定位模式生成 |
| `W006` | 使用了绝对源路径，降低可复现性 |
| `W007` | `.global` 显式 `null` init 与省略 init 的哨兵值不同 |

当前实现不产生 `W002`、`W004`。`--warn-as-error` 会把任何警告视为失败。

## 9. 完整示例

以下程序与仓库中的 [hello.mta](../examples/hello.mta) 相同。三个导入由 `motevm` 的内置宿主函数绑定。

```asm
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

```text
$ moteasm hello.mta -o hello.mbc
$ motevm hello.mbc
hello, mote
```

`const_addr` 和 `const_len` 取得字符串常量的地址与长度，`print_str` 按“地址、长度”打印；`r0` 中的 0 选择模块中索引 0 的字符串常量 `"hello, mote"`。

## 10. 往返校验

```text
motedis hello.mbc -o hello.canon.mta
moteasm hello.canon.mta -o hello2.mbc
```

`moteasm --check-roundtrip` 会在汇编成功后自动执行同等的“反汇编为 canonical 文本、再汇编、逐字节比较”流程；任何差异都会报告首个不同字节并以退出码 4 失败。canonical 文本格式见 [motedis](motedis.md)，容器与段布局见 [02 · 字节码格式](../02-bytecode-format.md)。
