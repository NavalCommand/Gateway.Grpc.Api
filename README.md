# Gateway.Grpc.Api 0.1.0

Project-wide documentation is maintained in the separate **Docs Git repository**. Before changing this module, explicitly read [Docs/AGENTS.md](../Docs/AGENTS.md), [architecture](../Docs/ARCHITECTURE.md), [module/build policy](../Docs/MODULE_STRUCTURE.md), [local sources](../Docs/LOCAL_SOURCES.md), and [code style](../Docs/CODE_STYLE.md) with its [owner export](../Docs/CodeStyle.xml). The shared instructions are not discovered automatically across repository boundaries; do not create a competing AGENTS.md in the parent workspace. Historical reports and reproduction guides are indexed under [Docs/Tasks/](../Docs/Tasks/README.md), with run receipts under [Docs/Evidence/](../Docs/Evidence/README.md).


The public gRPC service imports the single canonical Gateway.Api schema version 4 and retains the existing RPC surface. `GetView` exposes only an order-free `ObserverSnapshot`; `DiscoverOrders` separately exposes typed, observer-scoped availability rows. Observation/order references use `ObserverId` plus `ObservationId` and session/version context, independent of control authority. Public RPCs carry no authoritative `UnitId`, simulation truth or truth/contact correlation. Exact optional values preserve known zero/false distinctly from absent; positions use partial coordinate/error observations, and repeated observation items mean only currently reported items. Missing order rows are not executable. A launch result may expose only its observer's spawned `ObservationId` when available.

Independent CLI: o/windows-msvc-release/app/naval-cli.exe --endpoint 127.0.0.1:50051. Default is read-only; --smoke opts into one waypoint and invalid-command/reconnect tests, --blocked into a protected-action rejection, --reload into a test-only old-session/old-observation probe. --movement-smoke exercises replace/append sequence, route observations and negative dispatch; --launch-smoke discovers launch selectors separately from observed inventory and follows the spawned observation's lifecycle when assigned. Every operational selection uses `DiscoverOrders`, never an identity tag as authority. No client auto-launch or scheduler. Library generated bindings are defined in src/, CLI in app/, canonical import test in test/.

## Build structure

Root CMakeLists.txt initializes/composes only. Shared policy/resolution lives in the independent Build.CMake repository, fetched as ncbuild. Production libraries are defined in src/, mocks in mock/, tests in test/, applications in app/. Namespace paths and existing target names are preserved.

Enter a VS 2026 x64 developer shell, set standard VCPKG_ROOT, and use CMake 4.2+ and Ninja:

```powershell
cmake --preset windows-msvc-release -DFETCHCONTENT_SOURCE_DIR_NCBUILD=/absolute/path/to/Build.CMake
cmake --build o/windows-msvc-release --parallel 8
ctest --test-dir o/windows-msvc-release --output-on-failure --no-tests=error
```

The bootstrap option is unnecessary once Build.CMake is available at its declared Git URL. Optional ignored Build.Local.cmake registers canonical module URLs with NavalCommandSetDependencyPath(URL, absoluteCheckout); it is read once at the top level before declaration and applies transitively. Without a registration the declared Git URL and main reference are used. There is no workspace-root environment variable or implicit sibling lookup. External dependencies remain source-built vcpkg manifest packages. An already provisioned prefix can use standard -DVCPKG_INSTALLED_DIR=... -DVCPKG_MANIFEST_INSTALL=OFF cache options. See the separate [Docs repository build policy](../Docs/MODULE_STRUCTURE.md) for current commands and limits; dated measurements belong to the [task archive](../Docs/Tasks/README.md).

Presets contain configurePresets only, windows-msvc-debug/release and linux-clang-debug/release where applicable. Output is o/${presetName}. C++26 policy is centralized; MSVC uses /std:c++latest for available features. No full C++26 conformance claim. Build outputs, Build.Local.cmake and CMakeUserPresets.json are ignored. No proprietary game binaries are included.

Windows target and host dependencies use `x64-windows-static`, matching `/MT` in Release and `/MTd` in Debug. Reconfigure with `--fresh` and relink when migrating earlier dynamic-CRT artifacts.
