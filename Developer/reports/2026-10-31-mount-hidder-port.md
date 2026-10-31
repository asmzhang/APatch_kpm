# 开发报告：mount_hidder 1.0.0

## 身份

| 项 | 值 |
| --- | --- |
| commit | `c223060ccfe07d83b16a9017568b9cb150ed48ed`（mount_hidder 模块落地提交；本报告写于提交前，身份栏于提交后回填） |
| KernelPatch commit | `b51197aaba8f2272dd8a3e30c85698a29aa928c9`（子模块，0.13.9） |
| Build ID / instance_id | 未登记（探索构建，未走 `tools/build_candidate.py`） |
| 配方指纹 | 未登记 |
| 构建事务 | 手工 `make -C modules/KPM/mount_hidder all debug`（未登记） |
| 工具链 | clang 17.0.2（NDK 26.1.10909125，`aarch64-linux-android31-clang`） |
| 产物（当前源码） | `mount_hidder_1.0.0.kpm` `sha256=0100faa348f0e7e233702b248b9ba31eeeaa0c6f1b50f07dc36cc33c85145425` `size=10152`；`mount_hidder_1.0.0_debug.kpm` `sha256=0a2aed56401c1815b870f437d72989e8cbaf64a1b6922e407b5bab22e05ba7ae` `size=10152` |
| 真机测过的产物 | `mount_hidder_1.0.0.kpm` `sha256=54ef82bce7976420cb00ed18b286ad464606b313852262f6d534987980737a0f` `size=10160`（含 ctl0 add/del/clear，本表「差分验证」与「加载挂钩」两节均基于它）。审计后仅改了 `hide_mount_control0` 的返回值（`return 0` → `return rc`，让 add/del 失败对调用方可见），因此当前源码产物与上表差 8 字节；该差异只影响 ctl0 返回码，随 ctl0 一起重测。 |
| 上游 | `_repos/mount_hidder`（`hide_mount.c` 110 行 / `hide_mount.h` 9 行 / `README.md` 2 行 / `LICENSE`） |

## 变更点

- 目标内核范围：Android 4.14～6.1（无结构体偏移依赖的通用 KPM；不引用任何 `*_offset` 变量）。
- 来源：`_repos/mount_hidder` 全量搬入 `modules/KPM/mount_hidder/`（`hide_mount.c`、`hide_mount.h` 保留原名，新增 `Makefile`）。
- 改了哪些 hook 点 / 偏移字段 / 数据结构：
  - 钩 `show_vfsmnt`（`/proc/mounts`）、`show_mountinfo`（`/proc/self/mountinfo`）、`show_vfsstat`（`/proc/self/mountstats`）三个 seq 输出回调，用 `hook_wrap2`（before 记 `m->count`，after 命中关键字则把 `m->count` 回滚到记下的值，等价于整条丢弃）。
  - 无偏移字段：只读 `struct seq_file` 的 `buf`/`size`/`count` 三个字段。
- 为什么这样改：
  - `KPM_NAME("mount_hidder")`、`KPM_VERSION(MYKPM_VERSION)`：与目录/管理名一致，版本由 Makefile 单点定义（仓库惯例）。上游写死 `KPM_NAME("hide_mount")` / `KPM_VERSION("1.0.0")`。
  - `KPM_AUTHOR` 由上游占位值 `"test"` 改为版权头里的 `bmax121`；`KPM_DESCRIPTION` 由 `"test"` 改为说明性文本。
  - **去掉 `get_task_mm(current)` 判空**：上游在 `before` 里每次 seq 输出都调一次 `get_task_mm` 却从不 `mmput`。`get_task_mm` 内部 `mmget()`（`mm_users++`），引用只增不减 → 每读一次 `/proc/self/mountinfo` 就给读进程的 mm 漏一个引用，读进程退出时 mm 不会被释放（页表等整块泄漏）。而该判空的唯一目的是跳过内核线程，`seq_file` 指针在内核线程里同样有效，去掉后行为不变。
  - **起始 count 改存 `count + 1`**：上游把 `m->count` 原值存进 `args->local.data0`，而 `after` 用 `if (args->local.data0)` 判断"是否记录过"。`m->count == 0`（缓冲区空、第一条挂载项）时 data0 为 0，after 直接跳过 → **第一条挂载项永远过滤不掉**。改成存 `count + 1`，0 专门表示"没记录"。同时 `before` 无条件先写 `data0 = 0`，因为 KP 的 `_transit4` 里 `hook_fargs4_t fargs` 是栈上局部变量，`local` 字段未初始化。
  - **去掉变长数组**：上游 `char lstr[llen]`（`llen = m->count - bcount`，最大可到 seq 缓冲区大小）在内核栈上开 VLA，无上界 → 栈溢出风险。改为直接在 `m->buf + base` 上用 `strnstr(..., len)` 定长匹配，不再拷贝。
  - `hide_mount_exit` 补 `return 0`（上游声明 `static long` 却没有返回语句）。
  - 卸载改用 `hook_unwrap_remove(func, before, after, 1)`：上游用 `unhook()`，而 `hook_wrap2` 装的是 `hook_chain_t`（`hook_t` 只是它的首成员），`unhook()` 不会走 `hook_chain_remove` 的清理路径。
  - 路径表可由加载参数覆盖：`args` 为逗号/分号/空白分隔的绝对路径（最多 8 条、每条 < 80 字节），为空时用上游那三条默认值。理由：上游三条硬编码（`/debug_ramdisk`、`/apex/com.android.art/bin/dex2oat64`、`dex2oat32`）只覆盖 Magisk 系痕迹，换 ROM/换工具链就得改源码重编。
  - `ctl0` 由上游的空实现改为打印当前生效的路径表与钩子数，便于真机确认加载参数是否生效。
  - `init` 在三个 `show_*` 一个都解析不到时返回 `-1`：上游会静默返回 0 留下一个什么都不做的空壳模块。
