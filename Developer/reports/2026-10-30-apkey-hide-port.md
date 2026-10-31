# 开发报告：apkey_hide 3.6.2（移植自上游 APKey_Hide_KPM）

## 身份

| 项 | 值 |
| --- | --- |
| commit | `648c4a40ce4fc94b76bb5cf4827c05d7462470e3`（模块落地提交；本报告写于提交前，身份栏于提交后回填） |
| KernelPatch commit | `b51197aaba8f2272dd8a3e30c85698a29aa928c9`（子模块，仓库内） |
| Build ID / instance_id | 未登记（探索构建，`identity.py` 尚未记录；候选交接时按 build_candidate.py 生成） |
| 配方指纹 | 未生成（同上） |
| 构建事务 | 探索期 `make all debug`（脏树允许）；候选交接必须改走 `tools/build_candidate.py apkey_hide --toolchain ndk26.1.10909125 --target "all debug"` 冻结提交后重建 |
| 工具链 | NDK r26.1.10909125 `aarch64-linux-android31-clang` + `ld.lld`（host: windows-x86_64，clang 带 OLLVM 提示正常） |
| 产物 | `apkey_hide_3.6.2.kpm` `sha256=5df0f31c9f1151290ae9adc90a9b46be7695c15ebdd4d4b9c109d17ca1517e5f` `size=22552` |
| 产物 | `apkey_hide_3.6.2_debug.kpm` `sha256=9e2f5c4c72c35b2435848b18eeac0cf7b7824c96cb91ab4d52fcfa87e39432ae` `size=28112` |

## 变更点

- 来源：`D:\work\1\kpm\_repos\APKey_Hide_KPM\APKey_Hide.c` v3.6.2（标准版；上游 README 与 APKey_Hide_S.c 自杀模式**未移植**——后者接管 sys_call_table[45] 整槽旁路，风险高且非必需）。
  上游源码 SHA-256：`a191840c0bbad46907d243f2d48cec523d7ecbc2b2858ecdbfd772b10793d27f`。
- 落位：`modules/KPM/apkey_hide/APKey_Hide.c`（文件名保留以便与上游 diff 对照）+ 自建 `Makefile`（上游无 Makefile，按 hosts_redirect 模板改写）。
- 源码仅改 2 行（声明区）：
  - `KPM_NAME("APKey_Hide")` → `KPM_NAME("apkey_hide")`（与目录/管理名一致，check_repository casefold 兼容）。
  - `KPM_VERSION("3.6.2")` → `KPM_VERSION(MYKPM_VERSION)`（仓库惯例：版本号由 Makefile `MYKPM_VERSION := 3.6.2` 单点管理，门禁核对 `metadata` 与 Makefile）。
  - **业务逻辑零改动**。KPM_LICENSE/AUTHOR/DESCRIPTION 保留原样（含上游 GitHub 链接溯源）。
- 功能语义（无偏移依赖 → 通用 KPM，不出 baselines）：inline hook KP 导出的 `get_ap_mod_exclude(uid)`（KP supercall before() 第一道闸，supercall.c:421）。非受信 uid return 1 → KP 早返回 → 落原生 truncate，消除 supercall 鉴权路径 `compat_strncpy_from_user` 读攻击者指针的缺页侧信道（懒分配页 / min_flt 计数 / VMA Referenced 位三条检测通道）。
- Makefile 要点：`debug` 目标独立 `debug/APKey_Hide.o`（目标专属变量对已存在前提不生效，两变体曾逐字节一致，已修）；debug 注入 `-DCONFIG_DEBUG -DAPK_DEBUG_LOG`（源码 debug 日志开关是 `APK_DEBUG_LOG`，:178 日志宏区）；Windows host 分支 `mingw64_nt-10.0-*-x86_64 → windows-x86_64`。
- 本轮覆盖：静态构建验证 + 导入表核对 + 字符串语义核对。未覆盖：真机加载（Tester 职责）、 Auditor 独立审计。

## 自检

```text
# 1. 导入表核对（无 libgcc 原子符号泄漏）
llvm-readelf -s apkey_hide_3.6.2.kpm | grep UND
→ 10 个 UND 符号全部在 KP 导出表：current_uid, is_trusted_manager_uid_android,
  get_ap_mod_exclude, set_ap_mod_exclude, kallsyms_lookup_name,
  hook, hook_wrap, unhook, hook_unwrap_remove, is_su_allow_uid
→ 无 __aarch64_* / libgcc 符号（源码禁 __atomic/__sync 的约束满足，加载不因导入表失败）

# 2. 构建确定性 + 变体分离
make clean && rm -rf debug && make all debug
→ release sha256 5df0f31c…17e5f 与首建逐字节一致
→ debug sha256 9e2f5c4c…39432ae（分离成功）

# 3. 变体语义（strings）
debug: 含 "version=3.6.2_d"、"[APKey_Hide] exit done"、
       "[APKey_Hide] refine(%s): no packages.list (data not ready?)" 等调试串
release: 仅 "version=3.6.2" + status 行（hook/refresh/swept/loader/mgr/.../n(excl,unexcl,skip)）
       + 无 "[APKey_Hide]" 前缀调试串 → release 零 printk 语义成立

# 4. 门禁
python tools/check_repository.py → exit 0；ci_build_selection 将 apkey_hide 计入 CI 构建列表（10 目录）
14 项检查 1 失败：tool_selftest/candidate_build_transaction —— git stash -u 后（树=HEAD，无本模块）同样 FAIL，
属 Windows 存量环境问题（fixture 的 #!/bin/sh mock-clang 无法被 subprocess 执行），与本移植无关。
```

