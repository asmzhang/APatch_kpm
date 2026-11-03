# wxshadow 移植记录（2026-11-03）

## 身份

- 模块：`wxshadow`，版本 `1.2.5`（产物名）／`MYKPM_VERSION`（1.2.1 起源码不再写死上游串，
  `KPM_VERSION(MYKPM_VERSION)` + `-DMYKPM_VERSION` 注入）。
- 上游：`mkpms-stealth` 的 `kpms/wxshadow/`，CHANGELOG 最新条目 `[1.2.0-k66] - 2026-07-24`。
- 产物（移植时，上游源码原样）：`modules/KPM/wxshadow/wxshadow_1.2.0.kpm`，157,904 B，
  `sha256=c254e1231f5c1655e1365060deee3fb8002c8c8a01c297a326e1e6e0e6f41a1c`。
- 产物（含 prctl 修复，1.2.0）：`wxshadow_1.2.0.kpm` 159,616 B，
  `sha256=482be50301bdfce7920368f00a629c78729f35df1fb58c56bd16d6128938d0cb`。
- 产物（含复核问题单修复，1.2.1，当前源码）：`wxshadow_1.2.1.kpm` 159,536 B，
  `sha256=069ce62815f9e89022d8846e069746001ebf1a115ad9fdd8918ab5459ab4f3eb`（探索构建值，
  冻结构建以 `metadata` 的 instance 为准）。
- 后续版本（每版都在下文有对应章节）：
  - 1.2.2：`wxshadow_1.2.2.kpm` 159,864 B，
    `sha256=7bf2ac5abbaa5a2d…`；instance `wxshadow-1.2.2+g1497a6db4e55.r0ac889dd.kpb51197a.ndk26.1.10909125#1`。
  - 1.2.3：`wxshadow_1.2.3.kpm` 163,760 B，
    `sha256=06ddb4d98d3c94a01340dcfce42dbbb573ef6696eef78508f1ffed02d30c4dfe`；instance
    `wxshadow-1.2.3+ge1e4f2897c5f.r5a259d01.kpb51197a.ndk26.1.10909125#1`；`source_commit` 见下一节。
  - 1.2.4：产物 `wxshadow_1.2.4.kpm`，构建/实例号见本节末尾与 `metadata/modules/wxshadow.json`。
- 客户端：`Developer/tools/wxshadow_client/wxshadow_client`，3,517,920 B（aarch64 静态）。

### 字节身份的行尾口径（AUD-025）

本仓 `core.autocrlf=true`、无 `.gitattributes`，模块源码在工作树里是 CRLF，在提交里是 LF。
因此 `metadata` 每条 source 同时记两份哈希：

- `blob_sha256`：**提交里的字节**（LF），按 `git show <commit>:<path>` 复算；
- `sha256`：**工作树里的字节**（CRLF），也就是编译器的实际输入。

核验时不要拿 `git cat-file` 的字节去对 `sha256`，那一定「假失败」；按上面的口径分别核对。

## 变更点（相对上游）

构建侧适配 + 一处通道修复（prctl，见文末小节），其余业务代码零改动：

1. 新增本仓风格的 `Makefile`（NDK clang、`-r` 链接、`KP_DIR` 相对路径），替代上游 cmake
   `add_kpm_module`。
2. **不生成 debug 变体**：上游源码没有任何 debug-only 分支，加 `-D` 编出的字节与 release
   完全相同（实测两次构建同哈希），多一个同哈希产物只会污染身份记录。
3. 客户端从 `cmake + aarch64-linux-gnu-gcc` 改成 NDK clang 静态编译，落在
   `Developer/tools/wxshadow_client/`。

源文件清单：`wxshadow.c`（**唯一改动**：prctl 通道改 `hook_wrap`）、`wxshadow_pgtable.c`、
`wxshadow_handlers.c`、`wxshadow_bp.c`、`wxshadow_scan.c`、`wxshadow.h`、`wxshadow_internal.h`
（其余 6 个与上游逐字节一致）。

## 自检（实现方，不是结论）

- 编译：`make all` 通过；frozen 构建日志里共 **53 条 warning、6 类**，其中绝大多数来自
  KernelPatch 头（`asm/cmpxchg.h` 的 `-Wc2x-extensions`、`rcupdate.h` 等），模块源码自身有 3 条：
  `wxshadow.c:361` `wxshadow_bitmap_clear_range` 未使用（上游既有）、`wxshadow_pgtable.c:116`
  `pa` set but not used、`wxshadow_pgtable.c:373` `mode_str` set but not used。
  （1.2.2 起 `wxshadow_bitmap_clear_range` 仍未被引用，属上游既有告警，未清理。）
