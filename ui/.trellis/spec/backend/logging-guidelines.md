# 日志
使用Python logging：初始化错误ERROR，连接异常WARNING；不逐样本打印。源时间ns与receive_ns分别保留，不能在未同步主机上把差值当精确延迟。
verify_ros输出输入来源、domain、样本量、实际Hz、缺值/丢弃、关节名称对应及hardware_tested=false。MuJoCo原生env始终DISARMED，必须核验sdk_publish_count=0。
