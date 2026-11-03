# wxshadow —— W^X 影子页（同 VA 双视图）

进程读取某页时看到的是**原始字节**，执行时跑的是**影子页里的补丁**。这是「双视图」的页级实现，
不占硬件断点槽位，也不改原页一个字节。

- 上游：`mkpms-stealth` 的 `kpms/wxshadow/`，上游版本串 `1.2.0-k66`；本仓自 1.2.1 起把源码里的
  `KPM_VERSION` 改成 `MYKPM_VERSION`（由 Makefile 注入），产物名与模块自报版本保持一致。
- 本仓：`modules/KPM/wxshadow/` 只搬 KPM 侧 5 个 `.c` + 2 个 `.h` 共 7 个源文件；用户态客户端在
  `Developer/tools/wxshadow_client/`。

## 原理

| 状态 | 页权限 | 谁看得见 |
| --- | --- | --- |
| `NONE` | 原页 | 原始代码 |
| `SHADOW_X` | `--x` 影子页 | 执行补丁；读会被挡下 |
| `ORIGINAL` | `r--` 原页 | 读到原字节；执行会被挡下 |
| `STEPPING` | `r-x` 原页 | BRK 触发后单步执行原指令 |

页面状态机：`NONE → SHADOW_X ↔ ORIGINAL ↔ STEPPING`，另有 `DORMANT`（钩子退休、影子页保留）。

页面在两种状态间来回切，代价是「读一次 + 执行一次」各缺一次页；代码页同时被读又被执行的紧循环
会明显变慢，这是页粒度方案的固有成本。

## 构建

```bash
cd modules/KPM/wxshadow
export ANDROID_NDK_LATEST_HOME=/path/to/ndk
make            # 产出 wxshadow_1.2.5.kpm
make clean
```

不生成 debug 变体：上游源码没有任何 debug-only 分支（无调试宏），加 `-D` 编出来字节完全相同，
多一个同哈希产物只会误导身份记录。

## 客户端

```bash
cd Developer/tools/wxshadow_client
make            # 产出 wxshadow_client（aarch64 静态可执行）
```

| 命令 | 作用 |
| --- | --- |
| `-p <pid> -m` | 列出目标进程可执行映射 |
| `-p <pid> -a <addr>` | 在地址上设隐藏断点 |
| `-p <pid> -a <addr> -r x0=7` | 断点命中时改寄存器 |
| `-p <pid> -b <lib> -o <off>` | 按「库名 + 偏移」定位 |
| `-p <pid> -a <addr> -d` | 删除断点（不带 addr 删全部） |
| `-p <pid> -a <addr> --patch <hex>` | 往影子页写自定义 patch |
| `-p <pid> -a <addr> --release` | 释放该地址的影子页（不带 addr 释放全部） |

prctl 接口（`wxshadow.h`）：`0x57580001` SET_BP / `0x57580002` SET_REG / `0x57580003` DEL_BP /
`0x57580004` SET_TLB_MODE / `0x57580005` GET_TLB_MODE / `0x57580006` PATCH / `0x57580008` RELEASE。

## 权限门与 ctl0 维护通道（1.2.3 起）

`prctl` 通道的身份门默认只放行 `uid 0`（root）与 `2000`（shell），名单可用 ctl0 在运行期增删，
与 `modules/KPM/stealth_hook` 的命令行一致：

```bash
su 10273 -c '/data/local/tmp/kpmctl 10273 ctl wxshadow status'      # wxshadow v1.2.5 uids: 0 2000
su 10273 -c '/data/local/tmp/kpmctl 10273 ctl wxshadow allow:10273' # allow 10273 rc=0
su 10273 -c '/data/local/tmp/kpmctl 10273 ctl wxshadow allowlist'   # allowlist: 0 2000 10273
su 10273 -c '/data/local/tmp/kpmctl 10273 ctl wxshadow deny:10273'  # deny 10273 rc=0
```

| 命令 | 行为 |
| --- | --- |
| `status` | 打版本与当前名单 |
| `allowlist` | 只打名单 |
| `allow:<uid>` | 加入名单（幂等；满 8 个返回 `rc=-1`；超 32 位或非数字返回 `bad uid`） |
| `deny:<uid>` | 移出名单（`uid 0` 恒不可移除，返回 `rc=-1`；不在表里也 `rc=-1`） |
| 其它 | 返回 `-1`（`kpmctl` 打印 `ctl rc=-1 errno=1`） |

ctl0 自身仍受 KernelPatch 的受信管理器身份约束（`kpmctl <mgr_uid> ctl …`，root 直接调会被 `-EPERM` 挡）。

## 真机状态（2026-11-03）

