# 审计简要（R3 第一轮）— 给独立审计方

> 本文件由 Developer 编写，只提供**范围与线索**，不含结论。每条线索都必须由审计方独立验证；
> 若与 Developer 报告冲突，以审计方自己复算到的事实为准。

## 1. 审计对象

仓库 `APatch_kpm`，冻结提交见下表。产物在 `artifacts/<instance_id>/` 与 `modules/KPM/<模块>/*.kpm` 两处都有副本。

| 模块 | 版本 | instance_id | source_commit | release 产物 | debug 产物 |
| --- | --- | --- | --- | --- | --- |
| apkey_hide | 3.6.2 | `apkey_hide-3.6.2+g70b54446ca1b.r62e1ff58.kpb51197a.ndk26.1.10909125#1` | 3109aa9550 | `apkey_hide_3.6.2.kpm` sha256 `5df0f31c9f11…` 22552B | `…_debug.kpm` `9e2f5c4c72c3…` 28112B |
| memcrc | 0.2.0 | `memcrc-0.2.0+g8a711a713802.r6461dd8f.kpb51197a.ndk26.1.10909125#1` | 3109aa9550 | `memcrc_0.2.0.kpm` `c813676cbcb4…` 93024B | `…_debug.kpm` `c4e5ffdcc1c6…` 93024B |
| mount_hidder | 1.0.0 | `mount_hidder-1.0.0+g3143c326dc4f.r0b23ffb6.kpb51197a.ndk26.1.10909125#1` | 3109aa9550 | `mount_hidder_1.0.0.kpm` `0100faa348f0…` 10152B | `…_debug.kpm` `0a2aed56401c…` 10152B |
| stealth_hook | 0.3.0 | `stealth_hook-0.3.0+gb71c7d8e6a6e.re4fbd446.kpb51197a.ndk26.1.10909125#1` | 3109aa9550 | `stealth_hook_0.3.0.kpm` `22ce8b98f206…` 26368B | `…_debug.kpm` `f9db0eca1220…` 29880B |
| panda_hide | 2.0.0 | `panda_hide-2.0.0+g02b5eb7a0e1d.r37fb85e0.kpb51197a.ndk26.1.10909125#1` | 3109aa9550 | `panda_hide_2.0.0.kpm` `63188d5a69a9…` 24464B | `…_debug.kpm` `6369246c334f…` 24464B |
| qti_battery_charger | 1.0.2 | `qti_battery_charger-1.0.2+g22c24856667c.r4df2b2d1.kpb51197a.ndk26.1.10909125#1` | 998837ee5d | `qti_battery_charger_1.0.2.kpm` `0c5f1c985450…` | 与 release **逐字节相同**（debug 目标未加 `-DCONFIG_DEBUG`，属既有实现） |
| xperia_ii_battery_age | 1.1.2 | `xperia_ii_battery_age-1.1.2+gf37d1edec293.r93eac43f.kpb51197a.ndk26.1.10909125#1` | 998837ee5d | `xperia_ii_battery_age_1.1.2.kpm` `c7e36de46eab…` | 同上，与 release 相同 |

完整哈希与 artifacts 列表在 `metadata/modules/<模块>.json` 的 `builds[]` 里——那是**输入**，不是结论。

## 2. 建议的独立核验动作

