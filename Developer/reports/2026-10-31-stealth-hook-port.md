# stealth_hook 移植开发报告（xiaojianbang-stealth-hook → modules/KPM/stealth_hook）

- 日期：2026-10-31
- 角色：Developer
- 上游：`_repos/xiaojianbang-stealth-hook`（GPL-2.0-or-later，作者 xiaojianbang，4533 行）
- 结论：**候选（探索构建，未冻结）**。三块能力（HWBP / Ghost / PTE 探测）在 Pixel 6 上真机跑通；
  PTE 真实重定向按设计不移植，见「边界声明」。

## 一、身份

| 项 | 值 |
| --- | --- |
| 仓库 | `APatch_kpm`（分支 `my`） |
| 本模块基线提交 | `47e5b5a`（feat(kpm): 移植 stealth_hook（HWBP / Ghost / PTE 能力探测）+ stealth_cli） |
| KernelPatch 子模块 | `b51197aaba8f2272dd8a3e30c85698a29aa928c9`（0.13.9） |
| instance_id | 未登记（探索构建，未走 `build_candidate.py`） |
| 工具链 | NDK 26.1.10909125（clang 17.0.2，`aarch64-linux-android31-clang`） |

产物（当前源码，clean 重建可复现）：

| 产物 | 大小 | SHA-256 |
| --- | --- | --- |
| `stealth_hook_0.3.0.kpm` | 26368 B | `22ce8b98f206870fb410c3d7d61bed38756fad6107a49ade3197197a31c36c17` |
| `stealth_hook_0.3.0_debug.kpm` | 29880 B | `f9db0eca1220159be48a35c6e4e97d0871c66ca18848c22f9be704da4c54e890` |

## 二、变更点

### 2.1 模块本体 `modules/KPM/stealth_hook/`

| 文件 | 行数 | 内容 |
| --- | --- | --- |
| `stealth_hook.h` | 139 | 通道常量、命令号、用户态/内核侧结构体（与上游逐字段一致） |
| `stealth_hook.c` | 304 | KPM 声明、uid 门、`sh_syscall_before`、命令分发、ctl0、生命周期 |
| `ghost_mem.c` | 267 | `vm_mmap` / `vm_munmap` / 读写 / 列表 |
| `hwbp.c` | 586 | perf 硬件断点注册、观察者状态机、改参/跳过、dump、清理 |
| `Makefile` | — | `MYKPM_VERSION := 0.3.0`，`all` / `debug` 双目标 |
| `README.md` | — | 命令表、通道、边界、更新记录 |

相对上游的改动：

1. **通道号 285 → 427**。上游用 `copy_file_range`（285），那是 IO 热路径，每次 IO 都要多一次
   magic 比较；改用 `io_uring_register`（427）。magic 仍为 `0x584A42`（"XJB"），便于对表上游协议。
2. **加 uid 门**。上游任何进程只要猜中 magic 就能下发命令；本移植默认只信 `{0, 2000}`，
   可 ctl0 维护（`allow:` / `deny:` / `allowlist` / `status`），uid 0 不可移除。
3. **MapsHide 移出本模块**，并入 `memcrc` 的 `RANGE:uid:start:end`（按 uid 作用域，上游按 pid；
   见 §四的 tgid_offset 教训）。
4. **PtraceSpoof 移出本模块**，并入 `memcrc` 的 `REGS:uid`（见 memcrc 0.2.0 报告）。
5. **PTE 只保留能力探测**。`0x2001` 返回本机是否具备 `__tracepoint_android_rvh_do_mem_abort`；
   `0x2002`–`0x2004` 一律 `-EOPNOTSUPP`。理由见「边界声明」。
6. **FULL_HOOK / AUTO_THREAD 未移植**。前者依赖用户态 DBI 整页重编译；后者靠钩
   `wake_up_new_task` 给新线程补断点，CLI 轮询 `/proc/<pid>/task` 可替代，少一个全局钩子。
