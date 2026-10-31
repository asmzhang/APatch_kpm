# panda_hide

把 Frida / 调试器在设备上的痕迹藏起来。来源：[kpm-panda-hide](https://github.com/)（GPL v2，作者 pandaos）的移植。

上游是一个单文件集（`src/` 下 5 个子系统 + `main.c`，共 686 行），本仓保持同一结构，只做了改名与若干正确性修补。

## 五个子系统

| 文件 | 钩子 | 作用 |
| --- | --- | --- |
| `debugger_hide.c` | `seq_put_decimal_ull`、`seq_puts`、`proc_pid_wchan`、`do_task_stat` | `/proc/<pid>/status` 的 `TracerPid:` 归 0、`t (tracing stop)` → `S (sleeping)`；`/proc/<pid>/wchan` → `0`；`/proc/<pid>/stat` 的第三字段 `t` → `S` |
| `frida_hide.c` | `show_map_vma`、`__get_task_comm` | `/proc/<pid>/maps` 里含 `frida`/`gadget`/`linjector`/`gmain` 的行整条回滚；线程名含 `gum-js-loop`/`pool-frida`/`linjector` 等改成 `binder` |
| `openat_hide.c` | `__NR_openat`、`__NR_faccessat` | 路径含 `re.frida.server`、`frida-agent*.so`、`frida-gadget`、`linjector` 时返回 `-ENOENT`（`/memfd:` 例外放行） |
| `net_hide.c` | `__NR_connect` | 连 27042 / 27043 / 23946 / 31415 端口返回 `-ECONNREFUSED`；`adbd` 进程放行 |
| `mem_hide.c` | `access_remote_vm` | 别人通过 `/proc/<pid>/mem` 读目标进程内存时，把 `LIBFRIDA`、`frida-agent`、`frida:rpc`、`GumScript` 等特征串就地清零（只处理读方向） |

## 命令（ctl0）

`ctl0` 只回显参数（`echo: <args>`），没有配置能力——所有规则都是编译期常量。
需要按需隐藏请配合 [memcrc](../memcrc/README.md) 的 `RANGE:` / `PATH:` 规则。

## 与上游的差异

| 项 | 上游 | 本移植 | 原因 |
| --- | --- | --- | --- |
| 模块名 | `panda-hide` | `panda_hide` | 与本仓目录/管理名一致（KPM_NAME 统一小写） |
| 版本 | 字面量 `2.0.0` | `MYKPM_VERSION` | 版本由 Makefile 单点维护 |
| 卸载 | `unhook()` | `hook_unwrap_remove(func, before, after, 1)` | `hook_chain_t` 首成员是 `hook_t`，用 `unhook` 卸 wrap 链不走 `hook_chain_remove` 清理路径 |
| 内存擦除长度 | 用请求长度 `len` | 用实际返回值 `args->ret`（≤ `len`） | 未填充的部分是内核缓冲区里的旧数据，不该按请求长度遍历 |
| adbd 放行 | 只在符号解析成功时判 adbd | `comm` 初值清零，符号缺失时按「非 adbd」处理 | 上游在符号缺失时反而**放行**全部连接，属 fail-open |
| `main.c` 参数 | `pr_info("%s", args)` 不判空 | 加空值兜底 | `args` 为 `NULL` 时上游会打 `(null)` 之外的空指针风险；`strncat` 长度也改成按缓冲区剩余量 |
| 函数属性 | `__attribute__((optimize("O0")))` | `__attribute__((optnone))` | clang 不认 `optimize`，会报 `-Wunknown-attributes` |
| 日志前缀 | `panda-hide:` | `panda_hide:` | 与模块名一致，便于 grep |

## 边界

- 只藏「痕迹」，不藏行为：已经被 attach 的调试器仍然能继续工作，只是 `TracerPid`/`State`/`wchan` 看起来正常。
- `mem_hide` 只处理 `access_remote_vm`（`/proc/<pid>/mem`、`process_vm_readv` 的上游路径之一），
  直接走 `ptrace(PTRACE_PEEKDATA)` 或内核模块读取的路径不在覆盖范围。
- maps 行是按 `frida`/`gadget`/`linjector`/`gmain` 四个子串匹配的，`gmain` 这种词很容易误伤
  （任何带 `gmain` 的合法映射都会被藏掉），上游如此，本仓未改判据。
- `net_hide` 只拦 `connect`，不拦 `sendto`/UDP。
- 依赖符号全部走 `kallsyms_lookup_name`，缺一个就少一个子系统（会 `pr_warn` 并继续），不会加载失败。

## 真机自检（实现方，非 Tester 结论）

Pixel 6（Android 15，6.1.99-android14），加载/卸载均由 APatch 管理器 UI 完成，探针为静态 aarch64 二进制（uid 2000 shell）：

| 信号 | 卸载态 | 加载态 |
| --- | --- | --- |
| `/proc/<pid>/status` 的 `TracerPid:` | `6765`（真实 tid） | `0` |
| `/proc/<pid>/status` 的 `State:` | `t (tracing stop)` | `S (sleeping)` |
| `/proc/<pid>/wchan` | `ptrace_stop` | `0` |
| `/proc/<pid>/stat` 第三字段 | `t` | `S` |
| `/proc/<pid>/mem` 读 `LIBFRIDA_MARKER_12345` | 原样返回 | 返回 32 字节全 0 |
| `connect` 到 127.0.0.1:27042 | `rc=0`（连上） | `rc=-1 errno=111`（ECONNREFUSED） |
| `/proc/self/maps` 含 `frida_payload.bin` 的行 | 1 行 | 0 行 |
| 创建 `/data/local/tmp/frida-agent-64.so` | 成功 | `ENOENT` |
| 创建 `/data/local/tmp/normal.txt` | 成功 | 成功（不误伤） |
| 线程名 `gum-js-loop`（`/proc/self/task/<tid>/comm`） | `gum-js-loop` | `binder` |
| 线程名 `my-worker-1` | `my-worker-1` | `my-worker-1`（不误伤） |

加载后 dmesg：

```
panda_hide init, event: load-file, args: (null)
panda_hide: debugger hide installed
panda_hide: frida hide installed
panda_hide: openat hide installed
panda_hide: net hide installed
panda_hide: mem hide installed
panda_hide: all modules installed
[+] KP I load_module_ex: [panda_hide] succeed with [(null)]
```

卸载后 dmesg 逆序拆卸，`unload_module: name: panda_hide, rc: 0`，全部信号回到卸载态基线。

与 memcrc 0.2.0（同样钩 `do_task_stat`）同时加载时共存正常，四项隐藏照旧生效；
若再开启 memcrc 的 `PTRACE:<uid>` 规则，`PTRACE_ATTACH` 会先被 memcrc 拦掉，本模块的状态伪装在该路径上观察不到。

未覆盖项：只测了 release 变体（真机每目标只测一个变体）；ctl0 回显未测（无配置语义）；
`mem_hide` 的 `process_vm_readv` 路径未单独构造用例；小米 13（5.10）未测。

## 构建

```bash
export ANDROID_NDK_LATEST_HOME=/path/to/ndk
make all debug
# panda_hide_<版本>.kpm / panda_hide_<版本>_debug.kpm
```
