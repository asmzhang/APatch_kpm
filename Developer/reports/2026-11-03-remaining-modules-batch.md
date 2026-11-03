# 剩余 9 个模块：冻结构建与真机验证批次报告

- 报告日期：2026-11-03
- 范围：`modules/KPM/` 下 9 个此前无冻结构建记录的模块
- 角色：Developer（自检，不是结论）
- 结论摘要：9 个模块全部完成冻结构建（`builds[]` 共 30 条记录；其中 22 条是本次候选，另 8 条是 36f0197 修复前的失败记录，仍为 candidate、未标 superseded）；真机共 11 条记录（pass 7 / unsupported 1 / fail 3）：7 个模块通过、`cgroupv2_freeze` 设计内拒绝、`zram_zstd` 未通过（加载触发设备重启）

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
  **归因更正（独立复核 AUD-103）**：真正拦截本次加载的只有 `kfree_skb` —— 可用性判定位于 `prepare_rekernel_genl_server()` 阶段内（`modules/KPM/re_kernel/re_kernel.c:261-268`），而 `kmalloc` 兜底在 `inline_hook_init` 里、该阶段成功之后才执行，未参与本次失败；`__kmalloc` 经 `kallsyms_lookup_name_by_suffix` 精确首轮即可命中，旧判据本就为假。`KernelPatch/kernel/patch/ksyms/misc.c:333-334` 注释掉 `kfunc_match` 只影响 KP 内部表，与模块 `kfunc()` 无关。
- 根因：
  1. `KernelPatch/kernel/patch/ksyms/misc.c` 把 `kfunc_match(kmalloc)` 注释掉了，`kfunc(kmalloc)` 恒为 NULL，而 genl server 用 `kmalloc(...)` 分配；
  2. 6.1 起内核不再导出 `kfree_skb`（只有 `kfree_skb_reason`），而判定要求它非空。
- 修法（提交 `3a9dcfc`）：把 `kfree_skb` 从可用性判定中移出（6.1 起改名为 `kfree_skb_reason`，后缀匹配不会命中）；`kmalloc` 加 `kallsyms_lookup_name("__kmalloc")` → `("kmalloc")` 兜底并纳入判定（fail-closed，非本次病因）。
- **另一处事实更正（独立复核 AUD-104）**：`kfree_skb` 并非「从未调用」—— `modules/KPM/re_kernel/re_utils.h:41` 的 `nlmsg_free` 就是 `kfunc_call_void(kfree_skb, skb)`，调用点在 `re_kernel.c:181`、`re_kernel_x/re_utils.h:110`/`:116`、`re_kernel_x/re_kernel.c:399`/`:460`；6.1 上该指针恒 NULL，错误路径因此静默泄漏 skb（有 NULL 保护，不崩）。已登记 `AUD-104`（open）。
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

`hosts_redirect` 的 base/debug 曾字节完全相同（`de45aba56677b072`）——这**不是设计**：`Makefile` 的两个目标依赖同一个 `hosts_redirect.o`，`make all debug` 单次调用时该对象已是最新，`-DCONFIG_DEBUG` 从未进入任何一次编译（独立复核 A-01 / `AUD-101`，已修）。修复后 debug 变体走独立对象 `hosts_redirect_debug.o`，需要重建与复测；本文档表格中的同哈希记录对应修复前的旧构建。

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
- 每个 `builds[]` 记录含该模块源文件的双哈希（`sources[]` 条目数 6~429，视模块规模而定，不是固定值）、`source_commit`、`source_dirty=false`、recipe（`cc`/`excerpt` 已归一：NDK 路径 → `<ANDROID_NDK_LATEST_HOME>`，仓库路径 → `<repo>`；两者都是占位符，不含本机绝对路径）。

## 6. 未覆盖与风险

