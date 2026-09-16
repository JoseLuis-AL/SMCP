# Architecture

SMCP is a Windows desktop application for projector-camera structured-light scanning. It is
built as one C++17 executable with Qt 5, OpenCV, OpenGL, and optional Spinnaker camera
support.

`smcp::Application`, a `QApplication` subclass, owns the application-wide state: persistent
settings, the capture-set model, calibration data, decoded patterns, detected corners, and
the current reconstructed point cloud. `MainWindow` and the dialogs coordinate that state
and present it to the user. Numerical algorithms are kept in `src/core` so they do not
depend on the UI.

## Module map

| Path | Main responsibility |
|---|---|
| `src/app` | Process entry point, global state, workflow orchestration, and the main window |
| `src/ui` | Dialogs that have Qt Designer `.ui` forms |
| `src/ui/pointcloud_editor` | Point-cloud editor and its parameter dialogs |
| `src/ui/preview` | Reusable image and OpenGL preview widgets |
| `src/ui/models` | `TreeModel`, the Qt model used to list capture sets |
| `src/ui/widgets` | Functional widgets without a standalone `.ui` form, currently `ProjectorWidget` |
| `src/camera` | Spinnaker camera discovery, configuration, and acquisition worker |
| `src/core` | Structured-light, calibration, reconstruction, point-cloud algorithms, and shared guards |
| `src/export` | XYZ point-cloud import and export |
| `src/common` | Settings keys, filenames, and OpenCV-to-Qt conversion helpers |
| `resources` | Embedded icons and the application-wide QSS theme |
| `scripts` | Dependency discovery and repeatable configure/build commands |
| `cmake` | Custom package discovery and compile-database support |

Most code is in the `smcp` namespace. Algorithm groups use nested namespaces such as
`smcp::Pointcloud`, `smcp::Scan3d`, `smcp::StructuredLight`, and `smcp::IoUtil`.

## Component reference

| Component | What it does |
|---|---|
| `Application` | Owns persistent settings and processing state; loads capture sets and implements decoding, corner extraction, calibration, and reconstruction orchestration |
| `MainWindow` | Connects the primary actions and parameter controls to `Application`, selects capture sets, switches previews, and exports reconstructed clouds |
| `CaptureDialog` | Coordinates the camera worker and projector, exposes capture settings, and records complete pattern sequences |
| `CalibrationDialog` | Displays calibration matrices, distortion coefficients, poses, and errors |
| `ProcessingDialog` | Reports progress and messages for long main-workflow operations and provides cancellation state |
| `AboutDialog` | Displays application and attribution information |
| `PointcloudEditorDialog` | Loads workspace XYZ files, schedules editing commands, caches parsed clouds, and exports the current overlay entries |
| `RemoveOutliersDialog`, `PlaneFitDialog`, `SphereFitDialog` | Validate and return parameters for their respective point-cloud operations |
| `TreeModel` | Represents capture-set directories and their images as a checkable Qt item model |
| `ImageLabel` | Adds image-oriented sizing and display behavior to a label |
| `PixmapWidget` | Displays camera preview pixmaps while preserving their useful layout behavior |
| `PointcloudWidget` | Renders the single reconstructed `Scan3d::Pointcloud` used by the main window |
| `PointcloudPreviewWidget` | Renders multiple editable colored clouds and owns the editor's overlay list |
| `ProjectorWidget` | Generates and displays fullscreen Gray-code patterns and alignment content |
| `CameraWorker` | Runs acquisition off the UI thread and emits preview/capture results |
| `CameraUtilities` | Discovers and configures Spinnaker devices and converts camera-node operations into reusable helpers |
| `CameraSettings` | Defines the values transferred when a camera is configured |
| `StructuredLight` | Generates/decodes Gray codes and estimates direct illumination |
| `Scan3d` | Creates projector views, triangulates correspondences, and computes point-cloud normals |
| `CalibrationData` | Owns calibration matrices and serializes/deserializes calibration YAML |
| `PointcloudOps` | Implements statistical filtering and RANSAC plane/sphere extraction for colored clouds |
| `IoUtil` | Writes PLY/PGM files and converts selected OpenCV matrices to Qt images |
| `BusyCursorGuard` | Restores the application cursor automatically after a scoped synchronous operation |
| `IOExport` | Reads XYZ/XYZRGB clouds and writes reconstructed or editor XYZ output |
| `Settings` | Centralizes every `QSettings` key and default value |
| `Literals` | Centralizes shared filenames and fixed text constants |
| `CvMatConvert` | Converts OpenCV matrices into Qt images for previews |

