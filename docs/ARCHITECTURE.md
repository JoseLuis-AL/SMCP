# Architecture

SMCP is a Windows desktop application for projector-camera structured-light scanning. It is
built as one C++17 executable with Qt 5, OpenCV, OpenGL, and optional Spinnaker camera
support.

`smcp::Application`, a `QApplication` subclass, owns the application-wide state: persistent
settings, the capture-set model, calibration data, decoded patterns, detected corners, and
the current reconstructed point cloud. `MainWindow` and the dialogs coordinate that state
and present it to the user. Numerical algorithms are kept in `src/core` so they do not
depend on the UI: the core reports progress and polls for cancellation through the
`ProgressSink` interface (`core/Progress.h`), and the application supplies a sink that forwards
to its dialogs.

The build has two targets: the static library `smcp_app` (everything except `main()`) and the
`SMCP` executable. Tools that need the whole application link the library, so each source is
compiled once.

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
| `src/ai` | Typed asynchronous bridge between Qt and the versioned `smcp` API in WSL |
| `src/core` | Structured-light, calibration, reconstruction, point-cloud algorithms, and shared guards |
| `src/export` | XYZ point-cloud import and export |
| `src/common` | Settings keys, filenames, and OpenCV-to-Qt conversion helpers |
| `resources` | Embedded icons and the application-wide QSS theme |
| `scripts` | Dependency discovery, repeatable configure/build commands, and release packaging |
| `scripts-wsl` | Conda environments, model registry, CLI dispatcher, verification, and CLI tests |
| `cmake` | Custom package discovery, the localized-compiler dependency fix, and compile-database support |
| `tests` | C++ tests registered with CTest |

Most code is in the `smcp` namespace. Algorithm groups use nested namespaces such as
`smcp::Pointcloud`, `smcp::Scan3d`, `smcp::StructuredLight`, and `smcp::IoUtil`.

## Component reference

