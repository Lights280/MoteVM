# motevm 命令行接口

[简体中文](cli.md) | [English](../en/self/cli.md)

本文描述 `motevm` 当前实现的位置参数 CLI、`--status` 输出、内置宿主函数、调试器与指令 trace。
工具的总体约定与退出码哲学见 [05 · 工具链参考](../05-toolchain.md) §8。

## 1. 语法

```text
motevm [extern_parameters] <entry_mbc_module> [ext_mbc_modules] [intern_parameters]
```

| 段 | 位置 | 含义 |
|---|---|---|
| `extern_parameters` | 入口模块之前 | 宿主级选项，见 §2 |
| `<entry_mbc_module>` | 第一个非选项 token | 唯一被执行的入口模块 |
| `[ext_mbc_modules]` | 入口之后连续以 `.mbc` 结尾的 token | 外部模块：加载并写入常量池，**不执行** |
| `[intern_parameters]` | 其余 token | 预留给脚本，当前解析后忽略 |

- `.mbc` 后缀大小写不敏感；长度必须大于 4（单独的 `.mbc` 不算模块 token）。
- 入口模块本身不要求以 `.mbc` 结尾。
- 选项只在外段解析；入口之后的 `-` 开头 token 属于 intern 参数，不会被当作选项。
- `--` 结束 extern 段；其后的第一个 token 即使以 `-` 开头也作为入口模块。

## 2. extern 参数

| 选项 | 含义 |
|---|---|
| `-v`, `--version` | 打印 `motevm 0.1.0 (MOTEBC 1.0)`，exit 0；命中后忽略其余参数 |
| `-h`, `--help` | 打印用法到 stdout，exit 0；命中后忽略其余参数 |
| `-d`, `--debug` | 调试模式：输出模块摘要、常量占用、执行摘要与停机上下文 |
| `-r`, `--release` | 发布模式（默认）：只输出错误；与 `-d` 同时给出时**后出现者生效** |
| `-s`, `--status` | 执行结束后把统计写 stderr（见 §6） |
| `-i`, `--interactive` | 在入口第一条指令前暂停并进入 REPL |
| `--step` | 在第一条指令前暂停（隐含 `--interactive`） |
| `--break SPEC` | 设置断点，可重复；语法见 §11.1 |
| `--trace LVL` | 指令 trace：`off`（默认）/`call`/`exec`/`full` |
| `--trace-file PATH` | trace 目标；缺省 stderr，`-` = stdout；给出 `-w` 时路径必须落在工作区内 |
| `--trace-limit N` | 最多记录 N 行（0 = 不限） |
| `-w`, `--workspace DIR` | 唯一目录授权；缺省仅 stdio；重复给出时后者生效 |

规则：

- `-i`、`--step`、`--break`、`--trace` 任一出现即安装 `CORE::DebugHook`；仅给 `--break`
  时**不**在入口暂停，仅给 `--trace` 时不进入 REPL。
- `--trace` 可与 REPL 同时使用；建议配 `--trace-file`，避免与提示符混流。
- 长选项支持 `--name=value`；`-w` 取下一个 token 原样作为值（允许以 `-` 开头）。
- `--break`/`--trace-file` 缺值或空值、`--trace`/`--trace-limit` 非法值、`--workspace=`
  空值 ⇒ 用法错误（exit 1），用法文本写 stderr、stdout 无输出。
- 未知选项、缺少入口模块 ⇒ 用法错误（exit 1）。

### 2.1 工作区

`-w/--workspace DIR` 是 `motevm` 唯一的目录授权：

| 状态 | 模块文件 | 脚本能力 |
|---|---|---|
| 未给出 `-w`（默认） | 按命令行 token 原样打开（相对 cwd） | 仅 stdio；不授予任何目录 |
| 给出 `-w DIR` | 相对路径以 `DIR` 为基准；绝对路径原样；两者规范化后必须落在 `DIR` 内 | 读写都只能落在 `DIR` 内 |

- 根在加载任何模块之前规范化（`weakly_canonical`）并校验为存在的目录；失败 ⇒ exit 2。
- 越界按路径组件逐段比较（Windows 大小写不敏感），因此 `ws2` 不算 `ws` 的子路径。
- 符号链接/Junction 按目标判定；`..` 规范化后仍在根内则允许。
- 空路径、含 NUL，以及 Windows 上的盘符相对（`C:foo`）与根相对（`\foo`）形式 ⇒ `invalid path`。
- 写侧（`--trace-file`、`file.*`）要求目标父目录已存在且仍在根内。
- 每个模块路径在打开前解析；越界 ⇒ exit 2，不执行任何字节码。

