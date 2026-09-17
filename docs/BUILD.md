# Building SMCP on Windows

This guide covers the supported CMake build on 64-bit Windows. The repository does not
track generated Visual Studio solutions, `.vcxproj` files, or qmake projects.

## Quick start

Install the required dependencies, open PowerShell in the repository root, and run:

```powershell
.\scripts\bootstrap.ps1
.\scripts\build.ps1 -Config Debug
.\build\ninja-debug\bin\SMCP_d.exe
```

`bootstrap.ps1` detects dependencies and creates the machine-local
`CMakeUserPresets.json`. `build.ps1` imports the Visual Studio x64 environment, configures
CMake, builds the application, and deploys the runtime DLLs beside the executable.

If no FLIR/Teledyne camera is needed, Spinnaker can be omitted. The bootstrap script will
automatically write `SMCP_WITH_SPINNAKER=OFF` when it cannot find the SDK. Capture will be
disabled, but loading, processing, calibration, reconstruction, visualization, and point
cloud editing remain available.

## Requirements

| Component | Tested version | Required installation |
|---|---|---|
| Windows | 10 or 11, x64 | A current Windows SDK |
| Visual Studio | 2022 (17.x) or 2026 (18.x) | **Desktop development with C++**, MSVC x64, CMake 3.28 or newer, and Ninja |
| Qt | 5.14.2, `msvc2017_64` kit | Core, Gui, Widgets, OpenGL, and Concurrent |
| OpenCV | 2.4.13.6 Windows package | `build/x64/vc14`, including core, imgproc, highgui, calib3d, features2d, and flann |
| Spinnaker SDK | 3.x or 4.x | Optional; required only for direct FLIR/Teledyne capture |
| WSL 2 + Ubuntu | 22.04.5 | Optional; required only for the AI models. See [WSL models quick start](WSL_MODELS_QUICKSTART.md) |

The older Qt and OpenCV binaries are ABI-compatible with the supported MSVC toolsets in
this project configuration. The build is intentionally Windows/MSVC-only.

Typical dependency locations are:

```text
D:\Qt\Qt5.14.2\5.14.2\msvc2017_64
D:\OpenCV\opencv\build
D:\Program Files\Teledyne\Spinnaker
```

## Configure dependency paths

Machine-specific paths belong in `CMakeUserPresets.json`, which is ignored by Git.

### Automatic setup

The recommended option searches common locations on `C:`, `D:`, and `E:`:

```powershell
.\scripts\bootstrap.ps1
```

Explicit hints can be supplied when dependencies are installed elsewhere:

```powershell
.\scripts\bootstrap.ps1 `
  -Qt "C:\Qt\5.14.2\msvc2017_64" `
  -OpenCV "C:\Libraries\opencv\build" `
  -Spinnaker "C:\Program Files\Teledyne\Spinnaker" `
  -Force
```

`-Force` replaces an existing user preset. Qt and OpenCV are mandatory; Spinnaker is
optional.

### Manual setup

Copy the example and edit the hidden `local-paths` preset:

```powershell
Copy-Item CMakeUserPresets.example.json CMakeUserPresets.json
```

```json
"cacheVariables": {
  "Qt5_DIR": "D:/Qt/Qt5.14.2/5.14.2/msvc2017_64/lib/cmake/Qt5",
  "OpenCV_DIR": "D:/OpenCV/opencv/build",
  "SPINNAKER_DIR": "D:/Program Files/Teledyne/Spinnaker",
  "SMCP_WITH_SPINNAKER": "ON"
}
```

`Qt5_DIR` may point either to the kit root or to its `lib/cmake/Qt5` directory.

## Build options

### PowerShell helper

```powershell
# Debug build
.\scripts\build.ps1 -Config Debug

# Release build and launch it
.\scripts\build.ps1 -Config Release -Run

# Generate and build with the Visual Studio 2026 generator
.\scripts\build.ps1 -Generator vs2026 -Config Release
```

Pass `-Clean` to remove the selected build directory before configuring it again.

### CMake presets

Run these commands from a **Developer PowerShell for Visual Studio**:

```powershell
cmake --preset ninja-debug
cmake --build --preset ninja-debug
```

