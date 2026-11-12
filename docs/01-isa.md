# 01 · ISA 参考（当前实现）

[简体中文](01-isa.md) | [English](en/01-isa.md)

本文描述 MoteVM 0.1.0（MOTEBC 1.0）**当前实现**的寄存器式 32 位指令集。指令固定 4 字节，CODE 段按小端存放；寄存器是原始 32 位 dword，浮点值直接以 IEEE-754 binary32 位模式存放。当前实现没有垃圾回收、没有对象模型、没有类型标签，也没有独立的数据栈。

相关文档：[总览](00-overview.md) · [字节码格式](02-bytecode-format.md) · [运行时](03-runtime.md) · [嵌入 API](04-embedding-api.md) · [moteasm](self/moteasm.md) · [motedis](self/motedis.md) · [示例](examples/hello.mta)

## 1. 范围

- 本文只覆盖 `src/` 中已实现并可由 `moteasm` / `motevm` 使用的指令与运行时语义。
- 指令字中的寄存器字段为 8 位：**每帧 256 个寄存器**（`r0`..`r255`）。
- 当前 ISA 共有 **58 个已定义操作码**；其余字节保留，加载时由验证器拒绝。
- 数值运算按 32 位补码或 IEEE-754 binary32 执行；没有异常标志寄存器。

## 2. 编码

### 2.1 指令字

每条指令是一个 32 位字，在 CODE 段中按 4 字节小端存放（先出现 opcode 字节）。字的布局固定为：

```text
[31:24] c | [23:16] b | [15:8] a | [7:0] opcode
```

| 字段 | 位 | 含义 |
|---|---|---|
| `opcode` | `[7:0]` | 操作码，见第 4 节 |
| `a` | `[15:8]` | 第一个操作数字节；通常是目标寄存器 |
| `b` | `[23:16]` | 第二个操作数字节 |
| `c` | `[31:24]` | 第三个操作数字节 |

### 2.2 操作数格式

| 格式 | 字段使用 | 用途 |
|---|---|---|
| `op` | 无 | 无操作数（`HALT`、`BREAKPOINT`） |
| `r` | `a` | 一个寄存器（`ZERO`、`RET`） |
| `rr` | `a`, `b` | 两个寄存器（`MOV`、`NEG`、`BNOT`、浮点一元操作） |
| `rr8` | `a`, `b`, `c` | 两个寄存器 + 8 位无符号立即数（`LOAD`、`STO`） |
| `rrr` | `a`, `b`, `c` | 三个寄存器（算术、逻辑、比较、浮点二元操作） |
| `ri16` | `a`, `b`+`c` | 一个寄存器 + 16 位立即数（`LHI`、`LLO`、`JZ`、`JNZ`） |
| `i24` | `a`+`b`+`c` | 24 位立即数（`JMP`、`TRAP`） |
| `call16` | `a`, `b`+`c` | 目标寄存器 + 16 位表索引（`CALL`、`HOSTCALL`） |

`rr8` 的第三个字段是无符号位移，范围 `0..255`。

### 2.3 立即数构造

```text
imm16 = b | (c << 8)
imm24 = a | (b << 8) | (c << 16)        ; equivalent to word >> 8
```

- `LHI` / `LLO` 的 `imm16` 按原始 16 位位模式使用；汇编器接受 `[-32768, 65535]`。
- `JZ` / `JNZ` 的 `imm16` 和 `JMP` 的 `imm24` 是**有符号字节偏移**，相对于下一条指令。
- `CALL` / `HOSTCALL` 的 16 位字段是表索引，不参与符号扩展。
- `TRAP` 的 `imm24` 是陷阱码；验证器要求 `1..16`。

### 2.4 对齐

- 所有指令 4 字节，因此所有合法跳转目标都必须是 4 的倍数。
- 跳转编码使用字节偏移；验证器要求偏移 `% 4 == 0`，且目标位于**当前函数**的指令范围内。
- CODE 段总长度必须是 8 字节的倍数。若函数指令总数为奇数，汇编器在 CODE 末尾追加一条 `NOP`（`0x00000000`，解码为字段全零的 `ZERO`）。该字不属于任何函数，也不计入 `FUNCS.instr`。

## 3. 执行模型

### 3.1 帧与寄存器