- 导入符号 17 个，全部为 KernelPatch 导出：`has_syscall_wrapper`、`hook_syscalln`、
  `hook_unwrap_remove`、`hook_wrap`、`kallsyms_lookup_name`、`kallsyms_on_each_symbol`、
  `kf_memcpy`、`kf_memset`、`mm_struct_offset`、`printk`、`sp_el0_is_current`、
  `sp_el0_is_thread_info`、`task_in_thread_info_offset`、`task_struct_offset`、
  `thread_info_in_task`、`thread_size`、`unhook_syscalln`。
- 真机加载：
  - Pixel 6（6.1.99-android14，kernel 形态）`kpmctl 10273 load` → `rc=0`，dmesg `[wxshadow] succeed`。
  - 小米 13（5.10.149-android12-9，APatch LKM 形态）`kpmctl 10270 load` → `rc=0`，同上。
  - **两形态符号面 0 缺失**，是目前唯一不需要任何符号适配就能双形态加载的模块。
- 页级切换确认在跑（Pixel 6）：`exec fault at 221d28 → shadow (--x)`、
  `read fault at 221c98 → switched to live original (r--)`。

## 已知问题（按「先移植、后修错」留待下一轮）

1. ~~**prctl 通道返回 EINVAL**~~（**已修**，见下方「prctl 通道修复与端到端验证」）。`wxshadow.c:1495` 用 `hook_syscalln(__NR_prctl, 5, prctl_before,
   NULL, NULL)`；这版 KernelPatch 的 `hook_syscalln` 走 `syscall_hook_add(..., allow_skip=0)`，
   不采纳 `args->skip_origin` / `args->ret`，于是请求落到原生 `prctl` → `EINVAL`，内核侧连
   `wxshadow_do_patch` 的第一行 `pr_info` 都没打。`panda_hide` 踩过同一个坑，修法是把
   syscall 钩改成 `syscalln_addr(nr, 0)` + `hook_wrap(addr, has_syscall_wrapper ? 1 : 6, ...)`，
   卸载相应改成 `hook_unwrap_remove`。
2. **卸载会让设备重启**。实测 `kpmctl unload wxshadow` 后设备从 adb 消失，回来后 `uptime` 归零。
   它 hook 了 `copy_process` / `exit_mmap`，卸载要逐个摘钩并释放全部影子页。按仓库
   「真机死机不自救」规则，当场停手、未反复重试；证据只到「设备重启」这一层，
   内核栈没能抓到（`/sys/fs/pstore` 里只有更早的一次 ramoops）。在修好之前：
   **只加载不卸载，清状态靠重启。**
3. 待复核：`/proc/<pid>/mem` 读到的是影子页内容还是原字节。6.6 上 `follow_page_pte` 不存在
   属上游已知项；6.1 上该钩子未装，GUP 路径可能泄漏影子页。需要干净复现一次再下结论。

## prctl 通道修复与端到端验证（2026-11-03）

### 修复

`modules/KPM/wxshadow/wxshadow.c`：新增 `wx_lookup_name3()` / `wx_syscall_addr()`（候选顺序与
KernelPatch 的 `syscalln_name_addr()` 一致：前缀 `__arm64_` → 无，后缀 `.cfi_jt` → `.cfi` → 无），
把 `hook_syscalln(__NR_prctl, 5, ...)` 换成 `hook_wrap((void *)wx_prctl_sys_addr,
has_syscall_wrapper ? 1 : 5, prctl_before, NULL, NULL)`，两处 `unhook_syscalln` 换成
`hook_unwrap(...)`。业务逻辑零改动。

**踩坑**：名字要传 `"sys_prctl"` 而不是 `"prctl"`——KP 的符号是 `__arm64_sys_prctl`（或
`__arm64_sys_prctl.cfi`），传 `"prctl"` 会拼出 `prctl` / `__arm64_prctl` 全部落空，dmesg 打
`prctl syscall address not found` + `failed to hook prctl: -2`，`load rc=-1`。
顺带确认：init 失败时装载器会调 `mod_exit()` 走完整清理（`module unloaded (cleaned 0 pages)`），
**没有 panic**——所以「卸载重启」很可能只在「有影子页要清」时出现。

### 产物

- `wxshadow_1.2.0.kpm` 159,616 B
  `sha256=482be50301bdfce7920368f00a629c78729f35df1fb58c56bd16d6128938d0cb`（两次构建同哈希）。
- 告警与既有项同前：`wxshadow_bitmap_clear_range` 未使用等 3 条模块源码告警（见上节 R5 更正）。

### 真机结果（Pixel 6 / 6.1.99-android14 / APatch 11282 / kernel 形态）