7. **`kf_vmalloc` / `kf_vfree` 不可用**。KP 在 `kernel/patch/ksyms/misc.c:345/:360` 只做了
   `kfunc_def`，**没有** `KP_EXPORT_SYMBOL`（`grep KP_EXPORT_SYMBOL | grep -c vmalloc` = 0），
   写 `kf_vmalloc(...)` 会在加载时因未定义符号失败 → 改为 `kallsyms_lookup_name("vmalloc"/"vfree")`。

### 2.2 客户端 `Developer/tools/stealth_cli/`

| 文件 | 内容 |
| --- | --- |
| `stealth_cli.c` | 单文件客户端：status / hook / unhook / enable / disable / query / override / skip / ghost / pte / selftest |
| `Makefile` | NDK 交叉编译，产出 aarch64 静态可执行 |
| `README.md` | 用法、权限、selftest 期望输出、与上游 CLI 的差异 |

去掉上游的 `.kpm` 加载器（走 supercall 会与 APatch 管理器抢通道）与 DBI 重编译；协议结构体不变。

## 三、自检

- 构建：`make all debug`，**0 警告 0 错误**（`-Wall -Wextra`）。
- 导入表 17 个 UND，全部为 KP 导出：
  `compat_copy_to_user cred_offset has_syscall_wrapper inline_unwrap_syscalln inline_wrap_syscalln
  kallsyms_lookup_name kf_memcpy kf_memset kf_snprintf kf_strncmp printk sp_el0_is_current
  sp_el0_is_thread_info task_in_thread_info_offset task_struct_offset thread_info_in_task thread_size`
- 内嵌元数据：`name=stealth_hook` / `version=0.3.0` / `license=GPL v2` / `author=xiaojianbang`。
- clean 重建哈希一致（构建确定性）。

## 四、真机自检（实现方，非 Tester 结论）

设备：Pixel 6 `sha256:433ab88276ec`（Android 15，内核 6.1.99-android14，APatch 11282）。
加载方式：APatch 管理器「加载」→ 文件选择器选 `stealth_hook_0.3.0.kpm`；
dmesg（需 `su -c`）：`[+] KP I load_module_ex: [stealth_hook] succeed with [(null)]`。

### 4.1 通道与 uid 门（0.1.0 起验证，0.3.0 复验）

| 调用 | 加载前 | 加载后（shell 2000） | 加载后（app 10274） |
| --- | --- | --- | --- |
| `syscall(427, 0, ...)` 无 magic | -1 / errno 95 | **不变** | **不变** |
| STATUS | -1 / errno 9 | `0x534B4F4B` | -1 / EPERM |
| ECHO `0xdeadbeef` | -1 / errno 9 | `0xdeadbeef` | -1 / EPERM |
| 未知命令 | -1 / errno 22 | -1 / EPERM | -1 / EPERM |

卸载后四条全部回到加载前的 `95 / 9 / 9 / 22` → `inline_unhook_syscalln` 干净复原。

### 4.2 HWBP 全链路（`stealth_cli selftest`）

```
call before hook: 68 (expect 68)
hook: rc=0
call with hook: 68 (expect 68)
query: rc=0 hits=2 enter=... total=1 x0=0x11 x1=0x22 lr=0x224724 ret_x0=0x44
override modify: rc=0
call with modify: 546 (expect 546)      ← X0 被改成 0x100
skip count=1: rc=0
call with skip(1): 546 (仍执行原函数，skip 只跳过记录)
override skip: rc=0
call with skip: 1911 (expect 1911)      ← 原函数被跳过，直接返回 0x777
unhook: rc=0
call after unhook: 68 (expect 68)
```

判读：`68 = 0x44` 原值；`546 = 0x222` 说明改参生效；`1911 = 0x777` 说明 SKIP_ORIGIN 生效；
卸载后回到 `68` 说明 perf 事件释放干净。

