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
| 本模块基线提交 | 见本报告入库提交（`feat(kpm): 移植 stealth_hook`） |
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