| 用例 | 命令 | 结果 |
| --- | --- | --- |
| 列可执行段 | `-p <pid> -m` | 正确列出 `0x221000-0x2a4000 r-xp /data/local/tmp/wx_t`、`[vdso]` |
| 设断点 + 改寄存器 | `-p <pid> -a 0x221cf4 -r x0=7` | `Breakpoint set` + `Register modification set`；靶子日志 `ret=7`；dmesg `Breakpoint Hit` / `modifying x0: 29 -> 7` |
| 双视图 | `-a 0x221df8 -r x0=7` 打 `wx_v` | 靶子每 50 轮打印自己 `.text` 前 8 字节 = `28 00 80 52 00 05 00 2a`（原始 `mov w8,#1; orr w0,w8,w0,lsl#1`），**同时** `ret=7` |

结论：**「读原字节 + 执行补丁」在同一个 VA 上成立**，不占硬件断点槽位、不改原页一个字节。

### 排查中被证伪的两条

- **不是 TLB 问题**：临时在 `wxshadow_flush_tlb_page()` 里追加 `tlbi vmalle1is` 全量刷，行为不变；
  已回退（`wxshadow_pgtable.c` 与上游逐字节一致）。`get_user_pte` 算出的 PTE 也是对的——用
  `/proc/<pid>/pagemap` 交叉验证：设断点前 PFN `0x900354`（与 PTE 一致），设断点后变成影子页 PFN，
  说明页表确实被改了。
- **不是 BRK 没写进影子页**：真正的原因是**靶子写错了**。第一版 `wx_t` 写 `target(20)`，编译器把
  常量折叠并把 `bl target` 提到循环外（`llvm-objdump` 循环体内没有 `bl`），所以循环里根本没执行
  被测函数，返回值恒 41。改成入参取 `volatile` 后立刻 `ret=7`。

### 页粒度的一个真实边界（记录，不修）

`wx_v` 的 `.text` 里同时有 `main` 和 `target`，读一次就把整页切到 `ORIGINAL`，下一次取指又切回
`SHADOW_X`——切回来之前执行的是**原始指令**。所以「同一页既读又执行」的紧循环里，断点命中是
**非确定性**的（实测 `i=1/2` 时 `ret=41`，`i=50` 时 `ret=7`）。要确定性，被改的代码页不能同时被读。

## 未覆盖

- 页级 patch（`--patch` 写自定义字节）只验了「写进影子页」，没验「执行侧生效」的独立用例。
- 小米 13 上只验了加载，没验页级切换。
- 多线程同页、fork/exit、`mprotect` 接管等上游声明的路径均未验证。

## 卸载稳定性观察（2026-11-03）

`wxshadow` 的 `mod_exit` 分七个阶段摘钩子（prctl → 页面 → dup_mmap/uprobe_dup_mmap → brk/step → do_page_fault → follow_page_pte → exit_mmap），
每阶段后调 `wait_for_handlers_drain()` 等 `wx_in_flight` 归零。开发过程中观察到两次卸载后设备重启，之后做了针对性复现：

| 场景 | 次数 | 结果 |
| --- | --- | --- |
| 空闲状态反复 load/unload | 5 | 全部 `rc=0`，`uptime` 连续，无异常 |
| 挂断点 → 靶子退出（`cleaned 1 pages`）→ 卸载 | 1 | `rc=0`，设备存活 |
| 挂断点 → 靶子仍在运行 → 卸载 | 1 | `rc=0`，设备存活 |

两次重启都发生在早期「刚跑完页级乒乓/重定向实验」的会话里，`/sys/fs/pstore/` 只留下 2025-03-25 的旧 `console-ramoops-0`，
没有抓到这次的 Oops。**当前无法复现，也不宣称已修**；`wxshadow_exit()` 与上游逐字节一致，未做任何改动。

使用建议：需要清状态时优先重启；必须在运行中卸载时，先 `release` 掉所有断点、让被挂进程退出或保持空闲，再执行卸载。

## 1.2.1：独立复核问题单修复（2026-11-03）

复核由独立审计员完成（workflow `wxshadow-102-audit`，代码路 + 文档路两路，未共享实现方脚本），
两路 verdict 均为「暂不可关单」。本轮按问题单逐条处理：