### 4.3 Ghost

```
ghost alloc: va=0x7f9a9f1000 size=8192 prot=7
ghost list: 1 segment(s)  [0] va=0x7f9a9f1000 size=8192 pid=0 prot=7
```

配合 `memcrc` 的 `RANGE:2000:<va>:<va+0x2000>` 可把该段从 `/proc/self/maps` 抹掉（0.2.0 已验：
`has=1 lines=29` → `has=0 lines=28`）。

### 4.4 PTE 能力探测

```
PTE_QUERY  rc=0   → 本机不支持
PTE_HOOK   rc=-1 errno=95 (-EOPNOTSUPP)
PTE_REARM  rc=-1 errno=95
PTE_UNHOOK rc=-1 errno=95
```

## 五、边界声明

1. **PTE 真实重定向不移植**。三条理由：
   - 6.1 起内核移除 `__tracepoint_android_rvh_do_mem_abort`（Pixel 6 实测 0 处，小米 13/5.10 实测 4 处），
     拦截点不存在；
   - 即便 5.10，重定向还需要用户态 DBI 把目标页重编译成「搬家版页」并给出 `offset_map[1024]`，
     否则 `regs->pc` 无处可跳；
   - 上游自身也标注了三个硬伤：一次性（清 UXN 后不重装，必须 `PTE_REARM`）、`node->ptep` 裸指针无页表锁、
     依赖厂商 hook 点。
   因此只保留 `PTE_QUERY`，其余命令返回 `-EOPNOTSUPP`——不猜、不降级、不做半套。
2. **只支持当前进程的 ghost**：上游同样限制（跨进程走 `kthread_use_mm` 会 panic，返回 `-95`）。
3. **卸载不 munmap**：卸载发生在管理器上下文，拿调用方地址 munmap 会误伤管理器；只回收记账节点。
4. **perf attr 是手写快照**：`struct sh_perf_event_attr` 128 字节 = `PERF_ATTR_SIZE_VER7`，
   在 5.10 / 6.1 上偏移对得上；6.7+ 内核若再扩字段会失配，届时必须重新对齐而不是猜。
5. **PAN 开关靠 `.inst` 硬编码**：`0xd500409f`（关）/ `0xd500419f`（开），只在 arm64 + ARMv8.1 以上有效。

## 六、未解决项

| 项 | 状态 | 说明 |
| --- | --- | --- |
| 小米 13（5.10.149）上的 PTE_QUERY 应返回 1 | **未实测** | 该机是 `kernelpatch-lkm` 形态且为日常机，未在其上加载本模块；仅以 `/proc/kallsyms` 中该 tracepoint 存在 4 处作为间接证据 |
| PTE 真实重定向 | 不移植 | 见边界声明 |
| FULL_HOOK / AUTO_THREAD | 不移植 | 依赖 DBI / 可用 CLI 轮询替代 |
| 多线程并发断点、跨进程断点 | 未覆盖 | 只测了单线程自命中 |
| 6.7+ 内核 | 未覆盖 | perf attr 尺寸假设可能失配 |

## 七、已知可疑点（请 Auditor 重点看）

1. **`hwbp.c` 的观察者状态机**：入口断点命中后 `rearm_to(node, LR)` 把断点挪到返回地址，
   返回那一拍再挪回原地址。若被 hook 的函数走尾调用或异常路径不经过 LR，断点会停在错误位置，
   表现为后续命中丢失。建议审 `rearm_to` 的失败分支是否会留下悬挂事件。
2. **`sh_perf_event_attr` 的 `bp_len` / `bp_type`**：执行断点用 `HW_BREAKPOINT_LEN_4`，
   在 AArch64 上执行断点长度必须为 4；若上游或后续改动传入别的长度，`perf_event_open` 会返回 `-EINVAL`。
