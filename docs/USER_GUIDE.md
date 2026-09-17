# User guide

This guide covers the normal SMCP workflow: choose a workspace, acquire or load Gray-code
captures, calibrate a projector-camera pair, reconstruct a point cloud, and inspect or edit
XYZ files.

Build the program first by following [BUILD.md](BUILD.md). A direct camera connection is
optional for processing existing data but requires a build with Spinnaker enabled.

## Concepts and data layout

A **workspace** is a directory that contains one subdirectory per capture set. A capture
set represents one board or object pose and contains the numbered Gray-code images plus a
`projector_info.txt` file. The main window lists these subdirectories and lets you select
which sets participate in decoding, corner extraction, and calibration.

XYZ files placed directly in the workspace are available to the Point Cloud Editor.
Calibration is stored separately as OpenCV YAML.

```text
workspace/
  capture-01/
    cam_01.png
    cam_02.png
    ...
    projector_info.txt
  capture-02/
    ...
  reconstructed-part.xyz
```

## Use the example workspace

SMCP provides a data-only `examples` branch with five complete capture sets, a saved
calibration, and three XYZ point clouds. Clone it into a separate directory:

```powershell
git clone --branch examples --single-branch https://github.com/JoseLuis-AL/SMCP.git SMCP-examples
```

Start an SMCP executable built from `main`, click **Directory**, and select the root
`SMCP-examples` directory as the workspace. The five timestamped directories appear as
capture sets, while the three XYZ files become available in the Point Cloud Editor.

For the supplied captures, configure 22 by 15 interior corners and a square size of 15 by
15. Run **Decode**, **Extract Corners**, and **Calibrate** in that order. You can then
select a decoded set and run **Reconstruct**, or load the supplied `calibration.yml` to
test reconstruction without recalibrating.

See [Example datasets](EXAMPLES.md) for the complete file inventory, expected editor
tests, and detailed validation workflow.

## First launch

1. Start `SMCP_d.exe` for Debug or `SMCP.exe` for Release from the generated `bin`
   directory.
2. Click **Directory** in the Workspace section and choose a workspace.
3. Enter the checkerboard's interior corner count and physical square width and height.
   Use one consistent physical unit, normally millimetres; reconstructed coordinates use
   that same unit.
4. Set the Decode, Calibration, and Reconstruction controls. The built-in defaults are a
   practical starting point.

Settings are saved automatically for the current Windows user.

## Hardware preparation

For reliable calibration and reconstruction:

- mount the camera and projector rigidly; do not move either device after calibration;
- focus both devices at the intended working distance;
- disable automatic exposure, gain, gamma, white balance, and focus at the device level;
- avoid saturation in both the light and dark projected frames;
- use a flat checkerboard with accurately known square dimensions; and
- reduce ambient-light changes and motion during each pattern sequence.

The projector must be connected as a Windows display. Extend the desktop to it instead of
mirroring when the displays have different resolutions.

## Capture a data set

The **Capture** action is available only in builds configured with
`SMCP_WITH_SPINNAKER=ON`.

1. Open **Capture** and choose the projector screen and camera.
2. Choose the pattern count. More Gray-code levels provide a larger addressable projector
   range but require more frames.
3. Use **Alignment Mode** to position the board or object within the shared camera/projector
   field of view.
4. Adjust black level, gain, gamma, exposure time, and the wait between patterns. The gray
   statistics help detect underexposure or clipping.
5. Set **Output directory** to the workspace where capture sets should be created.
6. Click **Capture** and keep the scene still until the progress completes.
7. Repeat for multiple checkerboard poses. Cover different image regions, angles, and
   depths while keeping every complete set sharply visible.

SMCP creates a timestamped subdirectory and writes its `cam_NN.png` sequence and projector
metadata there. The Workspace tree is rescanned automatically when capture finishes.

## Calibrate the system

Select the desired capture sets in the Workspace tree. **All** and **None** provide quick
selection controls.

1. Click **Decode** to convert the Gray-code frames into camera-to-projector
   correspondences.
2. Click **Extract Corners** to locate and refine the checkerboard corners.
3. Inspect **Image View**, **Pattern View**, and **Projector View**. Exclude sets with poor
   corner detection, motion, saturation, or large undecoded regions, then rerun the needed
   operations.
4. Click **Calibrate**. SMCP calibrates the camera and projector and estimates their relative
   pose.
5. Use **Calibration > Display...** to inspect the matrices and reprojection errors.
6. Use **Calibration > Save...** to save the result as YAML.

An existing calibration can be restored with **Calibration > Load...**. The expected file
shape is illustrated by [samples/sample_calibration.yml](samples/sample_calibration.yml).

### Parameter guidance

