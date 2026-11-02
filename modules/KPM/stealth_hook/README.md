# stealth_hook

硬件断点（perf hw_breakpoint）/ 幽灵内存 / 缺页重定向能力探测的内核模块。

来源：[xiaojianbang-stealth-hook](https://github.com/xiaojianbang/xiaojianbang-stealth-hook)（GPL-2.0-or-later）的移植。
上游是一个整体模块（syscall #285 + HWBP + PTE + Ghost + MapsHide + PtraceSpoof），本仓按职责拆开：

| 上游子系统 | 本仓落点 | 说明 |
| --- | --- | --- |
| HWBP | **本模块** `0x1001`–`0x1007` | 硬件执行断点，按线程注册 |
| Ghost | **本模块** `0x3001`–`0x3004` | 在当前进程建正规 VMA（`vm_mmap`），可读可写可执行 |
| PTE | **本模块** `0x2001`（仅能力探测） | 需要 5.10 厂商 hook，6.1+ 已移除；真正的重定向还需用户态 DBI，本移植未做 |
| MapsHide | `memcrc` 的 `RANGE:uid:start:end` 规则 | 按 uid 作用域隐藏地址区间 |
| PtraceSpoof | `memcrc` 的 `REGS:uid` 规则 | 伪造 `NT_ARM_HW_BREAK` / `NT_ARM_HW_WATCH` 视图 |

## 通道

syscall **427**（`io_uring_register`）。上游用 285（`copy_file_range`）——那是 IO 热路径，
挂上去等于给每次 IO 加一次 magic 判断，故选冷门号。

约定：`syscall(427, MAGIC, cmd, arg1, arg2, arg3, arg4)`，`MAGIC = 0x584A42`（"XJB"，沿用上游取值）。

- magic 不匹配 → **原样放行**，行为与未加载时完全一致（返回 `-ENOTSUP`，errno 95）。
- magic 匹配但调用者 uid 不在受信表 → `-EPERM`。
- 受信表默认 `{0, 2000}`（root / shell），可用 ctl0 维护：

```
allow:<uid>     加入受信表
deny:<uid>      移出受信表（uid 0 不可移除）
allowlist       打印当前受信表
status          打印模块状态
```

ctl0 通过 APatch 管理器的「参数」对话框下发，或 `kpatch <superkey> kpm ctl0 stealth_hook <cmd>`。

## 命令表

| 命令 | 值 | 参数 | 返回 |
| --- | --- | --- | --- |
| STATUS | `0x0001` | — | `0x53484F4B + 版本码` |
| ECHO | `0x0002` | `arg1` | `arg1`（通道自检） |
| HWBP_HOOK | `0x1001` | `struct sh_hwbp_request *` | 0 / 负错误码 |
| HWBP_UNHOOK | `0x1002` | `addr, tid` | 摘掉的节点数 / `-1` 无匹配 / `-2` 已退役 |
| HWBP_ENABLE | `0x1003` | `addr, tid` | 变更的节点数 / `-1` |
| HWBP_DISABLE | `0x1004` | `addr, tid` | 变更的节点数 / `-1` |
| HWBP_QUERY | `0x1005` | `struct sh_hwbp_query *`（`target_addr=0` 列出全部） | 0 / `-2` 无节点 |
| HWBP_SET_OVERRIDE | `0x1006` | `struct sh_hwbp_override *` | 0 |
| HWBP_SET_SKIP | `0x1007` | `addr, count, tid` | 0（忽略接下来 count 次命中，只跳过记录） |
| PTE_QUERY | `0x2001` | — | 1=支持 / 0=不支持 |
| PTE_HOOK/REARM/UNHOOK | `0x2002`–`0x2004` | — | `-EOPNOTSUPP`（本移植未实现） |
| GHOST_ALLOC | `0x3001` | `va, size, prot` | 分配的 VA |
| GHOST_FREE | `0x3002` | `va` | 0 |
| GHOST_WRITE | `0x3003` | `struct sh_ghost_write_request *` | 0 |
| GHOST_LIST | `0x3004` | `struct sh_ghost_info *, max` | 条数 |

结构体定义见 `stealth_hook.h`，与上游逐字段一致，方便直接对表上游 CLI 协议。

### 节点身份、去重与通配（0.3.1）

- 一个断点节点的身份是 **(target_addr, tid)**，不是地址。同一个函数挂到 10 个线程
  就是 10 个节点，各自独立计数、独立 query——CLI 循环给 90 个线程挂钩用的就是它。
- `HWBP_HOOK` 按 `(addr, tid)` 去重：参数完全相同的重复 hook 返回 0（幂等，脚本循环
  重跑安全）；参数不同返回 `-EEXIST`（-17），要改配置先 `unhook`，或走
  `HWBP_SET_OVERRIDE`。**不做静默覆盖**——覆盖会让先挂的节点失去归属，多线程调试时
  很难发现。
- `unhook` / `enable` / `disable` / `set_skip` 的 `tid` 传 0 表示「该地址的全部线程」，
  一次改齐，返回值是实际影响的节点数；`set_override` 本来就按 `target_pid=0` 通配。
- `HWBP_QUERY` 的 `target_addr` 传 0 表示列出全部节点（CLI 的 `list` 走这条），
  配合 `node_index`/`node_total` 分页。

## 典型用法

```bash
# 对当前线程里某个函数下断点，抓入口参数与返回值
stealth_cli hook 0x7a1b2c3d4e --dump 64
# ... 触发目标函数 ...
stealth_cli query 0x7a1b2c3d4e

# 改第 0 个参数（X0）
stealth_cli override 0x7a1b2c3d4e --set 0=0x100 --modify
# 跳过原函数，直接返回 0x777
stealth_cli override 0x7a1b2c3d4e --skip-ret 0x777
# 看当前挂了哪些断点（等价 query 0）
stealth_cli list
# 摘断点（只摘当前线程）
stealth_cli unhook 0x7a1b2c3d4e
# 摘掉这个地址上所有线程的断点
stealth_cli unhook 0x7a1b2c3d4e --tid all
```

`stealth_cli selftest` 会在自己的进程里对一个 `noinline` 目标函数跑完整链路
（hook → query → 改参 → 跳过 → 卸载复原），用来在一台陌生设备上确认能力。

## 边界与已知差异

- **rearm 是 MSR 直写（0.3.0 起）**：断点命中后的重装（入口拍 → LR、返回拍 → 原地址）
  在调试异常上下文内直写 `DBGBVRn_EL1`/`DBGBCRn_EL1`（清使能位 → 改址 → 原样写回），
  纯寄存器操作，不碰任何可睡眠的 perf API。代价：perf 框架内部记录的地址不感知这次
  直写，上下文切换 / CPU 热插拔 / 休眠唤醒后框架可能按旧地址重装 BVR，断点会回旧
  地址**错采一拍**（多记或少记一次命中、`ret_x0` 可能错配一拍），自愈、不死循环、
  不会错采到非法地址。
- **enable/disable 语义**：异常上下文里校验失败会先摘武装（BCR bit0=0）再置
  `is_disabled`；之后用户态 enable 命令只走 perf API 重武装，恢复到 BVR 当时的值
  （可能是重装回的旧地址，不一定是 target_addr）。
- **PTE 未移植**：6.1 起内核移除了 `__tracepoint_android_rvh_do_mem_abort`，PTE 的拦截点不存在；
  5.10 上虽有该 hook，但真正的重定向还需要用户态 DBI 把目标页重编译成「搬家版页」并给出
  `offset_map[1024]`，否则钩子无处跳转。本移植只保留 `PTE_QUERY` 能力探测，其余命令统一返回
  `-EOPNOTSUPP`——不做半套，也不猜。
- **FULL_HOOK / AUTO_THREAD 未移植**：前者依赖用户态 DBI 整页重编译；后者靠钩 `wake_up_new_task`
  给新线程补断点，CLI 轮询 `/proc/<pid>/task` 即可替代，少一个全局钩子。
- **ghost 不自动隐藏**：上游建完 VMA 会调 maps_hide 抹掉；本仓 maps_hide 在 memcrc 里，按 uid 作用域。
  需要隐藏时另行下发 `RANGE:uid:<va>:<va+size>`，两块模块各管一件事。
- **ghost 只支持当前进程**：上游同样如此（跨进程走 `kthread_use_mm` 会 panic）。
- **模块卸载不 munmap**：卸载发生在管理器上下文，拿调用方地址去 munmap 会误伤管理器；
  只回收记账节点，VMA 交目标进程退出时回收。

## 加载形态（0.3.4）

0.3.3 及更早只在 kernel 形态装得上，小米 13（`kernelpatch-lkm`）上会依次报三个
`unknown symbol`：`inline_wrap_syscalln`、`inline_unwrap_syscalln`、`cred_offset`。
两个 `inline_*_syscalln` 与 `cred_offset` 都只有内核集成形态导出（`KP_EXPORT_SYMBOL`），
LKM 兼容表里没有；装载器不分 weak/binding，解析不到就在 `apply_relocations()` 前中止。

0.3.4 的两处替换：

1. `sh_current_uid()` 改为调用 KP 导出的 `current_uid()`（`kernel/patch/common/utils.c:178`
   + LKM 兼容表两边都有），不再自己算 `task_struct_offset.cred_offset` + `cred_offset.uid_offset`。
2. syscall 桥改为 `syscalln_addr(SH_SYSCALL_NR, 0)` + `hook_wrap()`，与 KP 自己的
   `inline_wrap_syscalln()` 同构（同样按 `has_syscall_wrapper` 把参数个数收窄到 1）。
   `syscalln_addr` 两形态都导出，`inline_wrap_syscalln` 只有内核形态有。

顺带修掉版本号漂移：status 不再读手写的 `SH_VERSION_CODE`，改从 `MYKPM_VERSION` 解析。
0.3.3 升版本时忘了改宏，status 一直报旧版本 —— 这类双份常量只留一份。

## 构建

```bash
export ANDROID_NDK_LATEST_HOME=/path/to/ndk/26.1.10909125
make all debug
# stealth_hook_<版本>.kpm / stealth_hook_<版本>_debug.kpm
```

## 更新记录

- **0.3.4（本仓补丁）**：两种装载形态同一份产物。
  1. `sh_current_uid()` 改用 KP 导出的 `current_uid()`，去掉 `cred_offset` 依赖。
  2. syscall 桥改 `syscalln_addr()` + `hook_wrap()`，去掉 `inline_wrap_syscalln` /
     `inline_unwrap_syscalln` 依赖。
  3. status 的版本号改从 `MYKPM_VERSION` 解析，删掉 `SH_VERSION_CODE`。
- **0.3.1** — 可用性一轮（AUD-B-1 根治）：`HWBP_HOOK` 按 `(addr, tid)` 去重
  （同参数幂等返回 0、不同参数 `-EEXIST`）；`unhook`/`enable`/`disable`/`set_skip`
  支持 `tid=0` 通配并返回影响个数——此前只改第一个匹配节点，同址多线程的其余节点
  成了没有任何命令能单独操作的孤儿（rearm 里「一次改齐所有匹配槽」的补偿逻辑就是
  给它擦屁股的）；`query` 的 `target_addr=0` 列出全部节点，CLI 新增 `list` 与
  `--tid all` 别名。同址不同线程仍然允许。
- **0.3.0** — HWBP 全链路（入口抓 X0–X7/LR、返回抓 X0、改参数、跳过原函数、按指针 dump 内存）+
  GHOST + PTE 能力探测；通道 uid 门；CLI `Developer/tools/stealth_cli/`。
  R4f–R4g 修复（登记 instance 见 metadata）：rearm 从 perf 三件套改 MSR 直写
  （异常上下文免睡，AUD-f-1 根治）、校验失败路径先摘武装、卸载恢复 cleanup、
  dump 兜底缺符号时整体禁用。
- **0.2.0** — 加 GHOST（`vm_mmap` + 读写 + 列表）。
- **0.1.0** — 骨架：syscall 427 通道、STATUS/ECHO、uid 门。

### 0.3.2

- **去重比较改为双向相等**（AUD-B-2-1）：原来用 `(dup->flags & mask) == (req.flags & mask)`
  只要求「新请求的 flag 是旧的子集」，于是「先挂 `--listen-ret`、再用更少 flag 重挂」
  会被误判幂等并返回 0。现在要求掩码内完全相等，参数不同一律 `-EEXIST`。
- **`skip` 返回影响个数**（AUD-B-2-2）：与 `unhook`/`enable`/`disable` 一致，
  不再恒返回 0。
- `selftest` 增加两条回归：同参数重挂返回 0、异参数重挂返回 `-EEXIST`(17)；
  `skip` 那一行改为打印期望的影响节点数。

### 0.3.3

- `status` 返回值只回 magic（`0x53484F4B`，即 `"SHOK"`）。0.3.2 及以前是 `SH_STATUS_MAGIC + SH_VERSION_CODE`，
  相加会污染 magic 字节（实测变成 `0x534B4F4E`，看起来像 `"SKON"`），任何按 `0x534b4f4b` 判「通道在」的探针要改成新值。
- `ctl0 status` 文案改为 `stealth_hook v0.3.3 nr=427 allowlist: ...`（原先 `v%x` 打成 `v30000`，且 `nr=427` 与
  `allowlist:` 之间没有分隔符）。
- `SH_VERSION_CODE` 统一按 `0x00MMmmpp` 编码（`0.3.3` = `0x00000303`）。

