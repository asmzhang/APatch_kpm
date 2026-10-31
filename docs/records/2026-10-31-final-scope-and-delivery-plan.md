# 移植范围与交付计划（最终方案，2026-10-31）

## 结论

本期交付集**冻结**为：在既有 9 个模块基础上新增 5 个（`apkey_hide`、`memcrc`、
`mount_hidder`、`stealth_hook`、`panda_hide`），可构建模块由 9 个增至 14 个；
检测侧 `runtime-sense` 新增 4 条检测（n171–n174）同步纳入。
`APKey_Hide_S` 与 wxshadow 系**不进入本期**，前者记录在案的取舍，后者列为有条件项。

本方案只决定「做什么、按什么顺序做、谁签字」，不产生验收结论。五份模块源码目前仍是
**探索构建**（未冻结提交、未登记 instance_id），转正是本方案的第一优先级。

## 一、交付集

### 进入本期（14 个可构建模块）

| 模块 | 版本 | 来源 | 状态 |
| --- | --- | --- | --- |
| apkey_hide | 3.6.2 | APKey_Hide_KPM | 已移植，真机 A/B 通过 |
| memcrc | 0.2.0 | memcrc（含 RANGE / REGS 扩展） | 已移植，真机 8 项通过 |
| mount_hidder | 1.0.0 | mount_hidder | 已移植，差分通过；ctl0 未验证 |
| stealth_hook | 0.3.0 | xiaojianbang-stealth-hook | 已移植（HWBP/Ghost/PTE 能力探测） |
| panda_hide | 2.0.0 | kpm-panda-hide | 已移植，11 项 A/B 通过 |
| 既有 9 个 | 不变 | — | 本期不改动，只随轮次复核 |

### 不进入本期

- **`kpm-hide-env`**：已于 [`2026-10-29-hide-env-rejected.md`](2026-10-29-hide-env-rejected.md) 否决，不再讨论。
- **`APKey_Hide_S`（2.2.0s，自杀模式，406 行）**：与标准版**绝不同时加载**，且自述「不要永久嵌入」；
  它解决的是「管理器 uid 不可信」的兜底场景，不是更强的隐藏。触发条件：Tester 在生产设备上
  出现管理器被卸载/uid 不可信而仍需隐藏时，再单独立项；届时按独立模块处理，不与 `apkey_hide` 合并。
- **wxshadow 系（`mkpms` 8.6k 行 / `mkpms-stealth` 17.1k 行）**：见第四节条件项。

## 二、检测侧口径

`runtime-sense` 本期完成 4 条，**不再新增**：

| 编号 | 检测 | 针对 |
| --- | --- | --- |
| n171 | supercall 缺页侧信道 | APatch / KernelPatch 本身 |
| n172 | procfs 输出改写 | memcrc |
| n173 | 硬件断点 hook（槽位差 + 计时 canary） | stealth_hook 的 perf 断点、wxshadow 类 BRK 单步 |
| n174 | 隐藏挂载（statx `STATX_ATTR_MOUNT_ROOT` 交叉） | mount_hidder、memcrc 的 `PATH:` |

四条都是「独立证据块 + 独立上报」，**不并入三层投票**——三层在痕迹被抹掉时会一致地错。
外部资料（看雪 riskhider 实战、NativeTest 检测器）证明「内核侧信道」已是商业检测器的公开卖点，
本仓库的检测与隐藏因此是同一场对抗的两侧，必须成对交付、成对回归。

## 三、先清的质量尾巴（R1）

按阻塞程度排序，前三条不清理则冻结无意义：

1. **五份模块缺元数据**（`apkey_hide` / `memcrc` / `mount_hidder` / `stealth_hook` / `panda_hide`），
   连同 `qti_battery_charger`、`xperia_ii_battery_age` 共 7 份 → 维护者 `identity.py import-manifest`。
2. **`tool_selftest` 在 Windows 跑不起来**（gbk 解码 / NoneType），空树同样失败，属环境存量；
   在 Linux 或 WSL 复跑转绿，禁止为此放宽判据。
3. **`public_file_hygiene` 检测不到设备序列号**：`docs/records/2026-10-29-hide-env-rejected.md:12`
   仍有明文序列号（存量）。补检测规则 + 清存量，两件一起做。
