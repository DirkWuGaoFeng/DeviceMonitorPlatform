cmake_minimum_required(VERSION 3.16)
project(dmp_ros2_bridge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
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

add_executable(bridge_node src/bridge_node.cpp src/bridge_main.cpp)
# 统一用 keyword 签名: ament_target_dependencies 内部走 plain 签名,
# 与上面的 PRIVATE 混用会让 CMake 报 "all-keyword or all-plain"。
# ROS2 依赖改用各包导出的 imported target (Humble 起 rclcpp::rclcpp / ${PKG_TARGETS})。
target_link_libraries(bridge_node PRIVATE
  dmp_gateway_client
  rclcpp::rclcpp
  ${std_msgs_TARGETS}
  ${diagnostic_msgs_TARGETS}
  ${dmp_msgs_TARGETS})

install(TARGETS bridge_node RUNTIME DESTINATION lib/${PROJECT_NAME})
install(DIRECTORY launch DESTINATION share/${PROJECT_NAME})

# 纯函数层断言 (不起节点、不依赖 DDS): 与仓库既有 171 项断言同一 CHECK 风格
enable_testing()
add_executable(test_rule_mapping test/test_rule_mapping.cpp)
target_include_directories(test_rule_mapping PRIVATE include ${DMP_REPO_ROOT}/include)
add_test(NAME rule_mapping_tests COMMAND test_rule_mapping)

ament_package()
