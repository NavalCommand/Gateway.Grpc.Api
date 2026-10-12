find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)
set(generatedDirectory "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${generatedDirectory}")
set(protoRelativePath "navalcommand/gateway/v2/service")
add_custom_command(
    OUTPUT "${generatedDirectory}/${protoRelativePath}.pb.cc" "${generatedDirectory}/${protoRelativePath}.pb.h"
        "${generatedDirectory}/${protoRelativePath}.grpc.pb.cc" "${generatedDirectory}/${protoRelativePath}.grpc.pb.h"
    COMMAND protobuf::protoc --proto_path=${PROJECT_SOURCE_DIR}/proto --proto_path=${GATEWAY_API_PROTO_DIR}
        --cpp_out=${generatedDirectory} --grpc_out=${generatedDirectory}
        --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
        "${PROJECT_SOURCE_DIR}/proto/${protoRelativePath}.proto"
    DEPENDS "${PROJECT_SOURCE_DIR}/proto/${protoRelativePath}.proto"
        "${GATEWAY_API_PROTO_DIR}/navalcommand/gateway/v2/domain.proto" protobuf::protoc gRPC::grpc_cpp_plugin)
add_library(NavalCommand.Gateway.Grpc.Api STATIC
    "${generatedDirectory}/${protoRelativePath}.pb.cc" "${generatedDirectory}/${protoRelativePath}.grpc.pb.cc")
target_include_directories(NavalCommand.Gateway.Grpc.Api PUBLIC "${generatedDirectory}")
target_link_libraries(NavalCommand.Gateway.Grpc.Api PUBLIC NavalCommand.Gateway.Api gRPC::grpc++)
NavalCommandConfigureTarget(NavalCommand.Gateway.Grpc.Api)