## 3. 解析算法

```text
1. extern    match aliases one by one; -w/--workspace also consumes the next token; -- ends it
2. entry     take the next token (missing => usage error)
3. ext       keep collecting while the token ends with ".mbc" (case-insensitive)
4. intern    save the remaining tokens verbatim; no parsing, no errors
```

示例：

```text
motevm -d -s main.mbc lib.mbc LIB2.MBC --verbose x
      │  │  │            │            │             └── intern parameters ("--verbose", "x")
      │  │  │            │            └── second external module
      │  │  │            └── first external module
      │  │  └── entry module
      │  └── --status
      └── --debug
```

## 4. 执行流水线

```text
workspace check (-w)            root unavailable -> exit 2; default -> stdio only
register built-in host funcs    print/print_str/...; add file.* when -w is given
Host::load(entry + ext modules) path outside -> exit 2 ("outside workspace")
                                failure -> exit 2 (PARSE 1..7; HOST 102 = unlinked import)
Host::initMemory()              failure -> exit 4
Host::runModule(0)              runs the entry module only; installs debug hooks as requested
                                DEBUG_STOP + quit -> exit 0; abort -> exit 3
                                CORE 1..15 -> exit 3; HOST 100/101 -> exit 4
--status                        prints stats before exit (success, trap and load failure)
```

- 外部模块只贡献常量池（写入 `Memory`），不会执行，也不能被入口模块 `call`
  （`Core::call` 只在单模块 FUNCS 内解析）。
- 未注册的 import 在**加载期**失败（`HOST::UNLINKED_IMPORT` ⇒ exit 2）。
- `-d` 的模块摘要只在加载与内存初始化都成功后输出；`--status` 在加载失败时只包含已加载模块。

## 5. 退出码

| 码 | 含义 | 触发场景 |
|---|---|---|
| `0` | 成功 | 加载/校验/执行完成；或调试器 `quit`（`execution.result=DEBUG_STOP`） |
| `1` | 用法错误 | 未知选项、缺少入口模块、`--break`/`--trace*` 取值非法、`--workspace=` 空值 |
| `2` | 输入失败 | 文件不可读、空/截断/超限、头部/CRC/段表/段内容/验证失败、import 未链接、工作区不可用、模块或 trace 文件越界、断点无法解析、trace 文件无法打开 |
| `3` | 运行期 trap | `TRAP`、`FUEL_EXHAUSTED`、`INTERRUPTED`、`MEMORY_FAULT` 及其它 CORE 1..15；调试器 `abort` |
| `4` | 内部/环境错误 | 宿主函数注册失败、内存初始化失败、`HOST::NO_MODULE`/`NO_MEMORY` 等 |

- `CORE::DEBUG_BREAK` 只在调试钩子返回 true 时产生：`quit` ⇒ exit 0，`abort` ⇒ exit 3。
- `--status` 始终输出实际退出码（`status.exit`），包括加载失败路径。
- 诊断示例：

```text
error: 'main.mbc': cannot open module file
error: 'main.mbc': invalid header, section table or checksum
error: 'main.mbc': invalid instruction stream (verifier)
error: memory: constant pool exceeds the 4 GiB address space
error: trap 3 (TRAP) at pc=8
error: fuel exhausted at pc=16
error: memory access fault at pc=8
error: 'x.mbc': import #0 'puts'/0: import not linked (no host function with matching name and arity)
error: workspace 'nope': no such directory
error: '..\out.mbc': outside workspace
```

运行期 `error:` 行的 `pc` 取自执行统计的 `lastPc`；`-d` 停机上下文使用出错指令地址
`errorPc`（见 §7）。两者在 trap 上可能相差一条指令。

## 6. `--status` 输出

`-s/--status` 在执行结束后把统计写 **stderr**。首行固定为 `# motevm status v1`，随后是
`key=value` 行，LF 结尾，无时间戳、无随机量。键的出现顺序固定：

1. 每个已加载模块的 `module[i].*`；
2. 若内存初始化成功：`memory.allocated_blocks`、`memory.allocated_bytes`；
3. 若已初始化且至少执行过一个模块：`execution.runs`、`execution.instructions`、
   `execution.max_depth`、`execution.pc`、`execution.result`，`TRAP` 时追加
   `execution.trap_code`；否则只输出 `execution.result=NOT_RUN`；
