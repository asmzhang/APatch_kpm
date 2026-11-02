# R7 验收记录（2026-11-02）

验收对象：七个可构建模块的当前冻结构建（release 变体），共 7 个实例、7 份产物。
本轮与 R6 的差别集中在两处：① 五个模块升版去掉「装载形态专属符号」，同一份产物在
kernel 形态（APatch 内核集成）与 lkm 形态（原厂内核 + `kernelpatch.ko`）下都能加载；
② 因此真机覆盖从「只有 Pixel 6」扩到「Pixel 6 + 小米 13」两台。

结论：**通过**——身份与产物链完整、门禁全绿、七个实例在两台设备（或明确记录的原因下）覆盖到用例；
未覆盖项与已批准风险见 §5、§6。

## 1. 验收对象

| 模块 | 版本 | 实例 | 产物 SHA-256 | 字节 |
| --- | --- | --- | --- | --- |
| mount_hidder | 1.0.3 | `mount_hidder-1.0.3+gc417ca5a5cb3.r40e04ffe.kpb51197a.ndk26.1.10909125#1` | `30e4b2181b6c2c99a992aff2e48710d1e9d76b4ebb0b6df2023904e9c698a579` | 10976 |
| panda_hide | 2.0.1 | `panda_hide-2.0.1+gba54f2717ba5.r8a9f0a97.kpb51197a.ndk26.1.10909125#1` | `f4f82fb103c62c4f45102f14f17a208fb534e9a8f7a4b12c26a1fa0dade30e6b` | 26936 |
| apkey_hide | 3.6.4 | `apkey_hide-3.6.4+gf70b85de3409.r299d55c4.kpb51197a.ndk26.1.10909125#1` | `9a39ec5294ca25fdb3173dc4ff42b390f3a4aadcfeb27b9cac9e4fbc37840b9e` | 22752 |
| stealth_hook | 0.3.4 | `stealth_hook-0.3.4+ga078f964bb00.r963abeef.kpb51197a.ndk26.1.10909125#1` | `490009c86a16cdaa1c8fb23a8b5910a70f4ece0321222aa04964377d64e4846b` | 34128 |
| memcrc | 0.2.5 | `memcrc-0.2.5+gcb6d49690719.r8a3f4b71.kpb51197a.ndk26.1.10909125#1` | `0ffdf5f79307a5cdbe2ca26997c41f5dd48b715c72be77f9b34c18d16804b84d` | 96320 |
| qti_battery_charger | 1.0.2 | `qti_battery_charger-1.0.2+g29951ee67dca.rb1b7cc59.kpb51197a.ndk26.1.10909125#1` | `9b8dfb9a240cddf4de804df721d009566cb2b8acda0c8c8ac7567f0b0916b8e3` | 6072 |
| xperia_ii_battery_age | 1.1.2 | `xperia_ii_battery_age-1.1.2+gf129865e4ea2.r8661a2cf.kpb51197a.ndk26.1.10909125#1` | `f4346ed03c8d89d43202766c90860df557e59e1f6058f12473cdb89ee8ebf955` | 6296 |

KernelPatch 子模块：`b51197aaba8f2272dd8a3e30c85698a29aa928c9`。工具链：`ndk26.1.10909125`。

## 2. 复算与核验

```bash
# 逐模块核验（candidate 档：源码 blob、参数、产物一致）
python3 tools/identity.py verify --module <module> --profile candidate
# mount_hidder 10 份 / panda_hide 8 / apkey_hide 6 / stealth_hook 24 / memcrc 14 / qti 6 / xperia 6
# 共 74 份产物，7 个模块全部「0 处问题」

# 严格门禁
python3 tools/check_repository.py --strict
# 15 项检查，0 项失败，2 项警告（存量：zram 清单缺 handoff 字段；历史实例未登记）
```

