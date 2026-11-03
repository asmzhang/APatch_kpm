# 剩余 9 个模块：冻结构建与真机验证批次报告

- 报告日期：2026-11-03
- 范围：`modules/KPM/` 下 9 个此前无冻结构建记录的模块
- 角色：Developer（自检，不是结论）
- 结论摘要：9 个模块全部完成冻结构建（共 22 个变体实例）；7 个真机通过、1 个设计内拒绝（cgroupv2_freeze）、1 个未通过（zram_zstd 触发设备重启）

## 1. 为什么之前没有冻结构建

这 9 个模块的 Makefile 是早期写的，与后来接入 CI 的模块相比缺两件事：

1. **宿主标签归一**：只有 `linux-x86_64` 归一，没有 `mingw64_nt-10.0-*-x86_64 → windows-x86_64`。在 Windows 上 NDK clang 路径因此指向不存在的 prebuilt 目录，`build_candidate.py` 的「有效编译参数捕获」直接失败（`有效编译参数捕获失败`）。
2. **生成物被当成构建输入**：`tools/identity.py` 的 `BUILD_INPUT_EXT` 含 `.json`，而 `baselines` 目标会在模块目录里生成 `<name>.kpm.json`，构建后输入哈希变化 → 候选构建被拒。

处理（均已提交）：

| 提交 | 内容 |
| --- | --- |
| `c704afd` | 9 个历史模块补 `windows-x86_64` 宿主标签归一（`re_kernel`/`re_kernel_x` 是 TAB 缩进，其余 7 个是 4 空格） |
| `36f0197` | `tools/identity.py` 的构建输入排除生成的 `.kpm.json`（`IGNORED_SUFFIX`） |
| `044ebfd` | `tools/build_candidate.py` 用 UTF-8 解码构建输出（原先 Windows 默认 gbk，非 ASCII 直接 `UnicodeDecodeError`） |

另外两条环境事实（不写进仓库，供复现者参考）：`patch_offsets.py` 需要 `--env PYTHON=python --env PYTHONUTF8=1`（Windows 上 `python3` 是 Store 别名，且该脚本按 UTF-8 读源码）；`zram_zstd` 的 codec 生成目录已存在时会拒绝覆盖，重新冻结前需要先把 `modules/KPM/zram_zstd/out` 移走。

## 2. re_kernel / re_kernel_x：6.1 上加载失败的真实缺陷

- 现象：Pixel 6（6.1.99-android14）上 `load rc=-1`，dmesg 只有 BTF 偏移打印 + `load_module_ex: [re_kernel] failed with [(null)] error: -95`。
- 定位：在 `prepare_rekernel_genl_server()` 前后插桩，逐项打印可用性判定的 11 个符号结果，得到 `kfree_skb=0`、`__reg=0`（`kfunc(kmalloc)` 为空）。
- 根因：
  1. `KernelPatch/kernel/patch/ksyms/misc.c` 把 `kfunc_match(kmalloc)` 注释掉了，`kfunc(kmalloc)` 恒为 NULL，而 genl server 用 `kmalloc(...)` 分配；
  2. 6.1 起内核不再导出 `kfree_skb`（只有 `kfree_skb_reason`），而判定要求它非空。
- 修法（提交 `3a9dcfc`）：`kmalloc` 先走 kfunc，失败再 `kallsyms_lookup_name("__kmalloc")` → `("kmalloc")` 兜底，并纳入判定；`kfree_skb` 本模块从未调用，从判定中移出并注明改名事实。
- 验证：修复后 `re_kernel`、`re_kernel_x` 在 Pixel 6 上 `load rc=0` / `unload rc=0`，uptime 连续。

> 说明：两个模块各有一批**修复前**的冻结件（`source_commit=36f0197`），在设备上实测 `load rc=-1`（error -95）。这批记录保留在 `builds[]` 里作为历史，`device_tests[]` 记为 fail；可用的是 `source_commit=3a9dcfc` 的那一批。