| Component | What it does |
|---|---|
| `Application` | Owns persistent settings and processing state; turns user actions into jobs for the core, runs them on a worker thread, and adopts their results on the GUI thread. Keeps decoded sets and reuses them while the images and decode parameters are unchanged |
| `TaskRunner` | `QueuedProgress` (thread-safe `ProgressSink` that re-emits reports as Qt signals), `RunBlocking` (runs a job on a worker while the caller keeps dispatching events) and `BusyProgressDialog` (stays modal until its job ends) |
| `MainWindow` | Connects the primary actions and parameter controls to `Application`, selects capture sets, switches previews, and exports reconstructed clouds |
| `CaptureDialog` | Coordinates the camera worker and projector, exposes capture settings, and records complete pattern sequences |
| `CalibrationDialog` | Displays calibration matrices, distortion coefficients, poses, and errors |
| `ProcessingDialog` | Reports progress and messages for long main-workflow operations and provides cancellation state |
| `AboutDialog` | Displays application and attribution information |
| `PointcloudEditorDialog` | Loads workspace XYZ files, schedules editing commands, caches parsed clouds, and exports the current layers |
| `WslModelService` | Discovers and describes registered AI models, converts paths, runs/cancels inference, and maps WSL diagnostics |
| `AiModelConfigDialog` | Edits and validates model-specific JSON against the schema supplied by WSL |
| `AiInferenceProgressDialog` | Shows the CPU/AI icon, Fluent Thinking title, current cloud name and count, grouped progress, elapsed time and model output; requests cancellation |
| `ExportPointcloudDialog` | Chooses between exporting the selected cloud, one file per selected cloud, or the selected visible clouds combined |
| `RemoveOutliersDialog`, `PlaneFitDialog`, `SphereFitDialog`, `IcpDialog` | Validate and return parameters for their respective point-cloud operations |
| `SphereMeasurementDialog`, `SphereResultsDialog` | Configure sphere measurement, then show object rows grouped by cloud, original and corrected values, error statistics and observations; export measurements and training data |
| `TreeModel` | Represents capture-set directories and their images as a checkable Qt item model |
| `ImageLabel` | Adds image-oriented sizing and display behavior to a label |
| `PixmapWidget` | Displays camera preview pixmaps while preserving their useful layout behavior |
| `PointcloudWidget` | Renders the single reconstructed `Scan3d::Pointcloud` used by the main window |
| `PointcloudPreviewWidget` | Renders multiple editable colored clouds and owns the editor's layers; emits `PointcloudsChanged()` on every change |
| `EditorScene` | The editor scene as plain data and the operations on it (selection, deletion, transforms, merging, history trimming), independent of widgets and OpenGL |
| `PointcloudLayersPanel` | Tree of the viewer's layers beside it: a check shows or hides a cloud, the highlight and the "Apply to all checked" box decide the target clouds of the commands, and it edits order, color and name through the viewer's public layer API |
| `ProjectorWidget` | Generates and displays fullscreen Gray-code patterns and alignment content |
| `CameraWorker` | Runs acquisition off the UI thread and emits preview/capture results |
| `CameraUtilities` | Discovers and configures Spinnaker devices and converts camera-node operations into reusable helpers |
| `CameraSettings` | Defines the values transferred when a camera is configured |
| `StructuredLight` | Loads images, estimates direct illumination and decodes Gray codes; `DecodeGraySet()` is the whole decoding stage of one capture set |
| `Calibration` | The calibration pipeline as plain functions: `ExtractCorners`, `ProjectorCorners`, `Solve`, and `Run` for the whole sequence |
| `Scan3d` | Creates projector views, triangulates correspondences, and computes point-cloud normals |
| `CalibrationData` | Owns calibration matrices and serializes/deserializes calibration YAML |
| `Parallel` | `ParallelFor`: splits a range into contiguous blocks, one per thread, and rethrows the first exception |
| `Progress` | `ProgressSink`, the observer interface between the core and whoever runs it |
| `PointcloudOps` | Implements statistical filtering, RANSAC plane/sphere extraction, and ICP registration for colored clouds; each accepts a `CancelFlag` and leaves by throwing `OperationCanceled` |
| `SphereMeasurement` | Detects reference spheres, fits them geometrically with sigma-based outlier rejection, and labels them against a CMM artifact |
| `Compensation` | Loads compensation models written by the pinn-smcp trainer, verifies their test vectors, and corrects colored clouds or reconstruction matrices |
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

Relevant code: `Application::DecodeAll()`, `Application::Decode()`, and
`src/core/StructuredLight.*`.

For every selected capture set, `StructuredLight::DecodeGraySet()` loads each image once (in
parallel), estimates direct and indirect illumination, and decodes horizontal and vertical Gray
codes by rows in parallel. `Application` stores the result with a key made of the image files
(path, size, date), `b`, `m` and the projector size; a later decode, calibration or
reconstruction with the same key reuses it instead of decoding again. The result is a
camera-sized projector-coordinate map plus minimum/maximum intensity data. The main window
uses these matrices to render Pattern View and Projector View.

The Decode controls provide the contrast threshold, black-light ratio (`b`), and minimum
direct-light component (`m`). These values affect which camera pixels receive a valid
projector correspondence.

### 3. Chessboard corner extraction

Relevant code: `Application::ExtractChessboardCorners()` and `Calibration::ExtractCorners()`.

OpenCV finds and refines the interior chessboard corners in each selected capture (sets run in
parallel). The button and the calibration use the same function, so the corners shown are the
corners calibrated. SMCP
associates those image points with physical board coordinates computed from the configured
row count, column count, and square dimensions. Results are stored as camera and world
corner arrays for calibration.

### 4. Projector-camera calibration

Relevant code: `Application::Calibrate()`, `src/core/Calibration.*` and
`src/core/CalibrationData.*`.

`Calibration::Run()` executes the stages in order. SMCP maps each detected camera corner into
projector coordinates by fitting a local homography in the decoded correspondence map (one
homography per corner, computed in parallel). A corner closer to the image border than the
homography window, or a window with too few decoded pixels, stops the calibration with a message
that names the set. OpenCV then calibrates the camera and
projector separately and performs stereo calibration to obtain their relative rotation and
translation.

