# wxprobe —— `struct break_hook` 的 ABI 探针

一次性诊断工具，**不是模块**：不参与 `modules/` 的构建、不进 metadata、不发布。

存在的原因只有一个：wxshadow 移植前，唯一「读源码定不了」的事是内核 `struct break_hook` 的布局。
wxshadow 自己抄了一份结构体交给 `register_user_break_hook()`，布局若差一个字段就会踩到别人的函数指针。
这个探针把那条假设变成一次可复现的实测。

结论（2026-11-02，Pixel 6 / 6.1 + 小米 13 / 5.10 均通过）与踩坑记录写在
[docs/records/2026-11-02-wxshadow-feasibility.md](../../../docs/records/2026-11-02-wxshadow-feasibility.md) 的 §10。

## 组成

| 文件 | 作用 |
| --- | --- |
| `wxprobe.c` | KPM：解析 `register_user_break_hook` / `unregister_user_break_hook`，注册 `imm=0x5758`、`mask=0` 的用户态 BRK 钩子，回调里计数并 `regs->pc += 4` |
| `brk_client.c` | 用户态客户端：`brk_client` 执行 `BRK #0x5758`（`0xd42aeb00`），`brk_client control` 执行 `BRK #0x1234`（`0xd4246800`）当对照 |
| `run_on_device.sh` | 上机脚本：推文件 → 加载 → 跑客户端 → 回读 `hits=` |
| `Makefile` | 与模块同一套 KP 头文件路径，`KP_DIR` 默认指向仓库内的 `KernelPatch/` |

## 用法

```bash
export ANDROID_NDK_LATEST_HOME=<ndk>
make -C Developer/tools/wxprobe                    # 生成 wxprobe_0.0.1.kpm
bash Developer/tools/wxprobe/run_on_device.sh <serial> <manager_uid>
```

管理器 uid：Pixel 6 是 10273，小米 13 是 10270（`kpmctl` 必须 root 起再 setuid 到管理器 uid，
因为 KP 只对 `is_trusted_manager_uid` 置 `is_authed`）。

## 判据

- `brk_client` 打印 `after brk (returned)` 且 `rc=0` → 钩子命中且 `pc` 前移生效；
- `brk_client control` 打印 `SIGTRAP (no hook)` 且 `rc=3` → 匹配是按 `imm` 精确的，不是「谁都拦」；
- `kpmctl <uid> ctl wxprobe status` 的 `hits=` 与 `pc=` / `esr=` 跟着变 → 回调拿到的 `pt_regs` 是有效的。

## 注意

`mask` 的语义是「置 1 的位**不**比较」（`(comment & ~mask) == imm`）。写成 `0xffff` 不会报错，
只会永远不命中 —— 第一版就栽在这里，详见可行性记录 §10。
