cmake_minimum_required(VERSION 3.16)
project(dmp_ros2_bridge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# 本包所有目标一律 PIC。原因不是风格, 是一条真链接错误:
#   relocation R_X86_64_PC32 against symbol `g_rcutils_logging_initialized' can not be used
#   when making a shared object; recompile with -fPIC
# 下面的 dmp_bridge_obj 是 OBJECT 库, 它的 .o 要同时喂给可执行文件与 .so —— 喂给 .so 那份
# 就必须是位置无关码。文件头注释里我写过"OBJECT 库能避开 RPATH 坑", 那句话不完整:
# 它确实避开了 RPATH, 但换来了"默认不编 PIC"这个新坑, 症状从 rc=127 变成一句 relocation 错。
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rclcpp_lifecycle REQUIRED)
find_package(rclcpp_components REQUIRED)
find_package(std_msgs REQUIRED)
find_package(diagnostic_msgs REQUIRED)
find_package(dmp_msgs REQUIRED)
find_package(Threads REQUIRED)

# DMP 核心是 header-only: 直接复用仓库根 include/,
# 于是"固件 C 内核 <-> 上位机 C++ <-> ROS2 节点"三方共用同一份帧协议实现。
get_filename_component(DMP_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
message(STATUS "dmp_ros2_bridge: DMP headers from ${DMP_REPO_ROOT}/include")

# 网关客户端不含任何 ROS 依赖 -> 独立静态库, 可被节点/测试/未来 CLI 共同链接
add_library(dmp_gateway_client STATIC src/gateway_client.cpp)
target_include_directories(dmp_gateway_client PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  ${DMP_REPO_ROOT}/include)
target_link_libraries(dmp_gateway_client PUBLIC Threads::Threads)

# ---------- 节点实现: 只编一次, 交给两个消费者 ----------
# OBJECT 库而不是"再写一遍 add_executable(bridge_node src/bridge_node.cpp ...)":
#   1) 独立进程与组合容器插件必须是同一份实现 —— 出现两份节点代码就等于两份契约(会漂移);
#   2) 也不把节点做成 SHARED 再让 bridge_node 去链接它, 那会引入安装后的 RPATH/LD_LIBRARY_PATH
#      问题: 现象是 ros2 run 直接 rc=127, 而错误信息只有一句 "error while loading shared libraries"。
add_library(dmp_bridge_obj OBJECT src/bridge_node.cpp)
target_include_directories(dmp_bridge_obj PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  ${DMP_REPO_ROOT}/include)
# 统一用 keyword 签名: ament_target_dependencies 内部走 plain 签名,
# 与上面的 PRIVATE 混用会让 CMake 报 "all-keyword or all-plain"。
# ROS2 依赖改用各包导出的 imported target (Humble 起 rclcpp::rclcpp / ${PKG_TARGETS})。
target_link_libraries(dmp_bridge_obj PUBLIC
  dmp_gateway_client
  rclcpp::rclcpp
  rclcpp_lifecycle::rclcpp_lifecycle
  ${std_msgs_TARGETS}
  ${diagnostic_msgs_TARGETS}
  ${dmp_msgs_TARGETS})

# (a) 独立进程: ros2 run dmp_ros2_bridge bridge_node  (需显式 lifecycle set 才出数据)
add_executable(bridge_node $<TARGET_OBJECTS:dmp_bridge_obj> src/bridge_main.cpp)
target_link_libraries(bridge_node PRIVATE dmp_bridge_obj)

# (b) 组合容器插件: 一台设备 = 一个组件实例, 多设备可塞进同一个进程
add_library(dmp_bridge_component SHARED
  $<TARGET_OBJECTS:dmp_bridge_obj> src/bridge_component.cpp)
target_link_libraries(dmp_bridge_component PRIVATE
  dmp_bridge_obj
  rclcpp_components::component)
# 顺带生成并安装一个单线程容器可执行 dmp_bridge (加载本插件), 便于手工试验。
# 想要多线程容器请用 rclcpp_components/component_container_mt (头阻塞对比需要它)。
rclcpp_components_register_node(dmp_bridge_component
  PLUGIN "dmpbr::DmpBridgeNode"
  EXECUTABLE dmp_bridge)

install(TARGETS bridge_node RUNTIME DESTINATION lib/${PROJECT_NAME})
install(TARGETS dmp_bridge_component LIBRARY DESTINATION lib)
install(DIRECTORY launch DESTINATION share/${PROJECT_NAME})

# 纯函数层断言 (不起节点、不依赖 DDS): 与仓库既有 171 项断言同一 CHECK 风格
enable_testing()
add_executable(test_rule_mapping test/test_rule_mapping.cpp)
target_include_directories(test_rule_mapping PRIVATE include ${DMP_REPO_ROOT}/include)
add_test(NAME rule_mapping_tests COMMAND test_rule_mapping)

ament_package()