With `Calibration::Params::subpixelCorners` (setting `Calibration/SubpixelCorners`) the local
homographies are fitted on the sub-pixel projector coordinates of `StructuredLight::RefinePattern`
instead of the whole decoded ones (`Calibration::SubpixelProjectorCorners`).

`Calibration::SolveOptions` selects how that last stage works. By default the stereo step keeps
the intrinsics of each device (`CALIB_FIX_INTRINSIC`), as the program always did. With
`jointRefinement` (setting `Calibration/JointRefinement`) it starts from them and optimizes every
parameter over all the views. Three other ideas were measured on boards left out of the
calibration and are not offered: estimating k3 changed nothing, the rational distortion model
changed nothing or made the projector unstable, and a bent model of the board made those boards
clearly worse. Measure any new idea the same way with `SMCP_calibration_compare` before adding it.

`CalibrationData` stores camera intrinsics and distortion, projector intrinsics and
distortion, stereo rotation and translation, and the three reprojection errors. It can load
and save OpenCV YAML files; `docs/samples/sample_calibration.yml` shows the expected schema.

### 5. Reconstruction

Relevant code: `Application::ReconstructModel()` and `src/core/Scan3d.*`.

`Scan3d::ReconstructModel` averages the camera pixels that decode to the same projector pixel,
builds a camera ray and a projector ray from the calibration, and takes their approximate
intersection as the 3D point. Projector pixels are triangulated in parallel. Points are rejected
when the correspondence is invalid, contrast is too low, or the distance between the two
rays exceeds the configured maximum.

The sub-pixel method (setting `Reconstruction/Subpixel`, on by default) is used instead of it
when the images of the set are available; a decoded dump is always reconstructed the classic way. `StructuredLight::RefinePattern` turns the decoded map of whole projector
coordinates into fractional ones: along each camera row (column) it finds where the decoded
column (row) steps by one, places that border between the two pixels where the stripe image of
the Gray bit that changes there crosses its inverse, and interpolates linearly between
consecutive borders. It needs the gray images of the set, which `Application` loads again with
`StructuredLight::LoadGraySet`; the decode cache is not changed. `Scan3d::ReconstructModelSubpixel`
then triangulates every camera pixel on its own, so that cloud is organized like the camera image
and not like the projector image. Both reconstruction functions take a `Scan3d::Triangulation`:
`RayMidpoint` (the classic one) or `ProjectorPlane` (setting `Reconstruction/PlaneTriangulation`),
which cuts the camera ray with the plane of light of the projector coordinate that runs across
the baseline. Coordinates keep the convention of the calibration: a whole
number is the centre of a projector pixel.

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
`src/core/PointcloudOps.*`, `src/ai/WslModelService.*`, and `scripts-wsl/*`.

The Point Cloud action opens a maximized modal editor. Its left **Point Clouds** panel owns
selection by stable cloud IDs. The header adds workspace XYZ files through a modal chooser
and exports selected visible clouds. Ctrl/Shift provide multiple selection; hiding a cloud
removes it from the editing selection. Row order determines drawing priority, with the top
row rendered over lower clouds at overlapping pixels.

`PointcloudPreviewWidget` owns scene states and a 50-command history. Point arrays and
immutable selection masks are shared by snapshots; modifications create new arrays. Scene
changes are prepared on workers and published together on the GUI thread. Undo/Redo uses the
same path and restores stable IDs, layer and point selection, visibility, colors, names, and
order. Each layer keeps positions and colors in separate GPU buffers: a selection re-sends only
colors, and a layer color is a shader parameter that re-sends nothing. GPU uploads are limited to
32 MiB per frame; while the buffers of an edited cloud are still uploading, the viewer keeps
drawing the ones they replace, so an edit never makes a cloud blink. Scenes above 4 million
visible points are drawn decimated while the camera moves. Rectangle and radial selections test
the points in parallel.

`RunBackground` captures worker exceptions before QtConcurrent can translate them, handles
completion failures, and runs cleanup through a scope guard. `RunSelected` stages a whole
multi-cloud method as one command. AI input writing is asynchronous; all inference results
are staged until the batch succeeds. Export and merge work also run outside the GUI thread.