- 每个调用帧拥有 256 个 32 位寄存器，编号 `r0`..`r255`。
- 寄存器是无类型的 32 位位模式：整数按补码解释，浮点指令把同一批位按 IEEE-754 binary32 重新解释。
- 根帧在 `run` 开始时全部清零。
- 没有专用栈指针、帧指针或条件标志寄存器；栈指针、条件等都需要程序自己在寄存器或内存中维护。

### 3.2 指令指针与执行顺序

- `pc` 是 CODE 内的**字节地址**。入口为 `FUNCS[entry].entry * 4`。
- 取指读取 4 字节并把 `pc` 前进 4，然后执行该指令。因此条件跳转和 `JMP` 的偏移都相对于**下一条指令**。
- 顺序执行到达 CODE 末尾之外会触发 `INVALID_TARGET (5)`（运行时边界检查）。验证器 V62 要求每个函数以终止指令结束，因此经过验证的代码不会顺序落入下一个函数；未经验证的模块则可能在函数之间顺序穿透。

### 3.3 调用约定

```text
CALL rDest, funcIndex
RET  rSrc
```

- `CALL` 通过 16 位索引查 FUNCS 表；被调函数从 `FUNCS[funcIndex].entry * 4` 开始。
- `CALL` 压入一个新帧，并把调用者寄存器窗口复制到新帧，然后跳转到被调函数入口。
- `RET` 从当前帧返回：把 `rSrc` 的值写入调用者 `CALL` 指令中的 `rDest`，然后恢复调用者的 `pc`。
- 被调函数对其它寄存器的修改不会回传给调用者；唯一返回值通道是 `CALL` 的目标寄存器。
- 被调函数的 `arity` 描述它期望在 `r0..r(arity-1)` 中收到多少个参数。

### 3.4 参数与返回值

- 调用者在 `r0..r(arity-1)` 中放置参数；被调函数按自己的 `arity` 读取。
- 被调函数用 `RET rSrc` 返回 `rSrc` 的 32 位值。
- `CALL` 的目标寄存器可以和参数寄存器重叠；返回写入发生在调用者恢复之后。
- 浮点参数和返回值同样只是 32 位位模式。

### 3.5 调用栈与深度限制

- 调用栈是一组帧，不是数据栈；函数内的临时值放在寄存器或平坦内存中。
- 最大调用深度 `MAX_CALL_DEPTH = 1024`，**包含根帧**。超过时 `CALL` 以 `STACK_OVERFLOW (4)` 停止。
- 每次调用都会复制调用者寄存器窗口；实现可能只复制静态可达的前缀，但任何经过验证的函数能访问到的寄存器都会被复制，因此观察结果等同于复制完整窗口。

### 3.6 终止

- `RET` 在根帧执行时正常结束执行，返回值被丢弃；这不是错误。
- `HALT` 在任意帧中都会立即结束整个 VM，`error()` 保持 `OK`。
- `TRAP` 停止执行并以 `TRAP (14)` 报告；具体陷阱码保存在 `trapCode()`。
- `BREAKPOINT` 在没有调试器时是 NOP；调试钩子会在**每条**指令执行前调用，并可在其中请求停止（`DEBUG_BREAK (16)`）。`BREAKPOINT` 本身不额外触发停止。

### 3.7 HOSTCALL

```text
HOSTCALL rDest, importIndex      ; 0x60, call16
```

- `importIndex` 是 IMPORTS 表索引，编码在 `b | (c << 8)`；`a` 是结果寄存器。
- 加载器在装载阶段按**名称和 arity** 把每个 IMPORTS 槽绑定到宿主回调；字节码中不保存宿主地址。
- 参数放在当前帧的 `r0..r(arity-1)`，以只读指针传给宿主函数；宿主函数返回一个 32 位值，写入 `rDest`。
- `HOSTCALL` 不压入 VM 调用帧，也不改变调用栈；除 `rDest` 外，当前帧寄存器不会被宿主直接改写。
- 如果索引越界、槽未绑定或 arity 非法，执行以 `INVALID_IMPORT (11)` 停止。
- 汇编写法：`HOSTCALL r1, print` 或 `HOSTCALL r1, #3`。
- 宿主回调不得抛出异常，也不得重入 VM。

## 4. 操作码参考

全部 58 个已定义操作码如下。表中“陷阱”列出该指令可能产生的 `CORE` 错误码（见第 5 节），`—` 表示不会因该指令本身停止。

### 4.1 0x0x 立即数与内存

