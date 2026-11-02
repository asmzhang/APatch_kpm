# 09 加载形态：kernel 与 lkm

一句话：**形态是装载环境，不是模块分类。**同一个 `.kpm` 可以同时属于两种形态，源码只有一份；
能否装载只取决于产物引用的未定义符号是否都落在该形态的符号面内。

## 两种环境

| 形态 | 装载环境 | 装载器 | 自带符号表来源 | 规模 |
| --- | --- | --- | --- | --- |
| `kernel` | KernelPatch 编进内核（APatch 内核模式） | `KernelPatch/kernel/patch/` | `kernel/**` 里的 `KP_EXPORT_SYMBOL(...)` | 173 个名字 |
| `lkm` | 原厂内核 + `kernelpatch.ko`（APatch LKM 模式） | `KernelPatch/lkm/kpm/module.c` | `lkm/kpm/symbols.c` 兼容表 40 项 + 它代理的 22 个 `kf_*` 槽位 | 62 个名字 |

两形态都提供的名字构成**可移植子集**：`python tools/load_forms.py --subset` 输出 60 个。

## 解析顺序决定失败形态

`KernelPatch/lkm/kpm/module.c:389-400`（kernel 形态同一套逻辑）：

1. 先查本形态自带表（`kp_kpm_symbol_lookup`）；
2. 再 `kallsyms_lookup_name(name)` 兜底查目标内核符号；
3. 两者都没有 → 打印 `unknown symbol: <名字>`、返回 `-ENOENT`，**在 `apply_relocations()` 之前中止**。

后果是模块 `init` 永远不执行。dmesg 的形状很好认——只有装载器的日志，没有模块自己的任何输出：

```
KPM [mount_hidder] layout done size=0x3801
kpm alloc module size: 3801
moved to ffffffde8f64a000 init=ffffffde8f64c000
unknown symbol: kf_strnstr
```

## 两类符号，两种命运

1. **KernelPatch 自造符号**：`kf_*` 槽位、`cred_offset`、`inline_wrap_syscalln`、
   `fp_wrap_syscalln`、`is_trusted_manager_uid_android`、`kallsyms_lookup_name_by_suffix` 之类。
   它们由某一形态的 KernelPatch 自己创建，kallsyms 兜不住。**某形态没有就是硬失败。**
2. **内核符号**：`printk`、`kfree`、`get_random_u64` 之类。靠 kallsyms 兜底，目标内核有就能解析。

所以判据不是「只能用那 60 个名字」，而是「**别依赖另一形态没有的 KernelPatch 自造符号**」。

## 写模块时的默认选择

| 用途 | 用这些（两形态都有） | 别用（只有 kernel 形态有） |
| --- | --- | --- |
| 挂钩 | `hook` / `hook_wrap` / `hook_syscalln` / `unhook` / `hook_unwrap_remove`（阻断型见下节） | `inline_wrap_syscalln` / `fp_wrap_syscalln` / `hotpatch` |
| 符号查找 | `kallsyms_lookup_name` / `kallsyms_on_each_symbol` | `kallsyms_lookup_name_by_suffix` / `symbol_lookup_name` |
| 字符串内存 | `kf_strlen` / `kf_memcmp` / `kf_memcpy` / `kf_memset` / `kf_strncmp` 等 22 个 | `kf_strnstr` / `kf_strpbrk` / `kf_strsep` / `kf_strspn` |
| 结构体偏移 | `task_struct_offset` / `mm_struct_offset` / `pgtable_entry` | `cred_offset` |

缺什么就本地实现一份，别等兼容层。示例：`modules/KPM/mount_hidder/hide_mount.c` 的
`hm_strnstr()` 用 `kf_memcmp` 自己找子串，替掉 `linux/string.h` 里展开成 `kf_strnstr` 的 `strnstr`。

## 阻断型 syscall 钩子：`hook_syscalln` 在 kernel 形态不认 `skip_origin`

`hook_syscalln` 两形态都有，但 `KernelPatch/kernel/patch/common/syscall.c:664-671` 是按环境分派的：

```c
if (syscall_hook_global) return syscall_hook_add(nr, 0, before, after, udata, 0, 0);
if (sys_call_table) return fp_wrap_syscalln(nr, narg, 0, before, after, udata);
return inline_wrap_syscalln(nr, narg, 0, before, after, udata);
```

