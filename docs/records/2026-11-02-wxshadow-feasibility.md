# wxshadow 可行性评估（2026-11-02）

评估对象：`_repos/mkpms`（1.1.0）与 `_repos/mkpms-stealth`（1.2.0-k66）里的 `kpms/wxshadow`。
结论先行：**HWBP 与 wxshadow 不是同一层的替代品**，HWBP 管「执行流拦截」，wxshadow 管「代码字节对读方不可见」。
我们现有的 `stealth_hook` 已经把前者做到了可用；后者有七个场景是 HWBP **原理上**做不到的（不是实现不够），
移植 wxshadow 可行，建议按三个阶段做，先只在 6.1 上验收。

## 1. 我们的 HWBP 现在能做什么

依据 `modules/KPM/stealth_hook/`（0.3.4）与两设备实测：

| 能力 | 现状 |
| --- | --- |
| 在指定地址拦截执行 | 可以。`hook <addr> [--tid N\|all]`，命中后回调可读改寄存器、跳过原指令、伪造返回值 |
| 改被拦截那一刻的寄存器 | 可以。`override <addr> --set x0=...`，`selftest` 里 `call with modify: 546` 即改后结果 |
| 不改被拦函数的字节 | 可以。HWBP 不往代码页写任何东西，`/proc/pid/mem` 读到的还是原字节 |
| 观察数据访问（watchpoint） | 可以。DBGBCR 的 BT 位切到 load/store 断点 |
| 内核态函数拦截 | 可以（`perf` 侧受限时走 MSR 直写） |
| 一地址多观察者 | 不可以。同地址重复注册按 `(addr, tid)` 去重，返回 `EPERM/EINVAL` |

## 2. HWBP 原理上覆盖不了的七个场景

这七条不是「还没写」，是硬件断点机制本身给不出的东西。每条都写明为什么。

1. **同一地址「读看到原样、执行跑改过的」**。HWBP 只在执行到该地址时陷入异常，
   而 `/proc/pid/mem`、`process_vm_readv`、`mmap` 共享、GUP 都是走页表取字节的路径，根本不经过断点。
   要让读方看到原字节、同时让执行跑另一份代码，必须让「读」和「执行」落在**两套页表项**上——这正是 wxshadow 的 W^X 分裂。
2. **替换任意长度的代码逻辑**。断点只能改寄存器、跳一条指令、造一个返回值；
   要在 `strcmp` 里插入整段判断、把函数体换成自己的实现，需要写字节。
3. **一次覆盖大量地址**。ARM64 每 CPU 只有 4–6 个断点槽位（本机 `perf` 实测 6），
   要覆盖某个 so 的十几个函数、或整张 JNI 表，槽位不够，且槽位占用本身可被 `perf_event_open` 计数发现（我们的 n173 就是这么检测的）。
4. **热路径不能有 trap**。每次命中断点都是一次同步异常，热函数上会带来数量级开销，且异常计数/耗时差可被外部测量。
5. **多线程与新线程一致性**。HWBP 按 task（perf）或 CPU 生效，多线程进程要逐线程装，
   `clone`/`fork` 出来的新线程还得跟——这里的「跟」永远有窗口。
6. **写侧的静默改写**。watchpoint 只能在写完**之后**知道，改不了这次写入的值本身。
7. **对抗检查页表/权限的自检**。HWBP 完全不碰页表，因此也无法制造（或消除）PTE 层的证据；
   相反，凡是检查「这段内存为什么 `--x` 不可读」「maps 的权限和 PTE 是否一致」的检测，只有真正改页表的方案才需要应对。

## 3. wxshadow 提供什么

机制（读自 `kpms/wxshadow/README.md` 与源码）：

- **shadow page**：复制原代码页，在断点处写 `BRK`（或自定义 patch），PTE 设为 `--x`（可执行不可读）。
- **读隐藏**：进程读取该页时，缺页处理把 PTE 切成 `r--` 指向原始页；执行时切回 `--x` 的 shadow 页。
- **单步恢复**：`BRK` 命中后切到 `r-x` 原始页执行被替换的那条原指令，单步结束后切回 shadow。
- 状态机：`NONE → SHADOW_X(--x) ↔ ORIGINAL(r--) ↔ STEPPING(r-x) ↘ DORMANT`。
- 接口：hook `prctl`，用 `PR_WXSHADOW_SET_BP / SET_REG / DEL_BP / SET_TLB_MODE / PATCH / RELEASE`（`0x57580001`–`0x57580008`）控制；
  另有 `READ/ACTIVE` 两个接口用于「先 `rw-` 让用户态写自定义 hook、再切 `--x` 激活」。