| 字节 | 助记符 | 格式 | 操作数 | 语义 | 陷阱 |
|---|---|---|---|---|---|
| `0x00` | `ZERO` | `r` | `rA` | `r[A] = 0` | — |
| `0x01` | `LOAD` | `rr8` | `rA, rB, imm8` | `r[A] = mem32[r[B] + imm8]` | `MEMORY_FAULT (15)` |
| `0x02` | `STO` | `rr8` | `rA, rB, imm8` | `mem32[r[B] + imm8] = r[A]` | `MEMORY_FAULT (15)` |
| `0x03` | `LHI` | `ri16` | `rA, imm16` | `r[A] = (r[A] & 0x0000FFFF) \| (imm16 << 16)` | — |
| `0x04` | `LLO` | `ri16` | `rA, imm16` | `r[A] = (r[A] & 0xFFFF0000) \| imm16` | — |

- `LOAD` / `STO` 的位移是无符号 8 位，有效地址为 64 位计算的 `r[B] + imm8`，不会因 32 位回绕而访问低地址。
- `LHI` / `LLO` 只替换目标寄存器的一半，另一半保持不变；通常先用 `LLO` 再 `LHI` 构造 32 位常量。

### 4.2 0x1x 整数运算

| 字节 | 助记符 | 格式 | 操作数 | 语义 | 陷阱 |
|---|---|---|---|---|---|
| `0x10` | `MOV` | `rr` | `rA, rB` | `r[A] = r[B]` | — |
| `0x11` | `ADD` | `rrr` | `rA, rB, rC` | `r[A] = r[B] + r[C]`（模 2³²） | — |
| `0x12` | `SUB` | `rrr` | `rA, rB, rC` | `r[A] = r[B] - r[C]`（模 2³²） | — |
| `0x13` | `MUL` | `rrr` | `rA, rB, rC` | `r[A] = r[B] * r[C]`（低 32 位） | — |
| `0x14` | `DIV` | `rrr` | `rA, rB, rC` | 有符号除法，向零截断 | `DIVIDED_BY_ZERO (2)`；`INTEGER_OVERFLOW (3)` |
| `0x15` | `MOD` | `rrr` | `rA, rB, rC` | 有符号余数，符号随被除数 | `DIVIDED_BY_ZERO (2)` |
| `0x16` | `NEG` | `rr` | `rA, rB` | `r[A] = -r[B]` | `INTEGER_OVERFLOW (3)` |
| `0x17` | `ADDU` | `rrr` | `rA, rB, rC` | 无符号加法；位结果与 `ADD` 相同 | — |
| `0x18` | `SUBU` | `rrr` | `rA, rB, rC` | 无符号减法；位结果与 `SUB` 相同 | — |
| `0x19` | `MULU` | `rrr` | `rA, rB, rC` | 无符号乘法；位结果与 `MUL` 相同 | — |
| `0x1A` | `DIVU` | `rrr` | `rA, rB, rC` | 无符号除法 | `DIVIDED_BY_ZERO (2)` |
| `0x1B` | `MODU` | `rrr` | `rA, rB, rC` | 无符号余数 | `DIVIDED_BY_ZERO (2)` |

- 加法、减法、乘法不会因有符号溢出而陷阱，结果按 32 位回绕。
- `DIV` 在除数为零时陷阱；`INT32_MIN / -1` 无法表示，触发 `INTEGER_OVERFLOW`。
- `MOD` 在除数为零时陷阱；`INT32_MIN % -1` 定义为 `0`，不触发溢出。
- `NEG` 对 `INT32_MIN` 触发 `INTEGER_OVERFLOW`。

### 4.3 0x2x 位运算

| 字节 | 助记符 | 格式 | 操作数 | 语义 | 陷阱 |
|---|---|---|---|---|---|
| `0x20` | `BAND` | `rrr` | `rA, rB, rC` | `r[A] = r[B] & r[C]` | — |
| `0x21` | `BOR` | `rrr` | `rA, rB, rC` | `r[A] = r[B] \| r[C]` | — |
| `0x22` | `BXOR` | `rrr` | `rA, rB, rC` | `r[A] = r[B] ^ r[C]` | — |
| `0x23` | `BNOT` | `rr` | `rA, rB` | `r[A] = ~r[B]` | — |
| `0x24` | `SHL` | `rrr` | `rA, rB, rC` | `r[A] = r[B] << (r[C] & 31)` | — |
| `0x25` | `SHR` | `rrr` | `rA, rB, rC` | 逻辑右移：`r[A] = r[B] >> (r[C] & 31)` | — |
| `0x26` | `SAR` | `rrr` | `rA, rB, rC` | 算术右移（符号扩展）：移位量 `r[C] & 31` | — |

