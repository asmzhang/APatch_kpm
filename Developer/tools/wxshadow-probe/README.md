# wxshadow-probe（一次性诊断，不是模块）

wxshadow 立项前的探针。P0 只读、不改页表；P1 会把目标进程一页 4 KiB 匿名页的 PTE 切到影子页，`release` 再切回来。

它回答四个问题：

1. 能不能从 pid 走到目标 `mm` 的 leaf PTE；
2. PTE 里的物理地址能不能换算成内核可读地址，且读出来的内容与目标进程自己读到的**逐字节一致**；
3. 能不能让目标进程读同一个 va 读到**另一份内容**（影子页）；
4. `release`/`unload` 之后能不能干净复原（PTE 回原值、内容回原样、不留残留）。

## 组成

| 文件 | 作用 |
| --- | --- |
| `wxs_probe.c` | KPM，`ctl0` 命令：`self`、`probe <pid> <hexva>`、`attach <pid> <hexva>`、`install <hexbytes>`、`release`、`state` |
| `wxs_client.c` | 对照进程：`mmap` 一页 rw，写固定字符串，把 pid/va 写文件，然后每 500 ms 读一次前 16 字节并追加到日志 |
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
adb push wxs_probe_0.0.2.kpm wxs_client /data/local/tmp/
adb shell "su -c '/data/local/tmp/kpmctl <mgr_uid> load /data/local/tmp/wxs_probe_0.0.2.kpm'"
adb shell "su -c '/data/local/tmp/kpmctl <mgr_uid> ctl wxs_probe self'"
adb shell "nohup /data/local/tmp/wxs_client /data/local/tmp/wxs_client.txt /data/local/tmp/wxs_client.log 90 >/dev/null 2>&1 &"
adb shell "cat /data/local/tmp/wxs_client.txt"          # 取 pid 与 va
adb shell "su -c '/data/local/tmp/kpmctl <mgr_uid> ctl wxs_probe \"attach <pid> <va>\"'"
adb shell "su -c '/data/local/tmp/kpmctl <mgr_uid> ctl wxs_probe \"install 534841444f572d7031\"'"
adb shell "tail -2 /data/local/tmp/wxs_client.log"      # 应变成 5348...
adb shell "su -c '/data/local/tmp/kpmctl <mgr_uid> ctl wxs_probe release'"
adb shell "tail -2 /data/local/tmp/wxs_client.log"      # 应回到 4f52...
```

注意 `kpmctl ctl` 只把**一个** argv 当参数传下去，多段参数必须整体加引号。

## 踩过的坑

- **`out_msg[len] = 0` 会 panic**：`out_msg` 是用户地址，内核直接赋值属于「access to user memory outside uaccess routines」，
  开了 PAN 的 arm64 上直接 Oops（Pixel 6 首次上机就是这么重启的）。回写只能 `compat_copy_to_user`，NUL 一起拷。
- **页内偏移要单独存**：`attach` 把 va 掩成页基址之后，`install` 再用 `va & 0xfff` 算偏移会恒为 0，
  结果是「PTE 换了、目标进程读到的内容没变」。判据要落在「目标进程读到什么」，不能只看 PTE。
- **`make clean` 会删掉客户端**：只跑 `make all` 就把旧二进制推上去，日志文件根本不会生成。改完客户端要 `make client`。
- `pkill -f wxs_client` 会匹配到执行它的那个 shell（把自己杀掉），清理用 `killall wxs_client`。
- `struct pt_regs` 需要 `#include <asm/ptrace.h>` 才是完整类型（P1 用不到，P2 的 BRK 单步会用）。
- `mm_struct_offset.pgd_offset` 两台机器不同（6.1 是 56，5.10 是 72），必须用 KP 解析出来的值，不能写死。

## 限制

只处理 4 KiB leaf；不取 `mmap_lock`（KP 没导出 `mmap_lock` 偏移）；只做数据视图，不做执行视图与 icache 同步；
单实例状态机，没有 generation/token。这些都在 P2 处理，详见 [docs/records/2026-11-02-wxshadow-p1.md](../../../docs/records/2026-11-02-wxshadow-p1.md)。
