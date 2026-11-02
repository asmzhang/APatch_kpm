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
| `wxs_probe.c` | KPM，`ctl0` 命令见下表 |
| `wxs_client.c` | 对照进程：`mmap` 一页 rw，写固定字符串，把 pid/va 写文件，然后每 500 ms 读一次前 16 字节并追加到日志 |
| `wxs_exec_client.c` | 执行视图对照进程：一页 rwx 匿名内存里放 `mov w0,#1; ret`，每 500 ms 调一次并记录返回值 |
| `wxs_oracle.c` | 自检 oracle：目标函数用全局 asm 定义，期望字节**编译进代码**（`oracle_ref[]`），每 2 秒 `memcmp` 一次并打印 `CLEAN`/`PATCHED` + 返回值。不读磁盘，所以内核改物理页也瞒不过它 |
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

## P3 结论：同一个 VA 上的「双视图」做不到（2026-11-02 实测）

P3 的目标是让同一个 VA 上「读看到原字节、执行看到补丁」。两条路都试过，都不成立。

### 路一：PTE 乒乓（原页 ↔ 影子页）—— 不可能

`dual` 之后让目标进程紧循环调用被补丁的函数，0.23 秒内 IABT/DABT 互切 **20 万次**触发 storm 保护：

```
wxs_probe: dual sw=5 ec=0x20 far=0x221d28
wxs_probe: dual sw=6 ec=0x24 far=0x221c98
wxs_probe: dual switch storm, disabling
```

原因是**一个 VA 同时只有一个 PTE 状态**：把影子页 PTE 的 `AP[1]` 清零确实能挡住 EL0 的数据读，
但同一位也挡住 EL0 的取指（这与早先「清 AP[1] 后取指仍成功」的 P2c 观察相反，以实测为准）；
`AP[1]=1` 又会让读看到补丁。读写在同一页的紧循环里必然抖。

### 路二：原页只置 UXN + 把取指重定向到影子 VA —— 页表对、语义错

原页 `| PTE_UXN` 后字节一个不动，`do_mem_abort` 里 `regs->pc = sva + (far - tva)` 改到影子 VA。
页表层面全部验证通过：

- 原页 `pte=0x6000093fdc7fc3`（UXN 已置），`probe` 读出来仍是原字节；
- 影子 VA `0x3ffc98` → `pte=0x2000094de74fc3`，`data=e0008052`（补丁 `mov w0,#7`）+ 原 `add/ret` 字节。

但目标进程在第一次重定向之后就死掉，`faults=1 iabt=1`，再没有第二次异常。读反汇编就能看出原因：

```
221d68: 11000694   add  w20, w20, #0x1
221d6c: 7109629f   cmp  w20, #0x258
221d70: 54fffdc1   b.ne 0x221d28
221d5c: 94005492   bl   0x236fa4 <printf>
```

`b`/`bl`/`adrp` 全是 **PC 相对**的。同一段字节搬到 `0x3ff000` 执行时：

- `b.ne 0x221d28` 变成跳到 `0x3ffd28`（还在影子页里，恰好能跑）；
- 但 `bl printf` 从 `0x3ffd5c` 出发算出来的目标是 `0x414fa4` —— 那里没有任何映射，取指异常落在
  影子范围之外、处理器不管，内核直接给目标进程 SIGSEGV。

**结论**：把执行搬到另一个 VA，等于要求整段代码与它引用的所有常量、GOT、外部函数一起搬家；
只要有一条 `bl` 指向页外就崩。这不是偏移没算对，是方案本身不成立。

### 因此

「同一个 VA 上读原字节、执行补丁」在 KPM 这一层只有三条出路，都不是 PTE 技巧：

1. **原地打补丁 + 拦住读路径**：页里放补丁（执行天然正确），把完整性校验会走的读（`/proc/self/mem`、
   二次 `mmap`、`pread`）在系统调用层拦下来改回原字节 —— 这正是本项目 KPM 侧一直在做的事。
2. **硬件断点单步**：不碰页表，用 `DBGBVR/DBGBCR` 或 perf 断点在被补丁指令处陷入，执行完再单步回去
   —— `modules/KPM/stealth_hook` 已经实现了这条路，且不依赖 VA 搬迁。
3. **同 VA 换地址空间**：需要控制 ASID/TTBR0 级别（hypervisor 或内核线程切换），KPM 里没有这个能力。

另外两个已修的坑（都在 `wxs_mem_abort_before` / `wxs_write_pte`）：

- **必须校验 mm**：同一个 VA 在别的进程里是另一套映射。静态链接程序 text 常落在 `0x221000` 一带，
  不比对 `mm->pgd` 就会把无关进程的取指重定向到它自己没有映射的影子 VA —— 实测 `kpmctl` 因此
  段错误，而 `su`/`dmesg` 正常，很容易误判成「设备坏了」。
- **TLB 必须全 ASID 刷**：`tlbi vaae1is` 只刷当前 ASID；目标进程在另一个 ASID 上，旧的可执行项
  不会失效，改过的 UXN 它根本看不见。改成 `tlbi vmalle1is` 只在安装/复原时付一次代价。
