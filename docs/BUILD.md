# Compilar SMCP

Guía completa para compilar el proyecto en Windows con CMake. Hay un único sistema de
build (CMake + presets); no existen archivos `.pro`, `.vcxproj` ni `.sln` versionados.

## 1. Requisitos

| Componente | Versión | Dónde obtenerlo | Notas |
|---|---|---|---|
| Visual Studio 2026 Community | 18.x, toolset v145 | visualstudio.microsoft.com | Carga de trabajo **Desarrollo de escritorio con C++** con los componentes *Herramientas de CMake de C++ para Windows* (incluye CMake ≥ 3.28 y Ninja) y *Windows 11 SDK*. |
| Qt | 5.14.2, kit **msvc2017_64** | Qt Online Installer (archivo → 5.14.2) | Módulos Core, Gui, Widgets, OpenGL, Concurrent. El kit msvc2017_64 es ABI‑compatible con los toolsets v141…v145. |
| OpenCV | 2.4.13 (paquete Windows) | github.com/opencv/opencv/releases (opencv-2.4.13.6-vc14.exe) | Se usa el binario `build/x64/vc14`. |
| Spinnaker SDK | 3.x / 4.x (Teledyne FLIR) | flir.com/products/spinnaker-sdk | Opcional. Sin él, `SMCP_WITH_SPINNAKER=OFF`. |

Rutas de referencia de una instalación típica (las que usa `CMakeUserPresets.example.json`):

```
D:\Qt\Qt5.14.2\5.14.2\msvc2017_64
D:\OpenCV\opencv\build
D:\Program Files\Teledyne\Spinnaker
```

## 2. Configurar las rutas (bootstrap)

Las rutas de máquina **no** están en `CMakePresets.json`. Viven en `CMakeUserPresets.json`,
que está en `.gitignore`. Hay dos formas de crearlo:

**Automática** (busca Qt, OpenCV y Spinnaker en `C:` y `D:` y escribe el archivo):

```powershell
.\scripts\bootstrap.ps1
```

Parámetros útiles: `-Qt`, `-OpenCV`, `-Spinnaker` para indicar rutas a mano y `-Force`
para regenerar un archivo existente. El script informa de lo que no encuentra.

**Manual**: copia la plantilla y edita las tres rutas del preset oculto `local-paths`.

```powershell
Copy-Item CMakeUserPresets.example.json CMakeUserPresets.json
```

```json
"cacheVariables": {
  "Qt5_DIR": "D:/Qt/Qt5.14.2/5.14.2/msvc2017_64/lib/cmake/Qt5",
  "OpenCV_DIR": "D:/OpenCV/opencv/build",
  "SPINNAKER_DIR": "D:/Program Files/Teledyne/Spinnaker",
  "SMCP_WITH_SPINNAKER": "ON"
}
```

`Qt5_DIR` admite tanto la raíz del kit (`…/msvc2017_64`) como `…/lib/cmake/Qt5`.

### Presets disponibles

| Preset de configuración | Generador | Salida |
|---|---|---|
| `ninja-debug` | Ninja + cl x64, Debug | `build/ninja-debug/bin/SMCP_d.exe` (con consola) |
| `ninja-release` | Ninja + cl x64, Release | `build/ninja-release/bin/SMCP.exe` (sin consola) |
| `vs2026` | Visual Studio 18 2026, x64 (multi‑config) | `build/vs2026/SMCP.slnx` (solución generada, no versionada), binarios en `build/vs2026/bin` |

Presets de build: `ninja-debug`, `ninja-release`, `vs2026-debug`, `vs2026-release`.

Los presets visibles se definen en `CMakeUserPresets.json` heredando de las bases ocultas
(`ninja-debug-base`, `ninja-release-base`, `vs2026-base`) de `CMakePresets.json`. Así el
archivo versionado no contiene rutas y el tuyo solo aporta las tres variables.

Tras compilar, el directorio `bin` contiene todo lo necesario para ejecutar sin modificar
`PATH`: `windeployqt` copia Qt y sus plugins, y CMake copia las DLL de OpenCV y de
Spinnaker (Spinnaker, GenICam y OpenMP).

## 3. Compilar

### Desde Visual Studio 2026 (Abrir carpeta)

1. **Archivo → Abrir → Carpeta…** y elige la raíz del repositorio.
2. En la barra de herramientas aparece el desplegable de configuraciones con
   `Ninja Debug (x64)`, `Ninja Release (x64)` y `Visual Studio 2026`. Elige uno.
3. **Compilar → Compilar todo** (Ctrl+Mayús+B).
4. **F5** ejecuta `SMCP_d.exe` (o `SMCP.exe` en Release). No hace falta `launch.vs.json`:
   el único ejecutable del proyecto se selecciona como elemento de inicio.

Si VS no muestra los presets, comprueba que existe `CMakeUserPresets.json` y usa
**Proyecto → Eliminar caché y volver a configurar**.

### Desde Zed

