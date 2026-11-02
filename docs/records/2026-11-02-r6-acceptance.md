# R6 验收记录（2026-11-02）

验收对象：本轮五个新移植模块与两个既有模块的冻结构建（release 变体），共 7 个实例、7 份产物。
结论：**有条件通过**——身份与产物链完整、门禁全绿、真机覆盖到用例清单；PTE@5.10 因设备侧原因未执行（见 §5），
另有 4 项已批准风险随交付（见 §6）。

## 1. 验收对象

| 模块 | 版本 | 实例 | 产物 | SHA-256 |
| --- | --- | --- | --- | --- |
| apkey_hide | 3.6.3 | `apkey_hide-3.6.3+g51e26a79f02e.r557fddd0.kpb51197a.ndk26.1.10909125#1` | `apkey_hide_3.6.3.kpm` | `8a340d1b1a7dc9fee5a6e43bcf851a39549dae9c9ca09b4acdc71f12bc379ac0` |
| memcrc | 0.2.2 | `memcrc-0.2.2+gd9b507b9c087.rc9cb12f1.kpb51197a.ndk26.1.10909125#1` | `memcrc_0.2.2.kpm` | `1d64faf393109c9c0016041c5cf6d5341c8984f430db7fc067633781c7a41f0d` |
| mount_hidder | 1.0.2 | `mount_hidder-1.0.2+gb7adce8021f5.r130db140.kpb51197a.ndk26.1.10909125#1` | `mount_hidder_1.0.2.kpm` | `32735f5ed1a2bd02c791e037fb35295b33df3047e72a75b354466e6c9ecc4d98` |
| stealth_hook | 0.3.3 | `stealth_hook-0.3.3+g9df3b80e1098.r46914147.kpb51197a.ndk26.1.10909125#1` | `stealth_hook_0.3.3.kpm` | `2c7e1ecbd1bf8638bd970ee8369b8f022788b22b48eaa4ea803afbe67dd3e313` |
| panda_hide | 2.0.0 | `panda_hide-2.0.0+gb68c8e3ee23a.r19a3bffb.kpb51197a.ndk26.1.10909125#1` | `panda_hide_2.0.0.kpm` | `2f354020a2a3f3a97bb2e70d4ce2f1aa9db2073135416f2e903832f4171e8d0b` |
| qti_battery_charger | 1.0.2 | `qti_battery_charger-1.0.2+g29951ee67dca.rb1b7cc59.kpb51197a.ndk26.1.10909125#1` | `qti_battery_charger_1.0.2.kpm` | `9b8dfb9a240cddf4de804df721d009566cb2b8acda0c8c8ac7567f0b0916b8e3` |
| xperia_ii_battery_age | 1.1.2 | `xperia_ii_battery_age-1.1.2+gf129865e4ea2.r8661a2cf.kpb51197a.ndk26.1.10909125#1` | `xperia_ii_battery_age_1.1.2.kpm` | `f4346ed03c8d89d43202766c90860df557e59e1f6058f12473cdb89ee8ebf955` |

KernelPatch 子模块：`b51197aaba8f2272dd8a3e30c85698a29aa928c9`。工具链：`ndk26.1.10909125`。

## 2. 复算与核验

```bash
# 逐模块核验（candidate 档：源码 blob、参数、产物一致）
python3 tools/identity.py verify --module <module> --profile candidate
# 7 个模块全部「0 个问题」

# 严格门禁
python3 tools/check_repository.py --strict
# 14 项检查，0 项失败，2 项警告（存量：zram 清单缺 handoff 字段；历史实例未登记）
```

- `identity_hashes`：复算 60 份产物全部命中记录里的 SHA-256。
- `version_consistency`：`.kpm.info` 内嵌版本 / Makefile `MYKPM_VERSION` / 源码 `KPM_VERSION(MYKPM_VERSION)` /
  `metadata.version_declared` 四处一致，debug 变体带 `_d`。
- UND 符号：七个模块的未定义符号全部落在 KernelPatch `b51197a` 的 `KP_EXPORT_SYMBOL` 集合（含 `kfunc(x)` → `kf_x` 展开）。
- 门禁脚本在 Linux（WSL Ubuntu-24.04）下运行；Windows 下 `tool_selftest` 因 gbk 解码无法运行，属环境问题，不改判据。

## 3. 独立审计