## End-to-end data flow

```text
capture sets
    |
    v
Gray-code decoding --> camera/projector correspondences
    |
    +--> chessboard corners --> camera/projector calibration
    |
    v
stereo triangulation --> point cloud --> XYZ/PLY export --> optional XYZ editing
```

### 1. Capture

Relevant code: `src/ui/CaptureDialog.*`, `src/ui/widgets/ProjectorWidget.*`, and
`src/camera/*`.

- `MainWindow` opens `CaptureDialog`. Camera capture is compiled only when
  `SMCP_WITH_SPINNAKER=ON`; a camera-free build displays an explanatory message instead.
- `CaptureDialog` selects a camera and projector display, restores capture settings, and
  starts `CameraWorker` in a dedicated `QThread`.
- `CameraWorker` configures the device through `CameraUtilities`, publishes preview frames,
  and saves requested images in the active capture-set directory.
- `ProjectorWidget` fills the selected display and generates the Gray-code sequence.
- Each capture set is a timestamped subdirectory containing `cam_NN.png` files and
  `projector_info.txt`, which records the projector dimensions and pattern count.

Changing the workspace calls `Application::SetRootDir()`. It rescans its immediate
subdirectories and updates `TreeModel`; these subdirectories become the selectable capture
sets in the main window.

### 2. Gray-code decoding

Relevant code: `Application::DecodeAll()`, `Application::DecodeGraySet()`, and
`src/core/StructuredLight.*`.

For every selected capture set, the decoder reads the projected sequence, estimates direct
and indirect illumination, and decodes horizontal and vertical Gray codes. The result is a
camera-sized projector-coordinate map plus minimum/maximum intensity data. The main window
uses these matrices to render Pattern View and Projector View.

The Decode controls provide the contrast threshold, black-light ratio (`b`), and minimum
direct-light component (`m`). These values affect which camera pixels receive a valid
projector correspondence.

### 3. Chessboard corner extraction

Relevant code: `Application::ExtractChessboardCornersV2()`.

OpenCV finds and refines the interior chessboard corners in each selected capture. SMCP
associates those image points with physical board coordinates computed from the configured
row count, column count, and square dimensions. Results are stored as camera and world
corner arrays for calibration.

### 4. Projector-camera calibration

Relevant code: `Application::Calibrate()` and `src/core/CalibrationData.*`.

SMCP maps each detected camera corner into projector coordinates by fitting a local
homography in the decoded correspondence map. OpenCV then calibrates the camera and
projector separately and performs stereo calibration to obtain their relative rotation and
translation.

`CalibrationData` stores camera intrinsics and distortion, projector intrinsics and
distortion, stereo rotation and translation, and the three reprojection errors. It can load
and save OpenCV YAML files; `docs/samples/sample_calibration.yml` shows the expected schema.

### 5. Reconstruction

Relevant code: `Application::ReconstructModel()` and `src/core/Scan3d.*`.

For each valid decoded camera pixel, `Scan3d` builds a camera ray and a projector ray from
the calibration. Their approximate intersection becomes a 3D point. Points are rejected
when the correspondence is invalid, contrast is too low, or the distance between the two
rays exceeds the configured maximum.

The resulting `Scan3d::Pointcloud` contains positions and can contain colors and computed
normals. `PointcloudWidget`, an OpenGL preview with an orbit camera, displays the current
result in 3D View.

