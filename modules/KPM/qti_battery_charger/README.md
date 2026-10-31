# qti_battery_charger

改高通充电器上报的电池属性：电量下限与型号名。

来源：lzghzr 的同名模块（GPL v2）。钩 `battery_psy_get_prop`，在返回后按属性改值：

| 属性 | 改成 |
| --- | --- |
| `POWER_SUPPLY_PROP_CAPACITY` | 小于 10 时抬到 **10** |
| `POWER_SUPPLY_PROP_MODEL_NAME` | 固定写 **`SNYSCA6`** |

（源码里还留着注释掉的 `CYCLE_COUNT` / `CHARGE_FULL*` 改法，需要时取消注释即可。）

## 加载时机

`battery_psy_get_prop` 在高通充电器模块（`qti_battery_charger`）初始化之后才存在。所以 init 分两步：

1. 先试 `kallsyms_lookup_name("battery_psy_get_prop")`，拿到就直接挂钩；
2. 拿不到就钩 `do_init_module`，等名为 `qti_battery_charger` 的模块加载完成时摘掉这个钩子，
   再去挂 `battery_psy_get_prop`。

两条都失败才返回错误（`-1` / `-2`），不会静默成功。

## 命令

无 ctl0。参数与配置都在源码顶部：

```c
char MODULE_NAME[] = "qti_battery_charger";   /* 等哪个模块 */
char MODEL_NAME[]  = "SNYSCA6";               /* 要伪装成的型号 */
```

## 边界

- 只影响通过 `power_supply` 框架读到的值，直接读 fuel gauge 节点的路径不受影响。
- 型号名是固定长度拷贝（`sizeof(MODEL_NAME)`，含结尾 NUL），目标缓冲区短于 8 字节会越界——
  上游如此，本仓未改；如遇异常机型需先确认 `strval` 实际长度。
- 依赖内核符号 `battery_psy_get_prop` / `do_init_module`；非高通平台或符号被裁掉时加载会失败。

## 构建

```bash
export ANDROID_NDK_LATEST_HOME=/path/to/ndk
make
# qti_battery_charger_<版本>.kpm
```
