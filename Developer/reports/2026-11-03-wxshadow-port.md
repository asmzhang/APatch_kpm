# wxshadow 移植记录（2026-11-03）

## 身份

- 模块：`wxshadow`，版本 `1.2.0`（产物名）／`1.2.0-k66`（源码内 `KPM_VERSION`，上游原值不改）。
- 上游：`mkpms-stealth` 的 `kpms/wxshadow/`，CHANGELOG 最新条目 `[1.2.0-k66] - 2026-07-24`。
- 产物（移植时，上游源码原样）：`modules/KPM/wxshadow/wxshadow_1.2.0.kpm`，157,904 B，
  `sha256=c254e1231f5c1655e1365060deee3fb8002c8c8a01c297a326e1e6e0e6f41a1c`。
- 产物（含 prctl 修复，当前源码）：同路径 159,616 B，
  `sha256=482be50301bdfce7920368f00a629c78729f35df1fb58c56bd16d6128938d0cb`。
- 客户端：`Developer/tools/wxshadow_client/wxshadow_client`，3,517,920 B（aarch64 静态）。

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

- 编译：`make all` 通过；仅 10 条告警，全部来自 KernelPatch 头文件
  （`asm/cmpxchg.h` 的 `-Wc2x-extensions` 与 `-Wsometimes-uninitialized`），本模块源码 0 告警。
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
- 唯一告警是既有的 `wxshadow_bitmap_clear_range` 未使用。

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
