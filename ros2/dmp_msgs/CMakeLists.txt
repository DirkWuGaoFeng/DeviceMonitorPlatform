cmake_minimum_required(VERSION 3.16)
project(dmp_msgs LANGUAGES C CXX)

# 必须显式启用 C: rosidl 为每个 msg/srv 生成 *_rosidl_generator_c 目标 (纯 .c 源),
# 只写 LANGUAGES CXX 时 CMake 找不到 C 编译器, 报 "Cannot determine link language" 而失败。

find_package(ament_cmake REQUIRED)
find_package(rosidl_default_generators REQUIRED)
find_package(std_msgs REQUIRED)

# 只定义接口, 不含任何实现: 消息层是被两侧(桥/上游消费者)共同依赖的契约
rosidl_generate_interfaces(${PROJECT_NAME}
  "msg/DeviceFrame.msg"
  "msg/DeviceFrameArray.msg"
  "msg/Rule.msg"
  "msg/RuleList.msg"
  "srv/Selftest.srv"
  "srv/SetRule.srv"
  "srv/GetRules.srv"
  DEPENDENCIES std_msgs
)

ament_export_dependencies(rosidl_default_runtime)
ament_package()
