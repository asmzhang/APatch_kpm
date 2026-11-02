# LKM 形态（不存放同一模块的第二份源码）

`modules/` 以前按「运行前提」分两层，`modules/LKM/` 预留给「原厂内核 + `kernelpatch.ko`」的
适配产物。现在这条路线改成**一份源码、一个产物、两种装载环境**，理由与判据见
[docs/process/09-load-forms.md](../../docs/process/09-load-forms.md)。

## 结论

- `modules/KPM/<模块>/` 的源码是**唯一**一份；`.kpm` 产物默认同时面向两种装载环境。
- 本目录**不再承接「把同一模块的源码复制一份」**。同一份代码两份拷贝会漂移、双份审计、
  身份翻倍，而真实差异通常只是一个符号。
- 只有将来出现**只能编成 `.ko`** 的东西（不是 `.kpm` 的另一种符号面，而是真的内核模块），
  才在这里新建 `<模块>/`，并在 `metadata/modules/<模块>.json` 的 build 记录里写明 `load_forms`。

## 怎么判断一个模块能不能在两种形态下装载

```bash
python tools/load_forms.py --module <模块>              # 看产物在两种形态下的缺失符号
python tools/load_forms.py --module <模块> --strict     # 按 build 记录的 load_forms 判定
python tools/load_forms.py --subset                     # 两形态都提供的 60 个名字
python tools/load_forms.py --kallsyms <导出表>           # 把内核符号也核到具体目标内核
```

## 已知形态差异（2026-11-02 实测）

| 类别 | kernel 形态 | lkm 形态 |
| --- | --- | --- |
| 自带符号表 | 173 个 | 62 个 |
| 可移植子集 | 两形态共有 60 个 | 同左 |
| 典型专属符号 | `kallsyms_lookup_name_by_suffix`、`cred_offset`、`inline_wrap_syscalln`、`fp_wrap_syscalln`、`is_trusted_manager_uid_android`、`kf_strnstr` | `kf_kmemdup`、`kf_kstrdup` |

目前两形态通用的模块：`mount_hidder` 1.0.3、`panda_hide` 2.0.1、`apkey_hide` 3.6.4、`stealth_hook` 0.3.4（Pixel 6 与小米 13 上都实测加载成功；`mount_hidder`、`panda_hide`、`stealth_hook` 在小米 13 上验到功能生效，`apkey_hide` 验到 `ctl0` 可用）。`memcrc` 0.2.4 符号面已清零并在 Pixel 6 验到功能生效（含与 `mount_hidder` 同挂 `show_*` 共存），小米 13 侧待设备重启后重测。
后者的额外收获：`hook_syscalln()` 在 kernel 形态不认 `skip_origin`，阻断型钩子要自己
`hook_wrap` 到 `__arm64_sys_xxx`，见 [09-load-forms.md](../../docs/process/09-load-forms.md)「阻断型 syscall 钩子」一节。