| 设备 | 形态 | 加载 | 权限门 | 卸载 | 说明 |
| --- | --- | --- | --- | --- | --- |
| Pixel 6 | 6.1.99-android14，kernel | `load rc=0` | 管理器 uid 被拒 / root 放行 | `unload rc=0`，不重启 | dmesg `[wxshadow] succeed`；端到端双视图已验 |
| 小米 13 | 5.10.149-android12-9，APatch LKM | `load rc=0` | `prctl denied for uid 10270` / root 放行（`bp at 556c6fc100`） | `unload rc=0`，不重启 | **两形态同一份字节 0 缺失符号** |

小米 13 上还确认了 `GUP hiding ENABLED (follow_page_pte hooked)` 打出（5.10 上该钩子挂成功）。

页级切换在 Pixel 6 上确认在跑：`exec fault at 221d28 → shadow (--x)` /
`read fault at 221c98 → switched to live original (r--)`。

### 端到端：读原字节 + 执行补丁（2026-11-03，Pixel 6）

prctl 通道修好之后，两个靶子都验过：

| 靶子 | 做什么 | 结果 |
| --- | --- | --- |
| `wx_t` | 每 100ms 调一次 `target(x)`，`x` 取自 volatile | 断点挂在 `ret` 上改 `x0=7` → 日志 `ret=7`，dmesg `Breakpoint Hit` + `modifying x0: 29 -> 7` |
| `wx_v` | 同上，另外每 50 轮把自己 `.text` 的前 8 字节打印出来 | 打印 `28 00 80 52 00 05 00 2a`（原始 `mov w8,#1; orr w0,w8,w0,lsl#1`）**且** `ret=7` |

`wx_v` 那一行就是「双视图」：同一个 VA，读路径拿到原始指令，执行路径跑的是改了寄存器的补丁。

**验证时踩过的坑（写下来免得再犯）**：第一版靶子写 `target(20)`，编译器把常量折叠、把 `bl target`
提到了循环外（`llvm-objdump` 里循环体内没有 `bl`），于是「断点不触发、返回值恒 41」看起来像模块坏了。
入参必须来自 `volatile`，并且要用 `llvm-objdump` 确认循环体内真的有 `bl target`。

**日志噪声**（**已修，本仓 1.2.2**）：init 末尾曾无条件调 `debug_print_tasks_list(10)`（`wxshadow_scan.c:1240-1296`，调用点 `wxshadow.c:1664`），每次加载往 dmesg 打 10 行内核任务指针（`task=%px mm=%px`）——上游调试代码，属内核地址泄漏 + 噪声。1.2.2 起包进 `#ifdef WXSHADOW_DEBUG_TASKS`，默认不编译。
## 已知问题（本移植未修，按「先移植、后修错」分开处理）

1. ~~**prctl 通道不通**~~（**已修，本仓 1.2.0**）：原实现用 `hook_syscalln(__NR_prctl, 5, ...)`，
   而这版 KernelPatch 的 `hook_syscalln` 走 `syscall_hook_add(..., allow_skip=0)`，不采纳
   `args->skip_origin` / `args->ret`，请求落回原生 `prctl` → `EINVAL`。改成自己按
   `__arm64_sys_prctl`（`.cfi_jt → .cfi → 无后缀`）解析地址再 `hook_wrap(...)`，卸载走
   `hook_unwrap(...)`；两种装载形态都验过。详见 `Developer/reports/2026-11-03-wxshadow-port.md`。
2. **卸载稳定性**（仍未定位根因）：累计 9 次：早期 2 次 `unload` 之后设备掉线、`uptime`
   归零，随后 7 次正常（空闲 5 次、靶子退出后 1 次、靶子仍在跑 1 次）。两次历史重启
   **复现不出来**，因此不宣称已修。

   1.2.1 曾把 `wait_for_handlers_drain()` 超时改成回 `-EBUSY` 中止卸载；1.2.2 按独立复核
   的 NX-1 **撤回**该做法——`unload_module()`（`KernelPatch/kernel/patch/module/module.c:540-547`，
   LKM 同构 `KernelPatch/lkm/kpm/module.c:865-880`）是先 `list_del`、再调 `mod->exit`、随后
   **无条件** `kp_free_exec(mod->start)`，`mod_exit` 的返回值没有任何生命周期语义。早退的真实
   后果是：prctl 钩子已摘、模块已不在册、代码已释放，而 dup_mmap / uprobe_dup_mmap /
   brk+step / do_page_fault / follow_page_pte / exit_mmap 六组钩子仍指向已释放代码，且用户态
   再没有任何通道能清掉断点。所以现在的行为是**只记录超时、始终走完七个 phase**；残留风险
   是 handler 可能在代码释放后短暂运行（`kp_free_exec` 用的是 TLSF 池，池本身从不解映射，
   因此不会立刻缺页，但块会被后续模块复用）。

   使用建议：能重启就重启；必须运行中卸载时，先 `-d` 删断点、让靶子退出或空闲再卸。

