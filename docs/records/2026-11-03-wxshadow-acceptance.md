# wxshadow 1.2.5 验收记录

- 日期：2026-11-03
- 角色：维护者
- 模块：`modules/KPM/wxshadow`
- 版本：1.2.5
- 冻结实例：`wxshadow-1.2.5+g35459c6bca01.rfbb58a4a.kpb51197a.ndk26.1.10909125#1`
- 冻结提交：`340729b19c57ae01991a4c4781b7630a4913d19c`
- 产物：`artifacts/wxshadow-1.2.5+g35459c6bca01.rfbb58a4a.kpb51197a.ndk26.1.10909125/wxshadow_1.2.5.kpm`
  - 162,584 B
  - `sha256=fa24d9d017c416dd777e2046f10f2c2590a89a91804058afd5d9cdb95f10d4aa`
- 配方指纹：`fbb58a4a819637bcc46d8caccd547411ab22465374e66c278a3b1be3ca5ae37d`
- 工具链：`ndk26.1.10909125`；KernelPatch `b51197aaba8f2272dd8a3e30c85698a29aa928c9`

## 一、验收依据

1. 独立复核（三路，`Auditor/reports/2026-11-03-wxshadow-125-review.md`）：代码路与文档/身份路均判
   **可关单**，关闭 AUD-wx-4 / AUD-wx-13 / AUD-wx-14 与 AUD-019…AUD-027；复审确认路逐条复核通过。
2. 身份三要素复算：产物 sha256 与 `metadata`、handoff、归档 `MANIFEST.json` 三处一致；
   `report_versions` 记录的报告字节（`-text` 入库）可复算；`sources[]` 双哈希（提交 LF / 工作树 CRLF）逐条吻合。
3. 真机（两条，绑定同一字节 `fa24d9d0…`）：
   - Pixel 6（6.1.99-android14，APatch 11282，kernel 形态）：`load rc=0`；`status` → `wxshadow v1.2.5 uids: 0 2000`；
     `mm helpers: pfn_valid=1 mmap_read_lock=1`；`stage3 brk/step hooks ok (method=1)`；`set_bp` → `ret=7`、
     `--patch` → `ret=9`；`unload rc=0`、`cleaned 1 pages`，`/proc/uptime` 连续（不重启）。
   - 小米 13（5.10.149-android12，APatch LKM 形态）：`load rc=0`；`method=1`；
     `mm helpers: pfn_valid=1 mmap_read_lock=0`（5.10 无该符号，按设计降级为无锁走查 + pfn 校验）；
     `set_bp`/`--patch` → `ret=7`/`ret=9`；`unload rc=0`，`uptime` 连续。
4. 门禁：`python3 tools/check_repository.py --strict` → 15 项检查 0 失败 2 警告
   （警告为存量：zram handoff 缺 `handoff_by`/`handoff_at`、历史实例未登记）。

## 二、范围与边界

- 覆盖：release 单变体（本模块按流程不产出 `_debug` 件——`WXSHADOW_DEBUG_TASKS` 仅加日志、默认关闭，
  不构成独立调试用途）；页级双视图的 set_bp / patch / release / unload 全链路；ctl0 权限门与 allowlist 维护。
- 不覆盖（记录在案）：
  - **页粒度双视图的非确定性**：同一页内既有被测函数又有调用方代码时，读操作会把页切到 ORIGINAL，
    下一次取指又切回 SHADOW_X，紧循环里命中断点是概率性的（上游设计边界，已在 README 与本记录声明）。
  - **LKM `current_uid` 语义潜伏缺陷**（AUD-wx-11）：本仓 fork 提案 `f0c9e72` 未采用；两台设备
    `# CONFIG_USER_NS is not set`、`unshare -Ur` 返 `EINVAL`，无法复现，保持 open。
  - **5.10 无锁走查**（AUD-wx-17）：登记为降级路径的已知残余风险。
  - **死解析与 `%px` 日志**（AUD-wx-15）：低严重度，留待下一版随日志降噪清理。
  - AUD-wx-12（历史归档 `MANIFEST.json` 内含本机路径）：历史产物不可覆盖，判据归维护者，保持 open。
- 不适用：`load_forms` 双形态声明一致（本模块 17 个 UND 全为 KernelPatch 导出，两种加载形态共用同一份字节）。

## 三、结论

`wxshadow 1.2.5` 通过验收，绑定上述实例。后续变更须以新版本、新实例重新走复核与验收。
