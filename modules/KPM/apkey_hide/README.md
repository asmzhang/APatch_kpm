# apkey_hide

把 APatch / KernelPatch 从应用侧藏起来：让 `get_ap_mod_exclude()` 这类「这个 uid 要不要排除 APatch 模块」
的查询对未受信应用返回「排除」，从而让模块目录、`/data/adb/ap` 之类痕迹不再被模块系统暴露。

来源：[APKey_Hide_KPM](https://github.com/Lun-OS/APKey_Hide_KPM)（MIT，作者 Lun.）的移植。
业务逻辑与上游一致，本仓只改了两处：`KPM_NAME` 统一为小写 `apkey_hide`、`KPM_VERSION` 走 `MYKPM_VERSION`。

## 它做什么

1. **钩 `get_ap_mod_exclude(uid)`**：非受信 uid 一律返回「排除」，于是该 uid 下看不到 KPM 模块；
   受信集合（root、shell、管理器 uid、`allow` 名单、加载者）照常放行。
2. **首次运行扫一遍已有排除位**：把 `[1, 20000)`（嵌入模式下是 `[1, 10000)`）与
   `[90000, 100000)` 区间的 uid 排除状态对齐，避免历史残留让某些应用看到不该看的东西。
3. **TOFU 认领窗口**：模块加载后、管理器 uid 还没确定时，开一个有限的放行窗口，
   让管理器完成一次 supercall 自我修正；窗口有调用次数上限（默认 8192），用完即关。
4. **降级模式**：拿不到 `get_ap_mod_exclude` 时不挂钩，只做一次区间扫描，模块仍可加载。

## 命令（ctl0）

通过 APatch 管理器的「参数」对话框，或 `kpatch <superkey> kpm ctl0 apkey_hide <命令>`：

| 命令 | 作用 |
| --- | --- |
| `status` | 打印状态：hook/refresh/swept/loader/mgr/embedded/claimed/cfgpkg/win/need_refine/refok/allow/deny/pass/计数/管理器包名 |
| `sweep` | 重新扫一遍 uid 排除位 |
| `allow=<uid>` | 加入受信名单（最多 8 个） |
| `deny=<uid>` | 移出受信名单 |
| `pkg=<包名>` | 指定管理器包名（前缀匹配，最长 6 字符） |
| `mgrname=<名字>` | 同上（历史命令名） |
| `unclaim` | 清掉 TOFU 认领、重置管理器 uid、重开窗口（换管理器时用） |

## 加载方式

- **普通加载**（管理器里「加载」/`kpatch kpm load`）：`event == "load-file"`，走通用模式。
- **嵌入模式**：模块被烧进 boot 镜像时，`event` 不是 `load-file`，此时会启用 TOFU 认领窗口，
  由第一个调用 supercall 的管理器 uid 自我认领。
  **不建议嵌入**——嵌入后管理器包名必须设对，否则认领会落到错误的 uid 上。

## 边界

- 每个 boot 重新开始：认领状态、窗口、扫描结果都不持久化。
- 管理员在 init 阶段加载时，自我修正依赖 root 域的 supercall 完成；若管理器从未发起过 supercall，
  管理器 uid 会一直未知（此时窗口保持开启，靠调用次数上限兜底）。
- 不负责文件层痕迹（`/data/adb/ap` 目录本身、管理器 APK 的包名等）。
- 与 `APKey_Hide_S`（自杀模式）**绝不同时加载**。

## 构建

```bash
export ANDROID_NDK_LATEST_HOME=/path/to/ndk
make all debug
# apkey_hide_<版本>.kpm / apkey_hide_<版本>_debug.kpm
```

debug 变体额外定义 `-DCONFIG_DEBUG -DAPK_DEBUG_LOG`，会打印 `[APKey_Hide]` 前缀的调试日志；
release 变体零 printk。
