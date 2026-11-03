# wxshadow 1.2.5 独立复核报告

- 日期：2026-11-03
- 被审对象：`modules/KPM/wxshadow` 版本 1.2.5
- 冻结提交：`340729b19c57ae01991a4c4781b7630a4913d19c`
- 冻结实例：`wxshadow-1.2.5+g35459c6bca01.rfbb58a4a.kpb51197a.ndk26.1.10909125#1`
- 产物：`artifacts/wxshadow-1.2.5+g35459c6bca01.rfbb58a4a.kpb51197a.ndk26.1.10909125/wxshadow_1.2.5.kpm`
  （162,584 B，`sha256=fa24d9d017c416dd777e2046f10f2c2590a89a91804058afd5d9cdb95f10d4aa`）
- 复核方式：三路独立 agent（代码路、文档/身份路、复审确认路），禁用实现方脚本
  （`tools/identity.py` / `tools/build_candidate.py` / `make`），自行读源码、自行复算哈希。
- 本轮结论：**可关单**。

## 一、身份三要素（文档/身份路自算）

| 项 | 值 | 一致性 |
| --- | --- | --- |
| `git rev-parse 340729b…` | `340729b19c57ae01991a4c4781b7630a4913d19c` | 存在，subject 与登记一致 |
| 产物 sha256（自算） | `fa24d9d017c416dd777e2046f10f2c2590a89a91804058afd5d9cdb95f10d4aa` | 与 `metadata:447`、handoff `:103`、归档 `MANIFEST.json` 三处一致 |
| instance | `wxshadow-1.2.5+g35459c6bca01.rfbb58a4a.kpb51197a.ndk26.1.10909125#1` | 三处一致 |
| 报告字节 | `git show 340729b:Developer/reports/2026-11-03-wxshadow-port.md` → `3fccb7b7…` | 与 `report_versions[0].versions[0].sha256` 一致（该文件按 `-text` 入库，工作树字节 = 入库字节） |
| `sources[]` 双哈希 | `wxshadow_bp.c`：工作树 `5a8ff473…`、blob `cd6f7d78…` | 与 metadata 逐字一致（11 个源文件里 10 个可复算，`tools/identity.py` 自身为冻结后演化） |

## 二、代码路（verdict：可关单）

已确认修复：

| 单号 | 证据 |
| --- | --- |
| AUD-wx-4 | `wxshadow_bp.c` 的 `copy_from_user_via_pte` 改为 `wxshadow_read_user()`（`wxshadow_pgtable.c` 末尾），走查前 `mmap_read_lock`、pfn 经 `wx_pfn_is_valid` 与 `is_kva(pfn_to_kaddr())` 双校验，失败统一 `-14`；`prepare_shadow_target` 的 `split_huge_pmd` 由 `wx_mmap_lock_begin/end` 包住 |
| AUD-wx-13 | register 回落（`wxshadow_brk_hook_fn`/`wxshadow_step_hook_fn`/`wx_unregister_brk_step_hooks`、init/exit 的 `WX_HOOK_METHOD_REGISTER` 分支）全部删除；method 判定收紧到只认 `kfunc_brk_handler && kfunc_single_step_handler`（`wxshadow_scan.c:405-412`）；失败路径与 phase 清理配对（`wxshadow.c:1500-1520`、`:1537-1546` 只走 `hook_unwrap`） |
| AUD-wx-14 | pin 代码整体撤除，`wxshadow_read_user()` 注释写明「唯一调用点是 prctl direct hook，钩子上下文不能缺页」；无半截 pin 残留 |
| WI-1 | `wx_allow_lock`（`DEFINE_SPINLOCK`）+ 无锁核心 `wx_uid_in_list()`，四个入口全部持锁 |
| WI-2 | ctl0 的两次 `compat_copy_to_user` 返回值均检查，失败 `pr_warn` + `-14` |
| WI-3/4/5/6/7 | 注释改 1.2.2 语义；`wxshadow_flush_icache_page` 已删；边界改减法式；`wx_dump_allow` 有界写；`debug_print_tasks_list` 定义与调用同受 `#ifdef WXSHADOW_DEBUG_TASKS` 覆盖 |

夹带核查：与上游逐文件 `git diff --no-index --ignore-cr-at-eol` → `wxshadow_handlers.c` 零差异，
`wxshadow.c` 342+/104−、`wxshadow_bp.c` 49+/36−、`wxshadow_pgtable.c` 163+/8−、`wxshadow_scan.c` 14+/4−、
`wxshadow_internal.h` 49+/2−、`wxshadow.h` 1 行注释，全部可归到已登记问题单与 1.2.5 的 prctl 通道改造，未发现夹带。

