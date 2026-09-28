# dmp_msgs/DeviceFrame - 设备上报单帧 (协议保真层)
# 与固件/上位机 12 字节帧一一对应; 帧内 CRC 已在桥内校验, 未通过的帧不会成为消息,
# 流级完整性计数随 DeviceFrameArray 一起发布 (完整性是流属性, 不是单帧属性)。
std_msgs/Header header   # stamp=采集侧接收时间(ms 精度), frame_id=设备标识
uint16 seq               # 帧序号 (回绕)
uint8  channel           # 通道号
uint8  kind              # 量纲类型: 1=TEMP 2=HR 3=SPO2 4=CONC 5=PRESS
string kind_name         # 同名的可打印形式, 省去下游查表
float32 value            # 测量值