3. **`sh_ghost_write` 的分块搬运**：按 256 字节分块 `__arch_copy_from_user` + `compat_copy_to_user`，
   跨页边界时 `compat_copy_to_user` 的返回值未逐块校验（失败只返回错误码，不回滚已写部分）。
4. **uid 门与 ctl0**：`sh_allow_uids` 是静态数组，`allow:` 满了直接返回错误；`deny:0` 被拒绝。
   若管理器 uid 不在表内且 root 也被移除，只能卸载重载恢复——这是有意的「不可自我锁死」设计，
   但需要 Auditor 确认没有绕过路径。
5. **`sh_dispatch` 的 default 返回 `-1`（EPERM）**：与上游一致，但会把「未知命令」伪装成权限错误，
   调试时容易误判（CLI 的 `die()` 已区分 `errno`）。

## R4h：节点身份、去重与通配（0.3.1）

### 触发

R5 前独立审计 **AUD-B-1（high）**：`hwbp.c` 的 rearm/disarm 按「BVR 值匹配」找槽，
同址第二个节点会被误伤（rearm 挪错槽、disarm 把别人 BCR.E 清零且 perf 不知道 →
断点永久静默）。根因是 `HWBP_HOOK` 不做去重，同一个 `(addr, tid)` 可以挂出多个节点。

### 决定：去重键是 (addr, tid)，不是 addr

审计前我先核对了每个命令的匹配方式：`unhook`/`enable`/`disable`/`query`/
`set_override`/`set_skip` 全部按 `(target_addr, tid)` 匹配（`tid == 0` 通配）。
所以「只按地址去重」会掐掉「同一个函数挂到多个线程」这个合法用法——CLI 循环给
90 个线程挂钩靠的就是它。去重键取 `(addr, tid)` 之后，AUD-B-1 的场景（同一线程
同一地址两个节点）在结构上不再可能出现，而多线程同址保留。

顺带修掉一处「孤儿」：`unhook`/`enable`/`disable`/`set_skip` 原来「匹配到第一个就
return」，同 `(addr, tid)` 第二次 hook 出来的节点没有任何命令能单独操作（只有
`query` 看得见）；rearm 注释里「一次改齐所有匹配槽」就是给它擦屁股的补偿逻辑。
现在这些命令按 `tid == 0` 通配、一次改齐，并返回实际影响的节点数。

### 改动

- `hwbp.c`：新增 `hwbp_find_node(addr, tid)`；`sh_hwbp_hook` 入口去重（参数完全相同
  返回 0 幂等，不同返回 `-17`/EEXIST 并 `pr_err` 说明先 unhook）；`sh_hwbp_unhook`/
  `sh_hwbp_enable` 改为遍历全部匹配并返回个数（`-2` 仍表示「只剩已退役节点」，
  保持 NEW-b-3a 的语义）；`sh_hwbp_set_skip` 增加 `tid` 形参；`sh_hwbp_query` 的
  `target_addr == 0` 表示列出全部，并在回包里回显节点的真实地址。
- `stealth_hook.c`：`sh_hwbp_set_skip` 调用点传 `arg3` 作 tid。
- `stealth_cli`：新增 `list`（等价 `query 0` 分页）；`--tid all`/`*` 别名；
  `unhook`/`enable`/`disable` 打印影响个数；`skip` 支持 `--tid`。
- 版本 0.3.0 → 0.3.1（同时关掉 NEW-g-3「版本未 bump」）。

### 真机验证（Pixel 6 / Android 15 / 6.1.99-android14 / APatch 11282）

探针 `r4h`（只走 syscall 427，不链接模块）三种模式全 PASS：