1. **产物不可覆盖/可复算**：`sha256sum` 自己算一遍，与上表和 metadata 对齐；确认 `artifacts/` 与 `modules/KPM/<模块>/` 两份副本一致。
2. **导入符号**：`llvm-readelf -s --wide <kpm> | awk '$7=="UND"'`，逐个比对 KernelPatch 的导出表（自己 grep `KP_EXPORT_SYMBOL`，注意 `kfunc_def` 机制：`KP_EXPORT_SYMBOL(kfunc(x))` 导出的符号名是 `kf_x`）。任何 UND 不在导出表里 → 加载必失败，属阻断级问题。
3. **内嵌元数据**：从 `.kpm` 里取 `name=` / `version=` / `license=` / `author=` / `description=`，与 `KPM_NAME`/`KPM_VERSION(MYKPM_VERSION)`、Makefile `MYKPM_VERSION`、`metadata.version_declared` 四处对齐。debug 版版本串应为 `<ver>_d`。
4. **代码结构**：`KPM_NAME`/`KPM_INIT`/`KPM_EXIT` 各只有一处；`hook`/`hook_wrap` 与 `unhook`/`hook_unwrap_remove` 成对，且卸载顺序不会在摘钩前先恢复被钩数据。
5. **失败路径**：kallsyms 解析失败、hook 失败、用户态指针非法时是否会泄漏内存 / 留下半挂状态 / 返回让人误判的 0。
6. **输入校验**：ctl0 与 syscall 通道的字符串解析有没有越界、有没有把用户指针直接当内核指针用、权限门（uid 白名单）是否有绕过。
7. **公开文件卫生**：报告与源码里有没有本机绝对路径、设备序列号、superkey 明文。

## 3. Developer 自认的可疑点（请独立判定，不要照抄）

- `memcrc`：`control/control.c` 的 `getcwd` 钩子把用户缓冲区内容当命令解析（0.1.0 起有 uid 作用域门与受信表，但通道本身是高频 syscall）；`redirect/redirect.c` 的 `do_filp_open` 重定向可递归（A→B→A）且无深度限制；`ptrace/ptrace_hide.c` 对 `m->count` 做 memmove 时未显式校验 `m->buf` 边界；`shared_data` 表无锁（并发写与遍历）。
- `panda_hide`：`mem_hide.c` 用 `args->ret` 当擦除长度（上游用 `args->arg3`）；`net_hide.c` 在缺少 `__get_task_comm` 符号时按「非 adbd」拦截（偏保守）；对 `access_remote_vm` 的 `gup_flags` 判定依赖上游语义。
- `stealth_hook`：`hwbp.c` 的 perf 事件生命周期（`perf_event_release_kernel` 是否与创建严格配对、线程退出时是否泄漏）；`ghost_mem.c` 卸载时只回收记账不清 VMA（有意为之）；`stealth_hook.c` 的 syscall 号 427 在 App 域被 seccomp 杀（仅影响 App 调用，非内核缺陷，但属可用性边界）。
- `apkey_hide`：TOFU 认领窗口（`APK_WINDOW_MAX` 8192 / cfg 256）在窗口未关闭时对 `[10000,90000)` uid 放行；`refresh_trusted_manager_state` 的 after-only 钩子在目标函数被内联时是否还能命中。
- `mount_hidder`：`hm_after` 里 `m->count > m->size` 的双边界检查是否覆盖所有内核版本；`strnstr` 匹配是子串匹配，可能误伤同名子串路径。
- `qti_battery_charger` / `xperia_ii_battery_age`：Makefile 的 debug 目标没有 `-DCONFIG_DEBUG`，两种变体逐字节相同——这会影响「debug 与 release 差异」类验收判据，请判定是否需要开单。

## 4. 明确不在本轮范围

- 真机行为（由 Tester 负责，已有实现方自检记录但非 Tester 结论）。
- KernelPatch 自身（子模块 b51197a）的审计。
- 偏移推导正确性（这几个模块都不依赖结构体偏移，除 `memcrc`/`stealth_hook` 读 `task_struct_offset.cred_offset`/`cred_offset.uid_offset`，这两项由 KP 导出，仍需核对其语义假设）。

## 5. 输出要求

- 报告写 `Auditor/reports/2026-10-31-audit-r1.md`，问题单 ID 用 `AUD-<序号>`，每条含：位置（`文件:行`）、影响、frequency、confidence、验证方法、建议修复方向。
- 结论必须绑定 `instance_id` + 产物 SHA-256；无法复算的项要显式写「未核实」。
- 声明同源风险：如果你用了 Developer 的脚本或报告作为线索，写明用了哪些、如何独立验证。