4. **两个真机缺口**：`mount_hidder` 的 ctl0 四条命令（`status` / `add:` / `del:` / `clear`）、
   `apkey_hide` 的嵌入模式（`apk_embedded` 路径）。前者因 Gboard 全角替换未能送达，
   留待 Tester 用软键盘手输；后者需管理器以嵌入方式加载，属 Tester 用例。
5. 五份模块 README 已补齐；`Developer/reports/` 五份报告身份栏已回填到各自提交。

## 四、wxshadow 条件项

不默认开工。先回答一个问题：**HWBP 覆盖不了哪些场景？** 需列出具体清单，例如

- 目标不是函数入口，而是常量/字符串/分支内的单条指令；
- 需要「改页立即生效」而不依赖断点命中的调用时机；
- 目标进程没有可用 perf 断点槽位（6 个，已被本进程其它组件占用）。

若清单成立且 HWBP 无法覆盖，再做独立模块，并预先接受三条硬约束（已由公开移植实证）：
THP 2 MiB 大页目标不支持（返回 `-14`）、必须实现 fork 保护、卸载必须 5 阶段顺序否则下次 syscall 重启。
顺序上它排在所有转正工作之后。

## 五、轮次与责任

| 轮次 | 内容 | 责任 | 退出条件 |
| --- | --- | --- | --- |
| R1 | 第三节五条尾巴 | Developer + 维护者 | 门禁 12/12；两个真机缺口有结论 |
| R2 | 冻结 + 登记 | Developer（`build_candidate.py`）/ 维护者 | 五个模块各有 `source_commit` + instance + 产物哈希；`freeze_check` 通过 |
| R3 | 独立审计 | Auditor | 审计报告 + 问题单（含严重度/频率/置信度），声明同源风险 |
| R4 | 按单修复 + 重冻 | Developer | 问题单关闭由发现方复核；新 Build ID、旧产物保留 |
| R5 | 真机全矩阵 | Tester | 每模块 release 变体 + 阳性/阴性对照 + 未覆盖清单 |
| R6 | 验收签名 | 维护者 | `check_repository.py --strict` 通过；覆盖矩阵更新；签名交付 |

R2 与 R5 允许并行（不同模块），R3 必须在 R2 之后，R4 之后需重新 R5。

## 六、验收判据

可机检部分：

- `python3 tools/check_repository.py` 全项 PASS（含转绿后的 `tool_selftest`）；`--strict` 通过。
- 每个模块 clean 重建后哈希可复算、与元数据记录一致；导入表 UND 全部为 KernelPatch 导出符号。
- 源码版本 = Makefile `MYKPM_VERSION` = 元数据 `version_declared` = 产物内嵌版本。

人工部分：

- 每模块至少 1 台设备、release 变体、一组阳性 + 一组阴性对照，报告写明判据与未覆盖项。
- 反检测类模块（`memcrc` / `mount_hidder` / `panda_hide` / `apkey_hide`）须同时记录
  `runtime-sense` 对应检测的命中状态，作为「隐藏是否真的成立」的外部判据。

## 七、不承诺的事

- 不承诺「不被任何检测器发现」。本期模块按信号类型逐个遮蔽或消除，检测方换一类信号即可能失效；
  这是分级（根治档 / 高级补丁 / 补丁档）的既有结论，不是缺陷。
- 不做 per-app 改检测结果、不做 TEE/keystore2 层对抗、不做属性层伪装——这些不在 KPM 的能力面内。
- `stealth_hook` 的 Ghost 不自动隐藏自身 VMA（由 `memcrc` 的 `RANGE` 规则另行下发）；
  PTE 子系统在 6.1+ 返回 `-EOPNOTSUPP`，不静默降级。

## 八、R1 执行记录（2026-10-31）

### 已完成

- **A3 元数据**：`metadata/modules/` 从 9 条增至 16 条。七个模块（`apkey_hide` / `memcrc` /
  `mount_hidder` / `stealth_hook` / `panda_hide` / `qti_battery_charger` / `xperia_ii_battery_age`）
  各登记 2 条 candidate 构建（release + debug），共 14 份产物，`identity_hashes` 复算全通过。
