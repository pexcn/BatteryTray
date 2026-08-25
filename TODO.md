# TODO

## 面板：剩下的静态字段

**状态**：未做，收益最小，不做也行。信息面板本体已经上了（见规格 2.10），
这里指的是面板还没显示的几项：

| 显示项 | 来源 | 备注 |
| --- | --- | --- |
| 化学类型 | `BATTERY_INFORMATION.Chemistry[4]`，如 `LION` / `LiP` | 已经在查了，只是没往上传；**非 null 结尾**，按定长 4 字节读 |
| 厂商 / 型号 / 序列号 | `IOCTL_BATTERY_QUERY_INFORMATION` + `BatteryManufactureName` / `BatteryDeviceName` / `BatterySerialNumber` | 需新加查询，返回宽字符串 |

都是静态字段，接进面板打开时那一次读取即可，不进每 2 秒的 tick。降级规则照旧：拿不到就整行不画。

**面板会因此变高**，而这几项是「看一次就够」的信息 —— 加之前先想清楚值不值得让每次打开面板都多几行。

## 待在实机上确认

`BatteryTemperature` 与 `BatteryManufactureDate` 的查询已经写了，但**没有在实机上验证过给不给值**。
先跑 `powercfg /batteryreport` 对一遍：那份报告里空着的字段，代码里也一定拿不到。
拿不到不是 bug（整行不画就是为这种情况设计的），但**如果实机确认永远拿不到，可以考虑把对应的查询删掉**，
省一次 IOCTL 和一段布局代码。
