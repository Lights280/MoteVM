# motevm Command-Line Interface

[简体中文](../../self/cli.md) | [English](cli.md)

This document describes the current positional CLI of `motevm`, its `--status` output, built-in
host functions, debugger, and instruction trace. General tool conventions and the exit-code
philosophy are in [05 · Toolchain Reference](../05-toolchain.md) §8.

## 1. Syntax

```text
motevm [extern_parameters] <entry_mbc_module> [ext_mbc_modules] [intern_parameters]
```

| Segment | Position | Meaning |
|---|---|---|
| `extern_parameters` | Before the entry module | Host-level options, see §2 |
| `<entry_mbc_module>` | First non-option token | The only module that executes |
| `[ext_mbc_modules]` | Consecutive tokens after the entry that end with `.mbc` | External modules: loaded into the constant pool, **not executed** |
| `[intern_parameters]` | All remaining tokens | Reserved for scripts; parsed and currently ignored |

- The `.mbc` suffix is case-insensitive and must be longer than 4 characters (a bare `.mbc` is not a
  module token).
- The entry module itself does not need to end with `.mbc`.
- Options are parsed only in the extern segment; tokens starting with `-` after the entry belong to
  intern parameters and are not treated as options.
- `--` ends the extern segment; the next token becomes the entry module even if it starts with `-`.

## 2. Extern Parameters

| Option | Meaning |
|---|---|
| `-v`, `--version` | Print `motevm 0.1.0 (MOTEBC 1.0)`, exit 0; remaining arguments are ignored |
| `-h`, `--help` | Print usage to stdout, exit 0; remaining arguments are ignored |
| `-d`, `--debug` | Debug mode: module summaries, constant usage, execution summary, stop context |
| `-r`, `--release` | Release mode (default): errors only; when both `-d` and `-r` are given, the later one wins |
| `-s`, `--status` | Write statistics to stderr after the run (see §6) |
| `-i`, `--interactive` | Stop before the first entry instruction and enter the REPL |
| `--step` | Stop before the first instruction (implies `--interactive`) |
| `--break SPEC` | Set a breakpoint, repeatable; syntax in §11.1 |
| `--trace LVL` | Instruction trace: `off` (default) / `call` / `exec` / `full` |
| `--trace-file PATH` | Trace target; default stderr, `-` = stdout; with `-w` the path must stay inside the workspace |
| `--trace-limit N` | Record at most N lines (0 = unlimited) |
| `-w`, `--workspace DIR` | The only directory grant; default stdio only; a later value overrides an earlier one |

Rules:

- Any of `-i`, `--step`, `--break`, `--trace` installs `CORE::DebugHook`. `--break` alone does
  **not** stop at entry; `--trace` alone does not enter the REPL.
- `--trace` may be combined with the REPL; use `--trace-file` to avoid mixing with prompts.
- Long options accept `--name=value`; `-w` takes the next token verbatim (even if it starts with `-`).
- Missing/empty values for `--break`/`--trace-file`, invalid `--trace`/`--trace-limit` values, and an
  empty `--workspace=` are usage errors (exit 1): usage text goes to stderr and stdout stays empty.
- Unknown options and a missing entry module are usage errors (exit 1).

### 2.1 Workspace

`-w/--workspace DIR` is `motevm`'s only directory grant:

| State | Module files | Script capability |
|---|---|---|
| No `-w` (default) | Opened verbatim from the command line (relative to cwd) | stdio only; no directory is granted |
| `-w DIR` given | Relative paths resolve against `DIR`; absolute paths are taken as-is; both must canonicalize inside `DIR` | Reads and writes must stay inside `DIR` |

- The root is canonicalized (`weakly_canonical`) and validated as an existing directory before any
  module loads; failure exits 2.
- Containment is checked component-wise (case-insensitive on Windows), so `ws2` is not a child of
  `ws`.
- Symbolic links/junctions are judged by target; `..` is allowed when canonicalization stays inside
  the root.
- Empty paths, embedded NUL, and the Windows drive-relative (`C:foo`) and root-relative (`\foo`)
  forms are `invalid path` on Windows.
- The write side (`--trace-file`, `file.*`) requires the target parent to exist and stay inside the
  root.
- Each module path is resolved before it is opened; an escape exits 2 and no bytecode runs.

## 3. Parsing Algorithm