- **A4 工具自检**：`tool_selftest` 在 WSL（Ubuntu-24.04 / python3.12.3）全绿——`37 项工具判据回归通过`。
  Windows 主机上该检查仍失败，根因是本机 locale 为 gbk 而工具按 UTF-8 读写（`selftest_tools.py`
  的 `subprocess.run(text=True)` 与多处 `read_text()` 未指定 encoding），属环境问题，不放宽判据。
  本机执行门禁请用：
  `wsl.exe -e bash -lc 'cd /mnt/d/work/1/kpm/APatch_kpm && python3 tools/check_repository.py'`
- **A5 公开文件卫生**：新增两条判据（大写十六进制 12–16 位序列号形态；本工程历史出现过的已知
  设备标识），并把 `docs/records/2026-10-29-hide-env-rejected.md` 里的序列号替换为 `sha256:433ab88276ec`。

### 期间修掉的工具/构建缺陷（都属实现方范围，判据未变）

- `tools/identity.py`：Windows 上 `Path.relative_to(REPO)` 产出反斜杠并被原样写进
  `sources[].path`、`artifacts[].path`、`handoff_manifest`，导致 WSL/CI 侧按路径取文件失败
  （`identity_hashes` 报「无法按绑定提交核验」「产物不在场」）。统一过 `posix_rel()`。
- `modules/KPM/qti_battery_charger`、`modules/KPM/xperia_ii_battery_age`：Makefile 缺
  `mingw64_nt-10.0-*-x86_64 -> windows-x86_64` 的 NDK HOST_TAG 映射，`make -n` 能过、真编译必然失败
  （也导致 `build_candidate` 认不出编译器、拒绝登记）。补上映射后两者首次可构建。
- 同两个模块的版本宏统一为 `MYKPM_VERSION`（原为 `QBC_VERSION` / `XIIBA_VERSION`），
  否则记录一旦存在，`version_consistency` 会因 Makefile 缺 `MYKPM_VERSION` 直接失败。
- 注意：这两个模块的 `debug` 目标没有 `-DCONFIG_DEBUG`，debug 产物与 release 字节相同，
  属既有实现，本轮不动；Tester 按「只测一个变体」处理即可。

### 当前门禁状态

`14 项检查，0 项失败，2 项警告`。两条警告均为存量历史项（2026-10-11 zram 清单缺 `handoff_by`；
2026-10-02 冻结候选实例未登记），不属于本轮交付集。

### 下一步

R2 冻结登记已完成（上述 7 模块），可以直接进 R3 独立审计；审计对象是上表 14 份产物对应的实例。

## 九、R3/R4 执行记录（2026-10-31）

### R3 独立审计（第一轮）

- 审计员：独立上下文 + 不同模型（`my-wb/glm-5.3-flash`），三组并行，报告在
  `Auditor/reports/2026-10-31-r1-a.md`、`-b.md`、`-c.md`。
- 结论：21 单（3 critical / 8 high / 5 medium / 5 low-info）。**apkey_hide 3.6.2 零单通过**；
  身份层（哈希、导入符号、内嵌元数据、四处版本）全部通过，问题都在代码逻辑层。
- critical 三张：memcrc `parser.c` REMOVE TYPE 栈溢出、memcrc 设备通道绕过 uid 作用域门、
  stealth_hook 断点回调里裸解引用用户指针可致 panic。

### R4 第一轮修复（提交 `ee8f97f`）

- 修完 3 张 critical + 5 张 high，重建 5 个模块并真机复验（`/dev/yuuki_misc` 变 0600、
  shell/app 写被拒、shell 跑 `stealth_cli selftest` 得 EPERM、root 全通过），登记为 `254fb89`。
- 复核（`Auditor/reports/2026-10-31-r4-review-a.md`、`-b.md`）：关闭 8 条，5 条仍开，
  另外复核方指出修复自身引入的两个新问题（重复 unhook 双重 release、注册失败路径漏 task 引用）。

### R4b 第二轮修复（提交 `7cbb61c`，登记 `e584cee`）

- AUD-a-6：`mod_control0` 改为先在内核侧补 NUL，再连 NUL 一起拷（`len + 1`）。
- AUD-b-1：命令门扩到 ghost 建映射/写内存（`0x3001-0x3003`），shell 只能 STATUS/ECHO/
  GHOST_FREE/GHOST_LIST 与只读查询。
