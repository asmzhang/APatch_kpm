# LKM 形态模块（当前为空）

`modules/` 按**运行前提**分两层：

| 目录 | 含义 |
| --- | --- |
| `modules/KPM/` | 现有全部模块。产物是 `.kpm`，由 kpatch / APatch 在内核里加载。 |
| `modules/LKM/` | 预留给 LKM 形态（原厂内核 + `kernelpatch.ko`）的适配产物。当前为空。 |

## 为什么现在为空

现有 11 个模块的产物都是 `.kpm`，但**能否装载取决于内核对外导出哪些符号**，两种形态并不等价：

- 依赖 `kallsyms_lookup_name_by_suffix`：`nomount`、`run_cmd_demo`、`zram_zstd`。
  该符号只在内建 KernelPatch（APatch 这种把 KernelPatch 编进内核的形态）里导出，
  LKM 形态的导出表中没有，装载会报 `unknown symbol`。
- 依赖 `task_ext` / `kf_task_ext_ensure`：`dont_kill_freeze`、`lmkd_dont_kill`、`re_kernel`。
- 无特殊符号依赖：`cgroupv2_freeze`、`qti_battery_charger`、`re_kernel_x`、`xperia_ii_battery_age`。
- `hosts_redirect` 已去掉 `accctl.h`/`task_ext` 依赖，两种形态都可用（Pixel 6 / Android 15 / 6.1.99 实测）。

所以「全部先放 KPM」是当前事实，而不是最终分类；等某个模块做出 LKM 适配版本后，
把**那一份**源码放进 `modules/LKM/<模块>/`，`modules/KPM/<模块>/` 保留 KPM 版本。
两层同名不算冲突：`tools/identity.py` 按模块名解析目录时会先找 `modules/*/<name>`。

## 放进来之前必须确认

1. 该模块在 LKM 形态下**确实装载成功**（`kpatch`/`insmod` 后 `dmesg` 无 `unknown symbol`）。
2. `Makefile` 的相对路径按新深度写：`KP_DIR = ../../../KernelPatch`，共享文件用 `../../../kpm_utils.h`。
3. 构建与门禁能识别：CI 与 `tools/` 已支持一层（`modules/<模块>/`）与两层（`modules/<形态>/<模块>/`）。
