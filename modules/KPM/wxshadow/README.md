# wxshadow —— W^X 影子页（同 VA 双视图）

进程读取某页时看到的是**原始字节**，执行时跑的是**影子页里的补丁**。这是「双视图」的页级实现，
不占硬件断点槽位，也不改原页一个字节。

- 上游：`mkpms-stealth` 的 `kpms/wxshadow/`，版本 `1.2.0-k66`（源码里 `KPM_VERSION` 写死该串，本仓不覆盖）。
- 本仓：`modules/KPM/wxshadow/` 只搬 KPM 侧 6 个源文件；用户态客户端在
  `Developer/tools/wxshadow_client/`。

## 原理

| 状态 | 页权限 | 谁看得见 |
| --- | --- | --- |
| `NONE` | 原页 | 原始代码 |
| `SHADOW_X` | `--x` 影子页 | 执行补丁；读会被挡下 |
| `ORIGINAL` | `r--` 原页 | 读到原字节；执行会被挡下 |
| `STEPPING` | `r-x` 原页 | BRK 触发后单步执行原指令 |

页面状态机：`NONE → SHADOW_X ↔ ORIGINAL ↔ STEPPING`，另有 `DORMANT`（钩子退休、影子页保留）。

页面在两种状态间来回切，代价是「读一次 + 执行一次」各缺一次页；代码页同时被读又被执行的紧循环
会明显变慢，这是页粒度方案的固有成本。

## 构建

```bash
cd modules/KPM/wxshadow
export ANDROID_NDK_LATEST_HOME=/path/to/ndk
make            # 产出 wxshadow_1.2.0.kpm
make clean
```

不生成 debug 变体：上游源码没有任何 debug-only 分支（无调试宏），加 `-D` 编出来字节完全相同，
多一个同哈希产物只会误导身份记录。

## 客户端

```bash
cd Developer/tools/wxshadow_client
make            # 产出 wxshadow_client（aarch64 静态可执行）
```

| 命令 | 作用 |
| --- | --- |
| `-p <pid> -m` | 列出目标进程可执行映射 |
| `-p <pid> -a <addr>` | 在地址上设隐藏断点 |
| `-p <pid> -a <addr> -r x0=7` | 断点命中时改寄存器 |
| `-p <pid> -b <lib> -o <off>` | 按「库名 + 偏移」定位 |
| `-p <pid> -a <addr> -d` | 删除断点（不带 addr 删全部） |
| `-p <pid> -a <addr> --patch <hex>` | 往影子页写自定义 patch |
| `-p <pid> -a <addr> --release` | 释放该地址的影子页（不带 addr 释放全部） |

prctl 接口（`wxshadow.h`）：`0x57580001` SET_BP / `0x57580002` SET_REG / `0x57580003` DEL_BP /
`0x57580004` SET_TLB_MODE / `0x57580005` GET_TLB_MODE / `0x57580006` PATCH / `0x57580008` RELEASE。

## 真机状态（2026-11-03）

| 设备 | 形态 | 加载 | 说明 |
| --- | --- | --- | --- |
| Pixel 6 | 6.1.99-android14，kernel | `load rc=0` | dmesg `[wxshadow] succeed` |
| 小米 13 | 5.10.149-android12-9，APatch LKM | `load rc=0` | 同上，**两形态符号面 0 缺失** |

页级切换在 Pixel 6 上确认在跑：`exec fault at 221d28 → shadow (--x)` /
`read fault at 221c98 → switched to live original (r--)`。

## 已知问题（本移植未修，按「先移植、后修错」分开处理）

1. **prctl 通道不通**：客户端所有命令返回 `EINVAL`，内核侧连日志都进不去。原因是
   `wxshadow.c:1495` 用 `hook_syscalln(__NR_prctl, 5, ...)`，而这版 KernelPatch 的
   `hook_syscalln` 不采纳 `args->skip_origin` / `args->ret`（`panda_hide` 踩过同一个坑）。
   修法：改 `syscalln_addr(nr, 0)` + `hook_wrap(addr, has_syscall_wrapper ? 1 : 6, ...)`。
2. **卸载会让设备重启**：`kpmctl unload wxshadow` 之后设备掉线、`uptime` 归零。它 hook 了
   `copy_process` / `exit_mmap`，卸载要逐个摘钩并释放所有影子页，这条路径在 6.1 上直接带崩内核。
   **在修好之前：只加载，不卸载；要清状态就重启。**

## 边界

- 仅 ARM64；patch 不跨页（`offset + len <= PAGE_SIZE`）。
- 每页最多 128 个断点、128 个 patch；每个断点最多 4 个寄存器修改。
- 需要 KernelPatch 框架（`hook` / `kallsyms_lookup_name` / `syscalln_addr` 等）。
- GUP 路径（`follow_page_pte`）在 6.6 上不存在，属上游已知项；6.1 上该钩子未装，
  `/proc/<pid>/mem` 这类读取是否泄漏影子页内容需单独复核。