| 编号 | 级别 | 问题 | 处理 |
| --- | --- | --- | --- |
| WX-1 | high | `prctl_before` 无任何权限校验，任何进程知道魔数即可对任意 pid 打补丁（等价代码注入原语） | `wxshadow_bp.c` 在 `WX_HANDLER_ENTER()` 之前加身份门：只允许 uid 0 / 2000，其余 `-EPERM` 并 `skip_origin` |
| WX-2 | high | `wait_for_handlers_drain` 超时只 `pr_warn` 后继续，`mod_exit` 仍返回 0 → KP 释放模块代码时 handler 可能还在跑 | 函数改返回 `int`；超时 `return -1`，由新增 `wx_drain_checked()` 转成 `-EBUSY` 中止卸载（8 个调用点全部改为检查返回值） |
| WX-3 | low | `buf_off + len > PAGE_SIZE` / `offset + len > PAGE_SIZE` 可被整型回绕绕过 | 改为 `len == 0 \|\| len > PAGE_SIZE - off` |
| WX-5 | info | 源内 `KPM_VERSION("1.2.0-k66")` 与产物名 1.2.0 不一致 | 改 `KPM_VERSION(MYKPM_VERSION)`，Makefile 注入；`MYKPM_VERSION := 1.2.1` |
| F4 | medium | 客户端 README 仍写「所有命令返回 EINVAL」 | 重写「当前状态」节，写清已修、必须 root、权限门、卸载兜底 |
| F5 | low | 模块 README 仍写「只能加载不能卸载」 | 已知问题 2 改写为「累计 9 次：早期 2 次重启 + 随后 7 次正常、不宣称已修、1.2.1 加兜底（1.2.2 已按 NX-1 撤回）、未实测该分支」 |
| WX-4 | low | `copy_from_user_via_pte` 无 `mmap_lock`、无 pin、未校验 `pfn_valid` | **未修**：加锁会与调用路径的锁序冲突，且影响面是「读到错页/旧数据」非提权；如实留在已知问题 |
| F9 | low | handoff JSON 的 `recipe.cc`/`excerpt` 含本机绝对路径 | 登记时把 `Developer/reports/handoffs/` 副本归一为占位符（`local/` 下的原件不动） |

### 真机验证（Pixel 6 / 6.1.99-android14 / APatch 11282）

| 项 | 命令 | 结果 |
| --- | --- | --- |
| 加载 | `kpmctl 10273 load wxshadow_1.2.1.kpm` | `load rc=0`，dmesg `load_module_ex: [wxshadow] succeed` |
| 权限门（拒绝） | `su 10273 -c 'wxshadow_client -p <pid> -a 0x221cf4 -r x0=7'` | `prctl(SET_BP) failed: Operation not permitted (errno=1)`，靶子仍 `ret=41` |
| 权限门（放行） | `su -c 'wxshadow_client -p <pid> -a 0x221cf4 -r x0=7'` | `Breakpoint set at 0x221cf4` + `Register modification set: x0 = 0x7`，靶子 `ret=7` |
| 双视图 | `su -c 'wxshadow_client -p <pid> -a 0x221df8 -r x0=7'`（`wx_v` 自读 `.text`） | 日志 `text @0x221df0: 28 00 80 52 00 05 00 2a`（原字节）+ `i=100 ret=7`（补丁生效） |
| 卸载 | `kpmctl 10273 unload wxshadow` | `unload rc=0`，`uptime` 8:45 连续、设备不重启 |

页粒度边界照旧（同一页里既有读又有执行时命中是非确定性的：`i=2/50` 是 41、`i=100` 是 7），
这是上游设计使然，本轮不改。

### 环境注意（本轮踩到）

`kpmctl` 内部要 `setgid/setuid` 到管理器 uid 才能过 `is_trusted_manager_uid`。当前这台
Magisk 环境下 `su -c 'kpmctl 10273 ...'` 会报 `setuid(10273) failed: Operation not permitted`，
改用 **`su 10273 -c 'kpmctl 10273 ...'`**（先切 uid，内部 setuid 变空操作）即可。与模块无关。

### 双形态复验（小米 13 / 5.10.149-android12-9 / APatch 11282 / LKM 形态）

同一份产物 `wxshadow_1.2.1.kpm`（159,536 B，`sha256=069ce62815f9e890…`）在 LKM 形态下的实测：

| 项 | 命令 | 结果 |
| --- | --- | --- |
| 加载 | `su 10270 -c 'kpmctl 10270 load /data/local/tmp/wxshadow_1.2.1.kpm'` | `load rc=0`，dmesg `kernelpatch-lkm: KPM [wxshadow] init returned 0` + `[wxshadow] succeed with []` |
| 权限门（拒绝） | `su 10270 -c 'wxshadow_client -p <pid> -a 0x1000 -r x0=7'` | `prctl(SET_BP) failed: Operation not permitted (errno=1)`；dmesg `wxshadow: prctl denied for uid 10270` |
| 权限门（放行） | `su -c 'wxshadow_client -p <pid> -a 0x556c6fc100 -r x0=7'` | `Breakpoint set at 0x556c6fc100` + `Register modification set: x0 = 0x7`；dmesg `bp at 556c6fc100 orig_pfn=9799c5 shadow_pfn=8a9640 once=0` |
| 卸载 | `su 10270 -c 'kpmctl 10270 unload wxshadow'` | `unload rc=0`，`/proc/uptime` 75315 连续（重启前 75205），**未重启** |

