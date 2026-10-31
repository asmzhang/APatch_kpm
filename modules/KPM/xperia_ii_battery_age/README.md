# xperia_ii_battery_age

改 Sony Xperia II 的电池老化等级（battery aging level）。

来源：lzghzr 的同名模块（GPL v2）。通过 fuel gauge 的 SRAM 写入接口改老化等级字：

```c
#define SOMC_AGING_LEVEL_WORD    291   /* fg_sram 里的字地址 */
#define SOMC_AGING_LEVEL_OFFSET  0
#define FG_IMA_DEFAULT           0
```

## 用法

加载参数是一个数字 `0`–`5`（超过 5 直接拒绝加载）：

```bash
kpatch <superkey> kpm load xperia_ii_battery_age_1.1.2.kpm 2
```

之后可用 ctl0 随时改：

```bash
kpatch <superkey> kpm ctl0 xperia_ii_battery_age 3
# → success, set batt_aging_level to 3
```

## 实现要点

- 内核里没有现成的 `fg_dev` 指针，所以要**先钩一次 `fg_sram_read`**：第一次被调用时从 `arg0`
  拿到 `fg`，随即摘掉这个钩子，然后用拿到的 `fg` 调 `fg_sram_write` 写入等级。
- 因此：**开机后必须先有一次 fuel gauge 读取，等级才会真正写下去**。模块加载时若还没发生读，
  写操作会失败（日志 `error, rc=...`），等下一次 ctl0 或重启后再试即可。
- 依赖符号：`fg_sram_read`、`fg_sram_write`（`lookup_name` 解析，缺失则加载失败）。

## 边界

- 仅 Sony Xperia II（`fg_sram` 接口与字地址是该机型专用），别的机型写了也没意义。
- 老化等级只是上报值，不改变真实电池健康；影响的是系统对充电策略/容量的判断。
- ctl0 只认单个 ASCII 数字字符，不做多字符解析（`args ? *args - '0' : 0`）。

## 构建

```bash
export ANDROID_NDK_LATEST_HOME=/path/to/ndk
make
# xperia_ii_battery_age_<版本>.kpm
```