- `identity_hashes`：复算 74 份产物全部命中记录里的 SHA-256。
- `load_forms`：12 份声明了 `load_forms` 的产物与其声明的形态一致；七个模块的当前版本
  `kernel缺=0`、`lkm缺=0`（`tools/load_forms.py --module <m> --strict`）。
- `version_consistency`：`.kpm.info` 内嵌版本 / Makefile `MYKPM_VERSION` / `metadata.version_declared`
  四处一致；debug 变体带 `_d`。本轮顺手把 stealth_hook 的 `SH_VERSION_CODE` 手写宏删掉，
  status 直接从 `MYKPM_VERSION` 解析三元组，消除版本号双份来源。
- 门禁脚本在 Linux（WSL Ubuntu-24.04）下运行；Windows 下 `tool_selftest` 因 gbk 解码无法运行，
  属环境问题，不改判据。

## 3. 从 R6 到 R7 的变化

| 变化 | 内容 | 记录 |
| --- | --- | --- |
| 装载形态重新设计 | `modules/LKM/` 不再存放同一模块的第二份源码；形态=环境，一份源码一个产物 | `docs/process/09-load-forms.md` |
| 新判据工具 | `tools/load_forms.py`（离线推导两形态符号面、按 ELF 的 UND 判缺失）、门禁新增 `load_forms`、selftest 新增 `load_forms_classify` 正反夹具 | `tools/`、`docs/process/09-load-forms.md` |
| 去形态专属符号 | mount_hidder 1.0.3（`kf_strnstr`）、panda_hide 2.0.1（`fp_*syscalln`）、apkey_hide 3.6.4（`is_trusted_manager_uid_android`）、stealth_hook 0.3.4（`cred_offset` + `inline_*syscalln`）、memcrc 0.2.3（9 个） | 各模块报告 |
| 钩子机制纠正 | panda_hide 改 `syscalln_addr()` + `hook_wrap()`（`hook_syscalln` 在 kernel 形态不认 `skip_origin`，阻断会静默失效）；memcrc 五个 procfs 钩子改链式（`hook()` 整函数替换在已有链式钩子的函数上返回 `-HOOK_BAD_RELO`） | `docs/process/09-load-forms.md` |
| 卸载幂等 | memcrc 0.2.5 修二次 `misc_deregister()` 的 BUG（连带 `misc_mtx` 死锁，使后续 `misc_register()` 永久阻塞） | `docs/records/2026-11-02-mi13-memcrc-hang.md` |

## 4. 真机覆盖

| 模块 | Pixel 6 / 6.1.99-android14 / kernel 形态 / uid 10273 | 小米 13 / 5.10.149-android12-9 / lkm 形态 / uid 10270 |
| --- | --- | --- |
| mount_hidder 1.0.3 | `load rc=0`；`add:/apex` 83→0，`clear` 回 83 | `load rc=0`；`add:/apex` 56→0，`clear` 回 56 |
| panda_hide 2.0.1 | `load rc=0`；openat/faccessat 探测 ENOENT；connect 27042 被拦、443 不拦；卸载后恢复 | `load rc=0`；探测 ENOENT；卸载后恢复 |
| apkey_hide 3.6.4 | `load rc=0`；`ctl0` status/sweep/allow=/deny=/pkg=/unclaim 全部有输出 | `load rc=0`；`ctl0 status` 输出完整（3.6.3 及更早在此设备装不上） |
| stealth_hook 0.3.4 | `load rc=0`；`ctl0 status` = `v0.3.4 nr=427`；`stealth_cli selftest` 全通过 | 同左（`v0.3.4`、selftest 全通过） |
| memcrc 0.2.5 | `load rc=0`；`PTRACE:` S→R；`RANGE:` 139→138、`[stack]` 1→0；与 mount_hidder 共存 | `load rc=0`；`PTRACE:` S→R；`RANGE:` 131→130；与 mount_hidder 共存；`unload`→`load` 循环干净 |
| qti_battery_charger 1.0.2 | 未上机（无对应硬件路径） | 未上机 |
| xperia_ii_battery_age 1.1.2 | 未上机（无对应硬件路径） | 未上机 |

