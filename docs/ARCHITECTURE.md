# Arquitectura

SMCP es una aplicación Qt de una sola ventana. `smcp::Application` (subclase de
`QApplication`) es el punto central: posee la configuración (`QSettings`), el modelo de
imágenes (`TreeModel`), los datos de calibración (`CalibrationData`) y los resultados
intermedios (patrones decodificados, esquinas, nube de puntos). La interfaz (`MainWindow` y
los diálogos) solo orquesta llamadas sobre `APP` y muestra los resultados; los algoritmos
viven en `src/core` sin dependencia de la interfaz.

```
src/app      Application, MainWindow           orquestación y estado global
src/ui       diálogos y widgets                 interacción con el usuario
src/camera   CameraWorker, CameraUtilities      hilo de adquisición Spinnaker
src/core     structured_light, scan3d,          algoritmos (sin Qt Widgets)
             CalibrationData, io_util
src/export   IOExport                           escritura de nubes de puntos
src/common   Settings, Literals, cvMatConvert   claves de configuración y utilidades
```

Todo el código está en el namespace `smcp`; los módulos de algoritmos usan namespaces
anidados (`smcp::sl`, `smcp::scan3d`, `smcp::io_util`, `smcp::IOExport`).

## Flujo de trabajo

```
 captura ──▶ decodificación ──▶ esquinas ──▶ calibración ──▶ reconstrucción ──▶ exportación
 CaptureDialog   Application     Application    Application     Application       MainWindow
 CameraWorker    sl::*           cv::find…      cv::calibrate…  scan3d::*         IOExport / io_util
 ProjectorWidget                                CalibrationData                   PointcloudWidget
```

### 1. Captura — `src/ui/CaptureDialog`, `src/ui/ProjectorWidget`, `src/camera`

- `MainWindow::on_capture_action_button_clicked` abre `CaptureDialog` (modal). Requiere
  Spinnaker (`USE_SPINNAKER`); sin él, el botón informa de que la compilación no incluye
  cámara.
- `CaptureDialog` enumera pantallas y cámaras, restaura los ajustes de `Settings::Camera`,
  `Settings::Projector` y `Settings::Capture`, y lanza un `CameraWorker` en un `QThread`.
- `CameraWorker` (hilo de adquisición) configura la cámara con `CameraUtilities`, entrega
  fotogramas para la vista previa (`PixmapWidget`, conversión con `cvMatConvert`) y, en
  modo captura, guarda cada imagen en el directorio de sesión cuando recibe
  `needStoreImageSignal`.
- `ProjectorWidget` se muestra a pantalla completa en el monitor del proyector y genera los
  patrones Gray code (`make_pattern`); `save_info` escribe `projector_info.txt`
  (`Literals::ProjectorInfoFilename`) con la resolución y el número de patrones.
- Resultado: una carpeta por conjunto con `NN.png` (patrones) y `projector_info.txt`.
  `Application::set_root_dir` recarga el `TreeModel` con las carpetas del directorio de
  trabajo (`Settings::App::RootDirectory`).

### 2. Decodificación — `Application::decode_all` → `src/core/structured_light`

- Para cada conjunto seleccionado, `Application::decode_gray_set` lee las imágenes y llama a
  `sl::decode_pattern` con `RobustDecode | GrayPatternDecode`; antes estima la luz directa
  con `sl::estimate_direct_light(b)` y usa los parámetros `Settings::Decode` (umbral, `b`,
  `m`).
- Produce por conjunto una imagen de patrón (`cv::Mat2f`: coordenada de proyector por
  píxel de cámara) y una imagen de mínimos/máximos, guardadas en `pattern_list` y
  `min_max_list`. `Application::make_pattern_images` y `sl::colorize_pattern` generan las
  vistas coloreadas que muestra `MainWindow` (*Pattern View*); `get_projector_view` y
  `scan3d::make_projector_view` la vista desde el proyector (*Projector View*).

### 3. Esquinas — `Application::extract_chessboard_corners_v2`

- Detecta las esquinas interiores del tablero (`Settings::Chessboard`: columnas, filas y
  tamaño de cuadro) con `cv::findChessboardCorners` + `cv::cornerSubPix` sobre la imagen
  gris del conjunto y calcula sus coordenadas de mundo (`get_chessboard_world_coords_v2`).
- Rellena `corners_camera` y `corners_world`; el progreso y los mensajes van a
  `ProcessingDialog` mediante los helpers `processing_*` de `Application`.

### 4. Calibración — `Application::calibrate` → `CalibrationData`

- Con la imagen de patrón decodificada, cada esquina de cámara se traslada al proyector
  ajustando una homografía local (ventana `Settings::Calibration::HWin`) y se obtienen las
  `corners_projector`.
- `cv::calibrateCamera` calibra cámara y proyector por separado y `cv::stereoCalibrate`
  obtiene la pose relativa (`R`, `T`). Los intrínsecos, distorsiones y errores quedan en
  `CalibrationData`, que sabe cargarse y guardarse en YAML (`load_calibration_yml` /
  `save_calibration_yml`, ver `docs/samples/sample_calibration.yml`) y exportarse a MATLAB.
- `CalibrationDialog` muestra el resultado y permite cargar/guardar el archivo
  (`Settings::Calibration::File`).

### 5. Reconstrucción — `Application::reconstruct_model` → `src/core/scan3d`

- `scan3d::reconstruct_model` triangula cada píxel de cámara con su correspondencia de
  proyector (`triangulate_stereo` / `approximate_ray_intersection`) usando la
  `CalibrationData`, descartando puntos sin decodificar, con contraste inferior al umbral o
  con distancia entre rayos mayor que `Settings::Reconstruction::MaxDist`.
- El resultado es un `scan3d::Pointcloud` (puntos, colores y, tras
  `scan3d::compute_normals`, normales) que `Application` conserva en `pointcloud`.
- `MainWindow` lo envía a `PointcloudWidget` (*3D View*), un `QOpenGLWidget` con cámara
  orbital que sube los puntos válidos a un VBO.

### 6. Exportación — `MainWindow` → `src/export/IOExport`, `src/core/io_util`

- *Point Cloud* pide un nombre de archivo y escribe la nube según la extensión:
  `IOExport::write_xyz` (XYZ o XYZRGB) o `io_util::write_ply` (PLY ASCII o binario con
  colores y normales opcionales, según `Settings::Reconstruction::Save*`).
- `reconstruct_dump_action` permite reconstruir y exportar a partir de un volcado de patrón
  previamente guardado (`Application::dump_decoded` / `load_dump`).

## Estado y configuración

- `Application::config` (`QSettings`, ámbito de usuario, formato nativo) es la única
  instancia de configuración; las claves y valores por defecto están en
  `src/common/Settings.h` y `Application::load_config` escribe los que faltan al arrancar.
- `MainWindow` guarda cada cambio de los controles del panel derecho en `config`
  inmediatamente (slots `on_*_valueChanged` / `editingFinished`).
- La geometría y el estado de la ventana se restauran al inicio y se guardan en
  `Application::deinit` (señal `aboutToQuit`).

## Hilos

Solo `CameraWorker` corre fuera del hilo de la interfaz. Se comunica con `CaptureDialog`
por señales y slots (conexiones en cola); el resto del procesamiento (decodificación,
calibración, reconstrucción) se ejecuta en el hilo principal y mantiene la interfaz viva
con `QApplication::processEvents` desde los helpers `processing_*`, lo que permite cancelar
desde `ProcessingDialog`.
