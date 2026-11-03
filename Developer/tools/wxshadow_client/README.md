# wxshadow_client

`modules/KPM/wxshadow/` 的用户态客户端。上游 `kpms/wxshadow/wxshadow_client.c` 原样搬入，
只把构建改成与本仓一致：不依赖 cmake、不依赖 `aarch64-linux-gnu-gcc`，用 NDK clang 出静态可执行。

## 构建

```bash
export ANDROID_NDK_LATEST_HOME=/path/to/ndk
make            # 产出 wxshadow_client
make clean
```

## 用法

```bash
# 看目标进程哪些段可执行
./wxshadow_client -p <pid> -m

# 在某个地址设隐藏断点
./wxshadow_client -p <pid> -a 0x7b5c001234

# 断点命中时把 x0 改成 7
./wxshadow_client -p <pid> -a 0x7b5c001234 -r x0=7

# 按「库名 + 偏移」定位
./wxshadow_client -p <pid> -b libc.so -o 0x12345

# 往影子页写 patch（例：mov w0,#7; ret）
./wxshadow_client -p <pid> -a 0x7b5c001234 --patch e00080d2c0035fd6

# 释放（不带 -a 释放该进程全部影子页）
./wxshadow_client -p <pid> -a 0x7b5c001234 --release
./wxshadow_client -p <pid> --release
```

| 参数 | 说明 |
| --- | --- |
| `-p, --pid <pid>` | 目标进程；`0` 表示自己 |
| `-a, --addr <addr>` | 虚拟地址（十六进制） |
| `-b, --base <lib>` / `-o, --offset <off>` | 库名 + 偏移，二选一替代 `-a` |
| `-r, --reg <reg>=<val>` | 改寄存器，`x0`–`x30` 或 `sp`，可重复 |
| `-d, --delete` | 删断点（不带 `-a` 删全部） |
| `-m, --maps` | 列出可执行映射 |
| `--patch <hex>` | 往影子页写自定义字节 |
| `--release` | 释放影子页（不带 `-a` 释放全部） |
| `--once` | 一次性断点，命中一次后自动解除 |

## 当前状态（2026-11-03，模块 1.2.1）

prctl 通道已打通：模块侧改成自己解析 `__arm64_sys_prctl` 再 `hook_wrap(...)`，实测
`-m` 能列出可执行映射、`-a <addr> -r x0=7` 能设断点并改写寄存器（靶子返回值从 41 变 7），
`--patch` / `--release` 同样可用。

**必须以 root 运行**。1.2.1 起模块在 `prctl_before` 加了权限门（独立复核问题单 WX-1）：
只有 uid 0（root）与 2000（shell）能下发命令，其他身份（含管理器 uid 10273）一律
`Operation not permitted (errno=1)`。这是有意的：这条通道能对任意 pid 打补丁，等价代码注入。

卸载：1.2.1 起 `wait_for_handlers_drain()` 超时会**中止卸载**（`mod_exit` 返回 `EBUSY`），
不再带着未排空的 handler 去释放模块代码。实测空闲卸载、挂过断点后卸载都是 `unload rc=0`
且设备不重启；早期两次「卸载即重启」在当前产物上复现不出来，详见模块 README 已知问题 2。
