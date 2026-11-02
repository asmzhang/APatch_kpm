# 小米 13（APatch LKM 模式）KPM 加载失败定位（2026-11-02）

## 1. 现象

小米 13（设备指纹 `sha256:cdc3e9167b02`，序列号不入库；Android 13 / `5.10.149-android12-9` / APatch 11282 / `kernelpatch-lkm`）
加载本轮产物一律 `load rc=-1`，dmesg 只到「模块搬进可执行内存」就停：

```text
kernelpatch-lkm: KPM [mount_hidder] layout done size=0x3801
kernelpatch-lkm: kpm alloc module size: 3801
kernelpatch-lkm: KPM [mount_hidder] moved to ffffffde8f64a000 init=ffffffde8f64c000
kernelpatch-lkm: unknown symbol: kf_strnstr
```

`unknown symbol: X` → `simplify_symbols()` 返回 `-ENOENT`（`KernelPatch/lkm/kpm/module.c:394-400`），
加载在 `apply_relocations()` 之前就中止，所以 `init` 从未被调用，模块侧没有任何日志。

## 2. 根因：LKM 模式的 KPM 符号面比内核集成模式窄

内核集成模式下 KPM 通过 `kallsyms_lookup_name()` 找 KP 导出的 `kf_*` 与偏移表变量；LKM 模式下这份
符号由两条来源拼成，实测合计：

| 来源 | 数量 | 说明 |
| --- | --- | --- |
| LKM 自身模块符号（`/proc/kallsyms` 内 `kf_*`） | 22 | `kf_kasprintf` `kf_kmemdup` `kf_kstrdup` `kf_memchr` `kf_memcmp` `kf_memcpy` `kf_memmove` `kf_memset` `kf_snprintf` `kf_sprintf` `kf_strcat` `kf_strchr` `kf_strcmp` `kf_strcpy` `kf_strlen` `kf_strncat` `kf_strncmp` `kf_strncpy` `kf_strnlen` `kf_strrchr` `kf_strstr` `kf_vsnprintf` |
| `KernelPatch/lkm/kpm/symbols.c` 兼容表 | 40 | `compat_copy_to_user` `current_uid` `printk` `kallsyms_lookup_name` `hook_wrap` `hook_syscalln` `task_struct_offset` `pgtable_entry` 等 |

内核集成模式额外提供、LKM 模式**没有**的四类符号：

| 缺失符号 | 使用方 | 性质 |
| --- | --- | --- |
| `kf_strnstr` `kf_strpbrk` `kf_strsep` `kf_strspn` | mount_hidder / memcrc | 纯字符串工具，模块侧可自实现 |
| `kallsyms_lookup_name_by_suffix` | re_kernel / zram_zstd / hosts_redirect / cgroupv2_freeze / nomount / dont_kill_freeze / run_cmd_demo | KP 的符号解析辅助，LKM 只导出 `kallsyms_lookup_name` |
| `cred_offset` | memcrc / stealth_hook / nomount / dont_kill_freeze / re_kernel / run_cmd_demo | `struct cred_offset` 偏移表 |
| `inline_wrap_syscalln` `inline_unwrap_syscalln` `fp_wrap_syscalln` `fp_unwrap_syscalln` | stealth_hook / memcrc / panda_hide | syscall 内联钩子原语；LKM 只提供 `hook_syscalln` |

## 3. 全量可加载性（按产物内 UND 符号实测）

判定方法：`llvm-readelf -sW` 取 `.symtab` 的 `UND` 符号，与「设备 kallsyms ∪ 兼容表」求差集。
脚本：`local/mi13_sweep.py`（`local/mi13_kallsyms.txt` 为设备导出）。

**LKM 模式可加载（0 缺失）：**

| 产物 | UND |
| --- | --- |
| `lmkd_dont_kill_1.0.0.kpm` | 13 |
| `qti_battery_charger_1.0.2.kpm` | 6 |
| `xperia_ii_battery_age_1.1.2.kpm` | 6 |

**不可加载（18 个）：**

