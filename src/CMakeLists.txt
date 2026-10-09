find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)
set(gen "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${gen}")
set(path "navalcommand/gateway/v1/service")
add_custom_command(
    OUTPUT "${gen}/${path}.pb.cc" "${gen}/${path}.pb.h" "${gen}/${path}.grpc.pb.cc"
           "${gen}/${path}.grpc.pb.h"
    COMMAND
        protobuf::protoc --proto_path=${PROJECT_SOURCE_DIR}/proto
        --proto_path=${GATEWAY_API_PROTO_DIR} --cpp_out=${gen} --grpc_out=${gen}
        --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
        "${PROJECT_SOURCE_DIR}/proto/${path}.proto"
    DEPENDS "${PROJECT_SOURCE_DIR}/proto/${path}.proto" protobuf::protoc gRPC::grpc_cpp_plugin)
add_library(Gateway.Grpc.Api STATIC "${gen}/${path}.pb.cc" "${gen}/${path}.grpc.pb.cc")
target_include_directories(Gateway.Grpc.Api PUBLIC "${gen}")
target_link_libraries(Gateway.Grpc.Api PUBLIC Gateway.Api gRPC::grpc++)
NavalCommandConfigureTarget(Gateway.Grpc.Api)
