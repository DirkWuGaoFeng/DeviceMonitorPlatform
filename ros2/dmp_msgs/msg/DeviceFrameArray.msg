# dmp_msgs/DeviceFrameArray - 一个发布周期内的帧批 + 流级健康计数
std_msgs/Header header
DeviceFrame[] frames
uint64 ok                # 解码成功帧数 (网关累计)
uint64 crc_err           # CRC 错帧数 (网关累计)
uint64 dropped           # 环形缓冲满丢弃数 (网关累计)