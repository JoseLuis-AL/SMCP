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
find_package(Qt5 5.14 REQUIRED COMPONENTS Core Gui Widgets OpenGL)
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
# Fuentes (listas explicitas, sin GLOB)
# ---------------------------------------------------------------------------
set(SMCP_SOURCES_V1
    src/main.cpp
    src/AboutDialog.cpp
    src/AboutDialog.hpp
    src/Application.cpp
    src/Application.hpp
    src/CalibrationData.cpp
    src/CalibrationData.hpp
    src/CalibrationDialog.cpp
    src/CalibrationDialog.hpp
    src/CaptureDialog.cpp
    src/CaptureDialog.hpp
    src/Config.h
    src/GLWidget.cpp
    src/GLWidget.hpp
    src/ImageLabel.cpp
    src/ImageLabel.hpp
    src/ImageUtil.cpp
    src/ImageUtil.hpp
    src/MainWindow.cpp
    src/MainWindow.hpp
    src/ProcessingDialog.cpp
    src/ProcessingDialog.hpp
    src/ProjectorWidget.cpp
    src/ProjectorWidget.hpp
    src/SpinnakerCameraConfig.h
    src/SpinnakerCaptureWorker.cpp
    src/SpinnakerCaptureWorker.h
    src/TreeModel.cpp
    src/TreeModel.hpp
    src/VideoInput.cpp
    src/VideoInput.hpp
    src/cognex_util.hpp
    src/im_util.cpp
    src/im_util.hpp
    src/io_util.cpp
    src/io_util.hpp
    src/scan3d.cpp
    src/scan3d.hpp
    src/structured_light.cpp
    src/structured_light.hpp
)

set(SMCP_SOURCES_V2
    src_v2/BusyCursorGuard.h
    src_v2/CameraSettings.h
    src_v2/CameraUtilities.cpp
    src_v2/CameraUtilities.h
    src_v2/CameraWorker.cpp
    src_v2/CameraWorker.h
    src_v2/CaptureQDialog.cpp
    src_v2/CaptureQDialog.h
    src_v2/CaptureQDialog.ui
    src_v2/IOExport.cpp
    src_v2/IOExport.h
    src_v2/Literals.h
    src_v2/PixmapQWidget.cpp
    src_v2/PixmapQWidget.h
    src_v2/PointcloudPreviewQDialog.cpp
    src_v2/PointcloudPreviewQDialog.h
    src_v2/PointcloudPreviewQDialog.ui
    src_v2/PointcloudWidget.cpp
    src_v2/PointcloudWidget.h
    src_v2/Settings.h
    src_v2/cvMatConvert.cpp
    src_v2/cvMatConvert.h
)

set(SMCP_FORMS
    forms/AboutDialog.ui
    forms/CalibrationDialog.ui
    forms/CaptureDialog.ui
    forms/MainWindow.ui
    forms/ProcessingDialog.ui
)

set(SMCP_RESOURCES
    assets/assets.qrc
)

# ---------------------------------------------------------------------------
# Ejecutable
# ---------------------------------------------------------------------------
add_executable(SMCP
    ${SMCP_SOURCES_V1}
    ${SMCP_SOURCES_V2}
    ${SMCP_FORMS}
    ${SMCP_RESOURCES}
)

set_target_properties(SMCP PROPERTIES
    OUTPUT_NAME SMCP
    DEBUG_POSTFIX _d
    # Release sin consola (WinMain via Qt5::WinMain); Debug con consola.
    WIN32_EXECUTABLE $<NOT:$<CONFIG:Debug>>
    AUTOUIC_SEARCH_PATHS "${CMAKE_SOURCE_DIR}/forms;${CMAKE_SOURCE_DIR}/src_v2"
)

target_include_directories(SMCP PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src_v2
)

target_compile_definitions(SMCP PRIVATE
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    _SCL_SECURE_NO_WARNINGS
    _USE_MATH_DEFINES
    _ENABLE_EXTENDED_ALIGNED_STORAGE
    UNICODE
    _UNICODE
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
    opencv_core
    opencv_imgproc
    opencv_highgui
    opencv_calib3d
    opencv_features2d
    opencv_flann
    OpenGL::GL
    $<$<BOOL:${SMCP_WITH_SPINNAKER}>:Spinnaker::Spinnaker>
    # DirectShow / VfW (VideoInput.cpp)
    Strmiids Vfw32 Ole32 OleAut32
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