1. **zram_zstd 崩溃未定性**：只观察到「加载即重启」，没有栈。再次加载有二次重启风险，未做。建议后续在可丢弃的设备上用串口/`ramoops` 或主机侧 `cat /dev/kmsg` 流式抓取复现一次，再决定是修实现还是把该模块降级为「仅构建、不真机」。
2. **nomount / run_cmd_demo 常驻不可卸**：本轮验证只覆盖「加载后不崩、日志正常」，没有做卸载路径（设计上不支持）。测试后设备需重启才能回到干净状态。
3. **cgroupv2_freeze 只在 6.1.99 上验过「被拒绝」**：其真正可用的场景（内核没有 `cgroup_freeze_write` 的旧内核）本机没有设备，未覆盖。
4. **只测一个变体**：除 `re_kernel`/`re_kernel_x` 的 base 外，其余模块的 baselines/debug 变体没有上真机；按仓库规则「真机只测一个变体」，验收需由审计确认变体差异仅为日志/断言，或补测。
5. **re_kernel / re_kernel_x 的 4 个变体只测了 base**：静态基线（`_baselines`）依赖目标 BTF 的偏移推导，未在真机上验证其偏移正确性。

## 9. 两路独立复核回执与处置（2026-11-03）

复核方式：两个独立 agent 只读取证，禁止调用 `tools/identity.py`、`tools/build_candidate.py`、`make`、`tools/check_repository.py`。
两路都确认主链是硬的：30/30 产物 sha256+size 与磁盘一致、30/30 instance_id 可复算、source_tree 双哈希 108 条全一致、
1578 条 blob_sha256 对 git 对象 0 不符、`source_commit` 均为 HEAD 祖先、本次提交未改 `tools/` 判据。问题集中在「记录自证性」与「事实断言」。

| 编号 | 严重度 | 内容 | 处置 |
| --- | --- | --- | --- |
| A-01 / AUD-101 | 中 | `hosts_redirect` debug 变体复用 base 的 `.o`，`-DCONFIG_DEBUG` 从未生效 | 已修 `Makefile`（独立对象）并重建复测（`8abac56`）：debug 11896 B ≠ base 10440 B，双机均 load/unload rc=0 |
| A-02 / AUD-103 | 中 | `3a9dcfc` 把失败归因给了 `kmalloc`，实际拦截项只有 `kfree_skb` | 已更正本文（提交说明按「只追加」不可改写，以本文为 errata） |
| A-04 / AUD-104 | 中低 | `kfree_skb` 有 5 处真实调用，6.1 上静默泄漏 skb | 登记 open，待按符号可用性二选一解析 |
| A-03 | 中 | `sanitize()` 对 `--env` 值做纯子串替换，`PYTHONUTF8=1` 把字面 `1` 变成 `<PYTHONUTF8>`，污染 recipe/instance 身份 | 已修 `tools/identity.py`（只把像路径的值当替换目标） |
| A-05 / AUD-02 | 中低 | 两条 `tested_bytes.sha256` 只有 16 hex，门禁只校验 `retest` 形态故漏过 | 已补全 64 hex；门禁收紧为「任何登记了 tested_bytes.sha256 的条目都必须 64 位且命中在册产物」 |
| A-10 / AUD-102 | 低 | `cgroupv2_freeze` 用 `-24`（EMFILE）表达设计内拒绝 | 已改 `-EOPNOTSUPP` 并重建复测（`8abac56`）：dmesg `error: -95`，仍按设计拒绝 |
| A-12 | 低 | `builds[].handoff_manifest` 指向被 gitignore 的 `local/handoff-rest/` | 已改指 `Developer/reports/handoffs/2026-11-03-remaining-modules/`（30 条） |
| A-06 | 中低 | baselines 的 `<name>.kpm.json` 偏移表被 gitignore、未进 `artifacts[]`；`patch_offsets.py` 不在 `sources[]` | open（涉及产物清单口径，需维护者定） |
| A-07 / AUD-07 | 中低 | 实例绑定的 `source_tree_sha256` 依赖工作树 EOL（本仓 536 条 CRLF / 1042 条 LF） | open（仓库级设计问题，需统一 `.gitattributes`） |
| A-08 | 中低 | 8 条 36f0197 之前的失败构建仍 `candidate`、无 `superseded_by` | open（历史记录只追加，不改写） |
| A-09 / AUD-08 | 低 | 报告数字口径（22 vs 30、11 个源文件、占位符名字） | 已更正 |
| A-11 | 低 | 宿主标签只归一 `mingw64_nt-*`，MSYS2/Cygwin 的 `MSYS_NT-*`/`CYGWIN_NT-*` 不匹配 | open（改 Makefile 会让 9 个模块的冻结树失效，留待下一轮统一） |
| A-13 | 低 | `device_fingerprint` 形如 `sha256:433ab88276ec`，只 12 hex 却标 `sha256:` | open（口径修正，涉及 11 条记录） |
| A-14 / AUD-105 | 低 | `zram_zstd` 重启归因未闭环（前次 unload 残留 vs 冻结件） | 登记 open，标为未验证风险 |
| AUD-06 | 低 | `kmalloc` 兜底是死代码（全模块无 `kfunc(kmalloc)(` 调用） | 记入 AUD-103 说明，不单开单 |
| AUD-11 | 低 | `IGNORED_SUFFIX` 潜在静默排除、构建日志 `errors="replace"` 吞非法字节 | open（工具级，留待下一轮门禁整理） |