4. 最后 `status.exit=<进程退出码>`。

| 键 | 含义 |
|---|---|
| `module[i].file` | 命令行给出的路径原样 |
| `module[i].format` | `MOTEBC <major>.<minor>` |
| `module[i].flags` | 头部 flags，两位十六进制 |
| `module[i].entry` | 入口 FUNCS 索引 |
| `module[i].file_size` | 头部 `file_size` |
| `module[i].sections` | `ID=size` 列表，按段表顺序；存在 DEBUG 段时为 7 段 |
| `module[i].counts` | `const=… func=… global=… import=… instr=…`；`instr` 为 CODE 段指令数，含函数间填充 |
| `memory.allocated_blocks` | 已分配的 64 KiB 块数 |
| `memory.allocated_bytes` | 已分配字节数（`blocks × 65536`） |
| `execution.runs` | 实际执行的模块数（当前恒为 1） |
| `execution.instructions` | 执行指令数（含 `ret`/`trap`） |
| `execution.max_depth` | 峰值调用深度（含根帧） |
| `execution.pc` | 停止时的 `lastPc` |
| `execution.result` | `OK` / `TRAP` / `FUEL_EXHAUSTED` / `INTERRUPTED` / `INVALID_IMPORT` / `DEBUG_STOP` / … / `NOT_RUN` |
| `execution.trap_code` | 仅当 result 为 `TRAP` 时出现 |
| `status.exit` | 进程退出码 |

示例（`hello.mbc`，stdout/stderr 合并显示）：

```text
$ motevm -s hello.mbc 2>&1
hello, mote
# motevm status v1
module[0].file=hello.mbc
module[0].format=MOTEBC 1.0
module[0].flags=0x00
module[0].entry=#0
module[0].file_size=432
module[0].sections=META=16 CONST=120 FUNCS=40 CODE=40 GLOBALS=8 IMPORTS=32
module[0].counts=const=5 func=1 global=0 import=3 instr=10
memory.allocated_blocks=1
memory.allocated_bytes=65536
execution.runs=1
execution.instructions=9
execution.max_depth=1
execution.pc=36
execution.result=OK
status.exit=0
```

加载失败且未初始化内存时，输出更短：

```text
# motevm status v1
execution.result=NOT_RUN
status.exit=2
```

## 7. `--debug` 附加输出

`-d/--debug` 的 note 写 stderr。成功运行示例（程序输出 `hello, mote` 在 stdout）：

```text
note: no workspace (stdio only)
note: module[0] 'hello.mbc': MOTEBC 1.0, flags=0x00, entry=#0, debug=no
note:   sections META=16 CONST=120 FUNCS=40 CODE=40 GLOBALS=8 IMPORTS=32
note:   counts const=5 func=1 global=0 import=3 instr=10
note: constants placed in 68 byte(s) of memory
hello, mote
note: executed 9 instruction(s), peak depth 1, pc=36
```

- 给出 `-w` 时首行是 `note: workspace '<规范化绝对路径>'`；否则是 `note: no workspace (stdio only)`。
- `debug=yes/no` 取决于 DEBUG 段是否存在。
- `constants placed in N byte(s)` 是 8 字节对齐后的常量池占用。
- 只有存在 intern 参数时才输出 `note: N intern parameter(s) ignored`。

运行期失败追加停机上下文：

```text
note: stop: TRAP at pc=4 (inst #1)
note: trap code 3
note: location: fn=#0 'main' src=doc_trap.mta:4:5
note: stack (innermost first):
note:   #0 pc=4 (inst #1) fn=#0 'main' src=doc_trap.mta:4:5
note: registers: r0=0x00000001
note: code:
note:          0  llo r0, 0x0001
note:   >      4  trap 3
note:          8  ret r0
note:         12  zero r0
note: executed 2 instruction(s), peak depth 1, pc=8
error: trap 3 (TRAP) at pc=8
```

- `stop:` 的 `pc` 来自 `Core::errorPc()`，是出错指令的字节地址；`DEBUG_STOP` 时是尚未执行的指令。
- `location`/`src` 来自 DEBUG 段；没有 DEBUG 信息时省略 `src`，仍给出函数与指令序号。
- `stack` 从最内层帧开始；外层帧的 `call=` 是调用点（callee 的 `returnAddress - 4`）。
- `registers` 只列当前函数 `regcnt` 与 16 的较小值。
- `code` 显示停机指令前后各 2 条，`>` 标记停机指令。
- `error:` 行的 `pc` 来自 `execution.pc`（`lastPc`），因此 trap 时可能比 `stop:` 的 pc 大 4。
- 调试器 `quit` 也输出停机上下文，但没有 `error:` 行且 exit 0。

