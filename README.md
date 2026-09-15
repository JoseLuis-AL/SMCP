# SMCP — Escáner 3D por luz estructurada

Aplicación de escritorio (C++17, Qt 5, OpenCV) para calibrar un sistema proyector‑cámara y
reconstruir nubes de puntos a partir de patrones Gray code proyectados sobre el objeto.

Implementa el método de calibración descrito en *Simple, Accurate, and Robust
Projector‑Camera Calibration* (Daniel Moreno y Gabriel Taubin, 3DimPVT 2012,
[doi:10.1109/3DIMPVT.2012.77](https://doi.org/10.1109/3DIMPVT.2012.77)) y lo extiende con:

- Proyección y captura automática de patrones Gray code.
- Captura desde cámaras FLIR/Teledyne mediante el SDK Spinnaker.
- Triangulación a nubes de puntos orientadas con color y exportación a PLY/XYZ.

## Requisitos

| Componente | Versión probada | Notas |
|---|---|---|
| Windows | 10 / 11 x64 | |
| Visual Studio | 2026 (toolset v145) | Carga de trabajo *Desarrollo de escritorio con C++* con CMake y Ninja. |
| Qt | 5.14.2 `msvc2015_64` | Módulos `Core`, `Gui`, `Widgets`, `OpenGL`. |
| OpenCV | 2.4.13 (paquete Windows, `x64/vc14`) | |
| Spinnaker SDK | 3.x (`lib64/vs2015`) | Opcional: `-DSMCP_WITH_SPINNAKER=OFF` compila sin cámara. |

Las instrucciones completas de instalación, configuración de rutas y compilación desde
Visual Studio, Zed o línea de comandos están en [docs/BUILD.md](docs/BUILD.md).

## Compilación rápida

```powershell
# 1. Copia la plantilla y ajusta las rutas de Qt, OpenCV y Spinnaker de tu máquina.
Copy-Item CMakeUserPresets.example.json CMakeUserPresets.json

# 2. Configura y compila (Debug).
cmake --preset ninja-debug
cmake --build --preset ninja-debug

# 3. Ejecuta.
.\build\ninja-debug\bin\SMCP_d.exe
```

En Visual Studio 2026 basta con **Archivo → Abrir → Carpeta** sobre la raíz del repositorio.

## Estructura

```
src/app       Punto de entrada, Application y MainWindow
src/ui        Diálogos y widgets Qt
src/core      Calibración, decodificación Gray code y reconstrucción
src/camera    Captura con Spinnaker
src/export    Exportación de nubes de puntos
src/common    Settings, utilidades compartidas, tema
resources/    Iconos, hoja de estilo (QSS) y recursos Qt
docs/         Guías de build, arquitectura y estilo visual
```

Ver [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) para el flujo completo.

## Uso

1. Desactiva todos los ajustes automáticos de la cámara (enfoque, exposición, ganancia,
   balance de blancos). El método requiere parámetros fijos.
2. Captura varios juegos de patrones con el tablero de ajedrez en distintas posiciones.
3. Decodifica, extrae esquinas y calibra.
4. Reconstruye y exporta la nube de puntos.

## Licencia

Software derivado del trabajo de Daniel Moreno y Gabriel Taubin (Brown University),
distribuido bajo licencia BSD de 3 cláusulas. Ver [LICENSE](LICENSE).