- AUD-b-2：调试异常上下文的用户读取改用 `copy_from_user_nofault`（kallsyms 解析），
  缺符号时退回 `__arch_copy_from_user` 并注释残留风险。
- AUD-b-3 / NEW-b-3a / NEW-b-3b：命令通道加 IRQ-safe 自旋锁（无符号时降级并打告警）；
  unhook 置 `bp_handle = 0` 后再退役，重复 unhook 返回 `-2`；enable/query 跳过退役节点。
- NEW-b-5a：perf 注册失败路径补 `put_task_struct`。
- 真机复验（Pixel 6，6.1.99-android14）：
  - shell `stealth_cli ghost alloc 4096` → `Operation not permitted (errno=1)`；
  - root `stealth_cli selftest` 全通过（含改参、跳过原函数、unhook 复原）；
  - **关键回归**：「先 hook/unhook（产生退役节点）再卸载模块」路径 `unload_module ... rc: 0`，
    管理器进程不再挂住。此前 `ee8f97f` 版本在同一路径上会二次 release 并挂死管理器进程
    （实测进程变僵尸、AMS 报 `refused to die`、只能重启设备）——这正是 NEW-b-3a 的现场证据。
- 产物（与真机测过的字节完全一致）：
  - memcrc `582f8aff4873ab46…`（95640B）/ debug `5f585513378e68fc…`
  - stealth_hook `1043f629821b4da9…`（28600B）/ debug `85bb5e1fec1c8cf2…`

### 新发现（待复核定级）

- 对无效地址下 exec 硬件断点（`stealth_cli hook 0x2247d0`）后设备重启（adb 断开约 25 秒后恢复），
  怀疑内核 panic。已交审计员定级（候选单号 AUD-d-1），修复方向是在注册前校验
  `target_addr` 落在调用者可执行映射内。

### 下一步

R4b 复核 → 若有仍开项继续修；之后进 medium/low 批量清理，再进 R5 真机矩阵。

## 十、R4c 执行记录（2026-10-31）

第三轮复核（`Auditor/reports/2026-10-31-r4b-review.md`）判定 R4b 未达标：
AUD-a-6 / b-1 / b-2 / b-5 已关闭，AUD-b-3 部分关闭，并新开 AUD-d-1（high）、
AUD-d-2（high，R4b 引入）、AUD-d-3（high，既有）、AUD-d-4（low）。

### 本轮改动（提交 `71d18c7`）

- AUD-d-2：命令通道串行化从关中断自旋锁改为 mutex（`mutex_lock`/`mutex_unlock`/
  `__mutex_init` 走 kallsyms，缺符号降级为不串行化并告警）。命令体里有
  `copy_from_user`/`vmalloc`/`vm_mmap` 等可睡眠操作，关中断自旋锁下执行必然触发
  `sleeping function called from invalid context`，争用时是 `scheduling while atomic`。
- AUD-d-1：新增 `sh_addr_is_valid(task, addr)`，HOOK 前校验目标必须是用户地址且能读到
  完整指令字（同进程 `copy_from_user_nofault`，跨进程 `access_remote_vm`；缺符号时只做
  区间检查），失败返回 `-22`。
- AUD-d-3：`rearm_to` 重装断点前用同一函数校验目标（含从 LR 取回的地址），失败置
  `is_disabled = 1`，不把断点装到内核或无效地址。
- AUD-d-4：`GHOST_LIST` 与 ALLOC/FREE/WRITE 一起只给 root。
- AUD-b-3 剩余项沿用 R4b 的 `retired` 语义，仅替换串行化实现。

### 真机自检（Pixel 6，6.1.99-android14，实现方结论）

- shell `ghost list` / `ghost alloc` → `Operation not permitted (errno=1)`；`status` 仍可用。
- root `selftest` 全通过；连跑前后 `scheduling while atomic` 计数保持 2（均为 R4b 版本
  t=59s 遗留），无新增——AUD-d-2 修法在真机站得住。
- `hook 0x10000`（未映射）→ `EINVAL`，dmesg `[hwbp] target not readable user code: 10000`。
- 卸载 `unload_module: name: stealth_hook, rc: 0`，管理器进程存活。