| 用例 | 期望 | 实测 |
| --- | --- | --- |
| 同参数重复 hook | rc=0 幂等 | rc=0，`query` total=1（没有孤儿） |
| 不同 dump 再 hook | `-EEXIST` | rc=-1 errno=17 |
| `unhook addr --tid all` | 返回摘掉个数 | removed=1 |
| 同地址挂两个线程 | 都允许 | rc=0、rc=0，`query` total=2 |
| 通配 unhook 两线程 | 一次摘 2 个 | removed=2 |
| 通配 disable / enable | 各命中 1 个且回显状态 | changed=1、`is_disabled` 1→0 |
| `skip` 带 tid | rc=0 | rc=0 |
| `selftest` 回归 | 全链路仍通过 | hook/query/modify/skip/SKIP_ORIGIN/unhook 全 PASS |

### 残余与边界

- 去重是「同 (addr, tid) 拒绝第二次」，不是「合并配置」。要改 flags/dump 必须先
  `unhook`；`MODIFY_ARGS`/`SKIP_ORIGIN` 走 `SET_OVERRIDE`，不触发去重比较
  （比较时只取 hook 期 flag 掩码）。
- `unhook`/`enable` 的返回值从 0 变成「个数」，调用方若断言 `rc == 0` 需要改成
  `rc >= 0`；CLI 已同步。
- 未变：链表仍无锁（AUD-b-3 的残余）、rearm 仍在异常上下文直写 MSR。

## R4i：去重双向相等与 skip 计数（0.3.2）

第六轮复核（`Auditor/reports/2026-11-02-r4h-review-a.md`）判 AUD-B-1「部分修好」，
留了两条：

- **AUD-B-2-1（medium）**：幂等判断写成单向子集比较
  `(dup->flags & (req.flags & mask)) == (req.flags & mask)`，只要新请求的 flag 是旧的
  子集就返回 0。「先挂 `--listen-ret` 再用更少 flag 重挂」因此被静默当成成功——
  正是本轮要消灭的失败模式。改成 `(dup->flags & mask) == (req.flags & mask)`。
- **AUD-B-2-2（low）**：`sh_hwbp_set_skip` 仍 `return found > 0 ? 0 : -2`，没按契约
  回传影响个数。改成 `return found > 0 ? found : -2`，CLI 打 `changed=`。

`selftest` 补两条回归（同参数重挂 → 0；异参数重挂 → `-EEXIST`），并在真机验证：

```
hook: rc=0
rehook same: rc=0 (expect 0, 幂等)
rehook diff flags: rc=-1 errno=17 (expect -1 / 17 EEXIST)
query: rc=0 hits=2 enter=2 total=1 x0=0x11 x1=0x22 lr=0x224ac0 ret_x0=0x44
override modify: rc=0 ; call with modify: 546
skip count=1: rc=1 (expect 1 = 影响节点数)
override skip: rc=0 ; call with skip: 1911
unhook: rc=1 ; call after unhook: 68
```

版本 0.3.1 → 0.3.2。

## R4l：0.3.3 修正 status 返回值与文案（P0 收尾）

R5 真机矩阵做 ctl0 验证时发现两处可读性/正确性问题，均属「输出与身份」而非机制：

1. **magic 被版本号污染**：`sh_dispatch` 的 STATUS 分支写的是 `return (long)SH_STATUS_MAGIC + SH_VERSION_CODE;`，
   加法让 `0x53484F4B + 0x00030003 = 0x534B4F4E`，按字节读出来是 `"SKON"` 而不是 `"SHOK"`。
   改成只回 magic（`0x53484F4B`），版本交给 `ctl0 status` 文本。
2. **`ctl0 status` 文案**：`v%x` 把 `0x00030000` 打成 `v30000`；`sh_dump_allow()` 的 `"allowlist:"` 没有前导空格，
   与 `nr=427` 黏成 `nr=427allowlist:`。改为 `v%d.%d.%d` + `" allowlist:"`，并把 `SH_VERSION_CODE` 统一成
   `0x00MMmmpp`（`0.3.3` = `0x00000303`）。

CLI 侧同步：`status` 只打印 magic 与四字符解码，不再把 magic 字节当版本号（原先打 `version 75.79.75`）。

