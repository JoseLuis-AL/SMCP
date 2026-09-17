# SMCP documentation

Use these documents in this order:

1. [Build guide](BUILD.md) — install dependencies, configure local paths, compile, run,
   and troubleshoot the Windows build.
2. [WSL models quick start](WSL_MODELS_QUICKSTART.md) — optional; recreate Miniconda, the
   three AI environments, and the shared model launcher. Skip it if you do not use AI models.
3. [User guide](USER_GUIDE.md) — prepare hardware, capture data, calibrate, reconstruct,
   edit point clouds, and diagnose common workflow problems.
4. [Example datasets](EXAMPLES.md) — download the data-only `examples` branch and use it
   to validate calibration, reconstruction, and point-cloud editing.
5. [Architecture](ARCHITECTURE.md) — understand data flow, module ownership, threading,
   persistence, and extension points.
6. [Visual style](STYLE.md) — maintain the Qt theme and add consistent controls and
   dialogs.

Additional material:

- [Sample calibration file](samples/sample_calibration.yml) documents the OpenCV YAML
  fields consumed and produced by SMCP.
- [`screenshots/`](screenshots/) contains visual references for the global theme and point
  cloud editor.

SMCP is a C++17, Qt 5, OpenCV, and OpenGL application for structured-light 3D scanning.
Direct FLIR/Teledyne camera capture additionally uses the optional Spinnaker SDK.