Manual selection includes every selected visible cloud point projected inside its rectangle,
without depth or occlusion rejection. Move/rotate prepares transformed copies around the
selection centroid. The Align Centroids command translates selected visible clouds onto the
first selected cloud in panel order without changing the reference. Header merge concatenates
selected visible clouds on a worker, replaces their rows at the first selected position, and
records one history state; Undo recovers original data and selection. ICP is enabled for two
or more selected clouds and labels results with `(ICP) (Merged/Reference/Aligned)` plus their
original names. Navigation initializes
its center once from the first loaded cloud and keeps manual pan separate. It uses middle-drag
orbit, Shift+middle pan, Ctrl+middle/wheel zoom, F to fit selection around the fixed center,
and a six-sided axis selector. Color overrides affect display only.

Radial selection records a stroke and its per-sample radius on the GUI thread, with a circle
and path preview. On release a worker rasterizes its swept disks into a temporary screen mask
and projects points once, adding only previously unselected hits. This keeps point processing
linear in cloud size rather than comparing every point with every mouse sample. The result
uses the normal atomic scene/history path; an unchanged mask produces no command. Cancellation
discards the uncommitted stroke. Ctrl+wheel changes brush size, including mid-stroke.

The editor menu bar exposes the unchanged command handlers through QAction entries. PointCloud
contains outlier removal, centroid alignment, ICP and the dynamic AI Models submenu. Fit contains
Plane/Sphere and Measure contains Spheres/Compensate. Menu actions need no selection-reset state.
`EditorDialogStyle` follows the editor's parent-widget chain
across nested dialog windows and gives acceptance/cancellation controls equal widths. File
pickers in this workflow use Qt dialogs so the same sizing and theme cover their buttons.

See [the detailed analysis and history contract](EDITOR_ASYNC_HISTORY.md).

`PointcloudProject` serializes complete editor sessions as `.smcppce`: shared XYZ/RGB assets,
selection masks, ordered layer metadata, up to 50 undoable commands and the current navigation view.
Atomic saves and bounded, checksummed reads run off the GUI thread. Restoration prepares current
buffers first, then installs the scene and history together. Project I/O and file exports do not
become geometry commands. Batch layer deletion uses the normal history pipeline and ignores hidden layers.

`PointcloudOps` is independent of Qt and PCL. It uses OpenCV FLANN for nearest-neighbor
searches and OpenCV numerical routines for model refinement. RANSAC scores each hypothesis by
counting inliers (in parallel on large clouds) and builds the inlier list only for a new best
model; rigid transforms are applied in parallel. `IOExport` formats XYZ text in memory and
writes it in 1 MB blocks.

`SphereMeasurement` turns structured-light scans of a reference artifact into metrology data.
`MeasureSpheres()` removes the dominant planes, detects spheres of the nominal diameter with the
RANSAC of `FitSpheres()`, takes every remaining point within three RANSAC thresholds of each
detection, and fits |p - c| - r by Levenberg-Marquardt. The fit is repeated without the points
beyond the rejection limit (2.5 standard deviations by default) until the kept set is stable.
`LabelSpheres()` tries every assignment of the spheres to the artifact names and keeps the one whose
center distances and diameters best match the CMM values. `Measurands()` lists the diameters and center
distances of labeled spheres with their reference values, `FixedScale()` fits the single
least-squares correction factor over them, and `Coverage()` gives the per-axis range of the centers
and the correlation between axes. `SphereResultsDialog` validates the fixed scale by leaving one scan
out at a time and stores it in the training manifest as the starting value of a compensation model's
global scale. `tests/SphereMeasurementTests.cpp` checks the fit, the labeling, the fixed scale, and
the coverage statistics on synthetic data. `tests/SphereResultsTests.cpp` checks grouping, missing and
unidentified objects, comparison statistics, validation and consistent unit switching in the UI and CSV.
The dialog defaults to µm; the mm checkbox changes all displayed lengths and measurement CSV columns.
Training coordinates and manifest values retain mm for model compatibility.
`ExcelExport` writes the visible result columns, error statistics and observations as a single-sheet
Open XML `.xlsx` report. The dialog snapshots its text and numeric cells before passing them to the
existing export worker. XML escaping and numeric cell types preserve literal cloud names, and a
new-only file open prevents overwrite races. ZIP packaging uses the Qt kit's `GuiPrivate` headers;
recheck that private API when upgrading Qt. The deployment still uses the existing QtGui DLL.