### 产物（当前源码）

- `stealth_hook_0.3.3.kpm` 34448B `sha256=2c7e1ecbd1bf8638bd970ee8369b8f022788b22b48eaa4ea803afbe67dd3e313`
- `stealth_hook_0.3.3_debug.kpm` 38016B `sha256=f838752dfb7902ae8a432240948d631527e433a9ee43683cd4d39a87ceb54671`

### 真机自检（Pixel 6 / Android 15 / 6.1.99-android14 / APatch 11282）

```
$ kpmctl 10273 ctl stealth_hook status
out: stealth_hook v0.3.3 nr=427 allowlist: 0 2000
$ stealth_cli status
status: 0x53484f4b  (magic "SHOK"; 模块版本用 ctl0 status 查看)
$ stealth_cli selftest
hook: rc=0 / rehook same: rc=0 / rehook diff flags: rc=-1 errno=17
query: hits=2 enter=2 total=1 x0=0x11 x1=0x22 lr=0x224b80 ret_x0=0x44
unhook: rc=1 / call after unhook: 68
```


## 0.3.4：去掉 LKM 形态缺失的三个符号（2026-11-02）

### 触发

小米 13（`kernelpatch-lkm`，5.10.149-android12-9）加载 0.3.3 报 `unknown symbol`：
先 `inline_wrap_syscalln`，改掉后又依次暴露 `inline_unwrap_syscalln`、`cred_offset`。
三者都只有内核集成形态导出，LKM 兼容表里没有；装载器不看 weak binding，
解析失败即 `-ENOENT`，在 `apply_relocations()` 之前中止，`init` 从不执行。

### 改动

1. `sh_current_uid()`（`stealth_hook.c:43-47`）原本自己算
   `*(struct cred **)(current + task_struct_offset.cred_offset)` 再取
   `cred + cred_offset.uid_offset`；改为 `return current_uid();`。
   `current_uid` 在 kernel 形态由 `kernel/patch/common/utils.c:178` 导出、
   LKM 形态在 `symbols.c` 兼容表里，两形态都可解析。
2. syscall 桥：`inline_hook_syscalln(SH_SYSCALL_NR, 6, ...)` 改为
   `sh_syscall_addr = syscalln_addr(SH_SYSCALL_NR, 0); hook_wrap(addr, has_syscall_wrapper ? 1 : 6, ...)`；
   exit 侧 `inline_unhook_syscalln(...)` 改为 `hook_unwrap(addr, ...)` 并清空地址。
   与 KP `syscall.c:313-330` 的 `inline_wrap_syscalln` / `inline_unwrap_syscalln` 同构。
3. 版本号：status 原先读手写宏 `SH_VERSION_CODE`（0x00000303），升 0.3.3 时漏改；
   改为 `sh_ver_triple()` 从 `MYKPM_VERSION` 解析，删掉该宏。

### 符号面

`tools/load_forms.py --artifact stealth_hook_0.3.4.kpm` → `und=19 形态=kernel,lkm kernel缺=0 lkm缺=0`
（0.3.3 为 `und=22`、lkm 缺 3）。

### 真机（同一份产物）

| 设备 | 形态 | 观测 |
| --- | --- | --- |
| Pixel 6（uid 10273） | kernel | `load rc=0`；`ctl0 status` → `stealth_hook v0.3.4 nr=427 allowlist: 0 2000`；`selftest` 全通过 |
| 小米 13（uid 10270） | lkm | `load rc=0`；同样 status 与 `selftest` 全通过（`rehook diff flags: rc=-1 errno=17`、`skip count=1: rc=1`、`unhook: rc=1`、`call after unhook: 68`） |

### 未覆盖

- PTE 能力探测在 6.1 上返回 -95（已知，见未覆盖项表）；5.10 上仍依赖未移植的 DBI。