顺带确认 LKM 形态下 `GUP hiding ENABLED (follow_page_pte hooked)` 也打出来了，说明 `follow_page_pte` 钩子在 5.10 上挂成功。

**发现的噪声问题（记录，未在本版修）**：init 末尾无条件调 `debug_print_tasks_list(10)`（`modules/KPM/wxshadow/wxshadow_scan.c:1240-1296`，调用点 `wxshadow.c:1664`），每次加载都往 dmesg 打10 行内核任务指针（`task=%px mm=%px`）与 `task_struct_offset` 三个偏移。是上游调试代码，无功能副作用，但属内核地址泄漏 + 日志噪声。**1.2.1 保持与上游一致不改**，留作下一版（1.2.2）用编译宏关掉，避免在一个已冻结的候选上再动源码。
### 未覆盖

- WX-2 的超时中止分支没有真机触发条件（需要制造一个卡死的 handler），只做了静态验证与正常路径回归。
- WX-4 未修（见上）。
- 小米 13（5.10 LKM 形态）已补测加载/权限门/设断点/卸载（见下「双形态复验」），但**未测端到端双视图**（该机没有现成的自读靶子，且 LKM 形态下无法用 `kpmctl load` 反复重启靶子）。

## 1.2.2：WX-9 根因（跨进程刷 icache 触发 kernel panic）与同批修复（2026-11-03）

### 现象与取证
Pixel 6（6.1.99-android14，APatch 11282，kernel 形态）加载 1.2.1 后，对普通系统进程
（`/system/bin/sleep`）设断点：命令无输出、adb 立即掉线、设备硬重启，**4/4 复现**
（`uptime` 回来分别为 7.37s / 6.56s / 6.15s）。同样的操作在小米 13（5.10.149，APatch LKM）
与自建静态靶子（`wx_t` / `wx_v`，映射在 `0x221000` 段）上完全正常。

pstore 里只有 2025-03-25 的旧 `console-ramoops-0`，`/proc/last_kmsg` 不存在，设备端「dmesg
落盘快照」在硬重启下丢页缓存，改为**主机侧流式抓取**：`adb shell su -c 'cat /dev/kmsg' > 主机文件`，
重启后主机侧日志完整保留。

### 根因
```
wxshadow: [set_bp] addr=6042f50100 once=0
wxshadow: created page for mm=ffffff8880c88800 addr=6042f50000
Unable to handle kernel paging request at virtual address 0000006042f50000
ESR = 0x96000145  EC = 0x25: DABT (current EL)  FSC = 0x05: level 1 translation fault
```
设断点路径（`wxshadow_page_activate_shadow_locked()` 等）在写完 PTE 后调
`wxshadow_flush_icache_page(addr)` → `wxshadow_flush_icache_range(addr & PAGE_MASK, +PAGE_SIZE)`
→ `kfunc___flush_icache_range()`（内部 `dc cvau` / `ic ivau` 循环）。传入的是**目标进程的用户 VA**，
而该 VA 在调用者（`wxshadow_client`，另一个进程）的 mm 里没有映射 → `dc cvau` 视为未映射地址，
在 EL1 触发 data abort → oops → panic → 重启。

静态靶子为什么没事：`0x221000` 恰好也映射在静态链接的客户端进程里（两者都是静态非 PIE），
`dc cvau` / `ic ivau` 不 fault，只是刷了错的物理行——既解释了「为什么以前看起来能用」，
也解释了「为什么只有跨进程到普通系统进程才炸」。

### 修复
新增 `wxshadow_flush_icache_kva(unsigned long kva)`（`wxshadow_internal.h`）：一律按**影子页的内核 VA**
刷（TTBR1 恒有效；dcache 是 PIPT，按内核 VA 清的就是指令取指要读的同一物理行），拿不到合法
内核 VA 时退化为全量 `ic ialluis`。把 11 处用户 VA 调用点全改：`wxshadow.c` 1 处、
`wxshadow_bp.c` 2 处、`wxshadow_pgtable.c` 8 处。