- 与上游的逐文件差异：`hide_mount.h` 增加 include guard 与注释（字段布局与上游一致，仍是 5 个字段）；`hide_mount.c` 按上述条目重写，业务模型（before 记位置 / after 回滚整条）保持上游原样。
- 本轮覆盖范围：编译 + 导入表核对 + 门禁。真机结论见下节，需 Tester 独立出具。

## 自检

```text
$ make -C modules/KPM/mount_hidder clean all debug
0 warnings, 0 errors（release / debug 各一次全量重建，clean 后重建哈希不变）

$ llvm-readelf -sW mount_hidder_1.0.0.kpm | awk '$7=="UND"'
11 个未定义符号，全部为 KP 导出：
  compat_copy_to_user hook_unwrap_remove hook_wrap kallsyms_lookup_name
  kf_memcpy kf_memset kf_snprintf kf_strlen kf_strncmp kf_strnstr printk
无 __aarch64_* libgcc 原子符号（本模块不使用 __atomic/__sync）

$ strings -a mount_hidder_1.0.0.kpm | sed -n '7,11p'
name=mount_hidder
version=1.0.0
license=GPL v2
author=bmax121
description=hide mount entries from procfs (hide_mount upstream port)

$ python tools/check_repository.py
12 项 PASS，1 项 FAIL（tool_selftest：'gbk' codec can't decode byte 0xba，
空树同样失败，Windows 存量问题），2 项 WARN 为存量。
ci_build_selection 已把 mount_hidder 计入（12 个模块）。
```

- 构建确定性：`make clean && make all debug` 两次，两份产物 sha256 均不变。
- release 与 debug 体积相同（10152B）：本模块没有 `#ifdef CONFIG_DEBUG` 分支，`logkd` 在两种变体里都在（上游也是无条件 `logkd`）。两版仅版本串 `1.0.0` / `1.0.0_d` 不同。

## 真机自检（实现方，非 Tester 结论）

设备：Pixel 6（设备指纹 `sha256:433ab88276ec`，序列号不入库），Android 15，内核 `6.1.99-android14-11-gd7dac4b14270-ab12946699`，APatch 11282 / KernelPatch 0.13.9。
产物：`mount_hidder_1.0.0.kpm`，经 APatch 管理器「加载」从 `/sdcard/Download/` 载入。

### 加载与挂钩

```text
[+] KP D hide_mount: init paths=3 hooks=3 (vfsmnt=ffffffd2631f6fa4 mountinfo=ffffffd2631f737c vfsstat=ffffffd2631f76bc)
[+] KP I load_module_ex: [mount_hidder] succeed with [(null)]
```

三条 `show_*` 在 6.1.99 上全部由 `kallsyms_lookup_name` 解析到并挂上，`hooks=3`。

### 默认路径：本机不存在，无假阳性

加载前后 `/proc/self/mountinfo`、`/proc/mounts`、`/proc/self/mountstats` 均为 144 行（`linkerconfig=2`、`data_mirror=9` 不变）。设备命名空间里没有 `/debug_ramdisk`、`dex2oat`、`overlay`、`magisk`、`apatch` 痕迹，默认三条不命中属预期。

### 差分验证（真实挂载，A/B）

默认路径在本机不存在，故造一个命中默认表第一项的挂载：`/data/local/tmp/debug_ramdisk` 含子串 `/debug_ramdisk`。

```text
$ su -c 'mkdir -p /data/local/tmp/debug_ramdisk && mount --bind /system /data/local/tmp/debug_ramdisk'
$ ls /data/local/tmp/debug_ramdisk | wc -l      # 15 → 挂载确实生效（与 procfs 无关的旁证）
```

| 状态 | `/proc/self/mountinfo` 行数 | `grep -c debug_ramdisk` |
| --- | --- | --- |
| 模块已加载 | 144 | 0 |
| 模块已卸载 | **145** | **1** |
| 重新加载 | 144 | 0 |

挂载真实存在（`ls` 能看到 15 项），但它的 mountinfo 行在模块加载时整条消失、卸载后立刻回来——证明是钩子在 `show_mountinfo` 里回滚了这条记录，而不是挂载本身失败。

### 未在真机验证的项

