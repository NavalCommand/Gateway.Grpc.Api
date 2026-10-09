add_executable(naval-cli NavalCommand/Gateway/Grpc/Api/Cli.cpp)
target_link_libraries(naval-cli PRIVATE Gateway.Grpc.Api protobuf::libprotobuf)
NavalCommandConfigureTarget(naval-cli)
