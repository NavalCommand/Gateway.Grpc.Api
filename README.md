# Gateway.Grpc.Api 0.1.0

Public gRPC service imports the single canonical Gateway.Api payload schema. It declares no duplicate messages and retains the existing RPC surface.

Independent CLI: o/windows-msvc-release/app/naval-cli.exe --endpoint 127.0.0.1:50051. Default is read-only; --smoke opts into one waypoint and invalid-command/reconnect tests, --blocked into a protected-action rejection, --reload into a test-only old-session/old-handle probe. No client auto-launch or scheduler. Library generated bindings are defined in src/, CLI in app/, canonical import test in test/.

## Build structure (Task 005)

Root CMakeLists.txt initializes/composes only. Shared policy/resolution lives in the independent Build.CMake repository, fetched as ncbuild. Production libraries are defined in src/, mocks in mock/, tests in test/, applications in app/. Namespace paths and existing target names are preserved.

Enter a VS 2026 x64 developer shell, set standard VCPKG_ROOT, and use CMake 4.2+ and Ninja:

```powershell
cmake --preset windows-msvc-release -DFETCHCONTENT_SOURCE_DIR_NCBUILD=/absolute/path/to/Build.CMake
cmake --build o/windows-msvc-release --parallel 8
ctest --test-dir o/windows-msvc-release --output-on-failure --no-tests=error
```

The bootstrap option is unnecessary once Build.CMake is available at its declared Git URL. Optional ignored Build.Local.cmake registers canonical module URLs with NavalCommandSetDependencyPath(URL, absoluteCheckout); it is read once at the top level before declaration and applies transitively. Without a registration the declared Git URL and main reference are used. There is no workspace-root environment variable or implicit sibling lookup. External dependencies remain source-built vcpkg manifest packages. An already provisioned prefix can use standard -DVCPKG_INSTALLED_DIR=... -DVCPKG_MANIFEST_INSTALL=OFF cache options. See the parent project's Docs/MODULE_STRUCTURE.md for exact verified commands and limits.

Presets contain configurePresets only, windows-msvc-debug/release and linux-clang-debug/release where applicable. Output is o/${presetName}. C++26 policy is centralized; MSVC uses /std:c++latest for available features. No full C++26 conformance claim. Build outputs, Build.Local.cmake and CMakeUserPresets.json are ignored. No proprietary game binaries are included.