### 产物（与真机测过的字节一致）

- `stealth_hook_0.3.0.kpm` 30656B `sha256=b6bd597c8ea92645…`
- `stealth_hook_0.3.0_debug.kpm` 34072B `sha256=c8d9ae8543166961…`
- instance `stealth_hook-0.3.0+gec933891ab21.ree8c93a8.kpb51197a.ndk26.1.10909125#1`

### 下一步

交同一批审计员做第四轮复核（AUD-b-3 / d-1 / d-2 / d-3 / d-4），通过后进 medium/low。

## 十二、R4f–R4g 执行记录（2026-10-31，AUD-f-1 根治）

第五轮复核（`Auditor/reports/2026-10-31-r4e-review.md`）认定 R4e 后仍不可交付：
AUD-f-1（high）——`rearm_to()` 在调试异常上下文调用
`perf_event_disable → modify_user_hw_breakpoint → perf_event_enable`，
6.1 内核这条链走 `perf_event_ctx_lock → mutex_lock`，可睡眠。R4f（`39c8254`，
登记 `25047bd`）修了 AUD-e-4（恢复误删的 `sh_hwbp_cleanup()`）与 AUD-f-2
（dump 兜底缺 `copy_from_user_nofault` 符号时整体禁用 dump），f-1 先按
「已接受残余风险」处置；随后维护者决定根治。

### 根治方案论证（为什么不是审计员建议的 pending-rearm）

对 v6.1 `arch/arm64/kernel/hw_breakpoint.c` 的核对推翻了 pending-rearm 方案：
框架 `breakpoint_handler()` 只对**默认** overflow handler 做 `step = 1` 单步 +
`toggle_bp_registers` 清/置断点寄存器；本模块注册**自定义 handler**，命中返回后
BVR 仍指向当前 PC，ERET 立即再次命中 → 无限调试异常循环。R4f 试过的 relay
双固定断点方案真机自旋（stealth_cli R 状态）正是这个机制。因此 rearm 必须
**在异常上下文内、免睡眠地当拍完成**。

方案：MSR/MRS 直写 ARM64 调试寄存器。命中匹配靠硬件寄存器值而非 perf 内部
字段（框架按 `BVR == instruction_pointer & ~0x3` 找槽），故直写 BVR 即改观察
地址：清 BCR bit0（停用）→ 写 BVR 新地址 → 原样写回 BCR（重新武装），纯寄存器
操作天然免睡，语义与旧三件套等价。槽位数取 `ID_AA64DFR0_EL1.BRPs`（实现数-1）
钳到 16。

### 本轮改动（提交 `8654cd0`，登记 `109eb10`）

- `rearm_to()` 重写：越界（`sh_user_addr_ok` 失败）先 disarm 再置 `is_disabled=1`；
  正常路径走 `sh_hwbp_rearm_msr()`。
- 新增 `sh_dbg_read/write_bvr/bcr()`（`dbgbvr<n>_el1`/`dbgbcr<n>_el1`，EL1 变体）、
  `sh_dbg_init_slots()`（DFR0 探测）、`sh_hwbp_rearm_msr()`/`sh_hwbp_disarm_msr()`。
- SKIP_ORIGIN LR 越界、INLINE trampoline 越界两条路径同样先 disarm（6.1 自定义
  handler 下留武装断点在原地会无限重触发——隐藏 bug 类）。
- 移除 `modify_user_hw_breakpoint` 符号依赖（声明/解析/打印）；修正 sh_dbg 符号
  打印变参错位（格式串残留 `modify=%llx`，实参已删，变参吃栈垃圾——真机
  `dis=72705f6670625f68` 暴露）。

### 已知局限（写入残余风险）

perf 框架内部（`perf_event.attr.bp_addr`、per-CPU 槽管理）不感知 MSR 直写：
上下文切换/热插拔/休眠唤醒后 `hw_breakpoint_reset` 会用 perf 记录的旧地址重装
BVR，断点可能回旧地址**错采一拍**（多记或少记一次命中），自愈不崩溃——以偶发
错采换确定性死锁，可接受。

### 真机自检（Pixel 6，6.1.99-android14，实现方结论）

