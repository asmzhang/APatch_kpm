# 归档记录：2026-10-28 仓库重建前的构建台账

## 为什么归档

上游仓库在 `D:\work\1\kpm` 被整体删除后重新克隆，`APatch_kpm` 的历史因此重置为新的 `main`。
重建前登记的 9 份模块记录里，`builds[].source_commit`、`builds[].build_transaction.source_commit`
与 `deliveries[].delivery_commit` 指向的提交在新历史中不存在（共 183 个唯一提交、366 条引用），
门禁的 `metadata_schema`、`delivery_binding`、`version_consistency` 因此持续失败。

这批引用无法逐条修复：旧对象已不在任何仓库中，凭 `instance_id` 与指纹也无法反推源码树。同时
`zram_zstd`、`run_cmd_demo`、`re_kernel*` 的记录里既有已消失的提交，也有仍然存在的提交，按条目
拆分会让"一次构建对应一棵源码树"这一不变量变得不可信。

因此按维护者决定采用归档口径：**整份旧记录原样移入本目录，`metadata/modules/` 下按当前源码
重建最小记录**（`status=candidate`，`builds`/`deliveries` 为空，版本取自当前 Makefile）。

## 迁移映射

| 旧记录 | 归档位置 | 归档前的构建条数 |
| --- | --- | --- |
| `metadata/modules/cgroupv2_freeze.json` | `cgroupv2_freeze.json` | 2 |
| `metadata/modules/dont_kill_freeze.json` | `dont_kill_freeze.json` | 2 |
| `metadata/modules/hosts_redirect.json` | `hosts_redirect.json` | 2 |
| `metadata/modules/nomount.json` | `nomount.json` | 78 |
| `metadata/modules/re_kernel.json` | `re_kernel.json` | 20 |
| `metadata/modules/re_kernel_static.json` | `re_kernel_static.json` | 8 |
| `metadata/modules/re_kernel_x.json` | `re_kernel_x.json` | 32 |
| `metadata/modules/run_cmd_demo.json` | `run_cmd_demo.json` | 10 |
| `metadata/modules/zram_zstd.json` | `zram_zstd.json` | 44 |

新记录通过 `history_archive` 字段指回本目录中的同名文件，便于追溯旧台账。

## 约束

- 本目录**不参与**门禁校验：`tools/check_repository.py` 只扫描 `metadata/modules/`，归档文件
  因此不再产生 `metadata_schema` / `delivery_binding` / `version_consistency` 失败。
- 归档只作为历史证据保留，**不得**据其重建产物或声明已验证：其中的提交、指纹、产物哈希都
  无法在当前历史上复核。
- 若日后重新拿到旧仓库对象（例如从备份或远端恢复），可将其恢复到 `metadata/modules/` 下并
  删除本目录中的对应文件，但需要同时复核 `instance_id` 与产物哈希。