## 8. intern 参数

入口模块之后、不属于外部模块 token 的剩余参数全部作为 intern 参数保存
（`CLI::Options::intern`）。当前实现只统计并在 `-d` 下提示，**不会传给 VM**，也不影响执行。

## 9. 已知限制

- 外部模块不执行、不能被 `call`；它们只贡献常量池。
- 调试器断点只作用于入口模块。
- 不做源码文本读取；`location` 只输出 `file:line:column`，没有表达式求值、watchpoint、
  条件断点或反向执行。
- `next` 只在当前指令是 `call` 时步过，否则等价于 `step 1`。
- trace/REPL 每条指令调用一次调试钩子，性能显著低于正常执行。
- 脚本文件能力只有 `-w` 下的 `file.*`；没有 `io.open`、目录遍历或标准流句柄 API。
- 未给出 `-w` 时仍可按任意路径加载命令行显式给出的模块文件——这是 CLI 输入，不是脚本能力。
- 只支持单个工作区根。
- 没有 `--fuel`/`--max-*` 选项；`Core::setFuel` 未在 CLI 暴露。
- 统计为单进程单次执行，不包含耗时字段，以保证确定性。

## 10. 内置宿主函数

寄存器是 32-bit 原始值，没有 `Value`/`String`。字节串以 `(addr, len)` 指向 4 GiB 线性内存，
`addr` 来自常量池（`const_addr`）或堆（`mem_alloc`）。返回值按 `dword` 解释：非负值成功，
负值是错误码；查询失败使用 `0xFFFFFFFF`（`NO_INDEX`）。

**这是过渡 ABI，会随值模型演进调整。** 总体约定与负错误码见
[05 · 工具链参考](../05-toolchain.md) §8.6/§8.7。

### 10.1 始终注册

| 名字 | arity | 参数 → 返回 | 副作用与错误 |
|---|---|---|---|
| `print` | 1 | `(int)` → `0` | 有符号 32-bit 十进制 + `\n` 写 stdout，flush，行计数 +1；arity 不符时无输出、返回 0 |
| `print_str` | 2 | `(addr, len)` → `0` / `-5` | 原样写 `len` 字节 + `\n` 到 stdout，flush，行计数 +1；`(addr,len)` 不可读 ⇒ `-5` |
| `eprint_str` | 2 | `(addr, len)` → `0` / `-5` | 同 `print_str`，写 stderr；同样计入 `total_prints` |
| `total_prints` | 0 | `()` → 行数 | 返回 `print`/`print_str`/`eprint_str` 累计写入行数 |
| `const_addr` | 1 | `(index)` → `addr` / `0xFFFFFFFF` | 当前模块常量 payload 在 `Memory` 中的地址 |
| `const_len` | 1 | `(index)` → `len` / `0xFFFFFFFF` | 当前模块常量 payload 字节数 |
| `mem_alloc` | 1 | `(size)` → `addr` / `0xFFFFFFFF` | 8 字节对齐 bump 分配；`size=0` 或 `>256 MiB` 拒绝 |
| `mem_free` | 1 | `(addr)` → `0` / `-5` | no-op；`addr == 0xFFFFFFFF` 或 arity 不符 ⇒ `-5` |

细节：

- `print_str`/`eprint_str` 的 `len` 可以为 0，此时只输出换行。
- `const_addr`/`const_len` 的 `index` 越界、当前模块不存在或 arity 不符 ⇒ `0xFFFFFFFF`。
- `mem_alloc` 的起点是 `align8(常量池占用字节数)`；每次分配后游标按 8 字节对齐推进；
  只增不减。目标区间超出 4 GiB 或块分配失败 ⇒ `0xFFFFFFFF`。
- `mem_free` 不回收内存；所有块在进程退出时统一释放。

### 10.2 仅在给出 `-w` 时注册（`file.*`）