- `stealth_cli selftest` 全过：before 68、hook rc=0、query `hits=2 enter=2
  x0=0x11 x1=0x22 ret_x0=0x44`、modify 546(0x222)、skip(1) 原函数执行、skip
  override 1911(0x777)、unhook 复原 68。**入口拍 rearm 到 LR 与返回拍 rearm 回
  原址均当拍生效**（延拍方案下返回拍 never fire、hits 停 1，实测 hits=2 即 MSR
  直写成功的直接证据）。
- `status`/`pte query`（6.1 正确降级 supported:no）/`ghost alloc/list/free` 全通。
- 非法地址 hook → `EINVAL`（AUD-d-1 保持）；卸载 `rc: 0`，cleanup 释放 1 node，
  管理器存活；全程 dmesg 无 BUG/oops/panic/scheduling while atomic（开机 9.9s/
  59.2s 两条 Call trace 为启动期旧账，与模块无关）。
- dbg 打印修正后 `register=.../counter=.../unregister=.../release=.../en=.../
  dis=...` 六参对齐，与 `/proc/kallsyms` 值一致。

### 产物（冻结提交 `8654cd0`，candidate 档位 verify 16/16 通过）

- base `stealth_hook_0.3.0.kpm` 33856B `sha256=971e47add6d8d6da…`
  instance `stealth_hook-0.3.0+g28d1a85ed6ce.r41ed197f.kpb51197a.ndk26.1.10909125#1`
- debug `stealth_hook_0.3.0_debug.kpm` 37440B `sha256=acc2a6a2a8c4c2ec…`
  instance `stealth_hook-0.3.0_debug+g28d1a85ed6ce.rc9c87052.kpb51197a.ndk26.1.10909125#1`
- 工具链指纹 `ndk26.1.10909125`（与 R4f 登记一致；`ndk26.3.11579264` 在本机对
  KP 头文件树汇编失败，`current.h` sp 全局寄存器变量不被接受）

### 独立窄复核结论（`Auditor/reports/2026-10-31-r4g-review.md`，覆盖 R4f+R4g）

总判断：**可交付（带条件）**，条件均为登记/文档动作，modules/ 零改动。

- AUD-f-1：**已根治，可由发现方关闭**。异常上下文安全成立（rearm/disarm 全为
  MRS/MSR+位运算+isb）；审计员对登记 debug 产物做字节级反汇编，106 条
  `0xD5xxxxxx` 指令全解码为 dbg 寄存器访问+ISB，无锁存寄存器。宏与上游
  v6.1 `AARCH64_DBG_READ/WRITE` 逐字符同构，`BRPs[15:12]+1` 语义正确。
  关单条件：错采局限写入 README 残余风险（已落实，见上文「已知局限」与
  README「边界与已知差异」）。
- AUD-f-3：现行写序（args 先、flags 后）已是最优，登记接受；AUD-f-4：仍在
  （现 hwbp.c:397-399），info 级不阻塞；AUD-f-5：维持 low 接受。
- AUD-c-5（qti_battery_charger）：**误判应关**——上游 module.h 证实
  state/list/name 为 struct module 固定前三成员，name 偏移恒为 24，
  "≈32" 机制被证伪；残余环境变量仅 CONFIG_GCC_PLUGIN_RANDSTRUCT（Android
  内核默认关），关单附一行注记。
- R4f 复核成立：cleanup 位置正确（先卸 syscall 桥再清节点）、释放路径无
  UAF/双释放、dump 兜底禁用正确。
- 新发现（均不阻塞）：NEW-g-1 low（cleanup 释放前未 disarm，理论 UAF 窗口，
  建议 R5 后跟进）；NEW-g-2 info（越界 disarm 静默无日志）；NEW-g-3 info
  （版本号多轮未 bump，建议随 R5 bump）。
- 复核局限：真机证据为实现方自测结论的一致性核对；上游对照为 torvalds
  v6.1 tag，未逐行比对 AOSP android14-6.1 分支；RANDSTRUCT 未从目标内核
  config 直接取证。

### 下一步

R5 真机全矩阵（NEW-g-1 随后跟进）；门禁：14 项检查 0 失败 2 警告（均为存量）。