- 构建：`make -C modules/KPM/apkey_hide all debug`（警告：0 条新增）
- 离线镜像回归：不适用（模块无结构体偏移依赖，不走 offset_harness）
- 代码走查结论：hook 点语义在本地代 KP（supercall.c:418-459）逐行核对成立——`get_ap_mod_exclude(uid)` 早返回仍在 `compat_strncpy_from_user` 读 x0（:426）之前；`refresh_trusted_manager_state` 未导出（导出的是 `refresh_trusted_manager_uid`），源码按 kallsyms after-only 钩子处理正确；`is_su_allow_uid`（sucompat.c:139 导出）兜底放行语义等价核对成立。

## 真机自检（实现方，非 Tester 结论）

设备：Pixel 6 / Android 15 / `6.1.99-android14-11-gd7dac4b14270-ab12946699`，APatch 11282（KernelPatch 0.13.9）。
模块经 APatch 管理器加载——该机预置了 superkey，`kpatch kpm load` 需要它，故未走命令行。

判据：supercall 缺页侧信道探针（`syscall(45, 懒分配页, -1)`；正对照 x1=0、负对照 getpid 先通过，
再取 5 轮多数票）。

| 运行身份 | 加载前 | 加载后 |
| --- | --- | --- |
| app uid 10274（非受信） | 5/5 命中 | **0/5** |
| shell uid 2000（su 白名单） | 5/5 命中 | 5/5 |

- 非受信调用者：缺页消失 ⇒ 钩子在 `supercall.c:421` 早返回，KP 不再读 x0，隐藏生效。
- su 白名单调用者：缺页仍在 ⇒ supercall 通路未被打断，APatch 自身功能不受影响。
  这一行是必需对照：若加载后 shell 也变 0/5，说明模块把整条通路打断了，那是另一种问题。
- 对照机（同代内核、无 KP 钩子）探针 0/5，证明信号来自钩子而非 truncate 本身；
  两台设备的正/负对照均成立，0/5 不是装置失效。

未覆盖：重启后状态、`ctl0` 命令（sweep/allow/deny/pkg/unclaim/mgrname）、嵌入模式（`pkg=`/`mgr=`）、
变体对比（只测了 release 件）。以上留给 Tester。

## 边界声明

> 本轮结论来自实现方自检（构建 + 导入表 + 字符串走查 + 一轮真机 A/B），**不是独立审计，也不是 Tester 结论**。
> 跨版本正确性、权限与生命周期安全、真机可用性分别由 Auditor 与 Tester 出具。

## 未解决项与降级行为

| 项 | 影响内核 | 当前行为 |
| --- | --- | --- |
| `refresh_trusted_manager_state` kallsyms 解析失败 | 全版本 | 降级：不挂 after 钩子，仅靠 uid0 supercall / 预算内窗口触发 refine；不失败不加载 |
| 6.8+ nofault 内核 | 6.8+ | 侧信道本身不存在，模块等效于无害空转（放行逻辑不变） |
| SELinux 拒读 packages.list | 全版本 | refine 不烧预算（APK_REFINE_MAX_TRY=4 保护），窗口按预算兜底（嵌入 8192 / cfg 256），超预算永久关窗 |
| tool_selftest candidate_build_transaction FAIL（Windows） | 与本模块无关 | 存量环境问题，交维护者/工具负责人；本机 `#!/bin/sh` fixture 无法执行 |

## 已知可疑点（请 Auditor 重点看）

- 源码为上游原样移植（仅 2 行声明宏改动），上游 v3.6.2 自身的并发模型（volatile 尽力而为 + ldaxr/stlxr 内联汇编）未经独立审计；重点核 `apk_refining`/`apk_pkgl_dirty` 的重入与 UAF 时序（v3.5/v3.6/v3.6.2 三轮时序修复都在这条线上）。
- ctl0 非受信返回 `-ENOENT`（模拟 find_module NULL）而非 `-EPERM`——审计请验证与「模块不存在」返回面逐位一致的结论（module.c:548-550）。
- TOFU 窗口期（嵌入模式、管理器 uid 未确认时放行应用段 [10000,90000) 触达 KP 原生鉴权）是设计上的开放面：窗口上限 8192 次，超限永久关窗；请审计关窗判据 `apk_mgr_cnt>0`（v3.6.2 修正）在 /data 未挂载时序下的完整性。
- 禁用 `__atomic/__sync` 的约束依赖源码自耦；若后续改动引入内建原子会生成 `__aarch64_*` libgcc 符号导致加载失败——建议 CI 加导入表黑名单断言（本轮只人工核对）。
