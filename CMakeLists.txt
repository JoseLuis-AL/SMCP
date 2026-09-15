cmake_minimum_required(VERSION 3.28)

project(SMCP
    VERSION 2.0.0
    DESCRIPTION "Escaner 3D por luz estructurada (calibracion proyector-camara)"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Opciones
# ---------------------------------------------------------------------------
option(SMCP_WITH_SPINNAKER "Compilar la captura con camaras FLIR/Teledyne (SDK Spinnaker)" ON)
option(SMCP_DEPLOY_RUNTIME "Copiar las DLL de Qt, OpenCV y Spinnaker junto al ejecutable tras compilar" ON)

# ---------------------------------------------------------------------------
# Configuracion global
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Tipo de build" FORCE)
endif()

# Todo (Debug y Release, Ninja o Visual Studio) sale a build/<preset>/bin.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
foreach(_cfg Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${_cfg}" _CFG)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_CFG} "${CMAKE_BINARY_DIR}/bin")
endforeach()

if(NOT MSVC)
    message(FATAL_ERROR "SMCP solo se compila con MSVC (Visual Studio 2022/2026, toolset x64).")
endif()

# ---------------------------------------------------------------------------
# Dependencias
# ---------------------------------------------------------------------------

# Qt 5: se acepta Qt5_DIR apuntando al kit (.../msvc2017_64) o a lib/cmake/Qt5.
if(DEFINED Qt5_DIR AND NOT EXISTS "${Qt5_DIR}/Qt5Config.cmake"
        AND EXISTS "${Qt5_DIR}/lib/cmake/Qt5/Qt5Config.cmake")
    set(Qt5_DIR "${Qt5_DIR}/lib/cmake/Qt5" CACHE PATH "Directorio lib/cmake/Qt5 del kit de Qt" FORCE)
endif()
find_package(Qt5 5.14 REQUIRED COMPONENTS Core Gui Widgets OpenGL Concurrent)
get_target_property(_qmake_location Qt5::qmake IMPORTED_LOCATION)
get_filename_component(SMCP_QT_BIN_DIR "${_qmake_location}" DIRECTORY)
message(STATUS "Qt ${Qt5_VERSION}: ${SMCP_QT_BIN_DIR}")

# OpenCV 2.4: su OpenCVConfig.cmake no reconoce toolsets posteriores a vc15,
# asi que se fija el binario x64/vc14 (compatible con v141..v145).
if(NOT DEFINED OpenCV_ARCH)
    set(OpenCV_ARCH x64)
endif()
if(NOT DEFINED OpenCV_RUNTIME)
    set(OpenCV_RUNTIME vc14)
endif()
find_package(OpenCV 2.4 REQUIRED COMPONENTS core imgproc highgui calib3d features2d flann)
message(STATUS "OpenCV ${OpenCV_VERSION}: ${OpenCV_LIB_PATH}")

# Spinnaker (opcional).
if(SMCP_WITH_SPINNAKER)
    find_package(Spinnaker REQUIRED)
    message(STATUS "Spinnaker: ${Spinnaker_LIBRARY_RELEASE}")
endif()

find_package(OpenGL REQUIRED)

# ---------------------------------------------------------------------------
# Fuentes (listas explicitas por subdirectorio, sin GLOB)
# ---------------------------------------------------------------------------
set(SMCP_SOURCES_APP
    src/app/main.cpp
    src/app/Application.cpp
    src/app/Application.h
    src/app/MainWindow.cpp
    src/app/MainWindow.h
    src/app/MainWindow.ui
)

set(SMCP_SOURCES_UI
    src/ui/AboutDialog.cpp
    src/ui/AboutDialog.h
    src/ui/AboutDialog.ui
    src/ui/BusyCursorGuard.h
    src/ui/CalibrationDialog.cpp
    src/ui/CalibrationDialog.h
    src/ui/CalibrationDialog.ui
    src/ui/ImageLabel.cpp
    src/ui/ImageLabel.h
    src/ui/PixmapWidget.cpp
    src/ui/PixmapWidget.h
    src/ui/PointcloudEditorDialog.cpp
    src/ui/PointcloudEditorDialog.h
    src/ui/PointcloudEditorDialog.ui
    src/ui/PointcloudPreviewWidget.cpp
    src/ui/PointcloudPreviewWidget.h
    src/ui/PointcloudWidget.cpp
    src/ui/PointcloudWidget.h
    src/ui/ProcessingDialog.cpp
    src/ui/ProcessingDialog.h
    src/ui/ProcessingDialog.ui
    src/ui/ProjectorWidget.cpp
    src/ui/ProjectorWidget.h
    src/ui/TreeModel.cpp
    src/ui/TreeModel.h
)

# Dialogo de captura: requiere Spinnaker.
set(SMCP_SOURCES_UI_CAPTURE
    src/ui/CaptureDialog.cpp
    src/ui/CaptureDialog.h
    src/ui/CaptureDialog.ui
)

set(SMCP_SOURCES_CORE
    src/core/CalibrationData.cpp
    src/core/CalibrationData.h
    src/core/IoUtil.cpp
    src/core/IoUtil.h
    src/core/PointcloudOps.cpp
    src/core/PointcloudOps.h
    src/core/Scan3d.cpp
    src/core/Scan3d.h
    src/core/StructuredLight.cpp
    src/core/StructuredLight.h
)

set(SMCP_SOURCES_CAMERA
    src/camera/CameraSettings.h
    src/camera/CameraUtilities.cpp
    src/camera/CameraUtilities.h
    src/camera/CameraWorker.cpp
    src/camera/CameraWorker.h
)

set(SMCP_SOURCES_EXPORT
    src/export/IOExport.cpp
    src/export/IOExport.h
)

set(SMCP_SOURCES_COMMON
    src/common/Literals.h
    src/common/Settings.h
    src/common/CvMatConvert.cpp
    src/common/CvMatConvert.h
)

set(SMCP_RESOURCES
    resources/resources.qrc
)

# ---------------------------------------------------------------------------
# Ejecutable
# ---------------------------------------------------------------------------
add_executable(SMCP
    ${SMCP_SOURCES_APP}
    ${SMCP_SOURCES_UI}
    ${SMCP_SOURCES_CORE}
    ${SMCP_SOURCES_EXPORT}
    ${SMCP_SOURCES_COMMON}
    ${SMCP_RESOURCES}
)

if(SMCP_WITH_SPINNAKER)
    target_sources(SMCP PRIVATE ${SMCP_SOURCES_UI_CAPTURE} ${SMCP_SOURCES_CAMERA})
endif()

# Vista de carpetas en Visual Studio / IDEs.
source_group(TREE "${CMAKE_SOURCE_DIR}" FILES
    ${SMCP_SOURCES_APP} ${SMCP_SOURCES_UI} ${SMCP_SOURCES_UI_CAPTURE} ${SMCP_SOURCES_CORE}
    ${SMCP_SOURCES_CAMERA} ${SMCP_SOURCES_EXPORT} ${SMCP_SOURCES_COMMON} ${SMCP_RESOURCES})

set_target_properties(SMCP PROPERTIES
    OUTPUT_NAME SMCP
    DEBUG_POSTFIX _d
    # Release sin consola (WinMain via Qt5::WinMain); Debug con consola.
    WIN32_EXECUTABLE $<NOT:$<CONFIG:Debug>>
)

# Un unico directorio de inclusion: los includes llevan el subdirectorio
# ("app/Application.h", "core/Scan3d.h", ...). Los .ui viven junto a su clase.
target_include_directories(SMCP PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_compile_definitions(SMCP PRIVATE
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    _SCL_SECURE_NO_WARNINGS
    _USE_MATH_DEFINES
    _ENABLE_EXTENDED_ALIGNED_STORAGE
    UNICODE
    _UNICODE
    # opencv2/flann (2.4) usa std::random_shuffle, retirado en C++17; la STL de MSVC lo
    # conserva si se activa esta macro.
    _HAS_AUTO_PTR_ETC=1
    $<$<BOOL:${SMCP_WITH_SPINNAKER}>:USE_SPINNAKER>
)

target_compile_options(SMCP PRIVATE
    /W3
    /MP
    /utf-8
    /external:W0   # sin avisos de cabeceras de Qt, OpenCV y Spinnaker
)

target_link_libraries(SMCP PRIVATE
    Qt5::Core
    Qt5::Gui
    Qt5::Widgets
    Qt5::OpenGL
    Qt5::Concurrent
    opencv_core
    opencv_imgproc
    opencv_highgui
    opencv_calib3d
    opencv_features2d
    opencv_flann
    OpenGL::GL
    $<$<BOOL:${SMCP_WITH_SPINNAKER}>:Spinnaker::Spinnaker>
)

# ---------------------------------------------------------------------------
# compile_commands.json para clangd (Zed): anade /vctoolsdir y el Windows SDK
# a cada comando para que clang-cl encuentre la STL fuera del entorno de VS.
# ---------------------------------------------------------------------------
if(CMAKE_EXPORT_COMPILE_COMMANDS AND CMAKE_GENERATOR MATCHES "Ninja|Makefiles")
    # .../VC/Tools/MSVC/<ver>/bin/Hostx64/x64/cl.exe -> .../VC/Tools/MSVC/<ver>
    get_filename_component(_vctoolsdir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    foreach(_i RANGE 2)
        get_filename_component(_vctoolsdir "${_vctoolsdir}" DIRECTORY)
    endforeach()
    set(_winsdkdir "$ENV{WindowsSdkDir}")
    set(_winsdkver "$ENV{WindowsSDKVersion}")
    string(REGEX REPLACE "[\\/]+$" "" _winsdkdir "${_winsdkdir}")
    string(REGEX REPLACE "[\\/]+$" "" _winsdkver "${_winsdkver}")
    file(TO_CMAKE_PATH "${_winsdkdir}" _winsdkdir)

    add_custom_target(clangd_compile_commands ALL
        COMMAND "${CMAKE_COMMAND}"
                -DDB=${CMAKE_BINARY_DIR}/compile_commands.json
                -DVCTOOLSDIR=${_vctoolsdir}
                -DWINSDKDIR=${_winsdkdir}
                -DWINSDKVER=${_winsdkver}
                -P "${CMAKE_SOURCE_DIR}/cmake/PatchCompileCommands.cmake"
        COMMENT "Completando compile_commands.json para clangd"
        VERBATIM)
endif()

# ---------------------------------------------------------------------------
# Post-build: ejecutable autocontenido en build/<preset>/bin
# ---------------------------------------------------------------------------
if(SMCP_DEPLOY_RUNTIME)
    find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${SMCP_QT_BIN_DIR}" REQUIRED)

    add_custom_command(TARGET SMCP POST_BUILD
        COMMAND "${WINDEPLOYQT_EXECUTABLE}"
                --no-compiler-runtime --no-translations --no-opengl-sw --no-system-d3d-compiler
                --no-angle --no-quick-import
                "$<TARGET_FILE:SMCP>"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:opencv_core>"
                "$<TARGET_FILE:opencv_imgproc>"
                "$<TARGET_FILE:opencv_highgui>"
                "$<TARGET_FILE:opencv_calib3d>"
                "$<TARGET_FILE:opencv_features2d>"
                "$<TARGET_FILE:opencv_flann>"
                "$<TARGET_FILE_DIR:SMCP>"
        COMMENT "Desplegando Qt y OpenCV junto a $<TARGET_FILE_NAME:SMCP>"
        VERBATIM)

    if(SMCP_WITH_SPINNAKER)
        add_custom_command(TARGET SMCP POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "$<IF:$<CONFIG:Debug>,${Spinnaker_RUNTIME_DEBUG},${Spinnaker_RUNTIME_RELEASE}>"
                    "$<TARGET_FILE_DIR:SMCP>"
            COMMENT "Desplegando Spinnaker junto a $<TARGET_FILE_NAME:SMCP>"
            COMMAND_EXPAND_LISTS
            VERBATIM)
    endif()
endif()
