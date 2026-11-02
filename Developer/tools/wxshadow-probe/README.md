# wxshadow-probe（一次性诊断，不是模块）

wxshadow 立项前的 P0 探针：只读，不改任何页表。用来回答两个问题——

1. 能不能从 pid 走到目标 `mm` 的 leaf PTE；
2. PTE 里的物理地址能不能换算成内核可读地址，且读出来的内容与目标进程自己读到的**逐字节一致**。

两件事成立，后面「克隆页 + 双视图切换」才有地基；不成立就得换方案。

## 组成

| 文件 | 作用 |
| --- | --- |
| `wxs_probe.c` | KPM，`ctl0` 两条命令：`self`（换算因子与自校验）、`probe <pid> <hexva>`（dump 该页） |
| `wxs_client.c` | 对照进程：`mmap` 一页 rwx，写一段固定字符串（见源码），把自己 pid/va/内容写文件后睡住 |
| `Makefile` | `make all` 出 `.kpm`，`make client` 出静态链接客户端 |

## 地址换算方式（不猜版本、不用 KP 私有全局）

`linear_voffset`、`memstart_addr`、`page_shift` 这些 KP 全局**都没有导出给 KPM**（`KP_EXPORT_SYMBOL` 里查不到），
所以不能像内核里那样写 `phys_to_virt()`。探针的做法是现场量：

1. `__kmalloc` 一页 → 写魔数 → `at s1e1w` 取它的物理地址（PAR_EL1）；
2. `linear_voffset = kva - pa`；
3. 用 `pa + linear_voffset` 回读同一页，比对魔数 → 自校验。

`PAGE_OFFSET` 只用于打印，公式是 `~0 << VA_BITS`（`VA_BITS` 从 `TCR_EL1.T1SZ` 读），不参与换算。

## 用法

```bash
make all client
adb push wxs_probe_0.0.1.kpm wxs_client /data/local/tmp/
adb shell "su -c '/data/local/tmp/kpmctl <mgr_uid> load /data/local/tmp/wxs_probe_0.0.1.kpm'"
adb shell "su -c '/data/local/tmp/kpmctl <mgr_uid> ctl wxs_probe self'"
adb shell "su -c 'chmod 755 /data/local/tmp/wxs_client; nohup /data/local/tmp/wxs_client /data/local/tmp/wxs_client.txt 120 >/dev/null 2>&1 & sleep 1; cat /data/local/tmp/wxs_client.txt'"
adb shell "su -c '/data/local/tmp/kpmctl <mgr_uid> ctl wxs_probe \"probe <pid> <va>\"'"
```

注意 `kpmctl ctl` 只把**一个** argv 当参数传下去，多段参数必须整体加引号。

## 踩过的坑

- **`out_msg[len] = 0` 会 panic**：`out_msg` 是用户地址，内核直接赋值属于「access to user memory outside uaccess routines」，
  开了 PAN 的 arm64 上直接 Oops（Pixel 6 首次上机就是这么重启的）。回写只能 `compat_copy_to_user`，NUL 一起拷。
- `struct pt_regs` 需要 `#include <asm/ptrace.h>` 才是完整类型（虽然 P0 用不到它）。
- `mm_struct_offset.pgd_offset` 两台机器不同（6.1 是 56，5.10 是 72），必须用 KP 解析出来的值，不能写死。