| Control | Effect | Adjustment guidance |
|---|---|---|
| Threshold | Minimum contrast accepted during decoding/reconstruction | Raise it to reject noisy low-contrast pixels; lower it if valid dark areas disappear |
| `b` | Black-light ratio used by robust direct-light estimation | Keep it between 0 and 1; start with `0.5` |
| `m` | Minimum direct-light component in Gray-code levels | Raise it when indirect light causes false codes; start with `5` |
| H Win | Local homography window used to map a camera corner to the projector | The default `60` is a starting point; use a window large enough to contain stable decoded pixels around each corner |
| Max. ray distance | Maximum separation between camera and projector rays | Lower values reject uncertain triangulations; the value uses the checkerboard's physical unit |

Change one parameter at a time and evaluate the resulting correspondence views and errors.

## Reconstruct a point cloud

1. Load a valid calibration if the current session has not just produced one.
2. Select one decoded capture set in the Workspace tree.
3. Choose whether the output should include **Normals** and **Colors**. **Binary format**
   applies to PLY export.
4. Click **Reconstruct**.
5. Choose XYZ, ASCII PLY, or binary PLY in the save dialog.
6. Open **3D View** to inspect the result.

Mouse controls in the 3D views are:

| Input | Action |
|---|---|
| Left-button drag | Orbit around the cloud |
| Middle-button drag | Pan |
| Mouse wheel | Zoom |

If reconstruction produces sparse or noisy geometry, first inspect Pattern View for holes,
then review Threshold and Max. ray distance. A bad calibration cannot usually be repaired by
loosening reconstruction thresholds; recapture problematic calibration poses instead.

## Point Cloud Editor

Click **Point Cloud** to open the editor. The **Point Cloud** selector lists the `.xyz` files
located directly in the current workspace. The list is read when the editor opens; close and
reopen the editor to see files added or exported to the workspace afterward.

Select a file and click **Load** to add it to the overlay in the top-right corner of the
viewer. Load several files, or the same file more than once, to compare them. Parsed files
are cached while the editor is open, so loading a large cloud again is fast.

### The first point cloud

Every command works on the **first point cloud**: the top row of the overlay, marked with an
arrow icon. Drag rows to change which cloud is first and the order in which clouds are drawn.
Commands never modify a cloud in place; their results are added as new rows.

Loading and processing run in background workers. The command bar is disabled and the wait
cursor is shown while an operation is active; AI inference shows its own progress window
instead. The window close button does not interrupt an operation in progress.

### Remove outliers

1. Make the cloud to filter the first cloud.
2. Click **Remove Outliers**.
3. Set **Neighbors (k)** and **Standard deviation multiplier**.
4. Click **Done** to run or **Cancel** to leave the cloud unchanged.

The operation removes points whose mean neighbor distance is above the global mean plus the
chosen number of standard deviations. A smaller multiplier removes more points; a larger
neighbor count smooths the density estimate but costs more processing time.

The filtered result is inserted at the top of the overlay as **Filtered**, so it becomes the
first cloud and the next command works on it. The input cloud stays in the list, recolored
red, which makes the removed points easy to see behind the result.

### Fit planes or spheres

Select **Plane** or **Sphere** from **Fit Figure**. Selecting **None** does nothing.

- Plane parameters control the number of planes, RANSAC iterations, point-to-plane distance,
  minimum inliers, and coefficient refinement.
- Sphere parameters add minimum and maximum radius and use a point-to-surface distance.
- A minimum-inlier value of **Automatic** derives a threshold from the cloud size.
- **Done** starts the fit; **Cancel** makes no change. The selector returns to **None** in
  either case.

The fit uses the first cloud. When it finds models, every existing row is hidden and each
detected model is added as a separate colored cloud (**Plane 1**, **Sphere 1**, ...). Use the
eye buttons to show the original clouds again. Sphere fitting first removes dominant planes
internally so planar backgrounds are less likely to overwhelm the sphere search; the status
bar reports each sphere's center, radius, and point count.

### Run an AI model

AI models are optional. They run in Ubuntu 22.04 under WSL and require the setup described
in [WSL models quick start](WSL_MODELS_QUICKSTART.md). Users who do not need them can ignore
the **AI Model** selector; the rest of the editor works the same without WSL.

When the editor opens, SMCP searches the validated `~/smcp-models/models.json` registry in
WSL. The selector shows **Searching...** and stays disabled until the search finishes, which
can take a few seconds while WSL starts. If no model is found, or WSL is unavailable, the
selector remains disabled and its tooltip explains why. Close and reopen the editor to search
again, for example after finishing the WSL setup.