所有移位指令都把移位量掩码为低 5 位（0..31）。

### 4.4 0x3x binary32 浮点

寄存器保存原始 IEEE-754 binary32 位模式。浮点指令不检查类型标签；它们直接重新解释寄存器内容。

| 字节 | 助记符 | 格式 | 操作数 | 语义 | 陷阱 |
|---|---|---|---|---|---|
| `0x30` | `FADD` | `rrr` | `rA, rB, rC` | `f32(rB) + f32(rC)` | — |
| `0x31` | `FSUB` | `rrr` | `rA, rB, rC` | `f32(rB) - f32(rC)` | — |
| `0x32` | `FMUL` | `rrr` | `rA, rB, rC` | `f32(rB) * f32(rC)` | — |
| `0x33` | `FDIV` | `rrr` | `rA, rB, rC` | IEEE 除法；除零得到 Inf/NaN | — |
| `0x34` | `FNEG` | `rr` | `rA, rB` | 翻转符号位（保留 NaN payload） | — |
| `0x35` | `FABS` | `rr` | `rA, rB` | 清除符号位 | — |
| `0x36` | `FSQRT` | `rr` | `rA, rB` | `sqrt(f32(rB))`；负数得 NaN | — |
| `0x37` | `I2F` | `rr` | `rA, rB` | `f32(int32(rB))` | — |
| `0x38` | `F2I` | `rr` | `rA, rB` | `int32(f32(rB))`，向零截断 | `NUMERIC_CONVERSION (9)` |
| `0x39` | `FEQ` | `rrr` | `rA, rB, rC` | `f32(rB) == f32(rC) ? 1 : 0` | — |
| `0x3A` | `FNE` | `rrr` | `rA, rB, rC` | `f32(rB) != f32(rC) ? 1 : 0`；NaN 得 1 | — |
| `0x3B` | `FLT` | `rrr` | `rA, rB, rC` | `f32(rB) < f32(rC) ? 1 : 0`；NaN 得 0 | — |
| `0x3C` | `FLE` | `rrr` | `rA, rB, rC` | `f32(rB) <= f32(rC) ? 1 : 0`；NaN 得 0 | — |
| `0x3D` | `FGT` | `rrr` | `rA, rB, rC` | `f32(rB) > f32(rC) ? 1 : 0`；NaN 得 0 | — |
| `0x3E` | `FGE` | `rrr` | `rA, rB, rC` | `f32(rB) >= f32(rC) ? 1 : 0`；NaN 得 0 | — |

- `FDIV` 永不因除零陷阱；`0/0` 产生 NaN，`x/0` 产生 ±Inf。
- `F2I` 对 NaN、±Inf 以及 `[-2³¹, 2³¹)` 之外的值触发 `NUMERIC_CONVERSION`；边界值 `-2³¹` 可接受，`+2³¹` 不可接受。
- `FSQRT` 对负数返回 NaN，不陷阱。
- 算术 NaN 的具体 payload 由宿主 C++ 浮点实现决定；`FNEG` / `FABS` 是纯位操作。
- `0x3F` 保留。

### 4.5 0x4x 比较

比较结果写入 `r[A]`，为 0 或 1。

| 字节 | 助记符 | 格式 | 操作数 | 语义 | 陷阱 |
|---|---|---|---|---|---|
| `0x40` | `EQ` | `rrr` | `rA, rB, rC` | 位相等：`r[B] == r[C]` | — |
| `0x41` | `NE` | `rrr` | `rA, rB, rC` | 位不等 | — |
| `0x42` | `LT` | `rrr` | `rA, rB, rC` | 有符号小于 | — |
| `0x43` | `LE` | `rrr` | `rA, rB, rC` | 有符号小于等于 | — |
| `0x44` | `GT` | `rrr` | `rA, rB, rC` | 有符号大于 | — |
| `0x45` | `GE` | `rrr` | `rA, rB, rC` | 有符号大于等于 | — |
| `0x46` | `LTU` | `rrr` | `rA, rB, rC` | 无符号小于 | — |
| `0x47` | `LEU` | `rrr` | `rA, rB, rC` | 无符号小于等于 | — |
| `0x48` | `GTU` | `rrr` | `rA, rB, rC` | 无符号大于 | — |
| `0x49` | `GEU` | `rrr` | `rA, rB, rC` | 无符号大于等于 | — |

