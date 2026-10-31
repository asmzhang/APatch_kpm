# 开发报告：panda_hide 2.0.0

## 身份

| 项 | 值 |
| --- | --- |
| commit | `75f62a4aca51bc25773706a437ded2d90a4a9ad9`（派生自该提交，`modules/KPM/panda_hide/` 本轮新增，见「变更点」） |
| KernelPatch commit | `b51197aaba8f2272dd8a3e30c85698a29aa928c9`（0.13.9） |
| Build ID / instance_id | 未登记（探索构建，未走 `tools/build_candidate.py`） |
| 配方指纹 | 未登记（同上，脏树阶段） |
| 构建事务 | `make all debug`（NDK 26.1.10909125，直接构建） |
| 工具链 | clang 17.0.2（`aarch64-linux-android31-clang`），linker `ld.lld` |
| 产物 | `panda_hide_2.0.0.kpm` `sha256=63188d5a69a9ab3abdbcdabbeb5dc8870d6f46263aaf6c49c12d988cf73c45fc` `size=24464` |
| 产物（debug） | `panda_hide_2.0.0_debug.kpm` `sha256=6369246c334fa93165bd0b6ccc328f99b604f8df785cf5c9de8ca1273b7ddb2f` `size=24464` |

## 变更点

- 目标内核范围：arm64，5.10 / 6.1（真机验证在 6.1.99-android14 上完成）；不使用任何结构体偏移，无 static/dynamic 双版本。
- 上游来源：`D:\work\1\kpm\_repos\kpm-panda-hide`（`src/` 6 个 `.c` + 6 个 `.h`，共 686 行，GPL v2，作者 pandaos）。
- 进仓结构：`modules/KPM/panda_hide/{Makefile,README.md,src/*}`，保留上游单目录多文件布局。
- 改了什么：
  1. `KPM_NAME("panda-hide")` → `KPM_NAME("panda_hide")`；`KPM_VERSION("2.0.0")` → `KPM_VERSION(MYKPM_VERSION)`；`Makefile` 定义 `MYKPM_VERSION := 2.0.0`。
  2. 全部 `unhook(fn)` → `hook_unwrap_remove(fn, before, after, 1)`，共 7 处（`debugger_hide.c` 4、`frida_hide.c` 2、`mem_hide.c` 1）。理由：KP 的 `hook_chain_t` 首成员就是 `hook_t`，`unhook()` 对 wrap 链不会走 `hook_chain_remove` 清理路径，链节点与 trampoline 残留。
  3. `mem_hide.c` 的擦除长度从 `args->arg3`（请求长度）改为 `args->ret`（实际读到的字节数，并夹到 `len` 以内）。理由：`access_remote_vm` 只填了 `ret` 个字节，其余是内核页缓冲区的旧数据。
  4. `net_hide.c` 的 adbd 放行逻辑：`comm` 初值清零，`__get_task_comm` 解析失败时按「非 adbd」处理（即**拦截**）。上游把 adbd 判断整个包在 `if (__get_task_comm_fn)` 里，符号缺失时会放行全部连接（fail-open）。
  5. `main.c`：`event`/`args` 增加空值兜底；`strncat(echo, args, 48)` 改为按 `sizeof(echo) - sizeof("echo: ") - 1` 限制，避免 `args` 长度不足 48 时越界读。
  6. `frida_hide.c`：`__attribute__((optimize("O0")))` → `__attribute__((optnone))`（clang 不认前者，会报 `-Wunknown-attributes`，共 2 处）。
  7. 日志前缀 `panda-hide:` → `panda_hide:`（与模块名一致）；`KPM_DESCRIPTION` 同步。
- 为什么这样改：前六条都是「不改判据、只改实现正确性」的机械修正；第 7 条是命名契约一致性。
- 本轮覆盖范围：全量移植五个子系统（debugger / frida / openat / net / mem），没有裁剪上游功能。
- 未覆盖原因：上游的 `KPM_CTL1` 空实现在本仓保留但不使用（无 `ctl1` 语义）。

