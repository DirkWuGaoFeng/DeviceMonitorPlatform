cmake_minimum_required(VERSION 3.16)
# C 必须启用: firmware/dmp_frame_core.c 是纯 C 内核, 仅声明 CXX 时 CMake 会静默跳过它
# (Windows 下用 g++ 手编时 .c 被当 C++ 处理因而隐蔽, Linux CI 才暴光)
project(DeviceMonitorPlatform LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(DMP_BUILD_QT "Build the Qt6 upper-computer monitor" OFF)
option(DMP_BUILD_TESTS "Build unit/integration tests" ON)

# 采集流水线线程化 (pipeline.h 的 ProducerThread) 需要线程库
find_package(Threads REQUIRED)

# ---------- 核心（不依赖 Qt）: 设备模拟器 ----------
add_executable(device_simulator src/device_simulator.cpp)
target_include_directories(device_simulator PRIVATE include)
if (WIN32)
    target_link_libraries(device_simulator PRIVATE ws2_32)
endif()

# ---------- 核心（不依赖 Qt）: 控制台监控端 ----------
add_executable(monitor_console src/monitor_console.cpp)
target_include_directories(monitor_console PRIVATE include)
target_link_libraries(monitor_console PRIVATE Threads::Threads)
if (WIN32)
    target_link_libraries(monitor_console PRIVATE ws2_32)
endif()

# ---------- 核心（不依赖 Qt）: 裸串口监控端 (真实 STM32 UART 数据源) ----------
add_executable(monitor_serial src/monitor_serial.cpp)
target_include_directories(monitor_serial PRIVATE include)
target_link_libraries(monitor_serial PRIVATE Threads::Threads)

# ---------- 核心（不依赖 Qt）: 遥测网关服务 (采集/告警后端服务化) ----------
add_executable(gateway_service src/gateway_service.cpp)
target_include_directories(gateway_service PRIVATE include)
if (WIN32)
    target_link_libraries(gateway_service PRIVATE ws2_32)
endif()

# ---------- 可选: Qt6 上位机 ----------
if (DMP_BUILD_QT)
    find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Charts Sql Network SerialPort)
    qt_standard_project_setup()
    add_executable(qt_monitor src/qt_monitor.cpp)
    target_include_directories(qt_monitor PRIVATE include)
    target_link_libraries(qt_monitor PRIVATE
        Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Charts Qt6::Sql Qt6::Network Qt6::SerialPort)
    if (WIN32)
        target_link_libraries(qt_monitor PRIVATE ws2_32)
    endif()
endif()

message(STATUS "DMP_BUILD_QT=${DMP_BUILD_QT}")

# ---------- 测试 ----------
if (DMP_BUILD_TESTS)
    enable_testing()
    add_executable(test_protocol tests/test_protocol.cpp)
    target_include_directories(test_protocol PRIVATE include)
    add_test(NAME protocol_tests COMMAND test_protocol)

    add_executable(test_storage tests/test_storage.cpp)
    target_include_directories(test_storage PRIVATE include)
    add_test(NAME storage_tests COMMAND test_storage)

    # 固件 C 内核与上位机 C++ 协议 逐字节一致性 (共用 firmware/dmp_frame_core.c)
    # 注: .c 按 C 编译 + 头内 extern "C" 守卫, 与测试侧 C++ 声明链接一致
    add_executable(test_frame_parity tests/test_frame_parity.cpp firmware/dmp_frame_core.c)
    target_include_directories(test_frame_parity PRIVATE include firmware)
    add_test(NAME frame_parity_tests COMMAND test_frame_parity)

    # 多线程采集流水线 (真 std::thread 跨线程不丢不重)
    add_executable(test_pipeline tests/test_pipeline.cpp)
    target_include_directories(test_pipeline PRIVATE include)
    target_link_libraries(test_pipeline PRIVATE Threads::Threads)
    add_test(NAME pipeline_tests COMMAND test_pipeline)

    # 网关文本行协议纯函数单测
    add_executable(test_service_proto tests/test_service_proto.cpp)
    target_include_directories(test_service_proto PRIVATE include)
    add_test(NAME service_proto_tests COMMAND test_service_proto)
endif()
