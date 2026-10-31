# 开发报告：memcrc 0.0.1

## 身份

| 项 | 值 |
| --- | --- |
| commit | `91c941819e728d85289ece965005887fdd4f4d45`（memcrc 模块落地提交；本报告写于提交前，身份栏于提交后回填） |
| KernelPatch commit | `b51197aaba8f2272dd8a3e30c85698a29aa928c9`（子模块） |
| Build ID / instance_id | 未登记（探索构建，未走 `tools/build_candidate.py`） |
| 配方指纹 | 未登记 |
| 构建事务 | 手工 `make -C modules/KPM/memcrc all debug`（未登记） |
| 工具链 | clang 17.0.2（NDK 26.1.10909125，`aarch64-linux-android31-clang`） |
| 产物 | `memcrc_0.0.1.kpm` `sha256=29cd7c9273451231bfd845de6d022a05094f7e60d5159bcf4a792a47dd6dd1c6` `size=75032`；`memcrc_0.0.1_debug.kpm` `sha256=2a9f9e46d0a74db948b5d7e65b7932ee61f6fc059697720f993ad97160860d33` `size=75040` |

## 变更点

- 目标内核范围：Android 4.14～6.1（无结构体偏移依赖的通用 KPM；仅依赖 KP 导出的 `task_struct_offset`/`cred_offset`，见下）。
- 来源：`_repos/memcrc/src` 全量搬入 `modules/KPM/memcrc/`（18 个文件：`mod_main.c`、`symbols.h` + `control/ data/ feature/ komm/ ptrace/ redirect/` 六个子系统）。业务逻辑保持上游原样，改动集中在下面几处。
- 改了哪些 hook 点 / 偏移字段 / 数据结构：
  - `feature.c` 钩 `show_vfsmnt` / `show_mountinfo` / `show_vfsstat` / `show_map_vma` / `show_smap`（seq_file 输出过滤）。
  - `ptrace_hide.c` 钩 `proc_pid_status` / `do_task_stat`，并 `fp_hook_syscalln(__NR_ptrace, 4, …)`。
  - `redirect.c` 钩 `do_filp_open`（路径重定向）。
  - 偏移字段只用 `task_struct_offset.cred_offset` + `cred_offset.uid_offset` 取当前 uid（KP 导出，非猜值）。
- 为什么这样改：
  - `KPM_NAME("memcrc")`、`KPM_VERSION(MYKPM_VERSION)`：与目录/管理名一致，版本由 Makefile 单点定义（仓库惯例）。
  - 新增 `mod_control0`：上游 `mod_control0` 只打印 args，规则只能经 `/dev/yuuki_misc` ioctl 注入；本仓 `kpatch kpm ctl0` 是唯一不依赖文件节点的通道，故让它复用同一个 parser（`parser_is_command` → `parser_process_command`，否则 `shared_data_add_parsed`），回写 `rc=<n>`。
  - `compat.h` 定义 `HOOK_NOT_HOOK`：上游面向旧 KP 的 `hook_err_t` 有该枚举值，现行 KP 只剩 `HOOK_NO_ERR`/`HOOK_BAD_ADDRESS`/`HOOK_DUPLICATED`/`HOOK_NO_MEM`/`HOOK_BAD_RELO`/`HOOK_TRANSIT_NO_MEM`/`HOOK_CHAIN_FULL`。
  - **去掉 SELinux 旁路**：上游 `set_priv_selinx_allow()` 用 `get_task_ext()` 取 `task_ext` 置 `priv_sel_allow`。现行 KP 的 `kf_get_task_ext` 未导出（`kernel/patch/common/taskob.c:76` 定义，`:205` 只导出 `task_ext_size`），且 `task_ext` 已从内核栈顶迁到 KP 私有的 1024 槽表（`taskob.c:29-46` 注释），模块无法用偏移自行算出。详见「未解决项」。
  - `struct filename` 零初始化 + `refcnt = 0x40000000`：上游只写 `->name`，`refcnt`/`aname` 是 vmalloc 未初始化内存；若 `do_filp_open` 内部对入参调 `putname()`，`refcnt` 恰为 0 时会走 `__putname()` 用 `kmem_cache_free` 释放一个 vmalloc 指针。把 refcnt 置成大值使 `--refcnt == 0` 永不成立，释放权始终留在本函数。
  - `feature.h` / `redirect.h` / `komm.h` 补文件作用域前置声明：否则编译器在每个原型作用域里各造一个不可见的不完整类型（`-Wincompatible-pointer-types` / `-Wvisibility` 刷屏）。
  - `data/shared_data.c` 与 `data/parser.c` 的 `switch` 补 `default`（未处理 `SHARED_DATA_TYPE_INVALID`）；`trim_spaces` 加 `unused`（上游遗留、无调用点）。