新开问题单：

- **AUD-wx-15（低，可延后）**：`wxshadow.c:85-87` 仍定义 `kfunc_register_user_break_hook` /
  `kfunc_register_user_step_hook` / `kptr_debug_hook_lock`，`wxshadow_scan.c:392-402` 仍解析并 `%px` 打印，
  全仓无消费者 —— 纯死解析 + 加载期内核地址泄漏（需 root 读 dmesg，非权限边界问题）。
  建议下一版随日志降噪一起删除，并把 init 期 `%px` 日志收进 `WXSHADOW_DEBUG_TASKS`。
- **AUD-wx-16（低，已修）**：`Makefile` 原注释「上游无任何 `#ifdef` 调试宏」在 1.2.2 引入
  `WXSHADOW_DEBUG_TASKS` 后失实；`007fe51` 已改写为准确描述。
- **AUD-wx-17（低，登记为已知残余风险）**：5.10 无 `mmap_read_lock` 符号时 `wxshadow_read_user()`
  整段无锁走查，`wx_pfn_is_valid` 兜底仅 `pfn != 0 && pfn < (1UL<<32)`；并发 THP collapse/split 与
  munmap 的 race 窗口内可能取到失效 pfn，最坏后果是读到错误物理页（补丁数据损坏），不会崩在页表上。
  代码注释与加载告警已如实声明该降级。

## 三、文档/身份路（verdict：可关单，修复后由复审确认路关闭）

| 单号 | 判定 | 关键事实 |
| --- | --- | --- |
| AUD-019 | 可关 | 身份节已含 1.2.4（标注「未单独冻结构建、并入 1.2.5」）与 1.2.5 的 sha256/instance/source_commit |
| AUD-020 | 可关 | `issues[]` owner 全部改为修复方 Developer，fixed 条目 `closed_by=Auditor`；`device_tests[]` 补双机条目并绑定 1.2.5 哈希 |
| AUD-021 | 可关 | `grep -n "PAGE_SIZE -"` 实测 6 处（`wxshadow.c:337/:356`、`wxshadow_bp.c:489/:599/:1144`、`wxshadow_pgtable.c:1109`），报告/README/metadata 三处口径统一 |
| AUD-022 | 可关 | `AUD-wx-11` status=open、owner=Developer，与「fork 提案未采用、指针仍钉 `b51197a`」相符 |
| AUD-023 | 可关 | 客户端 README 的 allowlist 语义（默认 {0,2000}、上限 8 项、uid 0 不可 deny）与 `wxshadow.c:1792-1869` 一致 |
| AUD-024 | 可关 | 新归档 `MANIFEST.json` 正则扫描 `[A-Za-z]:[/\\]` 零命中；`tools/identity.py` 的 `_path_variants()` 覆盖三种分隔符 + 兜底正则 |
| AUD-025 | 可关 | 行尾口径一节明确 `blob_sha256`（LF 提交字节）与 `sha256`（CRLF 工作树字节）；本报告改用 `-text` 入库后两者统一 |
| AUD-026 | 可关 | `.gitmodules` 仍指上游、指针 `b51197a`、fork `f0c9e72` 仅存提案；README 源文件计数改为 5 `.c` + 2 `.h`；Makefile 注释改写 |
| AUD-027 | 可关 | 行尾口径一节不再写「本仓无 `.gitattributes`」，改为说明本报告的 `-text` 豁免 |

## 四、未验证（两路共同声明）

- 未独立重编译：产物字节与源码的一致性以登记记录互校为准，未重跑 `make`。
- 未做内核级行为验证：AUD-wx-13/14 的拒载分支两台设备均无法触发，只有代码审查覆盖；
  真机数据（报告 1.2.5 一节的 dmesg/`ret` 值）以实现方记录为准，本报告只核对文档与哈希一致性。
- `prctl` inline-hook（`hook_wrap` 直挂 `__arm64_sys_prctl`）属超出问题单清单的改造，行为正确性经
  `KernelPatch` 侧 `hook.c`/`fphook.c`/`syscall.h` 语义核对，未上机做异常路径验证。

## 五、结论

代码路与文档/身份路均判**可关单**；剩余三条为低严重度且已登记（AUD-wx-15 可延后到下一版、
AUD-wx-16 已修、AUD-wx-17 为登记在案的降级残余风险）。`AUD-wx-11`（LKM `current_uid` 语义潜伏缺陷）
与 `AUD-wx-12`（历史归档 MANIFEST 路径，判据归维护者）保持 open，不阻塞本次复核结论。