## 自检

```text
$ make clean && make all debug
aarch64-linux-android31-clang -Wall -O2 ... -DMYKPM_VERSION=\"2.0.0\" -c -o src/main.c ...
（0 warning / 0 error）

$ strings panda_hide_2.0.0.kpm | grep ^version=
version=2.0.0
$ strings panda_hide_2.0.0_debug.kpm | grep ^version=
version=2.0.0_d

$ llvm-readelf -sW panda_hide_2.0.0.kpm | awk '$7=="UND"{print $8}' | sort -u
compat_copy_to_user  compat_strncpy_from_user  fp_unwrap_syscalln  fp_wrap_syscalln
has_syscall_wrapper  hook_unwrap_remove  hook_wrap  kallsyms_lookup_name
kf_memcmp  kf_memcpy  kf_memset  kf_strcmp  kf_strlen  kf_strncat  kf_strstr
kpver  printk  sp_el0_is_current  sp_el0_is_thread_info
task_in_thread_info_offset  thread_info_in_task  thread_size
```

- 构建：`make -C modules/KPM/panda_hide all debug`（警告变化：首版 2 条 `-Wunknown-attributes`，改 `optnone` 后 0 条）。
- 导入表核对：22 个 UND 全部为 KernelPatch 导出符号。逐个确认：
  `kf_memcmp/kf_memcpy/kf_memset/kf_strcmp/kf_strlen/kf_strncat/kf_strstr` 来自 `kernel/patch/ksyms/libs.c`（`KP_EXPORT_SYMBOL(kfunc(...))` 形式导出，符号名带 `kf_` 前缀），
  `kpver` 来自 `kernel/base/start.c`，其余为 hook/syscall/线程信息基础设施。**无新增外部依赖**。
- 离线镜像回归：本模块不依赖结构体偏移，未跑 `kernel_img/offset_harness`（无可推导项）。
- 代码走查结论：五个子系统的钩子与还原路径配对完整；卸载顺序与安装逆序（`exit` 里 mem → net → openat → frida → debugger）。

## 真机自检（实现方，非 Tester 结论）

- 设备：Pixel 6（设备指纹 `sha256:433ab88276ec`，Android 15，内核 6.1.99-android14-11），APatch 11282。
- 加载/卸载方式：APatch 管理器 UI（「内核模块」→ 右下 FAB → 加载 → SAF 选 `panda_hide_2.0.0.kpm`；卸载走卡片「卸载」+ 确认框），全程未用 superkey。
- 加载 dmesg（原文节选）：

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

- 差分表（探针 `phide`/`tname`，均为静态 aarch64 二进制，uid 2000 shell 身份运行）：

| 信号 | 卸载态 | 加载态 |
| --- | --- | --- |
| `/proc/<pid>/status` `TracerPid:` | `6765`（真实 tid） | `0` |
| `/proc/<pid>/status` `State:` | `t (tracing stop)` | `S (sleeping)` |
| `/proc/<pid>/wchan` | `ptrace_stop` | `0` |
| `/proc/<pid>/stat` 第三字段 | `t` | `S`（连测 3 次一致） |
| `/proc/<pid>/mem` 读 `LIBFRIDA_MARKER_12345` | 原样 32 字节 | 32 字节全 0 |
| `connect` 127.0.0.1:27042 | `rc=0` | `rc=-1 errno=111`（ECONNREFUSED） |
| `/proc/self/maps` 含 `frida_payload.bin` | 1 行 | 0 行 |
| 创建 `/data/local/tmp/frida-agent-64.so` | 成功 | `ENOENT` |
| 创建 `/data/local/tmp/normal.txt` | 成功 | 成功（负对照，不误伤） |
| 线程名 `gum-js-loop` | `gum-js-loop` | `binder` |
| 线程名 `my-worker-1` | `my-worker-1` | `my-worker-1`（负对照） |

