# 小米 13（LKM 形态）加载 memcrc 0.2.3 的两次失败与一次挂死

- 时间：2026-11-02
- 设备指纹：`sha256:cdc3e9167b02`（Android 13 / `5.10.149-android12-9` / APatch 11282 / `kernelpatch-lkm` / 管理器 uid 10270）
- 对象：`memcrc_0.2.3.kpm`（`sha256=193207bb…5e4822`），可移植化之后的第一版
- 结论：**模块自身逻辑没有跑起来**，两次失败分别停在内核补丁的 `hook()` 与 `misc_register()`；设备需重启清掉 D 状态进程。已按 `docs/process/07-escalation-device.md` 停止一切重试。

## 1. 第一次：`-HOOK_BAD_RELO`

`kpmctl 10270 load /data/local/tmp/memcrc_0.2.3.kpm` 的完整 dmesg：

```
KPM [memcrc] symbols resolved
KPM [memcrc] relocations applied
KPM [memcrc] icache flushed
KPM [memcrc] entering init
[yuuki] Initializing...
[yuuki] komm loaded successfully
[yuuki] hook failed at 0 with error -4092
[yuuki] hook installation failed
[yuuki] feature_init failed
[yuuki] komm unloaded
KPM [memcrc] init returned -5
KPM [memcrc] mod_exit, uninstalled hook.
```

`-4092` = `-HOOK_BAD_RELO`（`KernelPatch/kernel/include/hook.h:20`）。索引 0 是 `feature/feature.c` 里 `hook_all()` 的第一个钩子 `ori_show_vfsmnt`，走的是 KP 的整函数替换：

```c
hook_err_t hook(void *func, void *replace, void **backup);      /* KernelPatch/kernel/base/hook.c:612 */
```

`hook()` 会读原始函数头几条指令做重定位（`hook_prepare()` → `relocate_inst()`，`hook.c:549-589`）。当时同一台设备上 **`mount_hidder` 已经用 `hook_wrap2` 把 `show_vfsmnt` 打成了跳转到自己的链式桩**，`hook()` 在这条跳转上重定位失败，于是返回 `-HOOK_BAD_RELO` —— 也就是说整函数替换无法与已存在的链式钩子共存。

同一份产物在 Pixel 6（6.1）上两模块共存没有报错，所以这条限制与内核版本有关，不能只在 6.1 上验。

## 2. 第二次：`misc_register()` 挂死

先 `kpmctl 10270 unload mount_hidder`（`rc=0`）再加载同一份产物，`kpmctl` 进程进入 D 状态：

```
25746 D kpmctl
```

`/proc/25746/stack`：

```
[<0>] __switch_to+0x244/0x4e4
[<0>] misc_register+0x38/0x298
[<0>] 0xffffffde902e1208
[<0>] 0xffffffde902e0344
[<0>] kp_load_module+0x1154/0x1230 [kernelpatch]
[<0>] kp_load_module_path+0xe4/0x1c4 [kernelpatch]
[<0>] kp_handle_supercall+0x1b4/0x604 [kernelpatch]
[<0>] kp_supercall_handler+0x1c0/0x218 [kernelpatch]
[<0>] el0_svc_common+0xd4/0x270
[<0>] el0_svc+0x28/0x88
```

dmesg 只到 `[yuuki] Initializing...`，没有走到 `komm loaded successfully`，说明卡在 `mod_init()` 的第二步 `ioctl_init()`（`modules/KPM/memcrc/mod_main.c:30`）里注册 `/dev/yuuki_misc` 的 `misc_register()`，还没进 `feature_init()`。

第一次失败时内核还打了：

```
note: kpmctl[25198] exited with preempt_count 1
```

这两条放在一起看：**上一次 init 失败后，装载器/模块的清理路径没有把内核状态复原**（进程带着非零 preempt_count 退出），随后任何需要同一把锁的 `misc_register()` 都会永久等待。这条属于 KernelPatch 装载器的失败回滚问题，不是 memcrc 的业务逻辑。

## 3. 设备现状

- 设备没死：`adb shell echo alive` 正常，无 RCU stall、无 oops。
- `kpmctl` 进程常驻 D 状态，模块处于半加载（`symbols resolved` 之前完成，`init` 未返回）。
- 按仓库规则**不再重试**，也不再执行任何 supercall。需要重启设备清理。

## 4. 已做的修改（离线）

`modules/KPM/memcrc/feature/feature.c` 的五个钩子从整函数替换改成链式钩子：

- 新增 `mc_before_count()`：把 `m->count + 1` 存进 `fargs->local.data7`（`local` 是整条链共享的，槽位约定见注释与 `docs/process/09-load-forms.md`）。
- 五个 `rep_*` 换成五个 `mc_after_*`，在 after 回调里用保存的旧 `count` 做同样的裁剪。
- `hook_all()`/`uninstall_hook()` 改走 `hook_wrap(func, 2, before, after, 0)` / `hook_unwrap(...)`，失败时逐个回退。

这样 memcrc 与 mount_hidder 挂同一函数时是**追加**而不是**覆盖**。Pixel 6 上已验证：`memcrc` 0.2.4 与 `mount_hidder` 1.0.3 同时加载，`add:/apex` 让 `/proc/mounts` 的 83 行归零（146 → 63），同时 memcrc 的 `RANGE` 规则仍然能把目标进程的 `[stack]` 行裁掉（141 → 140）。

## 5. 待验证（需要重启后的 5.10 设备）

1. `load memcrc_0.2.4.kpm` 是否不再出现 `-HOOK_BAD_RELO`（与 mount_hidder 共存）。
2. 第二次加载是否还会在 `misc_register()` 挂死 —— 若仍复现，说明失败回滚问题与本次改动无关，需要单独报给 KernelPatch。
3. `PTRACE:` / `RANGE:` 在 5.10 上的实际效果（6.1 已验）。
