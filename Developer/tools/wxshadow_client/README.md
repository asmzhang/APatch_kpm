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

## 当前状态（对应模块 1.2.4）

prctl 通道已打通，命令都可用：

- `-m` 列出目标进程的可执行映射；
- `-a <addr> -r x0=7` 设断点并改寄存器（命中时回显到 dmesg：`Breakpoint Hit` / `modifying x0`）；
- `-a <addr> --patch <hex>` 写影子页（读侧仍返回原字节）；
- `--release` 释放该进程全部影子页。

**权限（1.2.3 起可维护，不再是硬编码）**：`prctl_before()` 只放行 `wx_uid_allowed()` 里的 uid，
默认 `{0, 2000}`，可用 ctl0 增删：

```
su 10273 -c '/data/local/tmp/kpmctl 10273 ctl wxshadow allow:10273'   # 允许管理器 uid
su 10273 -c '/data/local/tmp/kpmctl 10273 ctl wxshadow deny:10273'    # 收回
su 10273 -c '/data/local/tmp/kpmctl 10273 ctl wxshadow allowlist'     # 查看
```

uid 0 恒放行且不可 deny；表满（8 项）时 `allow:` 返回 `rc=-1`。
注意：ctl0 本身仍要求「受信管理器 uid」，所以给别的身份放行要用管理器的上下文执行。

**卸载（1.2.2 起语义变了）**：`wait_for_handlers_drain()` 等 in-flight handler 超时后**只记录
并继续卸载**（`mod_exit` 的返回值在 KP 里没有生命周期语义，见模块 README「已知问题 2」），
不再返回 `-EBUSY` 中止。
