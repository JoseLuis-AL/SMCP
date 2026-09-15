# SMCP — Escáner 3D por luz estructurada

Aplicación de escritorio (C++17, Qt 5, OpenCV) para calibrar un sistema proyector‑cámara y
reconstruir nubes de puntos a partir de patrones Gray code proyectados sobre el objeto.

Implementa el método de calibración descrito en *Simple, Accurate, and Robust
Projector‑Camera Calibration* (Daniel Moreno y Gabriel Taubin, 3DimPVT 2012,
[doi:10.1109/3DIMPVT.2012.77](https://doi.org/10.1109/3DIMPVT.2012.77)) y lo extiende con:

- Proyección y captura automática de patrones Gray code.
- Captura desde cámaras FLIR/Teledyne mediante el SDK Spinnaker.
- Triangulación a nubes de puntos orientadas con color y exportación a PLY/XYZ.
- Visor 3D integrado (OpenGL) de la nube reconstruida.
- Editor de nubes de puntos (`.xyz`): eliminación de outliers, ajuste RANSAC de planos y
  esferas, comparación con la nube original y guardado, sin dependencias externas (solo OpenCV).

## Requisitos

| Componente | Versión probada | Notas |
|---|---|---|
| Windows | 10 / 11 x64 | |
| Visual Studio | 2026 (18.x, toolset v145) | Carga de trabajo *Desarrollo de escritorio con C++* con CMake ≥ 3.28 y Ninja. |
| Qt | 5.14.2, kit `msvc2017_64` | Módulos `Core`, `Gui`, `Widgets`, `OpenGL`, `Concurrent`. |
| OpenCV | 2.4.13 (paquete Windows, `build/x64/vc14`) | Módulos `core`, `imgproc`, `highgui`, `calib3d`, `features2d`, `flann`. |
| Spinnaker SDK | 3.x / 4.x (`lib64/vs2017` o `lib64/vs2015`) | Opcional: `-DSMCP_WITH_SPINNAKER=OFF` compila sin cámara (el botón *Capture* lo indica). |

Las instrucciones completas de instalación, configuración de rutas y compilación desde
Visual Studio, Zed o línea de comandos están en [docs/BUILD.md](docs/BUILD.md).

## Compilación rápida

```powershell
# 1. Detecta Qt, OpenCV y Spinnaker y genera CMakeUserPresets.json (o copia
#    CMakeUserPresets.example.json y ajusta las tres rutas a mano).
.\scripts\bootstrap.ps1

# 2. Configura y compila (importa el entorno de Visual Studio si hace falta).
.\scripts\build.ps1 -Config Debug

# 3. Ejecuta.
.\build\ninja-debug\bin\SMCP_d.exe
```

En Visual Studio 2026 basta con **Archivo → Abrir → Carpeta** sobre la raíz del repositorio
y elegir el preset `Ninja Debug (x64)` o `Ninja Release (x64)`; **F5** lanza el ejecutable.
En Zed, `zed .` tras el primer build: clangd usa `build/ninja-debug/compile_commands.json`.

Presets de configuración: `ninja-debug`, `ninja-release` y `vs2026` (genera `SMCP.slnx`).
El directorio de salida `build/<preset>/bin` contiene el ejecutable y todas las DLL
necesarias (Qt, OpenCV, Spinnaker), de modo que arranca sin modificar `PATH`.

## Estructura

```
CMakeLists.txt                 Único sistema de build (CMake ≥ 3.28, MSVC x64)
CMakePresets.json              Bases de los presets (sin rutas de máquina)
CMakeUserPresets.example.json  Plantilla con las rutas de Qt, OpenCV y Spinnaker
cmake/                         FindSpinnaker.cmake, PatchCompileCommands.cmake
scripts/                       bootstrap.ps1 (detecta dependencias), build.ps1
src/app                        main.cpp, Application, MainWindow (+ MainWindow.ui)
src/ui                         Diálogos y widgets Qt (los .ui viven junto a su clase)
src/core                       CalibrationData, structured_light, scan3d, io_util, pointcloud_ops
src/camera                     CameraWorker, CameraUtilities, CameraSettings (Spinnaker)
src/export                     IOExport (PLY / XYZ)
src/common                     Settings.h, Literals.h, cvMatConvert
resources/                     icons/, theme/smcp.qss y resources.qrc
docs/                          BUILD.md, ARCHITECTURE.md, STYLE.md, samples/, screenshots/
```

Convenciones: todo el código vive en el namespace `smcp`, cabeceras `.h` con
`#pragma once`, includes cualificados por módulo (`"core/scan3d.h"`), tabuladores y llaves
Allman (`.editorconfig`, `.clang-format`). Ver [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
para el flujo completo y [docs/STYLE.md](docs/STYLE.md) para el tema visual.

## Configuración en tiempo de ejecución

Los ajustes (directorio de trabajo, tablero, umbrales, cámara, proyector…) se guardan con
`QSettings` en el ámbito de usuario y formato nativo: en Windows,
`HKEY_CURRENT_USER\Software\CENAM\SMCP`. Todas las claves y valores por defecto están en
[`src/common/Settings.h`](src/common/Settings.h). El repositorio no versiona datos de
ejecución; [`docs/samples/sample_calibration.yml`](docs/samples/sample_calibration.yml) es
un ejemplo del archivo de calibración que produce la aplicación.

## Uso

1. Desactiva todos los ajustes automáticos de la cámara (enfoque, exposición, ganancia,
   balance de blancos). El método requiere parámetros fijos.
2. Captura varios juegos de patrones con el tablero de ajedrez en distintas posiciones
   (*Capture*).
3. Decodifica (*Decode*), extrae esquinas (*Extract Corners*) y calibra (*Calibrate*).
4. Reconstruye (*Reconstruct*): la nube se guarda en XYZ o PLY y se muestra en *3D View*.
5. Abre el editor (*Point Cloud*) para limpiar outliers, ajustar planos o esferas y guardar
   el resultado.

## Licencia

Software derivado del trabajo de Daniel Moreno y Gabriel Taubin (Brown University),
distribuido bajo licencia BSD de 3 cláusulas. Ver [LICENSE](LICENSE).