| 名字 | arity | 参数 → 返回 | 说明 |
|---|---|---|---|
| `file.read` | 4 | `(pathAddr, pathLen, bufAddr, maxLen)` → `n` / 负码 | 二进制读，最多 `maxLen` 字节；`n=0` 表示 EOF |
| `file.write` | 4 | `(pathAddr, pathLen, dataAddr, dataLen)` → `n` / 负码 | 截断写，不存在则创建 |
| `file.append` | 4 | 同上 → `n` / 负码 | 追加写 |
| `file.exists` | 2 | `(pathAddr, pathLen)` → `0`/`1` / 负码 | 路径存在性 |
| `file.size` | 2 | `(pathAddr, pathLen)` → `n` / 负码 | 非普通文件或 > 4 GiB-1 ⇒ `-3` |
| `file.remove` | 2 | `(pathAddr, pathLen)` → `0` / 负码 | 仅删除普通文件；目录 ⇒ `-3` |

- import 名含点号，可直接书写（汇编器接受点分标识符）：
  `.import file.write 4` / `.import file.read 4`，`HOSTCALL rDest, file.write`。
- 未给出 `-w` 时这些 import 在**加载期**失败（exit 2），不会执行任何指令。
- 给出 `-w` 时所有路径经 `Workspace::resolveRead`/`resolveWrite`：读写都只能落在工作区内。
- 写操作要求目标父目录已存在且仍在工作区内（映射为 `-3`）。
- 路径字节按平台窄字符解释，必须为 1..4096 字节且不含 NUL。
- 单次读写上限 16 MiB；文件一律二进制打开，不做换行转换。

### 10.3 错误码

| 值 | 含义 |
|---|---|
| `0` | 成功 |
| `-3` | 系统 I/O 失败（打开/读/写/删除失败、父目录不存在、目标不是普通文件） |
| `-4` | 未授权（路径越界 / 没有工作区） |
| `-5` | 参数非法（路径长度 0 或 >4096、含 NUL、缓冲区越界、单次 I/O >16 MiB；适用函数的 arity 不符） |
| `0xFFFFFFFF` | 查询类失败（`const_addr`/`const_len`/`mem_alloc`） |

## 11. 调试器

`-i`、`--step`、`--break`、`--trace` 任一出现才安装 `CORE::DebugHook`；钩子在**指令执行前**
调用，`pc` 指向尚未执行的指令。普通运行路径不安装钩子。`-i` 或 `--step` 使第一次暂停发生在
入口第一条指令之前；仅给 `--break` 时不会在入口自动暂停。

### 11.1 断点语法

`--break` 与 REPL 的 `break`/`b` 使用同一语法：

| 形式 | 含义 |
|---|---|
| `#N` | 模块 CODE 的绝对指令序号（pc = N × 4） |
| `N` | 字节偏移，必须是 4 的倍数 |
| `函数名` | 函数入口（FUNCS 的 name 常量） |
| `函数名:偏移` / `#函数序号:偏移` | 函数入口 + 指令偏移，偏移必须 `< instr` |

- 越界、未知函数、未对齐、重复设置同一 pc ⇒ `--break` 报错并 exit 2；REPL 内只提示，不退出。
- 断点地址是入口模块的 CODE 偏移。
- 断点命中时暂停在该指令执行前；`continue` 会跳过同一 pc 的一次命中，避免立即重入。

### 11.2 REPL 命令

REPL 从 stdin 读命令；提示符、命令输出和诊断都写 stderr。

| 命令 | 说明 |
|---|---|
| `help` / `h` / `?` | 命令列表 |
| `continue` / `c` | 继续到下一个断点或结束 |
| `step [N]` / `s` | 执行 N 条指令后暂停（默认 1；上限 1,000,000） |
| `next` / `n` | 当前指令是 `call` 时步过，否则等价 `step 1` |
| `finish` / `f` | 运行到当前帧返回 |
| `break` / `b <spec>` | 设置断点（语法见 §11.1） |
| `delete` / `d <id\|*>` | 删除断点 |
| `info breakpoints` | 列出断点（id、pc、函数、源码位置）；也接受 `info break` / `info b` |
| `backtrace` / `bt` / `where` | 调用栈，最内层在先 |
| `regs` / `registers` / `r [start [n]]`、`r all` | 当前帧寄存器；缺省 start=0、n=16 |
| `print` / `p <rN>` | 单寄存器（十六进制 + 有符号） |
| `x` / `mem` / `x/mem <addr> [len]` | 内存转储；len 1..256，缺省 16 |
| `disasm` / `u [#pc] [n]` | 反汇编；`#N` 为指令序号，缺省从当前 pc 起 8 条 |
| `locals` | 当前 pc 可见的命名局部变量（需 DEBUG 段 Locals） |
| `location` / `loc` / `line` | 当前 `file:line:column` 与函数 |
| `quit` / `q` | 停止程序，exit 0 |
| `abort` / `kill` | 停止程序，exit 3 |