- 辅助隐藏：`do_page_fault`/`do_mem_abort`（读路径）、`follow_page_pte`（GUP 路径）、`copy_process`/`dup_mmap`（fork 保护）、`exit_mmap`（清理）。

与现有模块的关系：**没有同函数冲突**。我们钩的是 `show_vfsmnt`/`show_mountinfo`/`show_vfsstat`（mount_hidder）、
`show_map_vma`/`show_smap` 等（memcrc）、`__NR_getcwd`/`__NR_ptrace`（memcrc）、`__NR_openat`/`__NR_faccessat`/`__NR_connect`（panda_hide）、
`get_ap_mod_exclude`（apkey_hide）；wxshadow 钩 `prctl`、`brk_handler`/`single_step_handler`、`do_mem_abort`、`follow_page_pte`、`copy_process`、`exit_mmap`。
唯一交叠面是「都用链式钩子」这条约束，`local` 槽位要另分配（现在 `data0`=mount_hidder、`data7`=memcrc）。

## 4. 移植可行性证据

### 4.1 符号面：0 缺失（两形态都过）

用 `tools/load_forms.py` 的两张表核对 wxshadow 实际使用的 KP API：

| 符号 | kernel | lkm |
| --- | --- | --- |
| `hook_syscalln`（prctl） | 有 | 有 |
| `hook_wrap` / `hook_wrap2` / `hook_unwrap_remove` | 有 | 有 |
| `kallsyms_lookup_name` | 有 | 有 |
| `kallsyms_on_each_symbol` | 有 | 有 |
| `task_struct_offset` | 有 | 有 |
| `pgtable_entry` | 有 | 有 |
| `syscalln_addr` / `has_syscall_wrapper` | 有 | 有 |
| `cred_offset` | 有 | **无**（wxshadow 不引用它，只有注释里提到） |
| `kallsyms_lookup_name_by_suffix` / `inline_wrap_syscalln` / `fp_wrap_syscalln` / `kf_strnstr` | 有 | **无**（wxshadow 一处都没用） |

也就是说，把 wxshadow 拷进来时**不会重演前五个模块那次「LKM 装不上」**——它天生只用可移植子集里的 API。

### 4.2 目标内核符号存在性

两台设备都查了 `/proc/kallsyms`（root）：

| 符号 | Pixel 6（6.1.99，`sha256:433ab88276ec`） | 小米 13（5.10.149，`sha256:cdc3e9167b02`） |
| --- | --- | --- |
| `register_user_break_hook` / `register_user_step_hook` | 有 | 有 |
| `brk_handler` / `single_step_handler` / `call_break_hook` | 有 | 有 |
| `do_mem_abort` / `do_page_fault` / `follow_page_pte` | 有 | 有 |
| `copy_process` / `exit_mmap` / `__flush_tlb_range` / `__split_huge_pmd` | 有 | 有 |
| `dup_mmap` | **无**（有 `uprobe_dup_mmap`，上游已做兜底） | 有 |

### 4.3 页表不靠版本号

`wxshadow_scan.c:185-195` 从 `TCR_EL1` 与 `va_bits` 现算 `page_shift`（12/14/16）与 `page_level`，
`wxshadow_pgtable.c` 按 `wxshadow_pgtable.c:32-47` 的 `pxd_bits` 计算各级 shift，3/4/5 级都走同一套代码。
没有 `if (kernel_version == 5.10)` 这种分支。

### 4.4 上游选哪个

| | `mkpms` 1.1.0 | `mkpms-stealth` 1.2.0-k66 |
| --- | --- | --- |
| wxshadow 行数 | 7530 | 7871 |
| 6.1+ maple tree | 未处理（`mm->mmap` 已移除） | 已改 `find_vma()` 探测 |
| `vm_mm` 默认偏移 | `0x40`（旧链表布局） | `0x10`（6.1+） |
| 页错误钩子 | `do_page_fault` | 优先 `do_mem_abort`，`do_page_fault` 兜底 |
| 符号解析 | `on_each_symbol` 为主 | 多别名 + `kallsyms_lookup_name` 兜底 |
| TBI/MTE | 无 | FAR 去 top-byte |

