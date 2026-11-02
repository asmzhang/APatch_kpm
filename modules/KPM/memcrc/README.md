# memcrc

## 模块作用

按规则改写内核对进程的可见输出，用于绕过内存 CRC 校验与隐藏自身痕迹。上游意图见 [memcrc](https://github.com/yuuki-kokoro/memcrc)。

五类规则：

| 规则 | 格式 | 作用 |
| --- | --- | --- |
| 路径重定向 | `MAP:uid:src:dst` | 打开 `src` 时实际打开 `dst` |
| 路径过滤 | `PATH:path` | 从 `/proc/*/maps`、`mountinfo`、`mountstats` 输出中抹掉含该路径的整行（`mountinfo` 逐字符打码，保留 `/`） |
| 状态伪装与拦截 | `PTRACE:uid` | 把该 uid 进程的 `State:` 改成 `R (running)`、`TracerPid:` 压成 `0`；并拒绝其 `ptrace` 请求（`-EPERM`） |
| 地址区间隐藏 | `RANGE:uid:start:end` | 从 `/proc/*/maps` 抹掉与该区间有交集的整行；`uid` 为 `0` 表示所有进程，地址为十六进制，可带 `0x` |
| 调试寄存器伪装 | `REGS:uid` | 对该 uid 的 `PTRACE_GETREGSET`/`SETREGSET` + `NT_ARM_HW_BREAK`/`NT_ARM_HW_WATCH` 返回伪造的硬件断点视图：`SET` 被吞掉（内核里不留断点），`GET` 回放这份假账本；`uid` 为 `0` 表示所有调用者 |

## 注入通道

规则通过 `getcwd` 的缓冲区送入模块：调用 `getcwd(buf, size)` 时 `buf` 里已有的内容会被当作一条规则解析。命令行示例：

```sh
# 需要先加载模块；<SUPERKEY> 为 APatch 设置中的超级密钥
kpatch <SUPERKEY> kpm ctl0 memcrc 'RANGE:0:0:ffffffffffffffff'
```

也可以读写字符设备：

```sh
cat /dev/yuuki_misc                  # 列出全部规则
echo 'CMD:CLEAR' > /dev/yuuki_misc   # 清空规则
```

设备节点是 `0600 root:root`，只有受信 uid 能开（`on_misc_open` 再校验一次）。
部分机型上 `>`（`O_TRUNC`）打开该字符设备会被 SELinux 的 `setattr` 检查挡成
`EACCES`，与模块无关；写规则改用下面两种不带截断的方式：

```sh
printf 'CMD:CLEAR\n' >> /dev/yuuki_misc
printf 'CMD:CLEAR\n' > /tmp/r.txt && dd if=/tmp/r.txt of=/dev/yuuki_misc conv=notrunc
```

管理命令：`CMD:STATUS`、`CMD:HELP`、`CMD:DUMP`、`CMD:LIST`、`CMD:CLEAR`、`CMD:REMOVE:<type>:<index>`。

## 信任模型

`getcwd` 通道对所有进程开放，因此按调用者 uid 限权：

- 受信 uid（默认 `0`、`2000`）：可给任意 uid 下规则，可加全局 `PATH` 规则。
- 非受信 uid：只能给自己下规则（`MAP`/`PTRACE`/`REGS`/`RANGE` 的 uid 必须等于调用者），不能加 `PATH`。
- 受信列表可用 `ALLOW:<uid>`、`DENY:<uid>` 维护，`ALLOWLIST` 查看；`0` 不可移除。

规则对所有进程生效，不需要 root，也不经过 APatch 的 supercall 通道；加载模块本身仍需管理器或超级密钥。

## 选择版本

| 文件 | 用途 |
| --- | --- |
| `memcrc_<版本>.kpm` | 发布版 |
| `memcrc_<版本>_debug.kpm` | 调试版，仅多日志 |

无结构体偏移依赖，同一产物可用于 5.10～6.1 的 ARM64 内核；`struct seq_file`、`struct file_operations` 按内核版本手写快照，6.12 及以上需另行核对。

## 加载形态（0.2.3 / 0.2.4 起）

同一份 `.kpm` 同时支持 kernel 形态（APatch 内核集成）与 lkm 形态（原厂内核 + `kernelpatch.ko`）。
依据与判据见 [docs/process/09-load-forms.md](../../docs/process/09-load-forms.md)。

**0.2.3：去掉形态专属符号。** 原先依赖的 `cred_offset`、`fp_wrap_syscalln`、`fp_unwrap_syscalln`、
`inline_wrap_syscalln`、`inline_unwrap_syscalln`、`kf_strnstr`、`kf_strpbrk`、`kf_strsep`、`kf_strspn`
在 lkm 形态的符号面里都不存在，装载器会以 `unknown symbol` + `-ENOENT` 在 `apply_relocations()` 前中止。
改用 `portable.c`：

- 字符串处理改成自己实现的 `mc_strnstr` / `mc_strspn` / `mc_strpbrk` / `mc_strsep`；
- uid 读取改用两形态都导出的 `current_uid()`；跨任务取 uid 用 `mc_task_uid()`，
  偏移由 `init_cred` 的结构特征推导（`usage` 非零 + 其后 `uid..fsgid` 连续 8 个 4 字节为 0），
  并用当前进程的 cred 交叉验证，推不出来就返回 -1 按「非目标进程」处理，不猜偏移；
- syscall 钩子改用 `syscalln_addr()` + `hook_wrap()`，不再依赖 `fp_*` / `inline_*` 包装。

**0.2.4：procfs 钩子改链式。** 五个 `show_*` 钩子原先用 `hook()` 做整函数替换，
在已存在链式钩子的函数上会返回 `-HOOK_BAD_RELO`（5.10 LKM 上 mount_hidder 先挂了 `show_vfsmnt`）。
改成 `hook_wrap(func, 2, before, after)` 后按 KP 的 `hook_chain` 追加，多个模块可共存。
旧 `count` 存在 `fargs->local.data7`（见 09 文档的槽位表）。

自检：

```bash
python3 tools/load_forms.py --module memcrc --strict
```
## 边界

- 只改写 procfs 文本输出与 open 路径，不改任何系统调用的返回值，也不隐藏自身存在（`/dev/yuuki_misc` 可被枚举）。
- 重定向依赖调用者本来就有读 `dst` 的权限；上游的 SELinux 旁路（`get_task_ext`）在当前 KernelPatch 上不可用，已移除。
- 进程状态伪装只改 `/proc/<pid>/status` 与 `/proc/<pid>/stat` 的状态字符，不改调度行为。

## 更新记录

### 0.2.5

- 卸载幂等：`ioctl_exit()` 只在 `misc_register()` 真的成功过之后才 `misc_deregister()`；`syscall_exit()` 同理只卸真挂上的 getcwd 钩子。
- 背景：`mod_init()` 失败路径已清过一次，装载器随后还会调 `mod_exit()`，重复 `misc_deregister()` 会在 `__list_del_entry_valid()` 里 `BUG()`，并带着 `misc_mtx` 死掉，使下一次 `misc_register()` 永久阻塞（小米 13 / 5.10 / LKM 实测，见 `docs/records/2026-11-02-mi13-memcrc-hang.md`）。

### 0.2.4

procfs 的五个 `show_*` 钩子从整函数替换（`hook()`）改成链式 before/after（`hook_wrap()`）：
`hook()` 无法安装在已被别人打过补丁的函数上（5.10 LKM 上 mount_hidder 先用 `hook_wrap2` 钩了
`show_vfsmnt`，`hook()` 直接返回 `-HOOK_BAD_RELO`）。改成链式后两者是追加关系。
旧 `count` 存进 `hook_fargs` 的 `local.data7`，槽位约定见 09 文档。

### 0.2.3

去掉 9 个只有 kernel 形态才导出的符号（`cred_offset`、`fp_*syscalln`、`inline_*syscalln`、
`kf_strnstr`、`kf_strpbrk`、`kf_strsep`、`kf_strspn`），改用 `portable.c` 自实现字符串函数、
`current_uid()` 与特征推导的 cred uid 偏移、`syscalln_addr()` + `hook_wrap()`。
`tools/load_forms.py` 复算：`und=27`，两形态缺失均为 0。
### 0.2.1

`komm.c` 空表覆盖分支的 `snprintf` 返回值按实际写入长度钳制（AUD-c-10）：
`snprintf` 返回「本应写入的长度」，缓冲不够时它大于 `count`，原实现直接赋给
`total_len` 会让随后的 `copy_to_user` 越过 vmalloc 缓冲，把小缓冲的 read
变成内核堆内存泄露。真机验证：`head -c 8 /dev/yuuki_misc` 返回 7 字节
（`[MAPPIN`），`head -c 1` 返回 1 字节，`cat` 仍是完整六段。

### 0.2.0

新增 `REGS` 调试寄存器伪装规则（`NT_ARM_HW_BREAK`/`NT_ARM_HW_WATCH` 的 `GETREGSET`/`SETREGSET`）<br />
`GETREGSET` 改为放行真实调用后覆盖寄存器数组，`dbg_info`/`iov_len` 与真实内核一致<br />
设备读端 dump 增加 `[RANGES]`、`[REGS]` 两段

### 0.1.0

新增 `RANGE` 地址区间隐藏规则<br />
新增 `getcwd` 通道的 uid 权限门与 `ALLOW`/`DENY`/`ALLOWLIST` 命令<br />
`ctl0` 支持全部规则与管理命令

### 0.0.1

上游版本移植，保留路径重定向、路径过滤、状态伪装与 ptrace 拦截

## 来源

基于 [memcrc](https://github.com/yuuki-kokoro/memcrc)（作者 yuuki），原始思路见 [看雪论坛帖](https://bbs.kanxue.com/thread-288041.htm)。移植改动见 `Developer/reports/2026-10-30-memcrc-port.md`。

### 0.2.2

- **`komm` 读路径的 snprintf 截断守卫**（AUD-C-11）：`temp[512]` 的格式化最长可达 525
  字节（`%u:%s:%s` + 两条 255 字节路径），`snprintf` 返回「本应写入长度」，
  原实现直接拿它 `memcpy` 会读越界 13 字节栈内存。11 处 `total_len + temp_len < count`
  判断统一加 `temp_len < (int)sizeof(temp)` 前置条件，截断时走 `buffer_full`。
- **ctl0 `ALLOWLIST` 的累加式 snprintf 钳制**（AUD-C-12）：`control_dump_allow` 原来
  `n += snprintf(...)`，缓冲不足时 `n` 会越过 `size`、后续 `size - n` 下溢。
  新增 `cm_append()` 按实际写入位置饱和到 `size - 1`。
