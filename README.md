# SMCP example datasets

This branch contains validation data for SMCP. It intentionally contains no application
source code. Build SMCP from the `main` branch or download a Release package, then select
the root of this checkout as the application's workspace directory.

## Contents

- Five timestamped capture sets. Each set contains 42 numbered Gray-code camera images and
  its `projector_info.txt` metadata.
- `calibration.yml` and `calibration.m`, generated from the supplied capture sets.
- `example_chessboard.xyz`, `example_piece.xyz`, and `example_sphere_bar.xyz` for the
  Point Cloud Editor.

## Validate calibration and reconstruction

1. Start SMCP and click **Directory** in the Workspace section.
2. Select the root directory of this branch.
3. Select the timestamped capture sets in the workspace tree.
4. Configure the checkerboard with 22 by 15 interior corners and a square size of 15 by 15.
5. Click **Decode** to generate camera-to-projector correspondences.
6. Click **Extract Corners** and verify the detected checkerboard points in **Image View**.
7. Click **Calibrate** and inspect the result with **Calibration > Display...**.
8. Select a decoded capture set and click **Reconstruct** to generate a point cloud.

The supplied `calibration.yml` can also be loaded through **Calibration > Load...** when
testing reconstruction without recalibrating first.

## Validate the Point Cloud Editor

The three XYZ files are located directly in the workspace root, so they appear in the
editor's source selector:

- Use `example_chessboard.xyz` to test loading and **Remove Outliers**.
- Use `example_piece.xyz` to test **Remove Outliers** followed by **Plane** fitting. Three
  planes provide a useful validation case.
- Use `example_sphere_bar.xyz` to test **Sphere** fitting with the default parameters.

The editor can also be used to verify visibility controls, deletion, editable cloud names,
and multi-cloud XYZ export.

## Clone only this branch

```powershell
git clone --branch examples --single-branch https://github.com/JoseLuis-AL/SMCP.git SMCP-examples
```

The branch is large because it stores the original PNG capture sequences. A single-branch
clone avoids downloading the source-code history when only the validation data is needed.