### 4.6 0x5x 控制流

| 字节 | 助记符 | 格式 | 操作数 | 语义 | 陷阱 |
|---|---|---|---|---|---|
| `0x50` | `JMP` | `i24` | `offset` | `pc = next_pc + int24` | `INVALID_TARGET (5)` |
| `0x51` | `JZ` | `ri16` | `rA, offset` | 若 `r[A] == 0`：`pc = next_pc + int16` | `INVALID_TARGET (5)` |
| `0x52` | `JNZ` | `ri16` | `rA, offset` | 若 `r[A] != 0`：`pc = next_pc + int16` | `INVALID_TARGET (5)` |
| `0x53` | `CALL` | `call16` | `rDest, funcIndex` | 压帧、复制寄存器、跳转到 FUNCS 入口 | `INVALID_FUNCTION (10)`、`INVALID_TARGET (5)`、`STACK_OVERFLOW (4)` |
| `0x54` | `RET` | `r` | `rSrc` | 写回调用者 `rDest` 并返回；根帧中正常结束 | — |
| `0x55` | `HALT` | `op` | — | 立即结束整个 VM | — |
| `0x56` | `BREAKPOINT` | `op` | — | 无调试器时为 NOP | — |
| `0x57` | `TRAP` | `i24` | `code` | 以陷阱码 `code` 停止 | `TRAP (14)` |

- 分支偏移是相对下一条指令的有符号字节偏移，必须为 4 的倍数，目标必须位于当前函数内（验证器 V61）。
- 未经验证的模块如果编码了非对齐偏移，运行时可能在跳转前以 `INVALID_TARGET` 停止；经过验证的模块不会出现这种情况。
- `CALL` 的索引是 FUNCS 索引；入口超出 CODE 时 `INVALID_TARGET`。
- `RET` 在根帧中是正常终止，不产生错误。
- `TRAP` 的 `code` 由验证器限制为 `1..16`；运行时把它存入 `trapCode()`，并报告 `TRAP (14)`。
- `DEBUG_BREAK (16)` 来自调试钩子对任意指令的停止请求，不是 `BREAKPOINT` 特有的陷阱。
- 汇编器接受的陷阱名：`TypeMismatch`、`DivByZero`、`IntegerOverflow`、`NumericConversion`、`IndexOutOfBounds`、`NullAccess`、`StackOverflow`、`InvalidBytecode`、`UnlinkedImport`、`OutOfMemory`、`HostError`、`FuelExhausted`、`Interrupted`、`NotImplemented`、`InternalError`、`InvalidArgument`。

### 4.7 0x6x 系统

| 字节 | 助记符 | 格式 | 操作数 | 语义 | 陷阱 |
|---|---|---|---|---|---|
| `0x60` | `HOSTCALL` | `call16` | `rDest, importIndex` | 调用 IMPORTS 槽绑定的宿主函数，返回值写入 `rDest` | `INVALID_IMPORT (11)` |

详见 §3.7 HOSTCALL。

## 5. 陷阱与错误

`CORE` 错误码是引擎执行状态，不是 `TRAP` 指令自带的 1..16 陷阱码空间。执行停止后：

- `error()` 返回下表中的非零码；正常 `HALT` 或根帧 `RET` 后保持 `OK (0)`。
- `errorPc()` 指向报告错误的指令；对 `DEBUG_BREAK`，它指向尚未执行的指令。由入口/attach 阶段产生的错误（如 `INVALID_FUNCTION`、`INVALID_TEXT`）可能没有对应指令，此时为 0。
- `currentPc()` 是执行循环的当前/下一条 `pc`。
- `trapCode()` 只在 `TRAP` 指令后保存其 1..16 的立即数。