## 10.1 AUD-104 / AUD-105 闭环（2026-11-03）

| 问题单 | 根因 | 修法 | 实测证据 |
| --- | --- | --- | --- |
| AUD-104（re_kernel / re_kernel_x） | `nlmsg_free()` 依赖 `kfunc(kfree_skb)`；6.1 只有 `kfree_skb_reason(skb, reason)`，`kf_kfree_skb` 恒 NULL → 5 处错误路径静默泄漏 skb | 模块内函数指针 + `kallsyms_lookup_name("kfree_skb_reason")`，`reason=0`。**不能**用 `kfunc_def(kfree_skb_reason)`：KPM 符号表无该项，实测装载报 `unknown symbol: kf_kfree_skb_reason`（rc=-1） | 冻结件 Pixel 6 `load rc=0`，dmesg `re_kernel: skb free path: kfree_skb_reason(fallback)` / `[ReKernel-X] skb free path: kfree_skb_reason(fallback)` |
| AUD-105（zram_zstd） | `zram_zstd_exit()` 只打警告、不 `crypto_unregister_alg`；卸载后内核算法列表仍指向已释放镜像 → 下次 `load` 的 `crypto_has_alg()` 解引用悬空项 → EL1 缺页 panic | `exit` 调用 `crypto_unregister_alg`（符号按需解析，缺失时退化告警） | 干净启动单独加载 1.0.0 → `rc=0`（崩的是「卸载残留 + 再加载」）；1.0.1 冻结件 `load rc=0` → `unload rc=0`（dmesg `unregistered zstd-kpm`）→ `load rc=0`，uptime 连续、设备不重启 |

新开 AUD-106（低）：1.0.1 卸载后再加载时 `zcomp_available_show` 显示钩子降级（`algorithm list hook failed: -4094`），算法注册仍成功。

## 10.2 AUD-107：1.0.1 只摘算法、没摘 hook（2026-11-03 复核发现）

独立复核（同一轮 AUD-104/105 复核）指出：`zram_zstd_exit()` 只调了 `crypto_unregister_alg`，
`start_zram_display()` 在 `modules/KPM/zram_zstd/zs_display.c:55`（`zcomp_available_show`）与 `:86`
（`do_init_module`）装的两个 `hook_wrap` 从未摘除；而 `unload_module()` 全文没有 hook 清理，
`kp_free_exec()` 只是把 TLSF 块回收（不清零、不 unmap）。后果：卸载之后**任何一次 LKM 加载**
（`do_init_module`）或读 `/sys/block/zram0/comp_algorithm`（`zcomp_available_show`）都会跳进
已回收/可能被复用的镜像块 → UAF / panic，且非特权可触发。

铁证：`-4094 == HOOK_DUPLICATED`（`KernelPatch/kernel/include/hook.h:18`、`hook.c:703` 同一 `after`
重复挂载即返回该码），1.0.1 第二次加载打印的 `algorithm list hook failed: -4094` 只能来自第一次
卸载时留下的 `zstd_list_after`；`module load hook unavailable` 同理。