### 同批按独立复核问题单修复
| 单号 | 处理 |
| --- | --- |
| NX-1 / WX-2 | **撤回** 1.2.1 的 `-EBUSY` 早退。`unload_module()` 是先 `list_del`、再调 `mod->exit`、随后无条件 `kp_free_exec(mod->start)`，返回值没有生命周期语义；早退会让六组钩子指向已释放代码且无法再清理。改为只记录超时、始终走完七个 phase。 |
| WX-8 | `debug_print_tasks_list(10)` 用 `WXSHADOW_DEBUG_TASKS` 宏关掉，默认不再往 dmesg 打 10 行 `task=/mm=` 内核地址。 |
| NX-6 | 可回绕的加法式判定统一改减法式。1.2.2 改了 5 处（`wxshadow.c:349`/`:368`、`wxshadow_bp.c:589`/`1096`/`1150`，其中两处是 clamp），1.2.3 复核又发现 `wxshadow_bp.c:479`（`write_shadow_bytes`，当时不可达）未改，1.2.4 补上一并加 `offset >= PAGE_SIZE` 上界（见下文 1.2.4 节 WI-5）。 |
| NX-5 / R3 | `metadata` 的 `version_declared` 与 `version_sources.makefile` 同步到 1.2.2，并把 WX-*/NX-*/R* 登记进 `issues[]`。 |

### 真机复验（1.2.2）
| 场景 | Pixel 6（6.1.99 kernel） | 小米 13（5.10.149 LKM） |
| --- | --- | --- |
| 加载 | `load rc=0` | `load rc=0` |
| 跨进程设断点（toybox 页，正是原先必崩场景） | `Breakpoint set at 0x6042f50100` + `bp at … orig_pfn=81ecd shadow_pfn=959534`，**设备不重启** | `Breakpoint set at 0x5edb84a000`，不重启 |
| 释放 | `Modification released at 0x6042f50100` | — |
| 卸载（页在场） | `unload rc=0`，七阶段全过 + `cleaned 0 pages` | `unload rc=0`，七阶段全过 + `cleaned 1 pages` |

### 1.2.3：ctl0 权限门维护通道（AUD-wx-10 / NX-3）

### 改动

- `modules/KPM/wxshadow/wxshadow.c`：新增 `wx_allow_uids[WX_ALLOW_MAX]`（默认 `{0, 2000}`，上限 8）、
  `wx_uid_allowed()`、`wx_allow_uid()`、`wx_deny_uid()`、有界解析 `wx_parse_uid()`（1~10 位十进制、
  超 32 位即失败、末尾只允许空白）、`wx_is_word()`、`wx_dump_allow()`；把上游的 ctl0 桩
  `long wxshadow_control(...) { pr_info(...); return 0; }` 换成 `status` / `allowlist` /
  `allow:<uid>` / `deny:<uid>` 四个命令，回显走 `compat_copy_to_user`（两次，含 NUL）。
- `modules/KPM/wxshadow/wxshadow_internal.h`：导出 `bool wx_uid_allowed(uid_t uid);`。
- `modules/KPM/wxshadow/wxshadow_bp.c`：权限门由 `wx_caller != 0 && wx_caller != 2000` 改成
  `!wx_uid_allowed(wx_caller)`，注释同步说明名单来源。
- `modules/KPM/wxshadow/wxshadow.c` 加 `#include <linux/kernel.h>`：ctl0 回显用
  `kfunc_def(snprintf)` 展开出来的 `kf_snprintf`，不引这个头会报 `call to undeclared function`。
- `Makefile`：`MYKPM_VERSION := 1.2.3`。

### 真机（两条形态各一台）

| 用例 | Pixel 6（6.1.99 / kernel 形态） | 小米 13（5.10.149 / APatch LKM） |
| --- | --- | --- |
| `status` | `wxshadow v1.2.3 uids: 0 2000` | 同 |
| `allow:<管理器 uid>` | `allow 10273 rc=0` → `allowlist: 0 2000 10273` | `allow 10270 rc=0` → `allowlist: 0 2000 10270` |
| 管理器身份设断点（allow 后） | `Breakpoint set at 0x5e557a7100` + `Register modification set: x0 = 0x7`（1.2.2 之前是 `EPERM`） | 未测 |
| `deny:<管理器 uid>` 后再设 | `prctl(SET_BP) failed: Operation not permitted (errno=1)` + dmesg `prctl denied for uid 10273` | `deny 10270 rc=0` |
| `deny:0` | `deny 0 rc=-1`（root 不可移除） | `deny 0 rc=-1` |
| `allow:99999` / `allow:abc` | `allow 99999 rc=0` / `allow: bad uid` | 未测 |
| 未知命令 | `ctl rc=-1 errno=1` | 未测 |
| 默认身份 uid 2000 | `Breakpoint set at …` + `x0 = 0x9`（放行） | 未测 |
| 卸载 | `unload rc=0`，七阶段全过、`cleaned 0 pages`、`uptime` 连续 | `unload rc=0`、`uptime` 76966→76975 连续 |

同时确认 **WX-8 噪声已消失**：加载 1.2.3 后 dmesg 里不再有 `task=%px mm=%px` 那 10 行。