| 产物 | 缺失数 | 缺失符号 |
| --- | --- | --- |
| `apkey_hide_3.6.3.kpm` | 1 | `is_trusted_manager_uid_android` |
| `mount_hidder_1.0.2.kpm` | 1 | `kf_strnstr` |
| `cgroupv2_freeze_1.0.12.kpm` | 1 | `kallsyms_lookup_name_by_suffix` |
| `hosts_redirect_2.0.0.kpm` | 1 | `kallsyms_lookup_name_by_suffix` |
| `zram_zstd_1.0.0(.baselines).kpm` | 1 | `kallsyms_lookup_name_by_suffix` |
| `re_kernel_11.7-20261010_baselines.kpm` | 1 | `kallsyms_lookup_name_by_suffix` |
| `re_kernel_x_1.6-20261010_baselines.kpm` | 1 | `kallsyms_lookup_name_by_suffix` |
| `panda_hide_2.0.0.kpm` | 2 | `fp_unwrap_syscalln` `fp_wrap_syscalln` |
| `NoMount_2.1.1-20261014(.baselines).kpm` | 2 | `cred_offset` `kallsyms_lookup_name_by_suffix` |
| `dont_kill_freeze_1.0.2.kpm` | 2 | `cred_offset` `kallsyms_lookup_name_by_suffix` |
| `re_kernel_11.7-20261010.kpm` | 2 | `cred_offset` `kallsyms_lookup_name_by_suffix` |
| `re_kernel_x_1.6-20261010.kpm` | 2 | `cred_offset` `kallsyms_lookup_name_by_suffix` |
| `run_cmd_demo_1.3.0(.baselines).kpm` | 2 | `cred_offset` `kallsyms_lookup_name_by_suffix` |
| `stealth_hook_0.3.3.kpm` | 3 | `cred_offset` `inline_unwrap_syscalln` `inline_wrap_syscalln` |
| `memcrc_0.2.2.kpm` | 9 | `cred_offset` `fp_*syscalln` `inline_*syscalln` `kf_strnstr` `kf_strpbrk` `kf_strsep` `kf_strspn` |

**后续进展（2026-11-02）**：`mount_hidder` 1.0.3 与 `panda_hide` 2.0.1 已按「去掉形态专属符号」
改成两形态通用，同一份字节在小米 13 上实测 `load rc=0` 并生效（`kf_strnstr` → 本地 `hm_strnstr`；
`fp_*syscalln` → `hook_wrap` 到 `__arm64_sys_xxx`）。上表是 1.0.2 / 2.0.0 的实测结果，保留作对照，
判据与做法见 [process/09-load-forms.md](../process/09-load-forms.md)。

## 4. 结论1. 小米 13 的 `rc=-1` **不是偏移推导问题、也不是产物损坏**，是 LKM 模式的 KPM 符号面窄于内核集成模式；
   加载在符号解析阶段即失败，任何 `init` 侧逻辑（含 PTE 能力探测）都没有机会执行。
2. 这解释了为什么 PTE@5.10 至今无法真机取证：唯一的 5.10 设备跑的是 LKM 模式，而 stealth_hook 依赖
   `inline_wrap_syscalln` 与 `cred_offset`，在该模式下不可加载。
3. 三个无外部符号依赖的模块（`lmkd_dont_kill` / `qti_battery_charger` / `xperia_ii_battery_age`）
   在 LKM 模式应当可加载——本轮未上机验证（会动 lmkd/充电路径），列为待验。

## 5. 三条可选路径

| 路径 | 代价 | 效果 |
| --- | --- | --- |
| A. 模块侧去依赖 | 小（`mount_hidder` 只需自实现 `strnstr`；`apkey_hide` 换 hook 目标） | 让单个模块能在 LKM 加载；syscall 钩子类模块无解 |
| B. 扩 LKM 兼容层 | 大（改 `KernelPatch/lkm/kpm/symbols.c` 补 `inline_*`/`fp_*`/`cred_offset`/`kallsyms_lookup_name_by_suffix` 的 shim，重建并替换设备上的 LKM） | 一次性打通全部模块，但属于 KernelPatch 侧改动 + 需重装 LKM |
| C. 记录并接受 | 零 | 承认 LKM 模式 KPM 生态受限；这些模块只在内核集成模式交付 |

建议：C 作为本轮结论落档；若确需在小米 13 上使用某个具体模块，再按 A 单点处理；B 只在需要成批支持时立项。