1.0.2 修法：`zs_display.c` 记住两个 hook 目标（`display_show_target` / `display_init_target`），
新增 `stop_zram_display()` 用 `hook_unwrap_remove(target, NULL, fn, 1)` 摘除并复位 `display_installed`
（API：`KernelPatch/kernel/include/hook.h:319`），`zram_zstd_exit()` 第一步调用它（先摘 hook、再摘算法）。
顺带把 `kfunc_def(crypto_unregister_alg)` 的返回类型从 `void` 改成 `int`（与内核签名一致，AUD-109）。

实测（Pixel 6 / 6.1.99 / APatch 11282）：`load rc=0` → `unload rc=0`（`unregistered zstd-kpm` 累计出现）
→ `load rc=0`，**无** `-4094`、**无** `module load hook unavailable`；随后加载 `re_kernel` `load rc=0`
/ `unload rc=0`（这正是 1.0.1 会跳进已回收块的路径），uptime 连续 12346→12418。

## 10. 修复后的针对性复审（2026-11-03，单路独立 agent）

复审范围：只判上一轮问题单的六项修复是否到位，不重审全量。结论：**部分项判为已修，另开 7 条新单。**

| 项 | 判定 | 关键证据 |
| --- | --- | --- |
| AUD-101 hosts_redirect debug | 已修 | `Makefile:37-38/49-50/52-53` 让 debug 走独立对象；自算 10440 B `de45aba5…` vs 11896 B `382f11e4…`；debug 件含 8 条 base 没有的 printk 串 |
| AUD-102 cgroupv2 errno | 已修 | `cgroupv2_freeze.c:412-414` 返回 `-EOPNOTSUPP`；整目录无 `-24`；产物级仅 2 字节差异（`0x928002e0`→`0x92800bc0`） |
| AUD-103 归因 | **部分修** | 结论（kmalloc 未参与）已对，但位置描述错：被删条件原在 prepare 阶段的可用性判定里（`re_kernel.c:261-268`），不在 prepare 之前；且 `3a9dcfc` 提交说明不可改写，本文已作为 errata |
| AUD-104 kfree_skb 5 处调用 | 已修（记录）+ 残留注释 | 记录与报告已承认 5 处；源码注释仍写「本模块未调用」→ 本轮已改 |
| identity.py sanitize | 已修 | 直接调 `sanitize()` 断言：`PYTHONUTF8=1` 不再污染、含 NDK 路径仍被替换成 `<ANDROID_NDK_LATEST_HOME>` |
| 设备测试字节绑定 | **部分修** | 判据已扩到「任何登记 sha256 的条目」，负向注入实测能红；但 `tools/selftest_tools.py` 对新分支**零夹具**（违反 R11），且本机 Windows 下 selftest 37/39 |

新增问题单（均已登记或记档）：

- `AUD-N01`（中低）`Auditor/tools/artifact_audit.py:557-562` 用 `startswith(r.rstrip("/") + "/")` 判归属，键已 normpath 成反斜杠 → Windows 上「已登记但产物缺失」分支永不触发。
- `AUD-N02`（中低）新判据缺正反夹具，建议固化为三例（16 位应红 / 64 位错哈希应红 / 64 位正确应绿）。
- `AUD-N03`（低）`re_kernel.c` / `re_kernel_x/re_kernel.c` 注释仍称未调用 → 本轮已改。
- `AUD-N04`（中低）AUD-103 的位置描述错误 → 本轮已改。
- `AUD-N05`（低）处置表「待重建复测」已过时 → 本轮已改。
- `AUD-N06`（中低）`tools/selftest_tools.py` 子进程未传 `encoding`，zh-CN Windows 下 gbk 解码失败；`candidate_build_transaction` 夹具用无扩展名 `mock-clang`，Windows 上 `shutil.which` 返回 None 必失败 → 本轮给子进程加 `encoding="utf-8", errors="replace"`；夹具的平台限制记为已知缺口。
- `AUD-N07`（低）`AUD-101/102` 在复核之前就被标 `fixed`，属「预先登记关闭」→ 流程上应待发现方复核后再置 fixed，后续按此执行。
