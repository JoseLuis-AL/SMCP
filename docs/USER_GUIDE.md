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

Click **Point Cloud** to open the editor. Its source selector lists `.xyz` files located
directly in the current workspace. Click **Refresh** after adding files externally.

Loading and processing run in background workers. The command bar is temporarily disabled
while an operation is active.

### Remove outliers

1. Load a cloud.
2. Click **Remove Outliers**.
3. Set **Neighbors (k)** and **Standard deviation multiplier**.
4. Click **Done** to run or **Cancel** to leave the cloud unchanged.

The operation removes points whose mean neighbor distance is above the global mean plus the
chosen number of standard deviations. A smaller multiplier removes more points; a larger
neighbor count smooths the density estimate but costs more processing time. The result and
original cloud are both shown for comparison.

### Fit planes or spheres

Select **Plane** or **Sphere** from **Fit Figure**. Selecting **None** does nothing.

- Plane parameters control the number of planes, RANSAC iterations, point-to-plane distance,
  minimum inliers, and coefficient refinement.
- Sphere parameters add minimum and maximum radius and use a point-to-surface distance.
- A minimum-inlier value of **Automatic** derives a threshold from the cloud size.
- **Done** starts the fit; **Cancel** makes no change. The selector returns to **None** in
  either case.

Each detected model becomes a separate colored cloud in the overlay. Sphere fitting first
removes dominant planes internally so planar backgrounds are less likely to overwhelm the
sphere search.

### Manage and export editor results

Each overlay row shows a color marker, editable name, point count, visibility button, and
delete button.

- Double-click the name to edit it; press Enter or click elsewhere to finish.
- Click the eye button to hide or show a cloud. Visibility affects only the preview.
- Click the trash button to remove the cloud from the editor.
- Drag rows to change draw order.
- Click **Export**, choose a directory, and SMCP writes one XYZ file for every row that is
  still present. Edited names become filenames. Hidden clouds are included; deleted clouds
  are not.

SMCP replaces characters that Windows does not allow in filenames and appends a number to
duplicates, preventing one exported cloud from overwriting another.

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

It scans only `.xyz` files in the workspace root, not PLY files or nested directories. Move
or export the desired XYZ file there, then click **Refresh**.

For implementation details and extension points, continue with
[ARCHITECTURE.md](ARCHITECTURE.md).