Available presets are:

| Configure preset | Generator | Output |
|---|---|---|
| `ninja-debug` | Ninja + MSVC x64, Debug | `build/ninja-debug/bin/SMCP_d.exe` |
| `ninja-release` | Ninja + MSVC x64, Release | `build/ninja-release/bin/SMCP.exe` |
| `vs2026` | Visual Studio 18 2026, x64 | Generated solution under `build/vs2026` |

Build presets are `ninja-debug`, `ninja-release`, `vs2026-debug`, and
`vs2026-release`.

### Visual Studio

1. Run `bootstrap.ps1` once.
2. Select **File > Open > Folder** and open the repository root.
3. Select `Ninja Debug (x64)`, `Ninja Release (x64)`, or `Visual Studio 2026` from the
   configuration list.
4. Select **Build > Build All**.
5. Press **F5** to run the selected executable.

If presets do not appear, verify that `CMakeUserPresets.json` exists and use
**Project > Delete Cache and Reconfigure**.

### Zed

The repository contains `.zed/settings.json`, `.zed/tasks.json`, and `.clangd`.

1. Run `bootstrap.ps1` and build `ninja-debug` once.
2. Open the repository with `zed .`.
3. Use the supplied configure, build, and run tasks.

The build patches `compile_commands.json` with the MSVC toolset and Windows SDK paths so
clangd works even when Zed was not started from a Developer PowerShell.

## CMake options

| Variable | Default | Purpose |
|---|---|---|
| `SMCP_WITH_SPINNAKER` | `ON` | Builds camera capture and links the Spinnaker SDK |
| `SMCP_DEPLOY_RUNTIME` | `ON` | Deploys Qt, OpenCV, and Spinnaker runtime DLLs beside the executable |
| `Qt5_DIR` | none | Qt kit root or `lib/cmake/Qt5` directory |
| `OpenCV_DIR` | none | OpenCV package `build` directory |
| `SPINNAKER_DIR` | none | Spinnaker SDK root |

With runtime deployment enabled, CMake runs `windeployqt` and copies the Qt plugins,
OpenCV DLLs, and optional Spinnaker DLL into the output `bin` directory. The MSVC runtime
is deliberately not copied.

## Verifying the build

A successful Debug build ends by linking and deploying:

```text
Linking CXX executable bin\SMCP_d.exe
...\build\ninja-debug\bin\SMCP_d.exe 64 bit, debug executable
```

For a stronger check, force a clean rebuild:

```powershell
.\scripts\build.ps1 -Config Debug -Clean
```

Then launch the executable and confirm that the main window opens. A camera is not needed
for this smoke test.

The C++ tests are registered with CTest and built with the application. Run them from a
Developer PowerShell, where `ctest` is available:

```powershell
ctest --test-dir build/ninja-debug --output-on-failure
```

`SMCP_ai_tests` also contains an opt-in integration test for configured machines. It discovers
the installed models in WSL and, when an input cloud and a configuration are supplied, runs
Score Denoise on them:

```powershell
$env:SMCP_TEST_WSL = "1"
$env:SMCP_TEST_INPUT = "$PWD\scripts-wsl\tests\fixtures\small-cloud.xyz"
$env:SMCP_TEST_CONFIG = "$PWD\scripts-wsl\tests\fixtures\score-smoke.json"
ctest --test-dir build/ninja-debug --output-on-failure
```

The WSL launcher tests run inside Ubuntu from the repository root:

```bash
python3 -m unittest discover -s scripts-wsl/tests -p "test_*.py"
```

## Distributing a Release build

Build and package the Release configuration:

```powershell
.\scripts\package-release.ps1
```

The helper runs `build.ps1 -Config Release`, stages `build/ninja-release/bin`, and adds the
Visual C++ runtime DLLs, `LICENSE.txt`, `READ_ME_FIRST.txt`, `README.md`, and the complete
`docs` directory, so the relative links in the documentation keep working. It writes
`dist/SMCP-<version>-win64.zip` together with its SHA256 file. Test executables such as
`SMCP_ai_tests.exe` are removed from the package, and the script stops if any other test file
remains. The `scripts-wsl` directory is not packaged: users who want the optional AI models
follow the [WSL models quick start](WSL_MODELS_QUICKSTART.md) with the source repository.
The version comes from
`CMakeLists.txt`; `-SkipBuild` reuses the current Release output and `-Version` overrides
the archive name. Upload that archive as the GitHub release asset.

