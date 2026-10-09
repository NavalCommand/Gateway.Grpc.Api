# Gateway.Grpc.Api 0.1.0
Public gRPC service definitions and an independently launched C++ CLI.

service.proto imports Gateway.Api session/world/orders messages. It contains no payload message declarations. Source imports and C++ bindings are fetched from the pinned Git revision in cmake/Modules.cmake through CPM; NC_REPOSITORY_BASE may override the repository URL base (including a local Git source directory). There is no sibling include/link assumption.

Set VCPKG_ROOT, NC_VCPKG_INSTALLED_DIR, NC_CMAKE; run ./scripts/Build-Native.ps1. CMake >=4.2 and x64 clang-cl /clang:-std=c++2c are required. vcpkg external dependencies are manifest-pinned source builds.

out/native/naval-cli.exe --endpoint 127.0.0.1:50051 is read-only. --smoke explicitly submits validation requests and one real waypoint, waits for displacement >=5 m and retries the same request ID after reconnect. --blocked explicitly attempts a currently blocked waypoint; rejection is expected. Neither option is normal client auto-start behavior.

CLI is a small test application, not a reusable simulation/decision scheduler. Network connects/queries have bounded deadlines. No remote unauthenticated binding is implemented.


