# 9 个历史模块批次：两路独立复核 + 针对性复审（2026-11-03）

本报告汇总三份独立取证的回执，供验收引用。三次都用独立 agent 执行，**禁止调用实现方/维护方脚本**
（`tools/identity.py` 的 import/verify、`tools/build_candidate.py`、`make`、`tools/check_repository.py`），
只读仓库与产物字节，SHA-256 由审计方自行计算。

## 1. 第一轮：两路并发（job `workflow-509ff83bdc6a438f`）

- 代码/构建链路：verdict「暂不可关单」
- 文档/身份路：verdict「暂不可关单」

两路都确认主链是硬的：

- 30/30 产物 sha256 与 size 与磁盘一致，30/30 instance_id 可独立复算；
- `source_tree_sha256` 两套算法对 108 条记录全一致；1578 条 `blob_sha256` 与 git 对象 0 条不符；
- 30 条 `source_commit` 均为 HEAD 祖先；本次提交只动 `metadata/`、`Developer/reports/`、`handoffs/`、`artifacts/`，未改 `tools/` 判据。

问题集中在记录自证性与事实断言，共 14 条（A-01…A-14 / AUD-01…AUD-11），详见
`Developer/reports/2026-11-03-remaining-modules-batch.md` 第 9 节处置表。其中：

- **中**：A-01（hosts_redirect debug 变体复用 base 的 `.o`，`-DCONFIG_DEBUG` 从未进入编译）、
  A-02（`3a9dcfc` 把失败归因给 `kmalloc`，实际拦截项只有 `kfree_skb`）、
  A-03（`sanitize()` 对 `--env` 值做纯子串替换，`PYTHONUTF8=1` 把所有字面 `1` 变成 `<PYTHONUTF8>`，污染 instance 身份）；
- **中低/低**：A-04（`kfree_skb` 有 5 处真实调用）、A-05（两条 `tested_bytes.sha256` 只有 16 hex）、
  A-06/A-07/A-08/A-09/A-10/A-11/A-12/A-13/A-14、AUD-06、AUD-11。

## 2. 处置后重建（提交 `411d011`、`8abac56`）

- `hosts_redirect`：debug 走独立对象 `hosts_redirect_debug.o`；重建后 base 10440 B `de45aba5…`、debug 11896 B `382f11e4…`，
  两件在 Pixel 6 上 `load rc=0` / `unload rc=0`；debug 件内含 8 条 base 没有的 `printk` 串，证明 `-DCONFIG_DEBUG` 真的进入编译。
- `cgroupv2_freeze`：设计内拒绝改 `-EOPNOTSUPP`；重建后 dmesg `error: -95`（修复前 `-24`），仍按设计拒绝。
- `tools/identity.py`：`sanitize()` 只把「像路径的值」当替换目标，`PYTHONUTF8=1` 不再污染 recipe。
- `tools/check_repository.py`：设备测试字节绑定从「仅 retest」扩到「任何登记了 `tested_bytes.sha256` 的条目」。
- 两条被截断的 `tested_bytes.sha256` 补全 64 hex；30 条 `builds[].handoff_manifest` 改指仓库内副本。

## 3. 第三轮：针对性复审（单路，只判六项修复）

verdict：**暂不可关单**（部分项只到「部分修」）。逐项：

| 项 | 判定 | 依据 |
| --- | --- | --- |
| AUD-101 hosts_redirect debug | 已修 | `modules/KPM/hosts_redirect/Makefile:37-38/49-50/52-53`；自算 10440 B vs 11896 B，字节不同 |
| AUD-102 cgroupv2 errno | 已修 | `cgroupv2_freeze.c:412-414`；产物级仅 2 字节差异（`0x928002e0`→`0x92800bc0`，即 MOVN #-24 → MOVN #-95） |
| AUD-103 归因 | 部分修 | 结论正确，但「可用性检查在 prepare 之前」的位置描述与 diff 不符 |
| AUD-104 kfree_skb 5 处 | 记录已修，注释残留 | 5 处调用逐行列出；源码注释仍写未调用 |
| identity.py sanitize | 已修 | 直接调用 `sanitize()` 断言：不再产出 `<PYTHONUTF8>`，路径仍被替换 |
| 设备测试字节绑定 | 部分修 | 判据已扩开且负向注入能红；但新分支零夹具，且本机 Windows 下 selftest 37/39 |

新增 7 条：AUD-N01（`Auditor/tools/artifact_audit.py:557-562` 在 Windows 上因 normpath 反斜杠导致「已登记但产物缺失」分支永不触发）、
AUD-N02（新判据缺正反夹具）、AUD-N03（`kfree_skb` 注释）、AUD-N04（AUD-103 位置描述）、AUD-N05（处置表过时）、
AUD-N06（`tools/selftest_tools.py` 子进程未传 `encoding`，zh-CN Windows 下 gbk 解码失败；`candidate_build_transaction` 夹具依赖 `shutil.which`）、
AUD-N07（`AUD-101/102` 在复核前即被标 fixed，属预先登记关闭）。

## 4. 处置后状态（提交 `858e348`）

- AUD-N03/N04/N05：源码注释、报告、metadata 证据均已更正；
- AUD-N02：`tools/selftest_tools.py` 的 `device_test_binding` 补三条正反夹具（16 位应红 / 64 位错哈希应红 / 64 位正确应绿），
  WSL 下 `tools/selftest_tools.py` **39/39 通过**、`tools/check_repository.py --strict` **15 项 0 失败 2 警告**（均为存量）；
- AUD-N01/N06/N07：属审计工具与流程问题，不改产品代码，记档于本报告与批次报告第 10 节；
- 仍未闭环：AUD-104（`kfree_skb` 解析为空导致错误路径静默泄漏 skb，需在目标 kallsyms 上确认 `kfree_skb_reason`）、
  AUD-105（`zram_zstd` 加载触发重启的归因）、A-06/A-07/A-08/A-11/A-13（产物清单口径、EOL 绑定、取代标记、宿主标签、指纹标签）。

## 5. 未验证声明

三轮均未做：独立重编译（禁止调用 `build_candidate.py`/`make`）、编译器二进制哈希复算、真机行为复现（第三轮无设备）、
C 侧内存安全的完整复核。第三轮只核了记录↔磁盘哈希一致与静态差异。
