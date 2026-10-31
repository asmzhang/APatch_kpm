# memcrc

## 模块作用

按规则改写内核对进程的可见输出，用于绕过内存 CRC 校验与隐藏自身痕迹。上游意图见 [memcrc](https://github.com/yuuki-kokoro/memcrc)。

四类规则：

| 规则 | 格式 | 作用 |
| --- | --- | --- |
| 路径重定向 | `MAP:uid:src:dst` | 打开 `src` 时实际打开 `dst` |
| 路径过滤 | `PATH:path` | 从 `/proc/*/maps`、`mountinfo`、`mountstats` 输出中抹掉含该路径的整行（`mountinfo` 逐字符打码，保留 `/`） |
| 状态伪装与拦截 | `PTRACE:uid` | 把该 uid 进程的 `State:` 改成 `R (running)`、`TracerPid:` 压成 `0`；并拒绝其 `ptrace` 请求（`-EPERM`） |
| 地址区间隐藏 | `RANGE:uid:start:end` | 从 `/proc/*/maps` 抹掉与该区间有交集的整行；`uid` 为 `0` 表示所有进程，地址为十六进制，可带 `0x` |

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

管理命令：`CMD:STATUS`、`CMD:HELP`、`CMD:DUMP`、`CMD:LIST`、`CMD:CLEAR`、`CMD:REMOVE:<type>:<index>`。

## 信任模型

`getcwd` 通道对所有进程开放，因此按调用者 uid 限权：

- 受信 uid（默认 `0`、`2000`）：可给任意 uid 下规则，可加全局 `PATH` 规则。
- 非受信 uid：只能给自己下规则（`MAP`/`PTRACE`/`RANGE` 的 uid 必须等于调用者），不能加 `PATH`。
- 受信列表可用 `ALLOW:<uid>`、`DENY:<uid>` 维护，`ALLOWLIST` 查看；`0` 不可移除。

规则对所有进程生效，不需要 root，也不经过 APatch 的 supercall 通道；加载模块本身仍需管理器或超级密钥。

## 选择版本

| 文件 | 用途 |
| --- | --- |
| `memcrc_<版本>.kpm` | 发布版 |
| `memcrc_<版本>_debug.kpm` | 调试版，仅多日志 |

无结构体偏移依赖，同一产物可用于 5.10～6.1 的 ARM64 内核；`struct seq_file`、`struct file_operations` 按内核版本手写快照，6.12 及以上需另行核对。

## 边界

- 只改写 procfs 文本输出与 open 路径，不改任何系统调用的返回值，也不隐藏自身存在（`/dev/yuuki_misc` 可被枚举）。
- 重定向依赖调用者本来就有读 `dst` 的权限；上游的 SELinux 旁路（`get_task_ext`）在当前 KernelPatch 上不可用，已移除。
- 进程状态伪装只改 `/proc/<pid>/status` 与 `/proc/<pid>/stat` 的状态字符，不改调度行为。

## 更新记录

### 0.1.0

新增 `RANGE` 地址区间隐藏规则<br />
新增 `getcwd` 通道的 uid 权限门与 `ALLOW`/`DENY`/`ALLOWLIST` 命令<br />
`ctl0` 支持全部规则与管理命令

### 0.0.1

上游版本移植，保留路径重定向、路径过滤、状态伪装与 ptrace 拦截

## 来源

基于 [memcrc](https://github.com/yuuki-kokoro/memcrc)（作者 yuuki），原始思路见 [看雪论坛帖](https://bbs.kanxue.com/thread-288041.htm)。移植改动见 `Developer/reports/2026-10-30-memcrc-port.md`。