### 未覆盖

- 小米 13 上「allowlist 放行后由管理器身份真正设断点」没有跑（该机 client 需要 root 且要有合法可执行地址，
  名单通道本身已验证）。
- `WX_ALLOW_MAX = 8` 的溢出分支（第 9 个 uid 返回 `rc=-1`）只在代码里检查，未在真机打满验证。
- `AUD-wx-4`（`copy_from_user_via_pte` 无 `mmap_lock`）仍 open。

## 未覆盖
- 未在 Pixel 6 上重跑「读原字节 + 执行补丁」端到端（该验证在 1.2.1 已做过；1.2.2 只改了刷 icache
  的方式与 drain 语义，未动映射/断点数据流）。
- 1.2.1 的两次卸载重启仍无法复现，1.2.2 亦未宣称已修。

## 权限门根治：KernelPatch fork f0c9e72 与两条设备事实（2026-11-03）

### 根因（跨子模块）
同一个符号 `current_uid` 在两种装载形态下语义不同：

- kernel 形态：`KernelPatch/kernel/patch/common/utils.c:172-178` 直接读 `cred->uid`（裸 kuid）。
- LKM 形态：`KernelPatch/lkm/kpm/symbols.c:132-134` 多一步 `from_kuid(current_user_ns(), current_uid())`，返回的是**调用者 user namespace 视角的 uid**。

于是凡是以 `current_uid() == 0` 判权的代码（本模块 1.2.3 的权限门、stealth_hook、memcrc，以及 KP 自身任何消费这个符号的逻辑）在「unshare 出 userns 并把 uid 映射成 0」的进程里都会被放行。

### 处置
- fork：`asmzhang/KernelPatch`（`POST /repos/bmax121/KernelPatch/forks` 返回 202）。
- 分支 `fix/lkm-current-uid-raw-kuid`，基线 `b51197a`，提交 **`f0c9e72`**「lkm: return raw kuid from the exported current_uid symbol」：`kp_kpm_current_uid()` 改为 `return current_uid();`，并加注释说明两形态语义必须一致。
- 已推到 fork（`git push -u fork fix/lkm-current-uid-raw-kuid`）。
- fork 只用于保存提案：`asmzhang/KernelPatch` 的 `fix/lkm-current-uid-raw-kuid` 分支上放着 `f0c9e72`。
  **本仓 `.gitmodules` 的 url 仍是 `https://github.com/bmax121/KernelPatch/`，submodule 指针仍钉在
  `b51197a`**（与下文「未采用」一致；曾短暂指向 fork，已回退）。

### 设备事实：这个缺陷在两台机器上不可利用
| 设备 | 内核 | 形态 | `CONFIG_USER_NS` |
| --- | --- | --- | --- |
| Pixel 6 | 6.1.99-android14 | kernel | **not set** |
| 小米 13 | 5.10.149-android12-9 | APatch LKM | **not set** |

两台都没有 user namespace，`from_kuid(current_user_ns(), x)` 在 `init_user_ns` 下是恒等映射，所以 1.2.3 及更早的权限门在这些设备上**没有被绕过**。这是潜伏缺陷（换了开着 userns 的内核才会显形），不是已经发生的绕过；`unshare -Ur` 在两台上都直接 `EINVAL`。

### 模块侧重复实现：试过、失败、已回退
在 wxshadow 1.2.4 里试过「不依赖 `current_uid`，从 `init_cred` 推导 `cred` 内 uid 偏移」：

- 6.1 kernel 形态：推导出 `cred uid offset: 4`，与 KP 启动时自己打的 `KP uid offset: 4` 一致，`allow:10273` 后管理器身份设断点正常放行。
- 5.10 LKM 形态：推导失败（dmesg `wxshadow: cred uid offset not found`），权限门 fail-closed 把管理器 uid 一起挡掉。
- 判定：结构体推导在两种形态的 `init_cred` 上不自洽，模块里再实现一遍的风险（fail-closed 锁死维护入口）大于收益 → **已回退**，模块保持 1.2.3，根治只留在 KernelPatch 一处。证据脚本 `local/patch_wx_real_uid.py`、`local/patch_wx_real_uid2.py`。

### 生效路径
改动落在 LKM 运行时的符号表里，设备上跑的是预装的 `kernelpatch-lkm` 二进制 → 要等上游重新出 `.ko`（`.github/workflows/build-lkm.yml` 用 DDK 编 `android12-5.10` / `android13-5.10`）并安装才生效。本机没有 DDK 构建环境，未尝试重编与重装 LKM。

### 验证结论：现有两台设备证不出这个问题，PR 暂缓

