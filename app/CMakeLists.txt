add_executable(naval-cli NavalCommand/Gateway/Grpc/Api/Cli.cpp)
target_link_libraries(naval-cli PRIVATE Gateway.Grpc.Api protobuf::libprotobuf)
target_include_directories(naval-cli PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
NavalCommandConfigureTarget(naval-cli)