- `ctl0` 的 `status` / `add:<path>` / `del:<path>` / `clear` 四条命令均未送达。两条通道都被堵：
  - APatch 管理器「参数」对话框用 `adb shell input text` 注入时，Gboard 把半角 `:` `/` 换成全角（实测回显 `add：／`），命令解析必然失败；
  - `kpatch kpm ctl0` 需要管理器 uid 或 superkey——`supercall.c:400-440` 里 `is_su_allow_uid` 只置 `is_trusted_caller=1`，`is_authed` 仍为 0，root/su 过不了 `KPM_CONTROL` 的 `if (!is_authed) return -EPERM`。
  留待 Tester 在真机上用软键盘手输。
- 重启后状态、嵌入模式、非默认路径、多规则并发、`/proc/mounts` 与 `/proc/self/mountstats` 的逐条对照（本轮只逐条对照了 mountinfo）。

### 清理

`umount /data/local/tmp/debug_ramdisk` + `rmdir`，管理器里卸载模块（`unload_module: name: mount_hidder, rc: 0`），设备临时文件已删，输入法已恢复为 Gboard。

## 边界声明

- 只处理 `show_vfsmnt` / `show_mountinfo` / `show_vfsstat` 三个 seq 输出回调。以下**不在**本模块职责内：
  - `/proc/self/mounts` 之外的其它暴露面（`/proc/self/fdinfo`、`/proc/self/mountstats` 之外的 debugfs、`/proc/filesystems`、`/proc/self/mountinfo` 的 `nsfs`/`bpf` 行等）；
  - VFS 层直接 `d_path()`/`readlink()` 拿到的路径（本模块不改 inode/路径）；
  - `statfs`/`statvfs` 的 `f_type`（overlay/tmpfs 类型本身仍可见）；
  - SELinux 策略、文件系统层痕迹。
- 三个 `show_*` 的签名按 `(struct seq_file *, struct vfsmount *)` 处理（4.x～6.1 实测成立；memcrc 的同名钩子在 6.1.99 与 5.10.149 上真机验证过）。若某内核把 `show_mountinfo` 改成三参数，`arg0` 仍是 `seq_file *`，不影响本模块取 `buf`/`count`，但回滚点仍正确。
- 若目标内核里 `show_*` 被内联（seq_operations 表里直接指向调用者）则 `kallsyms_lookup_name` 取不到，此时该条通道自动不生效；三条全部取不到时 `init` 返回 -1（加载失败而不是假成功）。
- 与 memcrc 的 `feature` 子系统功能重叠（memcrc 也钩这三个函数）。两者同时加载时：两个 wrap 会按链顺序执行，各自回滚到自己的起点，不会互相破坏，但隐藏范围是两者并集。不建议同时加载。

## 未解决项

1. **`m->count` 回滚的可见副作用**：整条丢弃会让 `seq_read` 的 `from`/`count` 计算少掉这一条，`/proc/self/mountinfo` 的总行数会变少（上游同样如此）。若某个 App 依赖"行数不变"（例如按行号索引），可能成为新的可观测差异。是否改成"等长打码"（像 memcrc 的 `replace_sensitive`）需要单独评估。
2. **跨调用分片**：若某条挂载项的文本由两次 `show_*` 调用拼成（seq 缓冲区满导致 `seq_buf` 分片），关键字可能落在两次调用的边界上而漏检。当前内核里每条挂载项都是一次 `show_*` 写完，未观察到分片。
3. **`strnstr` 依赖**：`kf_strnstr` 是 KP 导出（`kernel/linux/include/linux/string.h:322` 的 `static inline strnstr` 直接调 `kfunc_direct_call`），已确认在导入表里。
4. **路径长度**：`HM_MAX_PATH_LEN 80`，超长路径在 `hm_add_path` 里被静默丢弃（不报错）。ctl0 只打印已生效的路径，可用它确认。

## 已知可疑点（请 Auditor 重点看）

1. `hm_after` 里 `m->count = base` 之前只校验了 `m->count > base` 与 `m->count <= m->size`，没有校验 `m->buf` 是否真的是这块 `seq_file` 的缓冲区；若目标内核的 `struct seq_file` 布局在 `count` 之前有变动（本仓按 4.x～6.x 稳定布局假设 `buf/size/from/count`），会写坏内存。这是本模块唯一的强假设。
2. `hook_wrap2` 装的是链式 hook，`before`/`after` 在同一 `fargs` 上串行执行；若另一个模块也 wrap 同一个函数并修改 `local.data0`，两者会互相覆盖。当前仓库没有别的模块用 `hook_wrap` 钩这三个函数（memcrc 用 `hook()` 整体替换）。
3. `hide_mount_init` 在 `kallsyms_lookup_name` 返回 0 时把 `hm_show_*` 留空并跳过安装，但 `hm_hook_count` 仍可能为 0 → 返回 -1 让加载失败。若用户只想要其中一条通道，这个"全失败才报错"的判据可能过严。
4. `ctl0` 的 `snprintf` 返回值累加在缓冲区写满时可能超过剩余长度，代码里已做 `left <= 1` 与 `n >= sizeof(buf)` 两处夹取，但 `n += snprintf(...)` 在截断时返回的是"本该写入的长度"，夹取逻辑依赖这两处保护，请复核边界。
