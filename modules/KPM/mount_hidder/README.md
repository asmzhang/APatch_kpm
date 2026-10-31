# mount_hidder

把指定的挂载项从 procfs 里藏掉。

来源：`mount_hidder`（GPL v2，作者 bmax121）的 `hide_mount.c`（110 行）移植。
钩 `show_vfsmnt` / `show_mountinfo` / `show_vfsstat` 三个 seq_file 输出函数：命中路径就回滚
`seq_file->count`，整条记录不出现；不命中就原样放行。

## 它做什么

- 三个钩子分别对应 `/proc/self/mountinfo`、`/proc/mounts`、`/proc/self/mountstats`。
- 命中判据是**子串**匹配（`strnstr`），所以给 `/debug_ramdisk` 就能连 `/debug_ramdisk/xxx` 一起藏。
- 默认路径表（上游原样保留，多数设备上并不存在，属无害配置）：
  `/debug_ramdisk`、`/apex/com.android.art/bin/dex2oat64`、`/apex/com.android.art/bin/dex2oat32`

## 命令（ctl0）

| 命令 | 作用 |
| --- | --- |
| `status` | 打印 `paths=N hooks=N` 与当前路径表 |
| `add:<路径>` | 追加一条（最多 8 条） |
| `del:<路径>` | 删除一条 |
| `clear` | 清空路径表（钩子保留，但不再命中任何路径） |

加载参数也可直接给路径（空格分隔），例如 `kpatch <key> kpm load mount_hidder.kpm "/debug_ramdisk /foo"`。

## 与上游的差异

| 项 | 上游 | 本移植 | 原因 |
| --- | --- | --- | --- |
| 起始计数 | `data0 = m->count` | `data0 = m->count + 1` | 上游在 `count == 0` 时算出的增量会漏掉第一条挂载项 |
| 增量比较 | `char lstr[llen]`（VLA）再 `strstr` | 直接在 `m->buf + base` 上 `strnstr` | VLA 在栈上按用户可控长度分配，不必要 |
| 卸载 | `unhook()` | `hook_unwrap_remove(addr, before, after, 1)` | `hook_chain_t` 首成员是 `hook_t`，用 `unhook` 卸 wrap 链不走 `hook_chain_remove` 清理路径 |
| 退出 | 无 `return` | `return 0` | 未定义行为 |
| 取 mm | 每次 `get_task_mm(current)` 且不 `mmput` | 去掉 | 每读一次 mountinfo 漏一个 mm 引用 |
| 路径表 | 编译期三条常量 | 加载参数 / ctl0 可维护 | 不同设备要藏的路径不一样 |

## 真机自检（实现方，非 Tester 结论）

Pixel 6（Android 15，6.1.99-android14）：加载后 dmesg 显示三个符号全部解析并挂钩成功——

```
[+] KP D hide_mount: init paths=3 hooks=3 (vfsmnt=... mountinfo=... vfsstat=...)
[+] KP I load_module_ex: [mount_hidder] succeed with [(null)]
```

默认三条路径在本机不存在，所以加载后行数不变（144 行，无假阳性）。差分验证用 bind mount 造了一个
真实命中 `/debug_ramdisk` 的挂载：

| 状态 | `/proc/self/mountinfo` 行数 | `grep -c debug_ramdisk` |
| --- | --- | --- |
| 模块已加载 | 144 | 0 |
| 模块已卸载 | **145** | **1** |
| 重新加载 | 144 | 0 |

`ctl0` 的 `add:` / `del:` / `clear` 未在真机验证（管理器参数对话框的输入被输入法改写，
两条下发通道都受 SELinux 限制），留给 Tester 用软键盘手输。

## 构建

```bash
export ANDROID_NDK_LATEST_HOME=/path/to/ndk
make all debug
```