→ **以 `mkpms-stealth` 1.2.0-k66 为基线**，它已经把 6.1 的坑踩过了；我们只需再补 5.10 的差异。

### 4.5 规模对比

| 模块 | 行数 | 移植耗时（本仓实际） |
| --- | --- | --- |
| mount_hidder | ~300 | 主移植 + 1.0.3 双形态约 1 天 |
| panda_hide | ~1200 | 主移植 + 2.0.1 约 1 天 |
| memcrc | ~3500 | 主移植 + 0.2.3/0.2.4/0.2.5 约 2 天 |
| **wxshadow** | **7871 + 461（client）** | 预估 3–4 天（含 PTE 调试） |

## 5. 风险

| 风险 | 说明 | 缓解 |
| --- | --- | --- |
| `struct break_hook` ABI | wxshadow 自己定义了 `struct wx_break_hook { struct list_head node; int (*fn)(...); u16 imm; u16 mask; }` 并传给内核的 `register_user_break_hook`；布局若与目标内核不一致会直接崩 | 优先走「直接钩 `brk_handler`」；`register` 路径加运行时自校验（注册后用 `BRK #imm` 触发一次，确认回调被调用）；两设备都要实测 |
| PTE 操作边界 | huge page、COW、MTE/TBI、`--x` 页被 `mprotect`/`mremap` 打断 | 上游已有 `__split_huge_pmd` 与 TBI 处理；验收时加 `mprotect`/`mremap` 干扰用例 |
| 与检测侧的对抗 | 我们的 n171–n174 都不覆盖它；反过来它会暴露新特征（`--x` 且不可读的匿名可执行页、`maps` 与实际 PTE 不一致） | 补一条检测（见 §6 P3），既是能力也是验证手段 |
| 性能 | 每次断点命中两次异常 + TLB flush | 上游有 `SET_TLB_MODE` 三档；验收时测热路径 |
| 审计成本 | 7871 行，且是内核页表代码 | 分三阶段，每阶段独立审计；不接受「一次性全量审计」 |

## 6. 分期计划与判据

**P1 核心（6.1 上先跑通）**
范围：`wxshadow.c`/`wxshadow_bp.c`/`wxshadow_pgtable.c`/`wxshadow_scan.c` 裁剪版 + `prctl` 接口 + client 最小集（`-p/-a/-b/-o/-d`）。
判据：① `prctl` 设断点后目标进程执行到该地址被拦；② `/proc/<pid>/mem` 读该地址仍是原字节；③ `/proc/<pid>/maps` 的权限位与普通可执行页无差别；
④ `--release` 后页恢复、读执行都回到原样；⑤ `unload` 后无残留、dmesg 无 BUG。

**P2 生命周期**
范围：`copy_process`/`dup_mmap`/`exit_mmap`/`do_mem_abort`/`follow_page_pte`。
判据：fork 出的子进程看不到断点也不崩；进程退出后 shadow 页被回收（无泄漏）；GUP 路径读到原字节。

**P3 双形态 + 检测**
范围：LKM 形态验证（`load_forms` 声明 `["kernel","lkm"]`）；给 runtime-sense 补一条「`--x` 无读权限的可执行页」检测。
判据：同一份产物两形态 `load rc=0` 且 P1 判据全过；检测侧能在设断点后稳定报出、`--release` 后不报。

## 7. 不建议做的事

- 不要为了「功能多」直接把 7871 行整体拷进来再改——P1 阶段只保留 core，GUP/fork 留到 P2，否则调试面太大。
- 不要用 `mkpms` 1.1.0 做基线（`mm->mmap` 在 6.1 已不存在，等于自带一个必然失败的路径）。
- 不要把 wxshadow 当成 stealth_hook 的替代：两者解决不同问题，共存时按 §3 的槽位表分配 `local`。

## 8. 结论

值得做，且难度不在「符号/形态」（这块已经是 0 缺失），在「页表状态机 + 单步恢复」的正确性与审计成本。
建议 P1 先做 6.1 上的核心闭环，用 §6 的五条判据当验收门；P1 通过再投入 P2/P3。