设备一律以指纹出现：Pixel 6 `sha256:433ab88276ec`，小米 13 `sha256:cdc3e9167b02`。

## 5. 未覆盖项（不构成通过）

1. **stealth_hook PTE@5.10**：0.3.4 起该模块已能在小米 13（5.10 / LKM）加载，但 PTE 真实重定向
   用例本轮未在 5.10 上执行（`pte query` 只在 6.1 上验过，返回 `-95`）。也就是说「能不能做」的设备侧
   前提已经具备，缺的是用例本身。
2. **stealth_hook PTE 全链路**：依赖用户态 DBI（`dbi_recompiler`）给 `offset_map`，本轮未移植。
3. **qti_battery_charger / xperia_ii_battery_age 真机**：无对应机型/内核路径，只有构建、身份与静态审计证据。
4. **apkey_hide 嵌入模式**：未测（需管理器以嵌入方式加载）。
5. **memcrc 主动失败路径**：重复加载会在装载器层以 `rc=-1` 拒掉、进不到 `init`，因此
   「`mod_init()` 失败 → 装载器再调 `mod_exit()`」这条路径没有在真机上构造出来；0.2.5 的幂等性
   目前由代码审查（`komm_registered` / `getcwd_hooked` 两个守卫）+ 正常卸载路径实测支撑。

## 6. 已批准风险

1. **AUD-f-1（stealth_hook）**：`LISTEN_RET` 的返回拍 rearm 仍走 perf API；触发条件为「调试内核 + 该拍」，
   真机自检未触发（其余路径已被 R4g 的 MSR 直写替换）。
2. **AUD-C-a-1 / AUD-C-a-2（memcrc，info）**：`[END]` 段缺 `else goto buffer_full`、若干 `if` 缩进异常。
   记录不改：相关版本已冻结并经真机验证，改动会使产物身份失效。
3. **mount_hidder AUD-race-add（low）**：`add`/`clear` 无锁并发可丢规则；ctl0 由管理器串行调用，窗口极窄。
4. **stealth_hook AUD-B-2（medium）**：MSR 直写与 perf 内部 `info->address` 失同步；enable/disable 仍走 perf API。
5. **memcrc 卸载幂等的失败路径（新增）**：见 §5 第 5 条，只有代码审查与正常路径实测。
6. **`load_forms` 影子符号判据（新增）**：`get_random_u64` 这类「KP 自己实现同名函数并导出」的符号，
   判据靠「同一文件里既有 `KP_EXPORT_SYMBOL(X)` 又有 `kfunc_call(X)`」的启发式识别，目前只命中一个；
   若将来出现更复杂形态，需要扩充该判据（selftest 已有正反夹具，改动会立刻暴露）。

## 7. 变体差异

- 七个模块的 debug 变体均与 release 独立构建（qti/xperia 自 R4d 起用独立对象目录 + `-DDEBUG`），
  差异为日志/断言详细度与版本串 `_d`，业务逻辑相同；本轮只对 release 上机，符合
  [tester-handbook](../process/tester-handbook.md) §4.1「每个目标只测一个变体」的判据。
- 本轮新增的 `load_forms` 声明对 release 与 debug 两个变体分别记录，`--strict` 按各自声明判定。

## 8. 交付状态

- 本记录完成**验收**（`acceptances[]` 已按实例追加，report 指向本文件）。
- **交付形态：自用，不做签名交付**（沿用 R6 §8 的结论与理由：产物只供维护者本人在自有设备上加载，
  签名链没有第三方验证需求）。
- **恢复触发条件**（任一成立即须补签并 `bind-delivery`）：① 产物对外分发（Releases、群共享、他人刷机）；
  ② 仓库出现第二位维护者或外部贡献者可直接改 `metadata/`。触发后需配置可复验的签名密钥，
  补签交付提交并重跑 `check_repository.py --strict` 与 `identity.py verify --profile delivery`。
