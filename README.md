# SMCP: Camera-Proyector Measuring System

SMCP is a Windows desktop application for calibrating a projector-camera system and
reconstructing colored, oriented point clouds from projected Gray-code patterns. It is
written in C++17 with Qt 5, OpenCV, and OpenGL.

The calibration workflow implements the method described in *Simple, Accurate, and Robust
Projector-Camera Calibration* by Daniel Moreno and Gabriel Taubin (3DimPVT 2012,
[doi:10.1109/3DIMPVT.2012.77](https://doi.org/10.1109/3DIMPVT.2012.77)).

## Features

- Project and capture Gray-code sequences automatically.
- Capture FLIR/Teledyne cameras through the optional Spinnaker SDK.
- Decode projector correspondences and calibrate the camera-projector pair.
- Reconstruct colored point clouds and inspect them in the integrated OpenGL viewer.
- Export reconstructed data as PLY or XYZ.
- Edit XYZ point clouds by removing statistical outliers and fitting RANSAC planes or
  spheres.
- Compare, rename, reorder, hide, delete, and export all point clouds in the editor.

## Requirements

| Component | Tested version | Notes |
|---|---|---|
| Windows | 10 or 11, x64 | The application is Windows-only. |
| Visual Studio | 2026 (18.x, toolset v145) | Install **Desktop development with C++**, CMake 3.28 or newer, and Ninja. |
| Qt | 5.14.2, `msvc2017_64` kit | Core, Gui, Widgets, OpenGL, and Concurrent modules. |
| OpenCV | 2.4.13.6 Windows package | Requires the `build/x64/vc14` libraries. |
| Spinnaker SDK | 3.x or 4.x | Optional; required only for direct FLIR/Teledyne capture. |

Qt and OpenCV are required. If Spinnaker is not installed, the bootstrap script creates a
configuration with `SMCP_WITH_SPINNAKER=OFF`; calibration, reconstruction, and point-cloud
editing remain available.

## Quick start

Run these commands from PowerShell at the repository root:

```powershell
# Detect local dependencies and generate the untracked CMakeUserPresets.json file.
.\scripts\bootstrap.ps1

# Configure and build with Ninja and MSVC.
.\scripts\build.ps1 -Config Debug

# Run the Debug executable.
.\build\ninja-debug\bin\SMCP_d.exe
```

Use `-Config Release` to produce `build/ninja-release/bin/SMCP.exe`. Add `-Run` to the
build command to launch the resulting executable automatically.

The available configure presets are `ninja-debug`, `ninja-release`, and `vs2026`. The
Visual Studio preset generates its solution under `build/vs2026`; its build presets are
`vs2026-debug` and `vs2026-release`.

See [Building SMCP](docs/BUILD.md) for dependency locations, manual preset configuration,
IDE setup, clean builds, troubleshooting, and Release distribution instructions.

## Basic workflow

1. Select a workspace directory.
2. Disable automatic camera controls such as focus, exposure, gain, and white balance.
3. Capture several Gray-code sets with the chessboard in different poses.
4. Run **Decode**, **Extract Corners**, and **Calibrate**.
5. Run **Reconstruct** to generate and display a point cloud.
6. Open **Point Cloud** to remove outliers or fit planes and spheres.
7. In the editor, click **Export** to write one XYZ file for every remaining cloud. Edited
   row names are used as filenames; hidden clouds are exported and deleted clouds are not.

For detailed operating instructions and file formats, read the
[User Guide](docs/USER_GUIDE.md).

## Repository layout

```text
CMakeLists.txt                 CMake build definition (CMake 3.28+, MSVC x64)
CMakePresets.json              Shared preset bases without machine-specific paths
CMakeUserPresets.example.json  Template for local Qt, OpenCV, and Spinnaker paths
cmake/                         CMake dependency and clangd helper modules
scripts/                       Dependency bootstrap and build helpers
src/app/                       Application entry point and main window
src/core/                      Algorithms, data objects, and shared utilities
src/camera/                    Optional Spinnaker camera integration
src/export/                    PLY and XYZ input/output
src/ui/                        Qt dialogs with Designer forms
src/ui/models/                 Qt item models
src/ui/pointcloud_editor/      Point-cloud editor and parameter dialogs
src/ui/preview/                Image and point-cloud preview widgets
src/ui/widgets/                Reusable UI widgets
resources/                     Qt resources, SVG icons, and the global QSS theme
docs/                          Build, usage, architecture, style, and screenshot guides
```

All C++ code belongs to the `smcp` namespace. Headers use `#pragma once`, includes are
qualified from `src` (for example, `"core/Scan3d.h"`), and formatting is defined by
`.editorconfig` and `.clang-format`.

## Release distribution

With `SMCP_DEPLOY_RUNTIME=ON` (the default), the Release build runs `windeployqt` and copies
the required Qt plugins, OpenCV DLLs, and optional Spinnaker DLL next to `SMCP.exe`. Copy
the complete `build/ninja-release/bin` directory to the target computer, not only the
executable. The target computer must also have the matching Microsoft Visual C++ x64
Redistributable installed; direct camera capture additionally requires compatible camera
drivers.

## Documentation

- [Documentation index](docs/README.md)
- [Build and troubleshooting guide](docs/BUILD.md)
- [User guide](docs/USER_GUIDE.md)
- [Architecture](docs/ARCHITECTURE.md)
- [UI style guide](docs/STYLE.md)

Runtime settings are stored with `QSettings` under
`HKEY_CURRENT_USER\Software\CENAM\SMCP`. The repository does not include user workspaces;
[`docs/samples/sample_calibration.yml`](docs/samples/sample_calibration.yml) documents the
calibration file schema.

## Authors

- José Luis Aguilera Luzania
- Agustín Brau Ávila
- Octavio Icasio Hernández

## License

SMCP was developed by José Luis Aguilera Luzania, Agustín Brau Ávila, and Octavio Icasio
Hernández for the University of Sonora as a Master's thesis project and for CENAM as an
internship project.

The software derives from work by Daniel Moreno and Gabriel Taubin at Brown University and
is distributed under the BSD 3-Clause License. See [LICENSE](LICENSE).
