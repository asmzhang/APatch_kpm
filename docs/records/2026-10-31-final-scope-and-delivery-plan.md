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