To distribute manually instead, copy the complete `build/ninja-release/bin` directory. Do
not copy `SMCP.exe` alone: the adjacent Qt plugins and DLLs are part of the application
package.

The target Windows computer does not need Qt, OpenCV, Spinnaker development files, CMake,
Ninja, or Visual Studio. It does need:

- the Microsoft Visual C++ x64 Redistributable compatible with the toolset used to build
  SMCP, unless the runtime DLLs travel next to `SMCP.exe` as `package-release.ps1` copies
  them;
- compatible FLIR/Teledyne drivers when direct camera capture is required; and
- WSL 2 with the Ubuntu 22.04 setup from the quick start only when the AI models are used.

Build with `SMCP_WITH_SPINNAKER=OFF` when the distributed application only needs existing
captures and point clouds. This removes the Spinnaker runtime dependency and disables
direct camera capture.

## Troubleshooting

### Qt5 package not found

`Qt5_DIR` must resolve to a kit containing `lib/cmake/Qt5/Qt5Config.cmake`. Install the
Qt 5.14.2 `msvc2017_64` kit and regenerate the user presets.

### OpenCV package or modules not found

`OpenCV_DIR` must point to the Windows package's `build` directory. It must contain
`OpenCVConfig.cmake` and `x64/vc14/lib`. The project fixes `OpenCV_ARCH=x64` and
`OpenCV_RUNTIME=vc14` because OpenCV 2.4 does not recognize modern MSVC toolset names.

### Spinnaker not found

Either set `SPINNAKER_DIR` to a root containing `include`, `lib64`, and `bin64`, or build
with `SMCP_WITH_SPINNAKER=OFF`. `FindSpinnaker.cmake` supports the `Spinnaker_v141` and
`Spinnaker_v140` library layouts.

### Header changes do not rebuild, or Debug reports heap corruption

Ninja rebuilds a source file after a header changes only if it recognizes the compiler's
`/showIncludes` lines. A Visual Studio installed in Spanish or another language prints that
prefix with accented characters in the console code page, and CMake stores it with a different
encoding. Ninja then records no header dependencies: editing a header leaves stale objects
with different class layouts, which in Debug typically ends with *HEAP CORRUPTION DETECTED*
when a dialog is destroyed.

`cmake/MsvcShowIncludesPrefix.cmake` detects the real prefix from `cl.exe` and corrects it
during configuration, printing `Corrected the cl.exe /showIncludes prefix` when it applies.
Build directories configured before this fix may still contain stale objects, so rebuild them
once with `-Clean`. To check that dependencies are tracked, run
`ninja -C build/ninja-debug -t deps` and confirm that objects list their headers instead of
`#deps 0`.

### `cl.exe`, CMake, or Ninja is not in `PATH`

Use `scripts\build.ps1`, or run CMake from a Developer PowerShell. If the helper cannot
import the environment, verify that the Visual Studio C++ workload and CMake tools are
installed.

### The executable reports missing DLLs

Do not move the executable out of its generated `bin` directory. Rebuild with
`SMCP_DEPLOY_RUNTIME=ON`; the required Qt plugins and runtime DLLs are deployed there.

### clangd cannot find standard headers

Build `ninja-debug` at least once so `compile_commands.json` is generated and patched with
the MSVC and Windows SDK paths.

### Visual Studio uses the wrong configuration

Check the active preset. Each preset has an independent directory under `build/`.

### CMake says the compiler changed

A build directory cannot safely switch between different MSVC installations or toolsets.
Regenerate only the affected preset directory with:

```powershell
.\scripts\build.ps1 -Config Debug -Clean
```

Use `-Config Release` instead when the message refers to `build/ninja-release`.

## Next step

Continue with the [User Guide](USER_GUIDE.md) to configure hardware, calibrate the system,
reconstruct a point cloud, and use the point cloud editor.