| 码 | 名称 | 触发条件 |
|---|---|---|
| 0 | `OK` | 正常结束或未发生错误 |
| 1 | `UNKNOWN_INSTRUCTION` | 遇到未定义操作码；只在未经验证的模块中可能发生 |
| 2 | `DIVIDED_BY_ZERO` | `DIV` / `DIVU` / `MOD` / `MODU` 的除数为 0 |
| 3 | `INTEGER_OVERFLOW` | `INT32_MIN / -1`，或 `NEG INT32_MIN` |
| 4 | `STACK_OVERFLOW` | 调用深度超过 `MAX_CALL_DEPTH`（1024，含根帧） |
| 5 | `INVALID_TARGET` | 跳转目标非 4 字节对齐或越界；函数入口或 `pc` 超出 CODE |
| 6 | `INVALID_TEXT` | attach 的 CODE 为空或长度不是 4 的倍数 |
| 7 | `NO_ACTIVE_FRAME` | 无活动帧时执行/保存/恢复上下文（引擎 API 级） |
| 8 | `NO_CALLER` | 根帧 `RET` 弹出后的内部信号；作为正常结束处理 |
| 9 | `NUMERIC_CONVERSION` | `F2I` 遇到 NaN、±Inf 或超出 `[-2³¹, 2³¹)` |
| 10 | `INVALID_FUNCTION` | FUNCS 索引越界 |
| 11 | `INVALID_IMPORT` | IMPORTS 索引越界、未绑定或 arity 非法 |
| 12 | `FUEL_EXHAUSTED` | 宿主设置的指令预算耗尽 |
| 13 | `INTERRUPTED` | 中断钩子请求停止 |
| 14 | `TRAP` | 执行了 `TRAP` 指令；具体码见 `trapCode()` |
| 15 | `MEMORY_FAULT` | `LOAD` / `STO` 访问未分配、被占用或越过 4 GiB 的地址 |
| 16 | `DEBUG_BREAK` | 调试钩子在指令执行前请求停止 |

- 加载期的验证失败不会产生 `CORE` 码；模块在开始执行前就被拒绝。
- 6、7、12、13、16 主要用于宿主/调试控制，不是普通程序主动触发的 ISA 故障。

## 6. 内存模型

- 地址空间是 **4 GiB 平坦空间**（`0x00000000`..`0xFFFFFFFF`），按 **64 KiB 块**管理：共 65536 块。
- 每个块有三种状态：

| 状态 | 含义 |
|---|---|
| `RELEASED` | 未分配；访问即故障 |
| `FREE` | 已分配，可读写 |
| `OCCUPIED` | 已占用，不可由 VM 访问 |

- 块由宿主按需调用 `Memory::allocate` 分配；`LOAD` / `STO` **不会**自动分配块。访问 `RELEASED` 或 `OCCUPIED` 块都产生 `MEMORY_FAULT (15)`。
- 有效地址按 64 位计算：`address = r[B] + imm8`。`address > 4 GiB - 4` 时产生 `MEMORY_FAULT`，不会 32 位回绕。
- 访问宽度固定为 4 字节，小端。允许非对齐访问。
- 允许跨 64 KiB 块边界的 4 字节访问：高/低两部分分别落在相邻块，两个块都必须是 `FREE`；任意一块不可用即故障。
- 跨块写在第二块故障前可能已经写入第一块的一部分，故障发生后不应假定目标内存完全未变。
- 内存与寄存器分离；没有隐式对象头、没有 GC 元数据。

## 7. 加载期验证规则

模块在加载时经过验证；下面这些规则最容易被手写字节码或手工修改的 `.mbc` 触发。规则失败时模块被整体拒绝，不会部分执行。

| 规则 | 要求（面向作者） |
|---|---|
| V51 | CODE 中每个 4 字节字都必须解码为已定义操作码，且未使用字段必须为 0（例如 `MOV` 的 `c`、`HALT` 的三个字段）。CODE 末尾的 `NOP` 也满足此规则。 |
| V53 | 每条指令中实际使用的寄存器编号必须小于该函数声明的 `regs`。 |
| V56 / V59 | `CALL` 的 FUNCS 索引必须小于函数数量；被调函数 `arity` 不得超过调用者的 `regs`。 |
| V56 / V60 | `HOSTCALL` 的 IMPORTS 索引必须小于导入数量；导入 `arity` 不得超过当前函数的 `regs`。 |
| V57 | `TRAP` 的陷阱码必须在 `1..16`。 |
| V61 | `JMP` / `JZ` / `JNZ` 的字节偏移必须是 4 的倍数，目标必须位于同一函数内。 |
| V62 | 每个函数的最后一条指令必须是终止指令：`JMP`、`RET`、`TRAP` 或 `HALT`。 |
| V76 | 模块头中的入口函数索引必须小于 FUNCS 数量。 |
| V77 | 入口函数的 `arity` 必须为 0。 |

汇编器在写出 `.mbc` 前还会用同一套 `Parse` / `Verifier` 自检；失败会报告 `E031` 并以内部错误退出。