El repositorio incluye `.zed/settings.json` (clangd sobre `build/ninja-debug`), `.zed/tasks.json`
y `.clangd`.

1. Ejecuta una vez `.\scripts\bootstrap.ps1` y la tarea **SMCP: configure (ninja-debug)**
   (o `.\scripts\build.ps1`). Esto genera `build/ninja-debug/compile_commands.json`.
2. Abre la carpeta en Zed: `zed .`. clangd ofrece autocompletado y diagnósticos.
3. Tareas (`task: spawn`): *build debug*, *build release*, *run (debug)*.

CMake completa `compile_commands.json` con `/vctoolsdir` y `/winsdkdir` (ver
`cmake/PatchCompileCommands.cmake`) para que clangd, en modo clang‑cl, encuentre la STL de
MSVC y el Windows SDK aunque Zed no se abra desde un *Developer Prompt*.

### Desde la terminal

Con cualquier PowerShell (el script importa el entorno x64 de Visual Studio si hace falta):

```powershell
.\scripts\build.ps1 -Config Debug
```

```powershell
.\scripts\build.ps1 -Config Release -Run
```

```powershell
.\scripts\build.ps1 -Generator vs2026 -Config Release
```

`-Clean` borra `build/<preset>` antes de configurar.

Con CMake directamente, desde un **Developer PowerShell for VS 2026** (para que `cl`,
`cmake` y `ninja` estén en `PATH`):

```powershell
cmake --preset ninja-debug
```

```powershell
cmake --build --preset ninja-debug
```

## 4. Opciones de CMake

| Variable | Por defecto | Efecto |
|---|---|---|
| `SMCP_WITH_SPINNAKER` | `ON` | Busca Spinnaker, define `USE_SPINNAKER` y enlaza `Spinnaker::Spinnaker`. |
| `SMCP_DEPLOY_RUNTIME` | `ON` | Ejecuta `windeployqt` y copia las DLL de OpenCV/Spinnaker en el post‑build. |
| `Qt5_DIR`, `OpenCV_DIR`, `SPINNAKER_DIR` | — | Rutas de las dependencias (normalmente vía `CMakeUserPresets.json`). |

## 5. Solución de problemas

**"Could not find a package configuration file provided by Qt5"**
`Qt5_DIR` no apunta a un kit válido. Debe existir
`<kit>/lib/cmake/Qt5/Qt5Config.cmake`. Instala el kit **msvc2017_64** de Qt 5.14.2.

**"Found OpenCV … but the following modules are missing" o no encuentra `OpenCVConfig.cmake`**
`OpenCV_DIR` debe ser la carpeta `build` del paquete Windows, que contiene
`OpenCVConfig.cmake` y `x64/vc14/lib`. El `CMakeLists.txt` fija `OpenCV_ARCH=x64` y
`OpenCV_RUNTIME=vc14` porque el script de OpenCV 2.4 no reconoce toolsets modernos.

**"No se encontro el SDK Spinnaker"**
Define `SPINNAKER_DIR` (raíz con `include`, `lib64`, `bin64`) o configura con
`-DSMCP_WITH_SPINNAKER=OFF`. El módulo `cmake/FindSpinnaker.cmake` acepta las bibliotecas
`Spinnaker_v141` (lib64/vs2017) y `Spinnaker_v140` (lib64/vs2015). En instalaciones donde
`lib64/vs2017` solo trae los módulos GPU se usa automáticamente `vs2015`.

**El ejecutable no arranca: falta `Qt5Core.dll`, `opencv_core2413.dll`, `Spinnaker_v140.dll`…**
El post‑build no se ejecutó (`SMCP_DEPLOY_RUNTIME=OFF`) o se copió el `.exe` fuera de `bin`.
Vuelve a compilar; todas las DLL se dejan junto al ejecutable. En Debug, Spinnaker requiere
además el runtime de depuración de VC (`MSVCP140D.dll`, `VCOMP140D.dll`), presente en cualquier
máquina con Visual Studio.

**"cl.exe no está en PATH" al usar `cmake --preset` a mano**
Abre un *Developer PowerShell for VS 2026* o usa `scripts\build.ps1`, que importa el entorno.

**clangd en Zed: "'type_traits' file not found" o sin autocompletado**
Compila al menos una vez el preset `ninja-debug`; `compile_commands.json` se genera y se
completa con las rutas del toolset en ese build.

**clangd se reinicia al pasar el ratón por un `++` de un iterador**
Es un fallo conocido de clangd (22.x y 23.x) en modo clang‑cl con los iteradores de
`std::vector<cv::Point2f>` de `scan3d.cpp` y `Application.cpp` (`clangd --check` lo
reproduce). Zed relanza el servidor solo; el diagnóstico y el autocompletado del resto del
archivo no se ven afectados.

**Visual Studio compila con la configuración equivocada**
Comprueba el desplegable de configuraciones; cada preset tiene su propio `build/<preset>`.
