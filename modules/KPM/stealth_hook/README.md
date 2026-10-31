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
| HWBP_UNHOOK | `0x1002` | `addr, tid` | 0 |
| HWBP_ENABLE | `0x1003` | `addr, tid` | 0 |
| HWBP_DISABLE | `0x1004` | `addr, tid` | 0 |
| HWBP_QUERY | `0x1005` | `struct sh_hwbp_query *` | 0 |
| HWBP_SET_OVERRIDE | `0x1006` | `struct sh_hwbp_override *` | 0 |
| HWBP_SET_SKIP | `0x1007` | `addr, count` | 0（忽略接下来 count 次命中，只跳过记录） |
| PTE_QUERY | `0x2001` | — | 1=支持 / 0=不支持 |
| PTE_HOOK/REARM/UNHOOK | `0x2002`–`0x2004` | — | `-EOPNOTSUPP`（本移植未实现） |
| GHOST_ALLOC | `0x3001` | `va, size, prot` | 分配的 VA |
| GHOST_FREE | `0x3002` | `va` | 0 |
| GHOST_WRITE | `0x3003` | `struct sh_ghost_write_request *` | 0 |
| GHOST_LIST | `0x3004` | `struct sh_ghost_info *, max` | 条数 |

结构体定义见 `stealth_hook.h`，与上游逐字段一致，方便直接对表上游 CLI 协议。

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
# 摘断点
stealth_cli unhook 0x7a1b2c3d4e
```

`stealth_cli selftest` 会在自己的进程里对一个 `noinline` 目标函数跑完整链路
（hook → query → 改参 → 跳过 → 卸载复原），用来在一台陌生设备上确认能力。

## 边界与已知差异

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

## 构建

```bash
export ANDROID_NDK_LATEST_HOME=/path/to/ndk/26.1.10909125
make all debug
# stealth_hook_<版本>.kpm / stealth_hook_<版本>_debug.kpm
```

## 更新记录

- **0.3.0** — HWBP 全链路（入口抓 X0–X7/LR、返回抓 X0、改参数、跳过原函数、按指针 dump 内存）+
  GHOST + PTE 能力探测；通道 uid 门；CLI `Developer/tools/stealth_cli/`。
- **0.2.0** — 加 GHOST（`vm_mmap` + 读写 + 列表）。
- **0.1.0** — 骨架：syscall 427 通道、STATUS/ECHO、uid 门。