## 3. 冻结清单（工具链统一 `ndk26.1.10909125`）

| 模块 | 版本 | 变体 | instance_id | 产物 SHA-256（前 16 位） |
| --- | --- | --- | --- | --- |
| re_kernel | 11.7-20261010 | base | `re_kernel-11.7-20261010+g758bfb26fb59.ra6cd1a34.kpb51197a.ndk26.1.10909125#1` | `e3e38af1439c723a` |
| re_kernel | 11.7-20261010 | baselines | `...+g758bfb26fb59.re3a4aca6...#1` | `2872e89d9886eb19` |
| re_kernel | 11.7-20261010 | baselines_debug | `...+g758bfb26fb59.rc06fd75d...#1` | `03906b3ab08798bb` |
| re_kernel | 11.7-20261010 | debug | `...+g758bfb26fb59.r15be2e44...#1` | `9e8cb5a5c46a9f3f` |
| re_kernel_x | 1.6-20261010 | base | `re_kernel_x-1.6-20261010+g6d0698e7dee2.r59f08e39...#1` | `faf968cd02766786` |
| re_kernel_x | 1.6-20261010 | baselines / baselines_debug / debug | 同前缀三条 | `cefa2bf4…` / `3a95979a…` / `edb79433…` |
| zram_zstd | 1.0.0 | base / baselines | `zram_zstd-1.0.0+g59eff1a2d83a...` | `9c41f8c48180a669` / `2840bae27b23ca42` |
| nomount | 2.1.1-20261014 | base / baselines | `nomount-2.1.1-20261014+gb87f65b2c5a5...` | `448af4ded5cbcf49` / `47f25f95482eb925` |
| hosts_redirect | 2.0.0 | base / debug | `hosts_redirect-2.0.0+g61678ab7fa13...` | `de45aba56677b072`（两者同哈希） |
| dont_kill_freeze | 1.0.2 | base / debug | `dont_kill_freeze-1.0.2+g7d2c1915cf6b...` | `bb21053fddc6c4ab` / `690a13e9affc5bac` |
| cgroupv2_freeze | 1.0.12 | base / debug | `cgroupv2_freeze-1.0.12+g991d303577f5...` | `3623b2bb422f38fe` / `3907edb0a5d11a45` |
| lmkd_dont_kill | 1.0.0 | base / debug | `lmkd_dont_kill-1.0.0+gbe5b5897aa86...` | `6658cd43585ebbe2` / `25aa21a553108224` |
| run_cmd_demo | 1.3.0 | base / baselines | `run_cmd_demo-1.3.0+g181293fd8e23...` | `49feb59a79c5768b` / `6a0d8b8b2eabb389` |

`lmkd_dont_kill` 原缺 `metadata/modules/lmkd_dont_kill.json`，本轮按 `cgroupv2_freeze` 的模板补建。

`hosts_redirect` 的 `debug` 目标只改版本串、不加 `-D`，因此 base 与 debug 字节完全相同（同一 SHA-256），真机只测一个变体。

## 4. 真机验证（Pixel 6 / 6.1.99-android14 / APatch 11282 / kernel 形态）

设备指纹 `sha256:433ab88276ec`（不使用序列号）。加载命令统一 `su 10273 -c '/data/local/tmp/kpmctl 10273 load <file>'`。