### 6. Reconstruction export

Relevant code: `MainWindow::on_reconstruction_action_button_clicked()`,
`src/export/IOExport.*`, and `src/core/IoUtil.*`.

The Reconstruct action processes the currently selected capture set and then asks for an
output file. XYZ output can contain positions or positions plus RGB. PLY can be ASCII or
binary and can include colors and normals according to the Reconstruction controls.

The Calibration menu also exposes reconstruction from a previously saved decoded-pattern
dump through `Application::LoadDump()` and `Application::ReconstructModelDump()`.

### 7. Point-cloud editor

Relevant code: `src/ui/pointcloud_editor/*`, `src/ui/preview/PointcloudPreviewWidget.*`,
and `src/core/PointcloudOps.*`.

The Point Cloud action opens a maximized modal editor over the workspace's `.xyz` files.
Selecting a file loads it asynchronously and keeps recently parsed clouds in a 512 MiB LRU
cache keyed by path, size, and modification time.

The editor provides:

- statistical outlier removal, configured by neighbor count and standard-deviation
  multiplier;
- iterative RANSAC plane extraction with optional least-squares coefficient refinement;
- iterative RANSAC sphere extraction after dominant planes have been removed;
- a multi-cloud OpenGL preview with an overlay showing each cloud's color and point count;
- drag-to-reorder drawing, eye buttons to change visibility, trash buttons to remove an
  entry, and double-click name editing; and
- directory export of every entry still in the overlay, using its edited name. Hidden
  entries are exported; deleted entries are not. Invalid filename characters and duplicate
  names are handled automatically.

Selecting Plane or Sphere from Fit Figure opens the corresponding parameter dialog and
starts the operation after **Done**. The selector returns to None after completion or
cancellation so a later selection always represents a new command.

`PointcloudOps` is independent of Qt and PCL. It uses OpenCV FLANN for nearest-neighbor
searches and OpenCV numerical routines for model refinement.

## State and persistence

`Application::config` is the single `QSettings` instance. On Windows it uses the native
user scope under `HKEY_CURRENT_USER\Software\CENAM\SMCP`. Keys and defaults live in
`src/common/Settings.h`.

Main-window controls write changes immediately. The workspace, calibration path, camera and
projector choices, algorithm parameters, and main-window geometry therefore survive a
restart. Machine-specific build paths are separate: `bootstrap.ps1` writes them to the
untracked `CMakeUserPresets.json`.

## Threading model

- Qt widgets and `Application` state belong to the main thread.
- `CameraWorker` owns camera acquisition in a dedicated `QThread` and communicates with the
  capture dialog through queued signals and slots.
- Point-cloud file loading, outlier removal, and model fitting use `QtConcurrent::run` with
  `QFutureWatcher`; completion handlers update widgets back on the UI thread.
- Decoding, corner extraction, calibration, and reconstruction currently run from the main
  thread. Their processing dialog calls `QApplication::processEvents()` to update progress
  and accept cancellation.

Never access a widget from a worker function. Return data through a signal, future result,
or immutable/shared ownership and update the UI only in the receiving main-thread code.

## Adding or changing a module

1. Put numerical logic without widget dependencies in `src/core`.
2. Put import/export format code in `src/export`.
3. Put a form-based dialog directly in `src/ui` or in a feature subdirectory when several
   forms belong together.
4. Put reusable preview widgets in `src/ui/preview`, Qt item models in `src/ui/models`, and
   non-form functional widgets in `src/ui/widgets`.
5. Add every new source explicitly to the matching list in `CMakeLists.txt`; the project
   deliberately does not use recursive source globs.
6. Add embedded assets to `resources/resources.qrc` and keep styling in
   `resources/theme/smcp.qss` when possible.
7. Use an asynchronous worker for operations that can noticeably block interaction, and
   document ownership of the returned data.

For user-facing operation, see [USER_GUIDE.md](USER_GUIDE.md). For build and dependency
setup, see [BUILD.md](BUILD.md).