- 卸载 dmesg：`panda_hide: {mem,net,openat,frida,debugger} hide uninstalled` → `unload_module: name: panda_hide, rc: 0`；卸载后全部信号回到上表左列基线。
- 未覆盖项：
  1. 只测 release 变体（仓库规范：真机每目标只测一个变体，debug 差异仅版本串与日志开关）。
  2. `ctl0` 只回显参数，无配置语义，未单独测。
  3. `mem_hide` 的 `process_vm_readv` 路径未单独构造用例（只测 `/proc/<pid>/mem`）。
  4. 小米 13（5.10.149-android12-9）未测；`seq_put_decimal_ull`/`proc_pid_wchan` 在 5.10 上同样存在，但未实测。
  5. 多模块共存场景（与 memcrc 的 `do_task_stat` 钩子同时生效）只做了「同机同时加载」的观测，未做交叉判据矩阵。

## 边界声明

> 本轮结论来自实现方自检（构建 + 导入表核对 + 门禁 + 实现方真机自检），**不是独立审计，也不是 Tester 结论**。
> 跨版本正确性、权限与生命周期安全、真机可用性分别由 Auditor 与 Tester 出具。

另需注意本模块的**用途边界**：它是「藏痕迹」，不是「防调试」。已 attach 的调试器不受影响；
`mem_hide` 只处理 `access_remote_vm` 这一条读路径；maps 判据里的 `gmain` 子串容易误伤。

## 未解决项与降级行为

| 项 | 影响内核 | 当前行为 |
| --- | --- | --- |
| `seq_put_decimal_ull` / `seq_puts` / `proc_pid_wchan` / `do_task_stat` 任一缺失 | 5.10+ | 跳过该钩子，`pr_warn` 提示，其余子系统照常 |
| `show_map_vma` / `__get_task_comm` 缺失 | 5.10+ | 跳过，maps 与线程名隐藏失效，模块仍加载 |
| `__arch_copy_from_user` 缺失 | 5.10+ | `connect` 钩子不安装（无法读 `sockaddr`），`pr_warn` |
| `access_remote_vm` 缺失 | 5.10+ | `mem_hide` 跳过 |
| 内核 < 5.10（无 `access_remote_vm` 签名或多参数差异） | — | 未验证；按符号逐个降级，不会加载失败 |

## 已知可疑点（请 Auditor 重点看）

- `mem_hide.c` 的 `scrub_frida_signatures()` 在**被别的进程读内存**的路径上做 `memset(buf + i, 0, sig_len)`，
  `buf` 是内核调用方提供的页面。长度已夹到 `args->ret` 且判过 `ret > 0`，但仍属「在别人提供的缓冲区上写」，
  建议重点核对 `access_remote_vm` 的调用点是否会传入长度不足的缓冲区，以及写方向 `gup_flags & FOLL_WRITE` 的判定是否覆盖全部调用形态。
- `debugger_hide.c` 的 `before_seq_put_decimal_ull()` 用 `strcmp((char *)args->arg1, "\nTracerPid:\t")` 直接读内核指针，
  没有长度校验；`before_seq_puts()` 同理。若上游调用点传入非 NUL 结尾串会发生越界读——上游如此，本仓未改。
- `frida_hide.c` 的 `is_hidden_map()` 临时把 `start[new_len]`（`m->buf` 内）写成 `\0` 再还原，
  当 `new_len == m->size`（缓冲区写满）时会改到 `m->buf` 边界外一个字节。上游如此，本仓只加了 `new_len == 0` 的早退。
- `openat_hide.c` 的路径匹配是 256 字节栈缓冲，超长路径会被 `compat_strncpy_from_user` 截断，
  截断后仍可能命中前缀（属宽松匹配，不是漏洞，但会漏判/误判）。
- `net_hide.c` 只拦 `connect`，不拦 `sendto`/UDP；`adbd` 放行按 `comm` 子串判断，改名即可绕过（上游设计如此）。
- `main.c` 的 `KPM_CTL1` 空实现被注册，但本仓未定义任何 `ctl1` 语义——若上层误用 `ctl1` 会得到 `0`（成功）。