| 模块 | 结果 | 观察 |
| --- | --- | --- |
| re_kernel 11.7-20261010 | pass | `load rc=0`、`succeed`、`unload rc=0`、uptime 连续 |
| re_kernel_x 1.6-20261010 | pass | 同上 |
| hosts_redirect 2.0.0 | pass | `load rc=0`（hook `do_filp_open` 成功）、`unload rc=0` |
| dont_kill_freeze 1.0.2 | pass | `load rc=0`（hook `do_send_sig_info`）、`unload rc=0` |
| lmkd_dont_kill 1.0.0 | pass | `load rc=0`、`unload rc=0` |
| **zram_zstd 1.0.0** | **fail** | 加载瞬间 adb 掉线、设备重启（回来 `uptime` 8.71s），pstore 无新记录 |
| nomount 2.1.1-20261014 | pass | `load rc=0`，`NoMount: ready, version=2.1.1-20261014, offsets=dynamic; resident until reboot, do not unload`；设计为常驻，卸载不在支持范围 |
| run_cmd_demo 1.3.0 | pass | `load rc=0`，`WARNING: do not unload this module; reboot to remove it` |
| cgroupv2_freeze 1.0.12 | unsupported | `load rc=-1`、`error -24`：设计内拒绝——`inline_hook_init` 先查 `cgroup_freeze_write`，本机 6.1.99 已存在该符号（内核自带 cgroupv2 freezer），模块主动不加载 |

验证结束后设备上仍保留 `nomount` 与 `run_cmd_demo`（两者按设计常驻、不可卸载），其余未加载；设备 health 正常、uptime 连续增长。

### 4.1 zram_zstd 崩溃：已停手，未定性

- 触发点：加载冻结件（`sha256=9c41f8c48180a669…`）的瞬间 adb 掉线，设备重启。
- 现场：`/sys/fs/pstore/` 只有 2025-03-25 的旧 `console-ramoops-0`，没有本次记录。
- 相关前情：探索构建阶段该模块曾加载成功并被 `unload rc=0` 卸载，而模块自身警告 `DO NOT UNLOAD: zram may retain codec callbacks`。
- 处置：按「真机死机不自救」立即停止，未复现、未重试；该变体真机结论记 **fail（触发设备重启）**，验收不涵盖。根因（很可能是先前 unload 遗留的 codec 回调）未证实，见第 6 节风险。

## 5. 身份与复现

- 冻结命令（每个模块一次，`<mod>` 与 `<handoff>` 按上表替换）：

```
python tools/build_candidate.py <mod> --toolchain ndk26.1.10909125 --target "all debug" \
  --env ANDROID_NDK_LATEST_HOME=D:/platform/Android/Sdk/ndk/26.1.10909125 \
  --env PYTHON=python --env PYTHONUTF8=1 \
  --handoff local/handoff-rest/<handoff>.json --shared-inputs
```

- 交接清单已复制到 `Developer/reports/handoffs/2026-11-03-remaining-modules/<mod>.json`，并由 `tools/identity.py import-manifest` 导入 `metadata/modules/<mod>.json` 的 `builds[]`。
- 每个 `builds[]` 记录含 11 个源文件的双哈希、`source_commit`、`source_dirty=false`、recipe（`cc`/`excerpt` 已归一为 `<NDK>`/`<repo>`）。

## 6. 未覆盖与风险

1. **zram_zstd 崩溃未定性**：只观察到「加载即重启」，没有栈。再次加载有二次重启风险，未做。建议后续在可丢弃的设备上用串口/`ramoops` 或主机侧 `cat /dev/kmsg` 流式抓取复现一次，再决定是修实现还是把该模块降级为「仅构建、不真机」。
2. **nomount / run_cmd_demo 常驻不可卸**：本轮验证只覆盖「加载后不崩、日志正常」，没有做卸载路径（设计上不支持）。测试后设备需重启才能回到干净状态。
3. **cgroupv2_freeze 只在 6.1.99 上验过「被拒绝」**：其真正可用的场景（内核没有 `cgroup_freeze_write` 的旧内核）本机没有设备，未覆盖。
4. **只测一个变体**：除 `re_kernel`/`re_kernel_x` 的 base 外，其余模块的 baselines/debug 变体没有上真机；按仓库规则「真机只测一个变体」，验收需由审计确认变体差异仅为日志/断言，或补测。
5. **re_kernel / re_kernel_x 的 4 个变体只测了 base**：静态基线（`_baselines`）依赖目标 BTF 的偏移推导，未在真机上验证其偏移正确性。