3. ~~**跨进程设断点必崩（kernel panic）**~~（**已修，本仓 1.2.2**）：Pixel 6（6.1.99 kernel
   形态）对普通系统进程设断点时，命令无输出、adb 掉线、设备硬重启（4/4 复现）；而同一操作
   在小米 13（5.10 LKM）与自建静态靶子（`0x221000` 段）上却正常。根因是设断点路径调用
   `wxshadow_flush_icache_page(addr)` → `__flush_icache_range(用户 VA, +PAGE_SIZE)`，而目标
   进程的用户 VA 在调用者上下文里**没有映射**：

   ```
   wxshadow: created page for mm=ffffff8880c88800 addr=6042f50000
   Unable to handle kernel paging request at virtual address 0000006042f50000
   ESR = 0x96000145  EC = 0x25: DABT (current EL)  FSC = 0x05: level 1 translation fault
   ```

   静态靶子不崩是因为 `0x221000` 恰好也映射在静态链接的客户端进程里，`dc cvau` / `ic ivau`
   不 fault（刷的是错的物理行，但没报错）。修法见 1.2.2：按影子页的内核 VA 刷。

## 1.2.5：独立复核问题单修复（代码侧）

1.2.3 送独立复核（代码路 + 文档/身份路，均判「暂不可关单」），本条版本处理代码侧问题单。

| 问题单 | 级别 | 处理 |
| --- | --- | --- |
| AUD-wx-4 | 中 | **读用户 buffer 不再拆大页**：`copy_from_user_via_pte()` 改走新的 `wxshadow_read_user()`（`wxshadow_pgtable.c` 末尾）——2 MB block 用块内偏移算术定位 pfn；PTE 要过 `PTE_VALID` / `PTE_TYPE_PAGE` / `PTE_AF` 三关；`pfn_to_kaddr()` 前用 `pfn_valid` 校验；6.1 有 `mmap_read_lock` 符号时加读锁。写路径的 `__split_huge_pmd()`（`prepare_shadow_target()`）在有锁时补 `mmap_read_lock` |
| WI-1 | 低 | allow 名单加 `spin_lock(&wx_allow_lock)`（**不能用 `spin_lock_irqsave`**：KP 的 KPM 加载器不导出 `kf__raw_spin_lock_irqsave`，实测 `unknown symbol` 导致 load rc=-1），读侧 `wx_uid_allowed()` 同锁 |
| WI-2 | 低 | ctl0 回显检查 `compat_copy_to_user()` 返回值，失败返回 `-EFAULT`——supercmd 路径把内核栈 `buffer[4096]` 当 out_msg 时不再「静默 rc=0 空输出」 |
| WI-3 | 信息 | `wait_for_handlers_drain()` / `wx_drain_checked()` 改 `void`，注释与 1.2.2 的「超时只记录、继续卸载」语义对齐 |
| WI-4 | 信息 | 删除死函数 `wxshadow_flush_icache_page()`（AUD-wx-9 的根因载体，已无调用点） |
| WI-5 | 信息 | `write_shadow_bytes()` 的 `offset + len > PAGE_SIZE` 改减法式并补 `offset >= PAGE_SIZE` |
| WI-6 | 信息 | `wx_dump_allow()` 每次写前判剩余长度，去掉 `out_size - n` 无符号下溢与 `(int)` 截断 |
| AUD-wx-13 | 中 | **拆掉 `register_user_*_hook` 回落**：direct hook 拿不到就 `return -1` 拒绝加载。回落路径把钩子挂进内核全局 debug 链表，卸载时只能 `list_del_rcu()`（KP 在 `rcu_read_lock` 里调 `mod_exit`，不能 `synchronize_rcu`），RCU 宽限期内的读者会走进即将被 `kp_free_exec()` 释放的模块内存——这是 1.2.0/1.2.1 两次卸载重启的同族风险 |
| AUD-wx-14 | 信息 | **pin_user_pages 修法经评估不适用**：`wxshadow_read_user()` 唯一调用点是 prctl 钩子（direct hook），那里不能缺页，而 pin/gup 慢路径会取 `mmap_lock`、可能触发缺页。保留「无缺页页表走查 + `pfn_valid` + 能拿到 `mmap_read_lock` 时加锁」（复核意见的 ①+②），代码里写了原因 |
| WI-7 | 信息 | `debug_print_tasks_list()` 的定义与声明一并包进 `#ifdef WXSHADOW_DEBUG_TASKS`，release 产物内不再存在该符号 |

