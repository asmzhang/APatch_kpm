# wxshadow —— W^X 影子页（同 VA 双视图）

进程读取某页时看到的是**原始字节**，执行时跑的是**影子页里的补丁**。这是「双视图」的页级实现，
不占硬件断点槽位，也不改原页一个字节。

- 上游：`mkpms-stealth` 的 `kpms/wxshadow/`，上游版本串 `1.2.0-k66`；本仓自 1.2.1 起把源码里的
  `KPM_VERSION` 改成 `MYKPM_VERSION`（由 Makefile 注入），产物名与模块自报版本保持一致。
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
make            # 产出 wxshadow_1.2.1.kpm
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

| 设备 | 形态 | 加载 | 权限门 | 卸载 | 说明 |
| --- | --- | --- | --- | --- | --- |
| Pixel 6 | 6.1.99-android14，kernel | `load rc=0` | 管理器 uid 被拒 / root 放行 | `unload rc=0`，不重启 | dmesg `[wxshadow] succeed`；端到端双视图已验 |
| 小米 13 | 5.10.149-android12-9，APatch LKM | `load rc=0` | `prctl denied for uid 10270` / root 放行（`bp at 556c6fc100`） | `unload rc=0`，不重启 | **两形态同一份字节 0 缺失符号** |

小米 13 上还确认了 `GUP hiding ENABLED (follow_page_pte hooked)` 打出（5.10 上该钩子挂成功）。

页级切换在 Pixel 6 上确认在跑：`exec fault at 221d28 → shadow (--x)` /
`read fault at 221c98 → switched to live original (r--)`。

### 端到端：读原字节 + 执行补丁（2026-11-03，Pixel 6）

prctl 通道修好之后，两个靶子都验过：

| 靶子 | 做什么 | 结果 |
| --- | --- | --- |
| `wx_t` | 每 100ms 调一次 `target(x)`，`x` 取自 volatile | 断点挂在 `ret` 上改 `x0=7` → 日志 `ret=7`，dmesg `Breakpoint Hit` + `modifying x0: 29 -> 7` |
| `wx_v` | 同上，另外每 50 轮把自己 `.text` 的前 8 字节打印出来 | 打印 `28 00 80 52 00 05 00 2a`（原始 `mov w8,#1; orr w0,w8,w0,lsl#1`）**且** `ret=7` |

`wx_v` 那一行就是「双视图」：同一个 VA，读路径拿到原始指令，执行路径跑的是改了寄存器的补丁。

**验证时踩过的坑（写下来免得再犯）**：第一版靶子写 `target(20)`，编译器把常量折叠、把 `bl target`
提到了循环外（`llvm-objdump` 里循环体内没有 `bl`），于是「断点不触发、返回值恒 41」看起来像模块坏了。
入参必须来自 `volatile`，并且要用 `llvm-objdump` 确认循环体内真的有 `bl target`。

**日志噪声（已知，未修）**：init 末尾无条件调 `debug_print_tasks_list(10)`（`wxshadow_scan.c:1240-1296`，调用点 `wxshadow.c:1664`），每次加载往 dmesg 打 10 行内核任务指针（`task=%px mm=%px`）。是上游调试代码、无功能副作用，但属内核地址泄漏 + 噪声，等 1.2.2 用编译宏关掉。
## 已知问题（本移植未修，按「先移植、后修错」分开处理）

1. ~~**prctl 通道不通**~~（**已修，本仓 1.2.0**）：原实现用 `hook_syscalln(__NR_prctl, 5, ...)`，
   而这版 KernelPatch 的 `hook_syscalln` 走 `syscall_hook_add(..., allow_skip=0)`，不采纳
   `args->skip_origin` / `args->ret`，请求落回原生 `prctl` → `EINVAL`。改成自己按
   `__arm64_sys_prctl`（`.cfi_jt → .cfi → 无后缀`）解析地址再 `hook_wrap(...)`，卸载走
   `hook_unwrap(...)`；两种装载形态都验过。详见 `Developer/reports/2026-11-03-wxshadow-port.md`。
2. **卸载稳定性**（1.2.1 起有兜底，仍未定位根因）：早期观察到两次 `kpmctl unload wxshadow`
   之后设备掉线、`uptime` 归零。随后的复现尝试 9 次里 7 次正常（空闲 5 次、靶子退出后 1 次、
   靶子仍在跑 1 次），两次历史重启**复现不出来**，因此不宣称已修；`wxshadow_exit()` 的七阶段
   顺序与上游逐字节一致，未改动。

   1.2.1 补了一道兜底（独立复核问题单 WX-2）：`wait_for_handlers_drain()` 原来是
   `void`，自旋上限到了只打一条 `pr_warn` 就继续往下走，而 KP 在 `mod_exit` 返回 0 后立刻
   `kp_free_exec(mod->start)`——handler 还在模块代码里就会执行到已释放页。现在改成返回
   `int`，超时由 `wx_drain_checked()` 转成 `-EBUSY` 中止卸载：宁可模块停在已加载状态，
   也不释放还被引用的代码。**该分支没有真机触发条件，未实测**（要制造一个卡死的 handler）。

   使用建议：能重启就重启；必须运行中卸载时，先 `-d` 删断点、让靶子退出或空闲再卸。

## 更新记录

- **1.2.1（2026-11-03）**：独立复核问题单修复——WX-1 `prctl_before` 加权限门（只允许
  uid 0 / 2000）；WX-2 `wait_for_handlers_drain` 超时改为中止卸载；WX-3 两处
  `offset + len > PAGE_SIZE` 改成 `len > PAGE_SIZE - offset`（防整型回绕）；
  `KPM_VERSION` 改用 `MYKPM_VERSION`，产物名与自报版本对齐。
- **1.2.0（2026-11-03）**：从上游原样搬入（7 个源文件，业务代码零改动）；新写 Makefile
  （不生成 debug 变体）；修 prctl 通道（`hook_syscalln` → `hook_wrap`），双视图端到端跑通。


## 边界

- 仅 ARM64；patch 不跨页（`offset + len <= PAGE_SIZE`）。
- 每页最多 128 个断点、128 个 patch；每个断点最多 4 个寄存器修改。
- 需要 KernelPatch 框架（`hook` / `kallsyms_lookup_name` / `syscalln_addr` 等）。
- GUP 路径（`follow_page_pte`）在 6.6 上不存在，属上游已知项；6.1 上该钩子未装，
  `/proc/<pid>/mem` 这类读取是否泄漏影子页内容需单独复核。