- 与上游的逐文件差异共 12 个文件、111 行（`diff -ru _repos/memcrc/src modules/KPM/memcrc`；搬入文件的行尾已与上游对齐，否则整文件都会显示为改写。git 侧 `core.autocrlf=true`，入库统一存 LF）。除上述几处外业务逻辑零改动。
- 本轮覆盖范围：编译 + 导入表核对 + 门禁 + 实现方真机自检（见「真机自检（实现方，非 Tester 结论）」）。真机结论需 Tester 独立出具（见「边界声明」）。

## 自检

```text
$ make -C modules/KPM/memcrc clean all debug
0 warnings, 0 errors（release / debug 各一次全量重建）

$ llvm-readelf -sW memcrc_0.0.1.kpm | awk '$7=="UND"'
34 个未定义符号，全部为 KP 导出：
  compat_copy_to_user compat_strncpy_from_user cred_offset fp_unwrap_syscalln
  fp_wrap_syscalln get_random_u64 has_syscall_wrapper hook inline_unwrap_syscalln
  inline_wrap_syscalln kallsyms_lookup_name kf_memcpy kf_memmove kf_memset
  kf_snprintf kf_strcat kf_strchr kf_strcmp kf_strcpy kf_strlen kf_strncmp
  kf_strncpy kf_strnstr kf_strpbrk kf_strsep kf_strspn printk
  sp_el0_is_current sp_el0_is_thread_info task_in_thread_info_offset
  task_struct_offset thread_info_in_task thread_size unhook
（去掉 SELinux 旁路后 kf_get_task_ext 已不在导入表）

$ python tools/check_repository.py
12 项 PASS；1 项 FAIL = tool_selftest（Windows 环境：'gbk' codec 无法解码自检夹具输出，
  与本次改动无关，空树同样失败）；2 项 WARN 为存量（handoff 缺字段 / 未登记实例）。
ci_build_selection 已把 memcrc 计入构建列表（11 个模块）。
```

- 构建：`make -C modules/KPM/memcrc all debug`，警告 0 条（首次移植时 39 条，逐类修完）。
- 离线镜像回归：不适用（无偏移推导，`kernel_img/` 无镜像参与）。
- 代码走查结论：`mod_init` 五步（`ioctl_init`/`feature_init`/`redirect_init`/`syscall_init`/`ptrace_hide_init`）与 `mod_exit` 逆序对称；`init_symbols` 任一符号缺失即整体失败（不会半初始化）；`mod_exit` 未 `unhook` 的部分由各子系统 `*_exit` 负责。

## 边界声明

> 本轮结论来自实现方自检（构建 + 导入表 + 门禁），**不是独立审计，也不是实机结论**。
> 跨版本正确性、权限与生命周期安全、真机可用性分别由 Auditor 与 Tester 出具。

## 未解决项与降级行为

| 项 | 影响内核 | 当前行为 |
| --- | --- | --- |
| SELinux 旁路缺失（`get_task_ext` 未导出、`task_ext` 已迁表） | 全版本 | `do_filp_open` 重定向只在**调用者域本来就能打开 dst** 时生效；否则内核返回 `EACCES`，模块不做任何绕过。上游「app 读不到也能重定向到 /data/adb/…」的能力丢失。 |
| `struct file_operations` 为手写快照（含 `splice_eof`，缺 6.12+ 的 `fop_flags`） | 6.12+ | 6.12 起 `fop_flags` 插在 `owner` 之后，其后所有成员偏移 +8，`unlocked_ioctl` 会指到错误成员。**5.10 / 6.1 上被依赖的成员（`owner`…`release`）偏移未变，安全**；6.12+ 需重排该结构。 |
| `struct seq_file` 为手写 4 字段快照（`buf/size/from/count`） | 全版本 | 与上游生态（mount_hidder、kpm-panda-hide、mkpms）同一做法；只读 `count` 与写 `count`，未触碰后续字段。 |
| `ctl0` 通道未验证 | — | 设备镜像预置了 superkey，命令行 `kpatch` 拿不到 key；`ctl0` 与 `CMD:*` 命令只通过 `/dev/yuuki_misc` 验证过，未走 KPM 控制通道。 |
| 无偏移依赖 | — | 本模块不使用结构体偏移推导（`cred_offset`/`task_struct_offset` 取自 KP 导出变量），不需要 static/dynamic 双产物。 |
| 未声明许可证 | — | 上游仓库没有 LICENSE，源码也没有 `KPM_LICENSE`；本仓其余 13 个模块均有该字段。不替上游猜授权，元数据暂缺 `license`。若维护者要求统一，需先确认上游授权。 |

## 真机自检（实现方，非 Tester 结论）