`AlignIcp()` builds one kd-tree over the target and, for point-to-plane, estimates the target
normals once from their ten nearest neighbors. Each iteration moves a fixed-seed random subset
of the source, pairs every point with its nearest target point, drops pairs beyond the maximum
distance, and keeps only the closest overlap fraction (trimmed ICP). The update is the linearized
point-to-plane least-squares step solved by SVD, or the Kabsch SVD solution for point-to-point;
the point-to-point step is the fallback when the linear system cannot be solved. Iteration stops
when the relative RMSE change falls below the threshold, when the RMSE reaches float precision,
or at the iteration limit. ICP only refines an approximate alignment: optional centroid alignment
is the only global initialization. `tests/PointcloudOpsTests.cpp` checks it on synthetic clouds.

### 8. AI models in WSL

Relevant code: `src/ai/WslModelService.*`, `src/ui/pointcloud_editor/Ai*`, and
`scripts-wsl/smcp`.

The AI models are optional and live entirely in Ubuntu 22.04 under WSL. The Windows side only
talks to the versioned `smcp` launcher installed in `~/.local/bin`:

1. **Discovery.** `WslModelService::ListModels()` runs `smcp --list-models --json` when the
   editor opens. The AI Models submenu is disabled while it runs and stays disabled when WSL
   is unavailable or no model is valid; reopening the editor searches again. Selecting a
   model opens its configuration on every activation. QAction data retains the model ID and its
   callback uses the existing configuration/inference path. Discovery preserves registry order.
2. **Configuration.** `smcp --describe-model` returns the defaults and schema.
   `AiModelConfigDialog` validates the edited JSON with the same rules as the launcher.
3. **Inference.** For each selected cloud in turn, the editor writes the cloud and the configuration to a temporary
   Windows directory, converts the paths with `wslpath`, and runs
   `smcp --model ... --job-id <uuid>`. `AiInferenceProgressDialog` stays open, window-modal
   and non-blocking, until the result has been read back and added to the layers.
4. **Cancellation.** Cancel runs `smcp --cancel-job <uuid>`, which sends SIGTERM to the job's
   process group. If the job has not stopped after a grace period, or the two-hour timeout
   expires, `wsl.exe` is terminated as well.

Every WSL command is a fixed `bash -lc` script that forwards values through `"$@"`, so model
identifiers, paths, and configuration are never interpolated by a shell.

The launcher does not run the model directly. It starts a supervisor (`smcp --supervise-job`)
in a new Linux session and keeps the write end of a pipe whose read end the supervisor
watches. Terminating `wsl.exe` kills the launcher without any chance to clean up, but the
kernel still closes that pipe; the supervisor then terminates `conda run` and the model,
escalating to SIGKILL after ten seconds. As a result, a crashed, force-closed, or timed-out SMCP does
not leave a model running on the GPU.

The registry also lists **trainers** (`smcp --list-trainers`, `smcp --train`), run through the
same supervised launch. `scripts-wsl/trainers/pinn-smcp/train_pinn_smcp.py` trains the SMCP
compensation from the folder written by **Export Training Data**: a physical block that scales
each point along its camera ray plus a bounded, regularized network, fitted so that the spheres
refitted on the corrected points match the CMM diameters and center distances. The sphere fits
are batched and differentiable (algebraic fit followed by Gauss-Newton steps). The trainer
cross-validates over scans against the fixed scale and the physical block alone, and writes a
JSON model (`format: smcp-compensation`) with the coefficients, the network weights, the covered
volume, and an optional calibration hash.