| 轮次 | 范围 | 结论 |
| --- | --- | --- |
| R3（21 单） | 七模块全量 | 3 critical / 8 high 全部修复并复核关闭 |
| R4b–R4l | 复核仍开项 | critical 3/3、high 8/8 关闭；medium/low 除 2 条 info 全关 |
| R5 前（三路） | 身份 + stealth_hook 深度 + 六模块三态复核 | 身份 14/14 复算一致；AUD-B-1 已由 R4h 去重解决 |
| R4k（mount_hidder 1.0.1） | 代码 + 文档两路 | AUD-del-oob 等已修入 1.0.2 |

审计方为独立会话的 `my-wb/glm-5.3-flash`，禁调实现方脚本、须自解析 ELF；报告存 `Auditor/reports/`。

## 4. 真机覆盖

Pixel 6（Android 15 / 6.1.99-android14 / APatch 11282）为唯一可加载 KPM 的设备。

- apkey_hide：ctl0 六条命令（status/sweep/allow=/deny=/pkg=/unclaim）全部有正确输出；supercall 侧信道在加载后对非受信 uid 关闭。
- memcrc：五类规则（MAP/PATH/PTRACE/RANGE/REGS）真机差分通过；uid 门（非受信拒绝跨 uid 与全局规则）实测；`/dev/yuuki_misc` 权限 0600。
- mount_hidder：三钩子（mountinfo/mounts/mountstats）差分通过；`add:/apex` 83→0、`/data` 不误伤 `/data_mirror`、后缀 `/persist` 命中；ctl0 add/del/clear/status 全验。
- stealth_hook：HWBP 抓参/改参/跳过原函数/卸载复原；ghost 分配读写 + RANGE 隐藏；PTE 命令在 6.1 返回 -95；uid 门（HWBP/GHOST 仅 root）。
- panda_hide：11 项信号（TracerPid/State/wchan/stat/LIBFRIDA/27042/maps/打开 frida-agent/线程名）+ 2 项负对照。
- qti_battery_charger / xperia_ii_battery_age：**仅构建与身份核验，未上机**（本机无对应硬件路径，见 §5）。

## 5. 未覆盖项（不构成通过）

1. **PTE@5.10**：唯一 5.10 设备（小米 13）为 APatch LKM 模式，KPM 加载 `rc=-1`（dmesg 只有 `layout done`，init 无输出），
   因此 PTE 真实重定向从未在支持的内核上执行过。当前行为：6.1 上命令返回 `-95`（-EOPNOTSUPP），这是已实现并可复现的。
2. **qti_battery_charger / xperia_ii_battery_age 真机**：无对应机型/内核路径，只有构建、身份与静态审计证据。
3. **apkey_hide 嵌入模式**：未测（需管理器以嵌入方式加载）。
4. **stealth_hook PTE 全链路**：依赖用户态 DBI（`dbi_recompiler`）给 `offset_map`，本轮未移植。

## 6. 已批准风险

1. **AUD-f-1（stealth_hook）**：`rearm_to` 在调试异常上下文调用 perf API 的路径已被 R4g 的 MSR 直写替换，
   仅 `LISTEN_RET` 的返回拍 rearm 仍走原实现；触发条件为「调试内核 + 该拍」，真机自检未触发。
2. **AUD-C-a-1 / AUD-C-a-2（memcrc，info）**：`[END]` 段缺 `else goto buffer_full`、若干 `if` 缩进异常。
   记录不改：0.2.2 已冻结并经真机验证，改动会使产物身份失效。
3. **mount_hidder AUD-race-add（low）**：`add`/`clear` 无锁并发可丢规则；ctl0 由管理器串行调用，窗口极窄。
4. **stealth_hook AUD-B-2（medium）**：MSR 直写与 perf 内部 `info->address` 失同步；enable/disable 仍走 perf API。

## 7. 变体差异

- 七个模块的 debug 变体均与 release 独立构建（qti/xperia 自 R4d 起用独立对象目录 + `-DDEBUG`），
  差异为日志/断言详细度与版本串 `_d`，业务逻辑相同；本轮只对 release 上机，符合「每个目标只测一个变体」的判据。
- 早期（R4d 之前）qti/xperia 的 debug 与 release 逐字节相同，该事实已记录在 R4d 执行记录里。

## 8. 交付状态

- 本记录只完成**验收**（`acceptances[]` 已按实例追加）。
- **未做交付绑定**：本机无可用签名密钥（`git verify-commit` / `git verify-tag` 无法复验），
  按 `docs/process/05-maintainer-acceptance.md`「信任配置不可用时保留记录并停止交付」停止在验收环节。
  恢复条件：配置可复验的签名密钥后补签并 `bind-delivery`。