### 双机复验（1.2.4）

| 项 | Pixel 6（6.1.99 / kernel 形态 / uid 10273） | 小米 13（5.10.149 / LKM / uid 10270） |
| --- | --- | --- |
| 加载 | `load rc=0`，dmesg `wxshadow: mm helpers: pfn_valid=1 mmap_read_lock=1` | `load rc=0`，`mm helpers: pfn_valid=1 mmap_read_lock=0`（5.10 无该符号，按设计降级为无锁 + pfn 校验） |
| ctl0 | `status`/`allow:10273`/`allowlist`/`deny:10273`/`deny:0`（`rc=-1`）全部符合预期 | `status` 正常 |
| 断点（写路径） | `wx_t` 靶子：`-a 0x221cf4 -r x0=7` → `ret=7` 连续命中 | 同样 `ret=7` 命中 |
| 补丁（读路径） | `-a 0x221cec --patch 20018052c0035fd6` → `ret=9` 连续命中（说明新读路径取到了用户 buffer） | 同样 `ret=9` |
| 卸载 | `unload rc=0`，`/proc/uptime` 连续 | `unload rc=0`，uptime 连续 |

## 更新记录

- **1.2.5（2026-11-03）**：按 1.2.3 独立复核问题单修代码项（**AUD-wx-4**、**AUD-wx-13**、
  **WI-1…WI-7**），并记录 **AUD-wx-14**（pin 修法为何不采用），见上节。产物 `wxshadow_1.2.5.kpm`。

- **1.2.3（2026-11-03）**：补 ctl0 维护通道（**AUD-wx-10 / NX-3**）。1.2.1/1.2.2 的 prctl 权限门把
  `{0, 2000}` 硬编码在 `wxshadow_bp.c` 里，想要别的身份（例如管理器 uid）只能改代码重编。现在改成
  `wx_uid_allowed()` 查 ctl0 维护的名单，命令与 `modules/KPM/stealth_hook` 的
  `allow:` / `deny:` / `allowlist` / `status` 同形；`uid 0` 恒放行且不可 deny。双机实测见
  `Developer/reports/2026-11-03-wxshadow-port.md`。产物 163,760 B。

- **1.2.2（2026-11-03）**：修 **WX-9**——设断点路径用**目标用户 VA** 调
  `__flush_icache_range()`，在跨进程场景（该 VA 不在调用者 mm 里）触发 EL1 translation
  fault → kernel panic/重启；新增 `wxshadow_flush_icache_kva()` 按**影子页内核 VA** 刷
  （拿不到合法 KVA 时退化 `ic ialluis`），11 处调用点全改。按 NX-1 撤回 1.2.1 的
  `-EBUSY` 早退（`mod_exit` 返回值无生命周期语义，早退会留下指向已释放代码的钩子），
  改为只记录超时、走完七个 phase。WX-8：`debug_print_tasks_list(10)` 用
  `WXSHADOW_DEBUG_TASKS` 关掉（不再往 dmesg 打内核地址）。NX-6：可回绕判定改减法式，最终 3 个文件共 6 处：`wxshadow.c:337`/`:356`、
  `wxshadow_bp.c:489`/`:599`/`:1144`、`wxshadow_pgtable.c:1109`（详见报告 AUD-021 一节）。
- **1.2.1（2026-11-03）**：独立复核问题单修复——WX-1 `prctl_before` 加权限门（只允许
  uid 0 / 2000）；WX-2 `wait_for_handlers_drain` 超时改为中止卸载；WX-3 两处
  `offset + len > PAGE_SIZE` 改成 `len > PAGE_SIZE - offset`（防整型回绕）；
  `KPM_VERSION` 改用 `MYKPM_VERSION`，产物名与自报版本对齐。
- **1.2.0（2026-11-03）**：从上游原样搬入（7 个源文件，业务代码零改动）；新写 Makefile
  （不生成 debug 变体）；修 prctl 通道（`hook_syscalln` → `hook_wrap`），双视图端到端跑通。


## 边界

- 仅 ARM64；patch 不跨页（`offset + len <= PAGE_SIZE`）。
- 每页最多 128 个断点、128 个 patch；每个断点最多 4 个寄存器修改。
- 需要 KernelPatch 框架（`hook` / `kallsyms_lookup_name` / `syscalln_addr` 等）。
- GUP 路径（`follow_page_pte`）在 6.6 上不存在，属上游已知项；6.1 上该钩子未装，
  `/proc/<pid>/mem` 这类读取是否泄漏影子页内容需单独复核。