`src/core/Compensation.*` evaluates those models in C++ (`p' = p * (1 + s) + w * r`, with the
network fading to zero over `extrapolation_margin` outside the trained volume) using all CPU
threads. Loading reproduces the model's `test_vectors` within 1e-6 mm, so a format or evaluator
mismatch is rejected rather than applied. The main window applies a model after reconstruction
(`MainWindow::ApplyCompensation`, settings `Reconstruction/Compensate` and
`Reconstruction/CompensationFile`) and compares the stored calibration hash with the current
calibration file; the editor's **Compensate** command and **Measure Spheres** apply it to layers and
campaigns. `tests/CompensationTests.cpp` checks the evaluator against a model trained by the
trainer (`tests/fixtures/compensation_model.json`).

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
- AI discovery and inference use non-blocking `QProcess` operations through
  `WslModelService`; result parsing and GPU-buffer preparation use `QtConcurrent::run`. The
  progress window is opened with `QDialog::open()` rather than `exec()`, so inference output
  and completion callbacks are delivered normally while it is visible.
- Decoding, corner extraction, calibration, and reconstruction run on a worker thread through
  `RunBlocking` (`app/TaskRunner.h`). The core reports through `QueuedProgress`, whose signals Qt
  delivers on the GUI thread; the modal processing dialog stays responsive and its Cancel button
  sets a flag the core polls. The job works on copies of its inputs and `Application` adopts the
  results when it returns.
- Inside a job the core uses `Parallel::ParallelFor` over independent items (images, rows,
  corners, projector pixels, points). Work is split into contiguous blocks and every item writes
  only its own output, so results do not depend on the number of threads.

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

## Verifying that results do not change

- `SMCP_pipeline_regress <workspace> --cols N --rows N [--skip i,j]` runs the original serial
  pipeline (kept untouched in `tests/legacy`) and the current one on a real workspace and
  requires corners, decoded maps, projector corners, calibration, point cloud, colors and normals
  to match bit for bit. It only reads the workspace.
- `SMCP_subpixel_tests` (ctest `subpixel_reconstruction`) checks the sub-pixel decoding and
  reconstruction on synthetic images with a known answer.
- `SMCP_reconstruct_compare <workspace>` reconstructs the calibration sets of a workspace with
  both methods and reports the scatter of the chessboard around a fitted plane, for the whole
  board and for small patches. `SMCP_decode_stats <set>...` reports how many camera pixels see
  each projector pixel and how noisy the classic correspondences are. Both only read the data;
  use them to judge a change of the reconstruction before and after making it.
- `SMCP_calibration_compare <workspace>` solves the calibration the classic way and with joint
  refinement from the corners saved in a workspace, leaving groups of sets out, and reports how
  well each one reconstructs the boards it did not see (distances between corners, flatness, gap
  between rays). `--write <folder>` saves both calibrations solved with every set.
- `SMCP_reconstruct_set <set> --calibration <yml> --out <cloud.xyz> [--subpixel] [--plane]`
  reconstructs one capture set with a given calibration and method. Together with
  `SMCP_calibration_compare --write` and `SMCP_measure_spheres` it measures a calibration or
  reconstruction option on real spheres before it is offered in the application.
- `SMCP_measure_spheres --artifact <artifact.json> <cloud.xyz>...` measures the spheres of
  point-cloud files against a reference artifact from the command line (diameter and distance
  errors, form RMS). Use it to compare denoising or compensation settings on existing clouds;
  `--export <folder>` writes the labeled sphere points for the compensation trainer.
- `tests/IoExportTests.cpp` requires the XYZ writers to produce the same bytes as the original
  stream-based writers.
- `SMCP_viewer_render_check` renders a synthetic cloud on a real OpenGL context and checks the
  pixels, including the frames in which a replacement buffer is still uploading.
- `SMCP_SETTINGS_DIR=<folder>` makes the application store its settings in an INI file under
  that folder instead of the registry, for tests and side-by-side copies.

Tests and tools must treat capture workspaces as read-only: copy the files they need, and never
place links to them inside a folder that is deleted afterwards.

For user-facing operation, see [USER_GUIDE.md](USER_GUIDE.md). For build and dependency
setup, see [BUILD.md](BUILD.md).
