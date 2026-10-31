# stealth_cli

`modules/KPM/stealth_hook` 的命令行客户端。用户态程序，走 syscall 427 与内核模块通信。

不是 KPM，不进 CI 构建列表（仓库只构建 `modules/<形态>/<模块>/` 下的目录）。
设备上单独 push 即可，用 APatch 管理器加载 `stealth_hook_*.kpm` 后再跑。

## 构建

```bash
export ANDROID_NDK_LATEST_HOME=/path/to/ndk/26.1.10909125
make            # 产出 stealth_cli（aarch64 静态可执行）
```

## 部署

```bash
adb push stealth_cli /data/local/tmp/stealth_cli
adb shell chmod 755 /data/local/tmp/stealth_cli
```

## 命令

```
status                              查询模块状态（返回 0x53484F4B + 版本码）
hook <addr> [--tid N] [--dump N] [--flags F] [--inline ADDR] [--listen-ret]
                                    下硬件执行断点，默认挂当前线程
unhook <addr> [--tid N]             摘断点
enable|disable <addr> [--tid N]     启停断点
query <addr> [--tid N] [--index N]  读命中记录（入口 X0–X7、LR、返回值、指针 dump）
override <addr> [--tid N] --set N=VAL ... [--modify]
                                    改第 N 个参数（X0..X7）
override <addr> [--tid N] --skip-ret VAL
                                    跳过原函数，直接返回 VAL
skip <addr> [--count N]             忽略接下来 N 次命中（只跳过记录，不跳过执行）
ghost alloc <size> [--va ADDR] [--prot rwx]
ghost write <va> <hex|@file>
ghost list
ghost free <va>
pte query                           本机是否支持 PTE 重定向
selftest                            对自己进程里的目标函数跑完整链路
```

地址可写 `0x...` 或十进制；`--tid` 省略时用当前线程。

## 权限

模块侧有 uid 门，默认受信 `{0, 2000}`（root / shell）。以普通 app 身份调用会拿到 `EPERM`。

```bash
# 以 app 身份跑，验证 uid 门
adb shell run-as <pkg> /data/data/<pkg>/stealth_cli status   # → EPERM
```

## selftest 期望输出

```
selftest: target=0x... tid=...
call before hook: 68 (expect 68)
hook: rc=0
call with hook: 68 (expect 68)
query: rc=0 hits=2 enter=... total=1 x0=0x11 x1=0x22 lr=0x... ret_x0=0x44
override modify: rc=0
call with modify: 546 (expect 546)
skip count=1: rc=0
call with skip(1): 546 (仍执行原函数，skip 只跳过记录)
call after skip used up: 546 (expect 546)
override skip: rc=0
call with skip: 1911 (expect 1911)
unhook: rc=0
call after unhook: 68 (expect 68)
```

判读要点：`68 = 0x44` 是原函数结果，`546 = 0x222` 说明 X0 被改成 `0x100`，
`1911 = 0x777` 说明原函数被跳过、直接返回了假值。

## 与上游 CLI 的差异

上游 `xiaojianbang_hook` 自带 `.kpm` 加载器（抢 supercall 通道）与 DBI 重编译。本客户端：

- **不带加载器**：模块由 APatch 管理器加载，客户端不碰 supercall。
- **不带 DBI**：PTE 重定向未移植，`pte` 子命令只做能力探测。
- **协议不变**：结构体与命令号沿用上游，方便对照。
