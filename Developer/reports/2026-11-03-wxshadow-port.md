# wxshadow 移植记录（2026-11-03）

## 身份

- 模块：`wxshadow`，版本 `1.2.0`（产物名）／`1.2.0-k66`（源码内 `KPM_VERSION`，上游原值不改）。
- 上游：`mkpms-stealth` 的 `kpms/wxshadow/`，CHANGELOG 最新条目 `[1.2.0-k66] - 2026-07-24`。
- 产物：`modules/KPM/wxshadow/wxshadow_1.2.0.kpm`，157,904 B，
  `sha256=c254e1231f5c1655e1365060deee3fb8002c8c8a01c297a326e1e6e0e6f41a1c`。
- 客户端：`Developer/tools/wxshadow_client/wxshadow_client`，3,517,920 B（aarch64 静态）。

## 变更点（相对上游）

只做构建侧适配，业务代码零改动：

1. 新增本仓风格的 `Makefile`（NDK clang、`-r` 链接、`KP_DIR` 相对路径），替代上游 cmake
   `add_kpm_module`。
2. **不生成 debug 变体**：上游源码没有任何 debug-only 分支，加 `-D` 编出的字节与 release
   完全相同（实测两次构建同哈希），多一个同哈希产物只会污染身份记录。
3. 客户端从 `cmake + aarch64-linux-gnu-gcc` 改成 NDK clang 静态编译，落在
   `Developer/tools/wxshadow_client/`。

源文件清单（全部原样）：`wxshadow.c`、`wxshadow_pgtable.c`、`wxshadow_handlers.c`、
`wxshadow_bp.c`、`wxshadow_scan.c`、`wxshadow.h`、`wxshadow_internal.h`。

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

1. **prctl 通道返回 EINVAL**。`wxshadow.c:1495` 用 `hook_syscalln(__NR_prctl, 5, prctl_before,
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

## 未覆盖

- 页级 patch 的端到端功能验证（读原字节 + 执行补丁）被问题 1 挡住，没做。
- 小米 13 上只验了加载，没验页级切换。
- 多线程同页、fork/exit、`mprotect` 接管等上游声明的路径均未验证。
