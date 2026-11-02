# mount_hidder

把指定的挂载项从 procfs 里藏掉。

来源：`mount_hidder`（GPL v2，作者 bmax121）的 `hide_mount.c`（110 行）移植。
钩 `show_vfsmnt` / `show_mountinfo` / `show_vfsstat` 三个 seq_file 输出函数：命中路径就回滚
`seq_file->count`，整条记录不出现；不命中就原样放行。

## 它做什么

- 三个钩子分别对应 `/proc/self/mountinfo`、`/proc/mounts`、`/proc/self/mountstats`。
- 命中判据见下面「匹配语义」：只在挂载点字段上按路径段边界比对。
- 默认路径表（上游原样保留，多数设备上并不存在，属无害配置）：
  `/debug_ramdisk`、`/apex/com.android.art/bin/dex2oat64`、`/apex/com.android.art/bin/dex2oat32`

## 匹配语义（1.0.1 起）

只在 mountinfo 的**挂载点字段**（第 5 个空格分隔字段）上比对，规则必须是绝对路径，命中以下任一即隐藏：

| 关系 | 例子 |
| --- | --- |
| 挂载点等于规则 | 规则 `/apex` 命中挂载点 `/apex` |
| 规则是挂载点的前缀，且下一字符是 `/` | 规则 `/apex` 命中 `/apex/com.android.art`；规则 `/data` **不**命中 `/data_mirror` |
| 规则是挂载点的后缀（规则自带前导 `/`，天然落在路径段边界） | 规则 `/persist` 命中 `/mnt/vendor/persist` |

1.0.0 是上游的任意子串匹配语义，`/data` 会误伤 `/data_mirror`，还可能在 root/source 字段里误命中（同版本内 R4d 曾加过一次字段边界检查，但匹配仍是整行子串）；
1.0.1 收紧为「只查挂载点字段 + 路径段边界」，同时保留「藏一棵子树」和「按尾部名字藏」两种用法。
1.0.2 再补三处 ctl0 健壮性：`del:` 长度上限、规则尾斜杠规整、参数尾部空白（含 CR/TAB）规整。

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
| 增量比较 | `char lstr[llen]`（VLA）再 `strstr` | 直接在 `m->buf + base` 上比对 | VLA 在栈上按用户可控长度分配，不必要 |
| 匹配语义 | 整行任意子串 | 只查挂载点字段 + 路径段边界（1.0.1） | 任意子串会让 `/data` 误伤 `/data_mirror`，也会在 source 字段误命中 |
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

默认三条路径在本机不存在，所以加载后行数不变（无假阳性）。1.0.1 用 ctl0 通道（`kpmctl`，管理器 uid）
做了逐项差分：

| 规则 | 观察 | 结果 |
| --- | --- | --- |
| `add:/apex` | 挂载点以 `/apex` 开头的记录 | 83 → **0**，总行数 146 → 63 |
| `add:/data` | `/data` 与 `/data_mirror` | 真实 `/data*` 4 条隐藏，`/data_mirror` 9 条**不受影响** |
| `add:/data_mirror` | 以 `/data_mirror` 开头 | 9 → **0** |
| `add:/persist` | 后缀匹配 `/mnt/vendor/persist` | 1 → **0** |
| `clear` | 全部规则 | 每次都能恢复到基线 |

`add` / `del` / `clear` / `status` 四条命令均已真机验证（1.0.0 时代这两条通道被输入法与 SELinux 堵住，
改用管理器 uid 直接发 supercall 后打通）。

## 构建

```bash
export ANDROID_NDK_LATEST_HOME=/path/to/ndk
make all debug
```

## 更新记录

### 1.0.2

- `del:` 补路径长度上限（`hm_del_path` 缺 `len >= HM_MAX_PATH_LEN` 检查，AUD-del-oob）。
- 规则尾斜杠规整：`add:/data/` 与 `add:/data` 等价（原先带尾斜杠永不命中子树，AUD-rule-slash）。
- ctl0 参数尾部空白（空格 / TAB / CR / LF）规整；`clear` 终止符白名单补 CR / TAB（AUD-ctl-ws）。
- 文档修正：`del:` 删不存在的规则返回 -1（AUD-d-1）；1.0.0「任意子串」措辞与 R4d 边界检查的关系（AUD-d-3）。

### 1.0.1

- 匹配语义改为「只查挂载点字段 + 路径段边界」（等于 / 前缀 / 后缀三种），修掉 `/data` 误伤 `/data_mirror`
  以及 source/root 字段误命中的问题（AUD-b-8 收口）。
- `ctl0` 的 `status` / `add:` / `del:` / `clear` 真机验证通过。

### 1.0.0

- 从上游 `hide_mount.c` 移植：三个 seq_file 钩子、起始计数 +1、去掉每次取 mm、`hook_unwrap_remove` 卸载、
  路径表可由加载参数维护。
