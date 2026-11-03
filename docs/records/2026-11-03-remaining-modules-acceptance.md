# 9 个历史模块批次：验收记录（2026-11-03）

## 1. 验收依据

- 独立复核：`Auditor/reports/2026-11-03-remaining-modules-batch-review.md`
  （两路并发复核 job `workflow-509ff83bdc6a438f` + 修复后的针对性复审）+ 批次报告
  `Developer/reports/2026-11-03-remaining-modules-batch.md` 第 9、10 节的处置表。
- 身份：每个被验收实例都绑定完整 commit + instance_id + 产物 SHA-256，见 `metadata/modules/<module>.json` 的
  `acceptances[]`；`builds[]` 中的 `source_dirty` 均为 `false`。
- 门禁：`python3 tools/check_repository.py --strict` 在 WSL `Ubuntu-24.04` 下 **15 项 0 失败 2 警告**
  （两条警告均为存量：`2026-10-11-zram-zstd-1.0.0-binary-ports.json` 缺 `handoff_by`/`handoff_at`；
  大量历史实例未登记 `handoff_registration`）。

## 2. 真机结果（Pixel 6 / 6.1.99-android14 / APatch 11282 / kernel 形态 / 管理器 uid 10273）

| 模块 | 变体 | 结果 | 说明 |
| --- | --- | --- | --- |
| re_kernel 11.7-20261010 | base | pass | `load rc=0` / `unload rc=0`，uptime 连续 |
| re_kernel_x 1.6-20261010 | base | pass | `load rc=0` / `unload rc=0`，uptime 连续 |
| hosts_redirect 2.0.0 | base + debug | pass | 重建后 debug 首次真正带 `-DCONFIG_DEBUG`；两件 `load rc=0` / `unload rc=0` |
| dont_kill_freeze 1.0.2 | base | pass | `load rc=0` / `unload rc=0` |
| lmkd_dont_kill 1.0.0 | base | pass | `load rc=0` / `unload rc=0` |
| nomount 2.1.1-20261014 | base | pass（常驻） | `NoMount: ready`，设计上常驻、只加载不卸载 |
| run_cmd_demo 1.3.0 | base | pass（常驻） | dmesg `WARNING: do not unload this module; reboot to remove it` |
| cgroupv2_freeze 1.0.12 | base | unsupported | 设计内拒绝：内核自带 `cgroup_freeze_write`，`error: -95`（EOPNOTSUPP） |
| zram_zstd 1.0.0 | base | **fail** | 加载瞬间设备重启，见 §4（后经 AUD-105 定位为「卸载残留 + 再加载」链）|
| zram_zstd 1.0.2 | base | pass | 卸载后加载另一模块（`re_kernel`）仍 `rc=0`，无 `-4094`；见 §4.1 |
| zram_zstd 1.0.1 | base | **部分** | 只摘算法、未摘 hook（AUD-107，已由 1.0.2 修复）；干净启动后 `load rc=0` → `unload rc=0`（dmesg `unregistered zstd-kpm`）→ 再次 `load rc=0`，uptime 连续；实例 `zram_zstd-1.0.1+g1fd533e14ac9.rbc4a0401.kpb51197a.ndk26.1.10909125#1` |

## 3. 覆盖范围

- 覆盖：冻结构建的可复现性（同一 tree/recipe 两次构建同哈希）、双变体产物差异、加载/卸载路径、
  `kpmctl` 与模块命名约定、真机 `load rc` 与 dmesg 关键字。
- 只测一个变体的模块（`re_kernel`、`re_kernel_x`、`dont_kill_freeze`、`lmkd_dont_kill`、`nomount`、`run_cmd_demo`）：
  其未测变体按仓库规则由「变体差异」结论绑定，差异限于日志/断言级别；`re_kernel`/`re_kernel_x` 的 baselines 变体只做构建与身份校验。
- `hosts_redirect` 例外：修复前 base/debug 同字节，修复后**两个变体都上机**（差异已不止日志）。

### 4.1 AUD-107：卸载后 hook 悬空（1.0.2 已修）

1.0.1 只摘了 `crypto_unregister_alg`，`do_init_module` 与 `zcomp_available_show` 两个 hook 留在原处；
镜像被回收后，卸载之后的任何一次 LKM 加载都会跳进已回收的块（`-4094 == HOOK_DUPLICATED` 是它的现场证据）。
1.0.2 在 `exit` 第一步摘掉两个 hook 再摘算法，并把 `kfunc_def(crypto_unregister_alg)` 的返回类型改成 `int`。
Pixel 6 实测：`load`→`unload`→`load` 三连 `rc=0` 且无降级日志；随后加载 `re_kernel` 也 `rc=0`/`unload rc=0`，设备不重启。

## 4. 不覆盖与残余风险

1. **zram_zstd 已由 1.0.1（摘算法）+ 1.0.2（摘 hook）闭环（原 1.0.0 记为未通过）**：1.0.0 的崩溃发生在「卸载残留 + 再加载」这条链上——`zram_zstd_exit()` 只打告警、不调用 `crypto_unregister_alg`，卸载后内核算法列表仍指向已释放的 KPM 镜像，再次 `load` 的 `crypto_has_alg()` 遍历时解引用悬空项 → EL1 缺页 panic。1.0.1 在 `exit` 摘除算法；但只摘算法、未摘 hook（AUD-107，高，见 §4.1），卸载后任何一次 LKM 加载都会跳进已回收镜像块。1.0.2 在 `exit` 第一步摘掉两个 hook 再摘算法，Pixel 6 实测见 §4.1。原 `AUD-106`（卸载后再加载 `-4094`）实为 AUD-107 的现场证据，不再单列残余。
2. **AUD-104**（`re_kernel` / `re_kernel_x`）：`kfree_skb` 在 6.1 上解析为空，`nlmsg_free` 的 5 处错误路径会静默泄漏 skb（不崩）。
   需在目标 kallsyms 上确认 `kfree_skb_reason` 后按符号可用性二选一解析，**不得按版本号猜**。
3. **A-07**：实例绑定的是工作树字节，本仓工作树 EOL 混合（CRLF/LF 并存），换机重检出后 `source_tree_sha256` 不可复算。
4. **A-06**：baselines 的 `<name>.kpm.json` 偏移表未进 `artifacts[]` 且被 gitignore，二进制移植侧的偏移表来源不在身份链上。
5. **A-08/A-11/A-13**：8 条历史失败构建仍标 `candidate`；宿主标签只归一 `mingw64_nt-*`；`device_fingerprint` 只 12 hex 却标 `sha256:`。
6. 未验证：`re_kernel`/`re_kernel_x` 的 baselines/baselines_debug/debug 变体未上真机；其余模块的 debug 变体未上机
   （`hosts_redirect` 除外）。
7. `zram_zstd`、`cgroupv2_freeze` 的结论只在本机（Pixel 6，6.1.99）成立；小米 13（5.10 LKM）本批次未复测这两项。

## 5. 结论

`re_kernel`、`re_kernel_x`、`hosts_redirect`（base+debug）、`dont_kill_freeze`、`lmkd_dont_kill`、`nomount`、`run_cmd_demo`
共 8 个实例（7 个模块）验收通过，绑定上文列出的冻结构建；`cgroupv2_freeze` 记为设计内拒绝（unsupported），
`zram_zstd` 1.0.0 记为未通过；1.0.1 部分闭环（摘算法，未摘 hook）、1.0.2 补齐摘 hook 并纳入验收（AUD-107 关单待独立复核）。
