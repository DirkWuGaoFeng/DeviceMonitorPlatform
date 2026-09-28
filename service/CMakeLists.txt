# service/CMakeLists.txt — 在 Linux/WSL2 用 apt 版 gRPC 构建真 gRPC 服务层
#
# 关键点: Debian/Ubuntu 的 libgrpc++-dev 只发布 pkg-config(.pc), 不发布 gRPCConfig.cmake,
#         所以不能用 find_package(gRPC CONFIG)。本文件用 pkg-config 探测 + find_program 定位
#         protoc / grpc_cpp_plugin, 兼容 Ubuntu 20.04(gRPC 1.26, 无 absl) 及更新版本。
#
# 依赖(Ubuntu): libgrpc++-dev protobuf-compiler-grpc protobuf-compiler libprotobuf-dev pkg-config
#
# 构建:
#   cmake -S service -B build_wsl -DCMAKE_BUILD_TYPE=Release
#   cmake --build build_wsl -j
# 产物: build_wsl/dmp_grpc_server  build_wsl/grpc_client_probe
cmake_minimum_required(VERSION 3.16)
project(dmp_service CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

# pkg-config 探测 gRPC/protobuf, 用 *_INCLUDE_DIRS / *_LIBRARIES / *_LIBRARY_DIRS 变量拼装
pkg_check_modules(GRPCPP REQUIRED grpc++)          # grpc++.pc 会连带 grpc / gpr / 传递依赖
pkg_check_modules(PROTOBUF REQUIRED protobuf)

# 定位代码生成器
find_program(PROTOC protoc REQUIRED)
find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin REQUIRED)

set(PROTO_FILE ${CMAKE_CURRENT_SOURCE_DIR}/../proto/telemetry.proto)
set(PROTO_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/../proto)
set(GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/gen)
file(MAKE_DIRECTORY ${GEN_DIR})

# 1) protoc --cpp_out -> telemetry.pb.{h,cc}
set(PB_SRCS ${GEN_DIR}/telemetry.pb.cc)
set(PB_HDRS ${GEN_DIR}/telemetry.pb.h)
add_custom_command(
    OUTPUT ${PB_SRCS} ${PB_HDRS}
    COMMAND ${PROTOC} --cpp_out=${GEN_DIR} -I ${PROTO_DIR} ${PROTO_FILE}
    DEPENDS ${PROTO_FILE}
    COMMENT "protoc: 生成 protobuf C++")

# 2) protoc --grpc_out -> telemetry.grpc.pb.{h,cc}
set(GRPC_SRCS ${GEN_DIR}/telemetry.grpc.pb.cc)
set(GRPC_HDRS ${GEN_DIR}/telemetry.grpc.pb.h)
add_custom_command(
    OUTPUT ${GRPC_SRCS} ${GRPC_HDRS}
    COMMAND ${PROTOC} --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
            --grpc_out=${GEN_DIR} -I ${PROTO_DIR} ${PROTO_FILE}
    DEPENDS ${PROTO_FILE}
    COMMENT "protoc: 生成 gRPC C++ 桩")

add_library(telemetry_gen STATIC ${PB_SRCS} ${GRPC_SRCS})
target_include_directories(telemetry_gen PUBLIC ${GEN_DIR} ${PROTOBUF_INCLUDE_DIRS} ${GRPCPP_INCLUDE_DIRS})
target_link_libraries(telemetry_gen PUBLIC ${GRPCPP_LIBRARIES} ${PROTOBUF_LIBRARIES})
target_link_directories(telemetry_gen PUBLIC ${GRPCPP_LIBRARY_DIRS} ${PROTOBUF_LIBRARY_DIRS})

# 可移植核心库 (header-only)
add_library(dmp_core INTERFACE)
target_include_directories(dmp_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/../include)

# 服务端
add_executable(dmp_grpc_server grpc_server.cpp)
target_link_libraries(dmp_grpc_server PRIVATE telemetry_gen dmp_core Threads::Threads)

# 客户端探针
add_executable(grpc_client_probe grpc_client_probe.cpp)
target_link_libraries(grpc_client_probe PRIVATE telemetry_gen Threads::Threads)