```text
1. extern    match aliases one by one; -w/--workspace also consumes the next token; -- ends it
2. entry     take the next token (missing => usage error)
3. ext       keep collecting while the token ends with ".mbc" (case-insensitive)
4. intern    save the remaining tokens verbatim; no parsing, no errors
```

Example:

```text
motevm -d -s main.mbc lib.mbc LIB2.MBC --verbose x
      │  │  │            │            │             └── intern parameters ("--verbose", "x")
      │  │  │            │            └── second external module
      │  │  │            └── first external module
      │  │  └── entry module
      │  └── --status
      └── --debug
```

## 4. Execution Pipeline

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

- External modules contribute only to the constant pool (written into `Memory`); they are not
  executed and cannot be called by the entry module (`Core::call` resolves only within the single
  module's FUNCS).
- An unregistered import fails at **load time** (`HOST::UNLINKED_IMPORT` ⇒ exit 2).
- `-d` module summaries are printed only after both loading and memory initialization succeed; on a
  load failure, `--status` covers only the modules that were loaded.

## 5. Exit Codes

| Code | Meaning | Trigger |
|---|---|---|
| `0` | Success | Load/verify/run completed; or debugger `quit` (`execution.result=DEBUG_STOP`) |
| `1` | Usage error | Unknown option, missing entry module, invalid `--break`/`--trace*` value, empty `--workspace=` |
| `2` | Input failure | Unreadable file, empty/truncated/oversized input, header/CRC/section-table/section-content/verification failure, unlinked import, unusable workspace, module or trace path outside the workspace, unparsable breakpoint, trace file cannot be opened |
| `3` | Runtime trap | `TRAP`, `FUEL_EXHAUSTED`, `INTERRUPTED`, `MEMORY_FAULT`, and other CORE 1..15 errors; debugger `abort` |
| `4` | Internal/environment error | Host function registration failure, memory initialization failure, `HOST::NO_MODULE`/`NO_MEMORY`, etc. |

- `CORE::DEBUG_BREAK` is produced only when the debug hook returns true: `quit` ⇒ exit 0,
  `abort` ⇒ exit 3.
- `--status` always reports the actual exit code (`status.exit`), including load-failure paths.
- Diagnostic examples:

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

The `pc` in a runtime `error:` line comes from the execution statistics `lastPc`; the `-d` stop
context uses the faulting instruction address `errorPc` (see §7). For traps the two may differ by one
instruction.

## 6. `--status` Output

`-s/--status` writes statistics to **stderr** after the run. The first line is always
`# motevm status v1`, followed by `key=value` lines with LF endings and no timestamps or random
values. Key order is fixed:

1. `module[i].*` for every loaded module;
2. if memory initialization succeeded: `memory.allocated_blocks`, `memory.allocated_bytes`;
3. if initialized and at least one module ran: `execution.runs`, `execution.instructions`,
   `execution.max_depth`, `execution.pc`, `execution.result`, plus `execution.trap_code` for `TRAP`;
   otherwise only `execution.result=NOT_RUN`;
4. finally `status.exit=<process exit code>`.

| Key | Meaning |
|---|---|
| `module[i].file` | Path exactly as given on the command line |
| `module[i].format` | `MOTEBC <major>.<minor>` |
| `module[i].flags` | Header flags, two hex digits |
| `module[i].entry` | Entry FUNCS index |
| `module[i].file_size` | Header `file_size` |
| `module[i].sections` | `ID=size` list in section-table order; 7 sections when DEBUG is present |
| `module[i].counts` | `const=… func=… global=… import=… instr=…`; `instr` is the CODE instruction count, including inter-function padding |
| `memory.allocated_blocks` | Number of allocated 64 KiB blocks |
| `memory.allocated_bytes` | Allocated bytes (`blocks × 65536`) |
| `execution.runs` | Number of modules actually run (currently always 1) |
| `execution.instructions` | Instructions executed (including `ret`/`trap`) |
| `execution.max_depth` | Peak call depth (including the root frame) |
| `execution.pc` | `lastPc` at stop |
| `execution.result` | `OK` / `TRAP` / `FUEL_EXHAUSTED` / `INTERRUPTED` / `INVALID_IMPORT` / `DEBUG_STOP` / … / `NOT_RUN` |
| `execution.trap_code` | Present only when result is `TRAP` |
| `status.exit` | Process exit code |

Example (`hello.mbc`, stdout/stderr merged):

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

When loading fails before memory initialization, the output is shorter:

```text
# motevm status v1
execution.result=NOT_RUN
status.exit=2
```

## 7. `--debug` Extra Output

`-d/--debug` notes go to stderr. A successful run looks like this (the program prints
`hello, mote` to stdout):

```text
note: no workspace (stdio only)
note: module[0] 'hello.mbc': MOTEBC 1.0, flags=0x00, entry=#0, debug=no
note:   sections META=16 CONST=120 FUNCS=40 CODE=40 GLOBALS=8 IMPORTS=32
note:   counts const=5 func=1 global=0 import=3 instr=10
note: constants placed in 68 byte(s) of memory
hello, mote
note: executed 9 instruction(s), peak depth 1, pc=36
```

- With `-w`, the first line is `note: workspace '<canonical absolute path>'`; otherwise it is
  `note: no workspace (stdio only)`.
- `debug=yes/no` reflects the presence of a DEBUG section.
- `constants placed in N byte(s)` is the 8-byte aligned constant-pool usage.
- `note: N intern parameter(s) ignored` appears only when intern parameters are present.

A runtime failure appends stop context:

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

- `stop:` uses `Core::errorPc()`, the byte address of the faulting instruction; for `DEBUG_STOP` it is
  the instruction that has not executed yet.
- `location`/`src` come from the DEBUG section; without debug info `src` is omitted but the function
  and instruction index remain.
- `stack` starts at the innermost frame; an outer frame's `call=` is the call site
  (`callee returnAddress - 4`).
- `registers` lists the smaller of the current function's `regcnt` and 16.
- `code` shows two instructions before and after the stop, with `>` marking the stop instruction.
- The `error:` line `pc` comes from `execution.pc` (`lastPc`), so for traps it may be 4 greater than
  the `stop:` pc.
- Debugger `quit` also prints stop context, but no `error:` line, and exits 0.

## 8. Intern Parameters

All remaining tokens after the entry module that are not external module tokens are saved as intern
parameters (`CLI::Options::intern`). The current implementation only counts them and mentions them
under `-d`; they are **not passed to the VM** and do not affect execution.

## 9. Known Limitations

- External modules are not executed and cannot be called; they contribute only to the constant pool.
- Breakpoints apply to the entry module only.
- Source text is never read; `location` prints only `file:line:column`. There is no expression
  evaluation, watchpoints, conditional breakpoints, or reverse execution.
- `next` steps over only when the current instruction is `call`; otherwise it behaves like `step 1`.
- Trace/REPL invoke the debug hook once per instruction and are significantly slower than normal
  execution.
- Script file capability is limited to `file.*` under `-w`; there is no `io.open`, directory
  traversal, or standard-stream handle API.
- Without `-w`, module files explicitly named on the command line may still be loaded from any path;
  that is CLI input, not a script capability.
- Only a single workspace root is supported.
- There are no `--fuel`/`--max-*` options; `Core::setFuel` is not exposed by the CLI.
- Statistics cover one process and one run, with no timing fields, to stay deterministic.

## 10. Built-in Host Functions

Registers are raw 32-bit values with no `Value`/`String`. Byte strings are passed as `(addr, len)`
pairs into the 4 GiB linear memory; `addr` comes from the constant pool (`const_addr`) or the heap
(`mem_alloc`). Return values are interpreted as `dword`: non-negative means success, negative is an
error code; query failures use `0xFFFFFFFF` (`NO_INDEX`).

**This is a transitional ABI and will change with the value model.** Overall conventions and
negative error codes are in [05 · Toolchain Reference](../05-toolchain.md) §8.6/§8.7.

### 10.1 Always Registered

| Name | Arity | Arguments → Return | Side Effects and Errors |
|---|---|---|---|
| `print` | 1 | `(int)` → `0` | Writes signed 32-bit decimal + `\n` to stdout, flushes, increments the line count; wrong arity produces no output and returns 0 |
| `print_str` | 2 | `(addr, len)` → `0` / `-5` | Writes `len` raw bytes + `\n` to stdout, flushes, increments the line count; unreadable `(addr,len)` ⇒ `-5` |
| `eprint_str` | 2 | `(addr, len)` → `0` / `-5` | Same as `print_str`, but to stderr; also counts toward `total_prints` |
| `total_prints` | 0 | `()` → line count | Cumulative lines written by `print`/`print_str`/`eprint_str` |
| `const_addr` | 1 | `(index)` → `addr` / `0xFFFFFFFF` | Address of the current module's constant payload in `Memory` |
| `const_len` | 1 | `(index)` → `len` / `0xFFFFFFFF` | Size of the current module's constant payload |
| `mem_alloc` | 1 | `(size)` → `addr` / `0xFFFFFFFF` | 8-byte aligned bump allocation; `size=0` or `>256 MiB` is rejected |
| `mem_free` | 1 | `(addr)` → `0` / `-5` | No-op; `addr == 0xFFFFFFFF` or wrong arity ⇒ `-5` |

Details:

- `print_str`/`eprint_str` accept `len == 0`, which writes only a newline.
- `const_addr`/`const_len` return `0xFFFFFFFF` for an out-of-range `index`, a missing module, or
  wrong arity.
- `mem_alloc` starts at `align8(constantBytes)`; the cursor advances 8-byte aligned and never
  shrinks. A range beyond 4 GiB or a block allocation failure ⇒ `0xFFFFFFFF`.
- `mem_free` does not release memory; all blocks are released when the process exits.

### 10.2 Registered Only with `-w` (`file.*`)

| Name | Arity | Arguments → Return | Notes |
|---|---|---|---|
| `file.read` | 4 | `(pathAddr, pathLen, bufAddr, maxLen)` → `n` / negative | Binary read of at most `maxLen` bytes; `n=0` means EOF |
| `file.write` | 4 | `(pathAddr, pathLen, dataAddr, dataLen)` → `n` / negative | Truncate and write; creates the file when absent |
| `file.append` | 4 | Same → `n` / negative | Append |
| `file.exists` | 2 | `(pathAddr, pathLen)` → `0`/`1` / negative | Path existence |
| `file.size` | 2 | `(pathAddr, pathLen)` → `n` / negative | Non-regular file or > 4 GiB-1 ⇒ `-3` |
| `file.remove` | 2 | `(pathAddr, pathLen)` → `0` / negative | Regular files only; directory ⇒ `-3` |

- Without `-w`, these imports fail at **load time** (exit 2) and no instructions run.
- With `-w`, every path goes through `Workspace::resolveRead`/`resolveWrite`: reads and writes must
  stay inside the workspace.
- Write operations require the target parent to exist and stay inside the workspace (mapped to `-3`).
- Path bytes use the platform narrow encoding and must be 1..4096 bytes with no NUL.
- A single read/write is limited to 16 MiB; files are always opened in binary mode with no newline
  translation.

### 10.3 Error Codes

| Value | Meaning |
|---|---|
| `0` | Success |
| `-3` | System I/O failure (open/read/write/remove failure, missing parent directory, target is not a regular file) |
| `-4` | Denied (path outside the workspace / no workspace) |
| `-5` | Bad argument (path length 0 or >4096, embedded NUL, buffer out of range, single I/O >16 MiB; arity mismatch where applicable) |
| `0xFFFFFFFF` | Query failure (`const_addr`/`const_len`/`mem_alloc`) |

## 11. Debugger

`CORE::DebugHook` is installed only when any of `-i`, `--step`, `--break`, `--trace` is present. The
hook runs **before** each instruction executes; `pc` points at the instruction that has not run yet.
The ordinary execution path installs no hook. `-i` or `--step` makes the first stop happen before the
entry instruction; `--break` alone does not stop at entry.

### 11.1 Breakpoint Syntax

`--break` and the REPL `break`/`b` share one syntax:

| Form | Meaning |
|---|---|
| `#N` | Absolute instruction index in module CODE (pc = N × 4) |
| `N` | Byte offset; must be a multiple of 4 |
| `function` | Function entry (the FUNCS name constant) |
| `function:offset` / `#functionIndex:offset` | Function entry + instruction offset; offset must be `< instr` |

- Out of range, unknown function, unaligned offset, or a duplicate pc makes `--break` report an error
  and exit 2; inside the REPL it only prints an error.
- Breakpoint addresses are CODE offsets in the entry module.
- A hit stops before the instruction executes; `continue` skips one hit at the same pc to avoid
  immediately re-entering.

### 11.2 REPL Commands

The REPL reads commands from stdin; the prompt, command output, and diagnostics all go to stderr.

| Command | Description |
|---|---|
| `help` / `h` / `?` | Command list |
| `continue` / `c` | Resume until the next breakpoint or the end |
| `step [N]` / `s` | Execute N instructions, then stop (default 1; maximum 1,000,000) |
| `next` / `n` | Step over when the current instruction is `call`, otherwise `step 1` |
| `finish` / `f` | Run until the current frame returns |
| `break` / `b <spec>` | Set a breakpoint (syntax in §11.1) |
| `delete` / `d <id\|*>` | Delete breakpoints |
| `info breakpoints` | List breakpoints (id, pc, function, source location); also accepts `info break` / `info b` |
| `backtrace` / `bt` / `where` | Call stack, innermost first |
| `regs` / `registers` / `r [start [n]]`, `r all` | Current-frame registers; default start=0, n=16 |
| `print` / `p <rN>` | One register (hex + signed) |
| `x` / `mem` / `x/mem <addr> [len]` | Memory dump; len 1..256, default 16 |
| `disasm` / `u [#pc] [n]` | Disassemble; `#N` is an instruction index; default starts at the current pc, 8 instructions |
| `locals` | Named locals visible at the current pc (requires DEBUG Locals) |
| `location` / `loc` / `line` | Current `file:line:column` and function |
| `quit` / `q` | Stop the program, exit 0 |
| `abort` / `kill` | Stop the program, exit 3 |

- The `(motevm) ` prompt is printed only when stdin is a terminal; it is disabled for pipes and
  redirection.
- stdin EOF is equivalent to `quit`.
- The REPL only calls `Core` read-only accessors and never re-enters the VM.
- `r all` shows the current function's `regcnt` (capped at 256); an explicit start without n still
  shows 16 registers.
- `x` must stay inside the 4 GiB address space; unmapped rows print `<unmapped>`.
- `disasm` treats a count of 0 or >64 as 8.

### 11.3 Example

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

- A breakpoint stop prints only the prompt; `-d` stop context is printed after the run ends.
- `continue` to completion produces no `note: stop`.

## 12. Instruction Trace

`--trace LVL` installs the debug hook and records instruction events:

| Level | Content |
|---|---|
| `off` (default) | No trace (unless other debug options require the hook) |
| `call` | `C` (call), `R` (ret), `T` (trap), `S` (stop snapshot), `X` (end) |
| `exec` | Adds `E` for every instruction |
| `full` | Adds `G` register snapshots after every instruction |

Target: stderr by default; `--trace-file PATH` writes a file, `-` = stdout. The trace file is opened
only when `--trace` is not `off`; `--trace-file` alone is ignored. With `-w` the path must stay
inside the workspace and the parent directory must exist; an open failure or escape exits 2
(`--status` is still printed).

Line format (`seq` increases per line, except `X`; no timestamps or host addresses):

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

| Line | Fields |
|---|---|
| `E` | `seq`, `f` (function index or `?`), `pc`, `op` (lowercase disassembly text), optional `src="file:line:col"` |
| `C` | `seq`, `f`, `pc`, `callee=#N`, `arity=N` (when the index is valid), `dest=rN` |
| `R` | `seq`, `f`, `pc`, `value` (signed decimal) |
| `T` | `seq`, `f`, `pc`, `code` |
| `G` | `seq`, `f`, `pc`, then `r0=0x…` etc. (smaller of the current function's `regcnt` and 16) |
| `S` | `depth`, `f`, `pc` |
| `X` | `status`; adds `trap=N` for traps and `code=N` for other errors |

- `E`'s `op` uses the raw lowercase `DISASM::format` text (for example `hostcall r1, #1`), which
  differs from the uppercase mnemonics and symbolic names in canonical `.mta`.
- `X status`: `halted` / `trap` / `memory-fault` / `fuel-exhausted` / `interrupted` / `debug-stop` /
  `trace-limit` / `error`.
- `--trace-limit N` records the first N lines, then stops recording (execution continues) and emits
  `X status=trace-limit`; `0` means unlimited.
- The same module plus the same host input produces byte-identical trace output.
- When `--trace` and the REPL are both active, both write to stderr; use `--trace-file` to separate
  them.