第一分支注册时 `allow_skip=0`。一旦全局 dispatcher 生效（`el0_svc_common`），回调照跑，
但回调里设的 `args->skip_origin` / `args->ret` **不被采纳**——「假装文件不存在」就退化成
「打一条 BLOCKED 日志，文件照样能打开」。认 skip 的只有 `hook_syscalln_override`
（`allow_skip=1`，且要求 handler 粒度 `invoke_syscall`），而它只在 kernel 形态导出。

两形态都成立的写法：自己解析 `__arm64_sys_xxx` 地址再 `hook_wrap`，参数个数按
`has_syscall_wrapper ? 1 : narg` 记账（arm64 GKI 的 syscall 函数只收一个 `struct pt_regs *`）。
参考实现 `modules/KPM/panda_hide/src/syscall_portable.c`：

- 名字候选顺序与 KP 的 `syscalln_name_addr()` 一致：前缀 `__arm64_` → 空，后缀 `.cfi_jt` → `.cfi` → 空；
- 卸载用 `hook_unwrap(addr, before, after)`；
- 代价是每个 syscall 一个 inline hook（有代码补丁指纹），全局 dispatcher 没有这个指纹。
  所以只在**必须阻断**时这么做，纯观察型钩子继续用 `hook_syscalln`。

## 声明与检查

- build 记录可写 `load_forms` 字段（`["kernel"]` / `["lkm"]` / `["kernel","lkm"]`）；不写按 `["kernel"]` 看待。
- `python tools/load_forms.py --module <名> --strict` 按声明判定：声明了某形态却装不上即判定失败。
- 加 `--kallsyms <导出表>` 可把「内核符号」也核到具体目标内核；不加则只判自带表，
  落在表外的符号记为「无法判定」。
- 交接前（Developer）与验收前（维护者）各跑一次；`tools/check_repository.py` 的
  `check_load_forms` 对声明过形态的构建做同一判定。

## 三条可选路线

| 路线 | 做法 | 代价 | 现状 |
| --- | --- | --- | --- |
| A | 模块侧去掉对形态专属符号的依赖 | 改模块源码，一次改动长期有效 | **已选**，mount_hidder 1.0.3 走完 |
| B | 扩 `kernelpatch.ko` 兼容层（补齐 `kf_strnstr` 等） | 要按目标内核源码重编 `.ko` 并重装 | 不做，成本与风险都不划算 |
| C | 记录并接受「只有 kernel 形态可用」 | 零改动，但 LKM 模式设备用不上 | stealth_hook / memcrc 暂按此处理（panda_hide 2.0.1 已改走 A） |

## 为什么不再按形态放源码

`modules/LKM/README.md` 原来写的是「等某个模块做出 LKM 适配版本后，把那一份源码放进
`modules/LKM/<模块>/`」。这条路线的问题是：同一份代码两份拷贝 → 漂移、双份审计、
身份翻倍（每次改动两条 instance、两次冻结），而真实差异往往只是一个符号。

1.0.3 的实测证明了更省的做法：把那个符号去掉，同一个产物就同时属于两种形态。
`modules/LKM/` 因此保留为「形态专属产物」的位置（例如将来真有只能编成 `.ko` 的东西），
**不存放同一模块的第二份源码**。

## 实测（2026-11-02）

- 小米 13（`kernelpatch-lkm`，5.10.149-android12-9，设备指纹 `sha256:cdc3e9167b02`）：
  21 份产物里 3 份能装（`qti_battery_charger` / `xperia_ii_battery_age` / `lmkd_dont_kill`），
  18 份报 `unknown symbol`。
- `tools/load_forms.py` 用同一批产物离线复算，缺失符号集合与 dmesg **逐条一致**
  （memcrc 9、stealth_hook 3、panda_hide 2、mount_hidder 1、apkey_hide 1）。
- `mount_hidder` 1.0.3（去掉 `kf_strnstr`）：
  - 小米 13：`load rc=0`，`init paths=3 hooks=3`，`/proc/mounts` 里 ` /apex` 命中 56 → 0 → 56；
  - Pixel 6（kernel 形态）：83 → 0 → 83。
  同一个 `.kpm` 字节在两种装载器下都能跑起来。
- `panda_hide` 2.0.1（去掉 `fp_wrap_syscalln` / `fp_unwrap_syscalln`，改为 `hook_wrap` 到
  `__arm64_sys_openat` / `__arm64_sys_faccessat` / `__arm64_sys_connect`）：
  - 小米 13：`load rc=0`，`cat` 探测文件返回 ENOENT，卸载后恢复；
  - Pixel 6：同样成立。此前一版用 `hook_syscalln` 时，`openat BLOCKED` 日志有、文件仍能打开——
    就是上面那节说的 `allow_skip=0`。
