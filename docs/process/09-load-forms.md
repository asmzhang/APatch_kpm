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

## 多个模块挂同一函数：只准用链式钩子，`local` 槽位要分工

两条 `show_*` / `seq_file` 函数经常被不同模块同时看上（例如 `show_vfsmnt` 既被
`mount_hidder` 用来藏挂载点，也被 `memcrc` 用来裁行）。两条硬约束：

1. **不要用 `hook()` 做整函数替换。** `hook()` 要在原始函数头几条指令上重定位
   （`KernelPatch/kernel/base/hook.c:549-589`），如果那里已经是别人 `hook_wrap2` 打下的跳转，
   重定位失败直接返回 `-HOOK_BAD_RELO`（-4092）。实测：5.10 LKM 上先加载 `mount_hidder` 再加载
   用 `hook()` 的 `memcrc` 0.2.3，`feature_init()` 第一个钩子就报 -4092。
   改用 `hook_wrap()` / `hook_wrap2()` 后按 `hook_chain` 追加，多模块共存。
2. **`fargs->local` 是整条链共享的**（`_transit4()` 里同一个 `hook_fargs4_t` 传给所有 before/after），
   所以同一个槽位只能有一个模块用。当前分配：

| 槽位 | 归属 | 用途 |
| --- | --- | --- |
| `data0` | `mount_hidder` | `m->count + 1`，0 表示无效 |
| `data7` | `memcrc` | `m->count + 1`，0 表示无效 |
| `data1`..`data6` | 空闲 | 新模块从这里取，并更新本表 |

`local` 在 transit 里是栈上未初始化内存，before 回调必须**先写再读**（不能只依赖 after 里判断非零）。
## 声明与检查

- build 记录可写 `load_forms` 字段（`["kernel"]` / `["lkm"]` / `["kernel","lkm"]`）；不写按 `["kernel"]` 看待。
- `python tools/load_forms.py --module <名> --strict` 按声明判定：声明了某形态却装不上即判定失败。
- 加 `--kallsyms <导出表>` 可把「内核符号」也核到具体目标内核；不加则只判自带表，
  落在表外的符号记为「无法判定」。
- 交接前（Developer）与验收前（维护者）各跑一次；`tools/check_repository.py` 的
  `check_load_forms` 对声明过形态的构建做同一判定。
- **同名影子符号不算缺**：KP 内核形态自己实现了一小批与内核同名的函数并把同名符号导出
  （判据：同一文件里既 `KP_EXPORT_SYMBOL(X)` 又 `kfunc_call(X)`，目前只有
  `get_random_u64`，见 `KernelPatch/kernel/patch/common/utils.c:163-169`）。这些名字在 lkm
  形态没有表项，但装载器会继续用 `kallsyms_lookup_name()` 拿到真实内核符号，所以工具把它
  记为「无法判定」而不是「必然缺」；不加 `--kallsyms` 时才能看出这条差别，加了表就按表判。

## 三条可选路线

| 路线 | 做法 | 代价 | 现状 |
| --- | --- | --- | --- |
| A | 模块侧去掉对形态专属符号的依赖 | 改模块源码，一次改动长期有效 | **已选**，mount_hidder 1.0.3 走完 |
| B | 扩 `kernelpatch.ko` 兼容层（补齐 `kf_strnstr` 等） | 要按目标内核源码重编 `.ko` 并重装 | 不做，成本与风险都不划算 |
| C | 记录并接受「只有 kernel 形态可用」 | 零改动，但 LKM 模式设备用不上 | 目前没有模块走这条（mount_hidder 1.0.3、panda_hide 2.0.1、apkey_hide 3.6.4、stealth_hook 0.3.4、memcrc 0.2.5 都已改走 A） |

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
- `apkey_hide` 3.6.4（去掉 `is_trusted_manager_uid_android` 的 extern，改为运行时解析）：
  - 小米 13：`load rc=0`，`ctl0 status` 输出完整（3.6.3 加载失败，更早的 3.6.2 在 LKM 上还会
    被 `_copy_to_user` 截成 8 字节）；Pixel 6：`load rc=0`；
  - 代价：该符号在内核 kallsyms 表里也不存在，两形态 `sig=0`，管理器识别退回包名匹配。
    这类「只能动态解析、解析不到就降级」的符号，是路线 A 的常见形态。
- `stealth_hook` 0.3.4（去掉 `cred_offset` + `inline_wrap/unwrap_syscalln`）：
  - `sh_current_uid()` 改用 KP 导出的 `current_uid()`；syscall 桥改
    `syscalln_addr()` + `hook_wrap()`，与 KP 的 `inline_wrap_syscalln` 同构；
  - 小米 13 与 Pixel 6 都 `load rc=0`，`ctl0 status` 与 `stealth_cli selftest` 全通过。
- `memcrc` 0.2.4 / 0.2.5（去掉 9 个形态专属符号 + procfs 钩子改链式 + 卸载幂等）：
  - Pixel 6：`load rc=0`，`PTRACE:2000` 让 uid 2000 进程的 `State:` 变 `R (running)`，
    `RANGE:2000:…` 让目标进程 `maps` 141 → 140、`[stack]` 1 → 0；
  - 同一台机器上再加载 `mount_hidder` 1.0.3 + `add:/apex`，`/proc/mounts` 83 → 0，
    且 memcrc 的规则仍然生效 —— 两个模块挂在同一批 `show_*` 上互不影响；
  - 小米 13（0.2.5，设备重启后）：`load rc=0`、五个子系统 installed、`PTRACE:` / `RANGE:` 生效、
    与 `mount_hidder` 1.0.3 共存、`unload` 后再 `load` 仍 `rc=0`。0.2.3 时期那次
    `misc_register()` 卡死是 memcrc 自身卸载不幂等导致的（`misc_deregister()` 二次调用在
    `__list_del_entry_valid()` 里 BUG，带着 `misc_mtx` 死掉），0.2.5 已修，见
    [2026-11-02-mi13-memcrc-hang.md](../records/2026-11-02-mi13-memcrc-hang.md)。