设备：Pixel 6（oriole），Android 15，内核 6.1.99-android14-11-gd7dac4b14270-ab12946699，APatch 11282。模块由 APatch 管理器加载，`dmesg` 五路初始化全部成功（komm / feature / redirect / getcwd / ptrace_hide），`load_module_ex: [memcrc] succeed`。

注入通道：`control/control.c` 的 `getcwd` 钩子（填缓冲区后调 `getcwd()`），**不需要 root、不需要 superkey**；管理通道 `/dev/yuuki_misc` 用 `cat`（读规则）与 `echo "CMD:…" >`（写命令），需要 root。

| 能力 | 注入 | 实测 | 判据 |
| --- | --- | --- | --- |
| 路径重定向 | `MAP:0:/storage/emulated/0/Download/mc_a.txt:/storage/emulated/0/Download/mc_b.txt` | `cat mc_a.txt` 由 `AAA_MC` 变为 `BBB_MC` | 读 src 拿到 dst 内容 |
| maps 过滤 | `PATH:/system/bin/toybox` | `grep -c toybox /proc/self/maps` 由 4 → **0**；`CMD:CLEAR` 后恢复 4 | 命中行整条回滚 |
| maps 过滤（二例） | `PATH:/system/lib64/libnetd_client.so` | 由 4 → **0** | 同上 |
| mountinfo 打码 | `PATH:/config` | 总行数 144 不变，`grep -c /config` 由 1 → **0**，实际挂载点变成 `/0hKlIB` | 逐字符随机替换、保留 `/` |
| ptrace 状态隐藏 | `PTRACE:2000` | uid 2000 的 `sleep` 进程：`State: S (sleeping)` → **`R (running)`**；`/proc/<pid>/stat` 第三字段 `S` → `R`；`/proc/1/status` 不受影响 | 按目标任务 uid 匹配 |
| ptrace 系统调用拦截 | `PTRACE:2000` | uid 2000 调 `PTRACE_TRACEME` 得 `-1/EPERM`，`dmesg` 记 `[yuuki] Blocking ptrace for UID: 2000` | 钩子命中且返回 -EPERM |
| 规则读回 | — | `cat /dev/yuuki_misc` 输出 `[MAPPINGS:2] … [PATHS:7] … [PTRACE:1] … [END]` | komm 读路径可用 |
| 规则清空 | `CMD:CLEAR` | 三类计数归零，maps 过滤随之失效 | 状态可回收 |

实测踩到的两个坑（写进文档，便于后续复用）：

1. **规则里的路径必须与 `d_path()` 给出的真实路径逐字一致**。`/sdcard` 是符号链接，重定向按 `/sdcard/...` 写规则不生效；改用 `/storage/emulated/0/...` 才命中。
2. **libc 不在 `/system/lib64/`**。Android 15 上 bionic 位于 `/apex/com.android.runtime/lib64/bionic/libc.so`，用 `/system/lib64/libc.so` 做过滤规则会“看起来没生效”，实则是路径不存在。

未覆盖：重启后状态、嵌入模式、`ctl0` 通道、非 root 应用发起重定向（只验证了 uid 0 条目）、多规则并发下的锁竞争、6.12+ 内核。



## 已知可疑点（请 Auditor 重点看）

- **权限面**：`control/control.c:11-22` `before_getcwd` 用 `inline_hook_syscalln(__NR_getcwd, 2, …)` 钩 `getcwd`，把**用户缓冲区内容**当命令交给 `shared_data_add_parsed()` 执行。即任何无 root 应用调用 `getcwd()` 并预填缓冲区，就能注入全局 `MAP:`/`PATH:`/`PTRACE:` 规则（`uid 0` 的 MAP 条目对所有 uid 生效）。这是上游设计，本轮**未改**，请评估是否应加「只接受 uid==0 或管理器 uid」的门槛。
- `redirect.c` 在 `do_filp_open` 钩子里再调 `backup_do_filp_open`（同函数第二次进入），重定向链（A→B，B 又命中 A）时可能递归；上游无深度限制，建议加一层「本线程已在重定向中」的标记。
- `ptrace_hide.c` 对 `m->count` 做 memmove 调整，未校验 `m->buf` 剩余空间与 `count` 边界；`proc_pid_status` 输出被并发读时是否安全需确认。
- `data/shared_data.c` 的 `kf_raw_spin_lock_irqsave` 使用 KP 的 `_raw_spin_lock_irqsave` 指针；若目标内核该符号被内联化（部分 6.6+ 配置）则 `init_symbols` 失败，模块整体不加载（安全默认，但等于不可用）。
- 门禁 `tool_selftest` 在本机恒 FAIL（gbk 解码），本次未修；若交接前仍需绿，需要单独处理该夹具的编码问题。
