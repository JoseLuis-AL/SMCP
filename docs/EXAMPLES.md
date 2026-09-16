# Example datasets

The repository's `examples` branch contains ready-to-use validation data for SMCP. It is
an independent data-only branch: it does not contain the application source code or share
the `main` branch history.

Use an SMCP executable built from `main` or from a Release package. Clone the datasets
separately:

```powershell
git clone --branch examples --single-branch https://github.com/JoseLuis-AL/SMCP.git SMCP-examples
```

The download is approximately 485 MiB because it includes the original PNG capture
sequences.

## Contents

```text
SMCP-examples/
  2025-jun.-27_09.22.39.764/
    cam_01.png
    ...
    cam_42.png
    projector_info.txt
  ... four additional capture sets ...
  calibration.yml
  calibration.m
  example_chessboard.xyz
  example_piece.xyz
  example_sphere_bar.xyz
```

The five timestamped directories are complete Gray-code capture sets. The YAML and MATLAB
files contain the calibration produced from those sets. The three XYZ files are sample
clouds for the Point Cloud Editor.

## Use the branch as an SMCP workspace

1. Start SMCP from the `main` build or a Release package.
2. Click **Directory** in the Workspace section.
3. Select the root `SMCP-examples` directory, not one of its timestamped subdirectories.
4. Confirm that the five capture sets appear in the Workspace tree.
5. Configure the checkerboard with 22 by 15 interior corners and a square size of 15 by 15.
6. Select the desired capture sets and run **Decode**.
7. Run **Extract Corners** and inspect the detected points in **Image View**.
8. Run **Calibrate**, then inspect the matrices and errors with
   **Calibration > Display...**.
9. Select a decoded capture set and run **Reconstruct**.

To test reconstruction without recalibrating, load `calibration.yml` with
**Calibration > Load...** before selecting a decoded set and running **Reconstruct**.

## Validate the Point Cloud Editor

Because the XYZ files are in the workspace root, the editor lists them automatically:

- `example_chessboard.xyz`: loading and statistical outlier removal.
- `example_piece.xyz`: outlier removal followed by plane fitting; three planes are a
  useful validation target.
- `example_sphere_bar.xyz`: sphere fitting with the default parameters.

These clouds can also validate row visibility, deletion, editable names, draw order, and
multi-cloud export. Click **Refresh** in the editor if files were added after the dialog
was opened.

## Keeping source and data separate

The `main` branch intentionally excludes these large datasets. Keep the source checkout
and the `examples` checkout in separate directories, and point SMCP at the examples
checkout whenever a repeatable calibration or point-cloud test is needed.