按「先用测试证明问题存在，再考虑提 PR」的标准，本次实测结果是**证不出来**：

| 步骤 | 命令 | 结果 |
| --- | --- | --- |
| 查内核配置 | `zcat /proc/config.gz \| grep CONFIG_USER_NS` | Pixel 6 / 小米 13 都是 `# CONFIG_USER_NS is not set` |
| 造 userns | `unshare -Ur id -u`（root 与管理器 uid 都试过） | `unshare: Invalid argument`（EINVAL） |
| 权限门行为 | `allow:10273` 后以 10273 设断点 | 放行；`deny:10273` 后 `prctl(SET_BP) → EPERM`，与 1.2.3 文档一致 |

没有 user namespace 时 `from_kuid(current_user_ns(), x)` 就是恒等映射，LKM 形态与 kernel 形态的 `current_uid` 返回值完全一致，因此：

- 这个缺陷在本项目的两台设备上**不可复现、不可利用**，只有换了开着 `CONFIG_USER_NS=y` 的内核才会显形；
- fork 上的 `f0c9e72` 作为**未验证的提案**保留，`KernelPatch` submodule 指针仍钉在 `b51197a`（未采用），保证构建输入与既有交付一致；
- 待有开 userns 的目标（设备或可加载 LKM 的内核）复现「userns root 被放行」之后再启用该提交并提 PR。

## 1.2.5：拆掉 register 回落 + 工具归一（2026-11-03）

1.2.4 的那批（AUD-wx-4 / WI-1…WI-7）与本节一起发版，避免「一个版本号塞两批不相干事」。

### 代码

- **AUD-wx-13（中）**：删除 `register_user_*_hook` 回落。`wxshadow.c` 的 init 现在只接受 direct hook
  （`hook_wrap3(brk_handler)` + `hook_wrap3(single_step_handler)`），两个符号任一缺失即
  `return -1` 拒绝加载；`wxshadow_scan.c` 的可用性判定同步收紧。连带删除
  `wxshadow_break_hook` / `wxshadow_step_hook` 两个实例、两个桥接回调和
  `wx_unregister_brk_step_hooks()`（RCU 风险的载体）。
  理由：回落路径把钩子挂进内核全局 debug 链表，卸载时只能 `list_del_rcu()`，
  而 `unload_module()` 随后无条件 `kp_free_exec(mod->start)`——RCU 宽限期内的读者会走进已释放内存。
- **AUD-wx-14（信息）**：`pin_user_pages_fast + kmap` 这条彻底修法**经评估不适用**：
  `wxshadow_read_user()` 唯一调用点是 prctl 钩子，钩子上下文不允许缺页，而 gup 慢路径会
  取 `mmap_lock`、可能触发缺页。故保留复核意见的 ①+②（无缺页走查 + `pfn_valid` +
  有符号时加 `mmap_read_lock`）。代码注释与本节都写明，避免下一个人重复尝试。

### 工具（AUD-024）

`tools/identity.py` 的 `sanitize()` 只替换 `str(REPO)` 一种写法，而 Windows 上 REPO 是
`D:\\work\\...`、make 配方里是 `D:/work/...`，于是归档的 MANIFEST 里留了本机路径。
改为「同一路径的多种分隔符写法一起替换」+ 兜底正则把其它盘符绝对路径换成 `<abs>`，
并用 NDK 环境变量名做标签。**历史 1.2.0–1.2.3 的 MANIFEST 保留原样**（产物不可覆盖），
1.2.5 起新归档的清单不再含本机路径。

### 双机复验（1.2.5）

| 项 | Pixel 6（6.1.99 / kernel） | 小米 13（5.10.149 / LKM） |
| --- | --- | --- |
| 加载 | `load rc=0`，`method=1`（direct），`mm helpers: pfn_valid=1 mmap_read_lock=1` | `load rc=0`，`method=1`，`mmap_read_lock=0`（5.10 无该符号，按设计降级） |
| 断点 + 改寄存器 | `-a 0x221cf4 -r x0=7` → `ret=7` | 同样 `ret=7` |
| 补丁（读路径） | `--patch 20018052c0035fd6` → `ret=9` | 未复跑（与 1.2.4 同一段代码，1.2.4 已验） |
| 卸载 | `unload rc=0`，`cleaned 1 pages`，`/proc/uptime` 5529.73 → 5531.84 连续 | `unload rc=0`，uptime 81493.65 → 81495.69 连续 |

### 未覆盖

- 「direct hook 拿不到时拒绝加载」这条分支在两台设备上都**无法触发**（两台都拿得到
  `brk_handler`/`single_step_handler`），只有代码审查覆盖；
- AUD-024 只对新归档生效，历史四份 MANIFEST 仍含旧路径（已在问题单记明）。
