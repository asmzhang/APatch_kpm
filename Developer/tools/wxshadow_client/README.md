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

## 当前状态（2026-11-03）

客户端能编译、能运行，但**所有命令都返回 `EINVAL`**：模块侧的 prctl 钩子用的是
`hook_syscalln`，这版 KernelPatch 不采纳 `args->skip_origin` / `args->ret`，请求根本没进到
`wxshadow_do_patch` / `wxshadow_do_set_bp_flags`。修法见
`modules/KPM/wxshadow/README.md` 的「已知问题」第 1 条。

另：模块**只能加载不能卸载**（卸载会让设备重启），清理状态请重启设备。
