
## 模块


日志模块：
权限管理
磁盘扫描
块读取
vss模块
备份
通信
增量备份


## 优化

等 DiskScanner 稳定后，可以改：

SetupDiGetClassDevs
        |
        |
GUID_DEVINTERFACE_DISK
        |
        |
枚举磁盘设备