- 提示符 `(motevm) ` 只在 stdin 是终端时输出；管道/重定向时自动关闭。
- stdin EOF 等价 `quit`。
- REPL 只调用 `Core` 的只读访问器，不重入 VM。
- `r all` 显示当前函数的 `regcnt`（上限 256）；显式 start 而不给 n 时仍显示 16 个。
- `x` 的范围必须落在 4 GiB 内；未映射行显示 `<unmapped>`。
- `disasm` 的 count 为 0 或 >64 时按 8 处理。

### 11.3 示例

```text
$ motevm --break=#2 -d hello.mbc
note: no workspace (stdio only)
note: module[0] 'hello.mbc': MOTEBC 1.0, flags=0x00, entry=#0, debug=no
note:   sections META=16 CONST=120 FUNCS=40 CODE=40 GLOBALS=8 IMPORTS=32
note:   counts const=5 func=1 global=0 import=3 instr=10
note: constants placed in 68 byte(s) of memory
(motevm) bt
stack (innermost first):
  #0 fn=#0 'main' pc=8 (inst #2)
(motevm) regs
r0=0x00000000 r1=0x00000000 ... r7=0x00000000
(motevm) quit
note: stop: DEBUG_STOP at pc=8 (inst #2)
...
```

- 断点暂停时只输出提示符；`-d` 的停机上下文在运行结束后统一输出。
- `continue` 运行到结束时不会产生 `note: stop`。

## 12. 指令 trace

`--trace LVL` 安装调试钩子并记录指令事件：

| 级别 | 内容 |
|---|---|
| `off`（默认） | 不安装 trace（除非其它调试选项要求钩子） |
| `call` | `C`（call）、`R`（ret）、`T`（trap）、`S`（停机快照）、`X`（结束） |
| `exec` | 在 `call` 基础上增加每条指令的 `E` |
| `full` | 在 `exec` 基础上增加每条指令后的 `G` 寄存器快照 |

输出目标：默认 stderr；`--trace-file PATH` 写文件，`-` = stdout。只有 `--trace` 非 `off`
时才打开 trace 文件；单独给出 `--trace-file` 会被忽略。给出 `-w` 时路径必须落在工作区内，
且父目录必须存在；打开失败或越界 ⇒ exit 2（`--status` 仍会输出）。

行格式（`seq` 逐行递增，`X` 不递增；不含时间戳/主机地址）：

```text
# motevm trace v1
E seq=0 f=0 pc=0 op=llo r0, 0x0000
C seq=3 f=0 pc=8 callee=#1 arity=1 dest=r2
R seq=6 f=1 pc=20 value=7
T seq=4 f=0 pc=12 code=3
G seq=7 f=0 pc=12 r0=0x00000007 r1=0x00000002
S depth=1 f=0 pc=12
X status=halted
```

| 行 | 字段 |
|---|---|
| `E` | `seq`、`f`（函数索引或 `?`）、`pc`、`op`（小写反汇编文本）、可选 `src="file:line:col"` |
| `C` | `seq`、`f`、`pc`、`callee=#N`、`arity=N`（索引有效时）、`dest=rN` |
| `R` | `seq`、`f`、`pc`、`value`（有符号十进制） |
| `T` | `seq`、`f`、`pc`、`code` |
| `G` | `seq`、`f`、`pc`，随后 `r0=0x…` 等（当前帧 `regcnt` 与 16 的较小值） |
| `S` | `depth`、`f`、`pc` |
| `X` | `status`，trap 时追加 `trap=N`，其它错误追加 `code=N` |

- `E` 的 `op` 使用 `DISASM::format` 的原始小写文本（例如 `hostcall r1, #1`），与 canonical
  `.mta` 的大写助记符和符号名不同。
- `X status`：`halted` / `trap` / `memory-fault` / `fuel-exhausted` / `interrupted` /
  `debug-stop` / `trace-limit` / `error`。
- `--trace-limit N`：前 N 行照常记录，之后停止记录（执行继续），末尾输出
  `X status=trace-limit`；`0` 表示不限。
- 同一模块 + 同一宿主输入 ⇒ trace 输出逐字节相同。
- `--trace` 与 REPL 同时使用时，两者都写 stderr；建议用 `--trace-file` 分离。
