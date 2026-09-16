# SMCP documentation

Use these documents in this order:

1. [Build guide](BUILD.md) — install dependencies, configure local paths, compile, run,
   and troubleshoot the Windows build.
2. [User guide](USER_GUIDE.md) — prepare hardware, capture data, calibrate, reconstruct,
   edit point clouds, and diagnose common workflow problems.
3. [Architecture](ARCHITECTURE.md) — understand data flow, module ownership, threading,
   persistence, and extension points.
4. [Visual style](STYLE.md) — maintain the Qt theme and add consistent controls and
   dialogs.

Additional material:

- [Sample calibration file](samples/sample_calibration.yml) documents the OpenCV YAML
  fields consumed and produced by SMCP.
- [`screenshots/`](screenshots/) contains visual references for the global theme and point
  cloud editor.

SMCP is a C++17, Qt 5, OpenCV, and OpenGL application for structured-light 3D scanning.
Direct FLIR/Teledyne camera capture additionally uses the optional Spinnaker SDK.