1. Make the input cloud the first cloud.
2. Select `Score Denoise`, `StraightPCF`, or `PointCleanNet` from **AI Model**.
3. Edit the JSON configuration. The dialog checks parameter names, types, enumerated values,
   and ranges while you type, and **OK** is available only for a valid configuration.
4. Click **OK** to start inference. The **Running AI Model** window shows *Thinking...*,
   the elapsed time, and the latest message reported by the model, and blocks the editor
   until the model finishes.
5. When inference finishes, the result is added to the overlay with the registered suffix,
   for example `example_piece_score_denoise.xyz`.

The selector returns to **None** after the model finishes, fails, or is cancelled, and when
the configuration dialog is cancelled, so selecting a model always opens its configuration.

Click **Cancel** in the **Running AI Model** window, or close that window, to stop the model.
The model process in WSL is terminated and its partial result is discarded. Escape does not
cancel, to avoid stopping a long run by accident.

Accepted settings are remembered per model while the editor remains open: select the same
model again to run it with the previous parameters or change them. Common tuning parameters
include `ld_num_steps` and `cluster_size` for Score Denoise, `niters`,
`patch_size`, and `cluster_size` for StraightPCF, and `iterations`, `cell_size`,
`batch_size`, and `smoothing_neighbors` for PointCleanNet. Some models reject very small
clouds; PointCleanNet, for example, needs enough points to fill its processing cells.

### Manage editor results

Each overlay row shows, from left to right:

- the first-cloud arrow, only on the top row;
- a color swatch. Click it to paint the whole cloud with a preset or `RRGGBB` color, or
  choose **None** to use the colors stored in each point;
- the row number and an editable name. Double-click the name to edit it; press Enter or
  click elsewhere to finish;
- the point count;
- an eye button to hide or show the cloud. Visibility affects the preview and combined
  export; and
- a trash button to remove the cloud from the editor.

Drag rows to change the first cloud and the draw order.

### Export point clouds

Click **Export** and choose what to write:

| Option | Result |
|---|---|
| **First point cloud only** | Asks for one `.xyz` filename and writes the first cloud |
| **Each point cloud as a separate file** | Asks for a directory and writes one `.xyz` file per row, hidden rows included |
| **All visible point clouds combined into one file** | Asks for one filename and writes every visible cloud merged into a single cloud |

Edited row names become filenames: the first cloud's name is suggested in the save dialog,
and separate files are named after their rows. SMCP replaces characters that Windows does not
allow in filenames and, for separate files, appends a number to duplicate names so one
exported cloud never overwrites another. The combined file is suggested as `combined.xyz`.
Removed rows are never exported.

## File formats

| Format | Use |
|---|---|
| Capture set | Numbered `cam_NN.png` images plus `projector_info.txt` |
| Calibration | OpenCV YAML with `cam_K`, `cam_kc`, `proj_K`, `proj_kc`, `R`, `T`, and errors |
| XYZ | ASCII positions, optionally followed by RGB; readable by the editor |
| PLY | ASCII or binary positions with optional colors and normals |
| Decoded dump | SMCP intermediate data used by **Reconstruct from dump...** |

## Common problems

### Capture says camera support is unavailable

The executable was built without Spinnaker. Install the SDK, rerun `bootstrap.ps1 -Force`,
and rebuild with `SMCP_WITH_SPINNAKER=ON`.

### A capture set does not appear

Confirm that it is an immediate subdirectory of the selected workspace and contains the
expected image sequence. Reselect the directory to force a scan.

### Corners are not found

Verify that the configured values are the number of **interior** corners, not squares. The
whole board must be sharp and visible, with sufficient contrast and no projected pattern
active in the reference image used for corner detection.

### Calibration errors are high

Remove visibly bad sets, check the real square dimensions, and capture more varied poses.
Several nearly identical front-facing poses provide less geometric information than poses
distributed across the field of view and depth range.

### Point-cloud editor shows no files

It scans only `.xyz` files in the workspace root, not PLY files or nested directories, and
only when the editor opens. Move or export the desired XYZ file there, then close and reopen
the editor.

### The AI Model selector finds no models

The selector stays disabled; hover over it to read the reason. Confirm that Ubuntu 22.04 is
installed in WSL, that `bash scripts-wsl/verify.sh --strict` succeeds inside it, and that
`smcp --list-models --json` lists the models. Then close and reopen the editor to search
again. See [WSL models quick start](WSL_MODELS_QUICKSTART.md) for setup and troubleshooting.

### An AI model fails

The status bar shows the model's error message. Select the model again to adjust its
parameters; for example, lower PointCleanNet's `min_points_cell` or increase
its `cell_size` for small clouds.

For implementation details and extension points, continue with
[ARCHITECTURE.md](ARCHITECTURE.md).
