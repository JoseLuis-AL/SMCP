cmake_minimum_required(VERSION 3.28)

project(SMCP
    VERSION 3.6.8
    DESCRIPTION "Structured-light 3D scanner with projector-camera calibration"
    LANGUAGES CXX RC)

# With a localized cl.exe, Ninja would otherwise record no header dependencies.
include("${CMAKE_SOURCE_DIR}/cmake/MsvcShowIncludesPrefix.cmake")

include(CTest)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(SMCP_WITH_SPINNAKER "Build capture support for FLIR/Teledyne cameras (Spinnaker SDK)" ON)
option(SMCP_DEPLOY_RUNTIME "Copy the Qt, OpenCV, and Spinnaker DLLs next to the executable after building" ON)

# ---------------------------------------------------------------------------
# Global settings
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
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# Every configuration (Debug or Release, Ninja or Visual Studio) outputs to build/<preset>/bin.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
foreach(_cfg Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${_cfg}" _CFG)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_CFG} "${CMAKE_BINARY_DIR}/bin")
endforeach()

if(NOT MSVC)
    message(FATAL_ERROR "SMCP can only be built with MSVC (Visual Studio 2022/2026, x64 toolset).")
endif()

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------

# Qt 5: Qt5_DIR may point to the kit root (.../msvc2017_64) or to lib/cmake/Qt5.
if(DEFINED Qt5_DIR AND NOT EXISTS "${Qt5_DIR}/Qt5Config.cmake"
        AND EXISTS "${Qt5_DIR}/lib/cmake/Qt5/Qt5Config.cmake")
    set(Qt5_DIR "${Qt5_DIR}/lib/cmake/Qt5" CACHE PATH "lib/cmake/Qt5 directory of the Qt kit" FORCE)
endif()
find_package(Qt5 5.14 REQUIRED COMPONENTS Core Gui Widgets OpenGL Concurrent)
get_target_property(_qmake_location Qt5::qmake IMPORTED_LOCATION)
get_filename_component(SMCP_QT_BIN_DIR "${_qmake_location}" DIRECTORY)
message(STATUS "Qt ${Qt5_VERSION}: ${SMCP_QT_BIN_DIR}")

# OpenCV 2.4: its OpenCVConfig.cmake does not recognize toolsets newer than vc15,
# so the x64/vc14 binaries are selected (compatible with v141..v145).
if(NOT DEFINED OpenCV_ARCH)
    set(OpenCV_ARCH x64)
endif()
if(NOT DEFINED OpenCV_RUNTIME)
    set(OpenCV_RUNTIME vc14)
endif()
find_package(OpenCV 2.4 REQUIRED COMPONENTS core imgproc highgui calib3d features2d flann)
message(STATUS "OpenCV ${OpenCV_VERSION}: ${OpenCV_LIB_PATH}")

# Spinnaker (optional).
if(SMCP_WITH_SPINNAKER)
    find_package(Spinnaker REQUIRED)
    message(STATUS "Spinnaker: ${Spinnaker_LIBRARY_RELEASE}")
endif()

find_package(OpenGL REQUIRED)

# ---------------------------------------------------------------------------
# Sources (explicit lists per subdirectory, no GLOB)
# ---------------------------------------------------------------------------
set(SMCP_SOURCES_APP
    src/app/Application.cpp
    src/app/Application.h
    src/app/MainWindow.cpp
    src/app/MainWindow.h
    src/app/MainWindow.ui
    src/app/TaskRunner.h
)

set(SMCP_SOURCES_UI
    src/ui/AboutDialog.cpp
    src/ui/AboutDialog.h
    src/ui/AboutDialog.ui
    src/ui/CalibrationDialog.cpp
    src/ui/CalibrationDialog.h
    src/ui/CalibrationDialog.ui
    src/ui/models/TreeModel.cpp
    src/ui/models/TreeModel.h
    src/ui/pointcloud_editor/AiInferenceProgressDialog.cpp
    src/ui/pointcloud_editor/AiInferenceProgressDialog.h
    src/ui/pointcloud_editor/AiInferenceProgressDialog.ui
    src/ui/pointcloud_editor/AiModelConfigDialog.cpp
    src/ui/pointcloud_editor/AiModelConfigDialog.h
    src/ui/pointcloud_editor/ExportPointcloudDialog.cpp
    src/ui/pointcloud_editor/ExportPointcloudDialog.h
    src/ui/pointcloud_editor/ExportPointcloudDialog.ui
    src/ui/pointcloud_editor/IcpDialog.cpp
    src/ui/pointcloud_editor/IcpDialog.h
    src/ui/pointcloud_editor/IcpDialog.ui
    src/ui/pointcloud_editor/PointcloudEditorAi.cpp
    src/ui/pointcloud_editor/PointcloudEditorCommands.cpp
    src/ui/pointcloud_editor/PointcloudEditorDialog.cpp
    src/ui/pointcloud_editor/PointcloudEditorFiles.cpp
    src/ui/pointcloud_editor/PointcloudEditorDialog.h
    src/ui/pointcloud_editor/PointcloudEditorDialog.ui
    src/ui/pointcloud_editor/PointcloudLayersPanel.cpp
    src/ui/pointcloud_editor/PointcloudLayersPanel.h
    src/ui/pointcloud_editor/PlaneFitDialog.cpp
    src/ui/pointcloud_editor/PlaneFitDialog.h
    src/ui/pointcloud_editor/PlaneFitDialog.ui
    src/ui/pointcloud_editor/RemoveOutliersDialog.cpp
    src/ui/pointcloud_editor/RemoveOutliersDialog.h
    src/ui/pointcloud_editor/RemoveOutliersDialog.ui
    src/ui/pointcloud_editor/SphereFitDialog.cpp
    src/ui/pointcloud_editor/SphereFitDialog.h
    src/ui/pointcloud_editor/SphereFitDialog.ui
    src/ui/pointcloud_editor/SphereMeasurementDialog.cpp
    src/ui/pointcloud_editor/SphereMeasurementDialog.h
    src/ui/pointcloud_editor/SphereMeasurementDialog.ui
    src/ui/pointcloud_editor/SphereResultsDialog.cpp
    src/ui/pointcloud_editor/SphereResultsDialog.h
    src/ui/pointcloud_editor/SphereResultsDialog.ui
    src/ui/pointcloud_editor/PointcloudProject.cpp
    src/ui/pointcloud_editor/PointcloudProject.h
    src/ui/preview/EditorScene.cpp
    src/ui/preview/EditorScene.h
    src/ui/preview/ImageLabel.cpp
    src/ui/preview/ImageLabel.h
    src/ui/preview/PixmapWidget.cpp
    src/ui/preview/PixmapWidget.h
    src/ui/preview/PointcloudPreviewInput.cpp
    src/ui/preview/PointcloudPreviewInternal.h
    src/ui/preview/PointcloudPreviewOverlays.cpp
    src/ui/preview/PointcloudPreviewWidget.cpp
    src/ui/preview/PointcloudPreviewWidget.h
    src/ui/preview/PointcloudWidget.cpp
    src/ui/preview/PointcloudWidget.h
    src/ui/ProcessingDialog.cpp
    src/ui/ProcessingDialog.h
    src/ui/ProcessingDialog.ui
    src/ui/widgets/ProjectorWidget.cpp
    src/ui/widgets/ProjectorWidget.h
    src/ui/widgets/ComboBoxPopupStyle.h
    src/ui/widgets/EditorDialogStyle.h
)

# The capture dialog requires Spinnaker.
set(SMCP_SOURCES_UI_CAPTURE
    src/ui/CaptureDialog.cpp
    src/ui/CaptureDialog.h
    src/ui/CaptureDialog.ui
)

set(SMCP_SOURCES_CORE
    src/core/AsyncTask.h
    src/core/BusyCursorGuard.h
    src/core/Calibration.cpp
    src/core/Calibration.h
    src/core/CalibrationData.cpp
    src/core/CalibrationData.h
    src/core/IoUtil.cpp
    src/core/IoUtil.h
    src/core/Parallel.h
    src/core/Progress.h
    src/core/PointcloudOps.cpp
    src/core/PointcloudOps.h
    src/core/Compensation.cpp
    src/core/Compensation.h
    src/core/SphereScan.cpp
    src/core/SphereScan.h
    src/core/SphereMeasurement.cpp
    src/core/SphereMeasurement.h
    src/core/Scan3d.cpp
    src/core/Scan3d.h
    src/core/StructuredLight.cpp
    src/core/StructuredLight.h
)

set(SMCP_SOURCES_AI
    src/ai/WslModelService.cpp
    src/ai/WslModelService.h
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
    src/export/ExcelExport.cpp
    src/export/ExcelExport.h
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
# Targets: smcp_app holds the whole application except main(); the executable and the
# application smoke test link it, so every source is compiled once.
# ---------------------------------------------------------------------------
add_library(smcp_app STATIC
    ${SMCP_SOURCES_APP}
    ${SMCP_SOURCES_UI}
    ${SMCP_SOURCES_CORE}
    ${SMCP_SOURCES_AI}
    ${SMCP_SOURCES_EXPORT}
    ${SMCP_SOURCES_COMMON}
)

if(SMCP_WITH_SPINNAKER)
    target_sources(smcp_app PRIVATE ${SMCP_SOURCES_UI_CAPTURE} ${SMCP_SOURCES_CAMERA})
endif()

# Folder view in Visual Studio and other IDEs.
source_group(TREE "${CMAKE_SOURCE_DIR}" FILES
    ${SMCP_SOURCES_APP} ${SMCP_SOURCES_UI} ${SMCP_SOURCES_UI_CAPTURE} ${SMCP_SOURCES_CORE} ${SMCP_SOURCES_AI}
    ${SMCP_SOURCES_CAMERA} ${SMCP_SOURCES_EXPORT} ${SMCP_SOURCES_COMMON} ${SMCP_RESOURCES})

# A single include directory: includes are qualified with their subdirectory
# ("app/Application.h", "core/Scan3d.h", ...). Each .ui form lives next to its class; the
# generated ui_*.h headers are reachable by the targets that link the library.
target_include_directories(smcp_app PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}/smcp_app_autogen/include
)

target_compile_definitions(smcp_app PUBLIC
    SMCP_VERSION="${PROJECT_VERSION}"
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    _SCL_SECURE_NO_WARNINGS
    _USE_MATH_DEFINES
    _ENABLE_EXTENDED_ALIGNED_STORAGE
    UNICODE
    _UNICODE
    # opencv2/flann (2.4) uses std::random_shuffle, removed in C++17; the MSVC STL
    # keeps it when this macro is defined.
    _HAS_AUTO_PTR_ETC=1
    $<$<BOOL:${SMCP_WITH_SPINNAKER}>:USE_SPINNAKER>
)

target_compile_options(smcp_app PUBLIC
    /W3
    /MP
    /utf-8
    /external:W0   # no warnings from Qt, OpenCV, and Spinnaker headers
)

target_link_libraries(smcp_app PUBLIC
    Qt5::Core
    Qt5::Gui
    Qt5::GuiPrivate
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

add_executable(SMCP
    src/app/main.cpp
    ${SMCP_RESOURCES}
    resources/app-icon.rc
)

set_target_properties(SMCP PROPERTIES
    OUTPUT_NAME SMCP
    DEBUG_POSTFIX _d
    # Release has no console (WinMain through Qt5::WinMain); Debug keeps the console.
    WIN32_EXECUTABLE $<NOT:$<CONFIG:Debug>>
)

target_link_libraries(SMCP PRIVATE smcp_app)

if(BUILD_TESTING)
    add_executable(SMCP_editor_tests
        tests/PointcloudEditorTests.cpp
        tests/SphereResultsTests.cpp
        tests/PointcloudProjectTests.cpp
        tests/IoExportTests.cpp
        src/core/Scan3d.cpp
        src/core/CalibrationData.cpp
        resources/resources.qrc
        src/ui/pointcloud_editor/IcpDialog.cpp
        src/ui/pointcloud_editor/IcpDialog.h
        src/ui/pointcloud_editor/SphereMeasurementDialog.cpp
        src/ui/pointcloud_editor/SphereMeasurementDialog.h
        src/ui/pointcloud_editor/SphereResultsDialog.cpp
        src/ui/pointcloud_editor/SphereResultsDialog.h
        src/ui/pointcloud_editor/PointcloudProject.cpp
        src/ui/pointcloud_editor/PointcloudProject.h
        src/ui/pointcloud_editor/AiInferenceProgressDialog.cpp
        src/ui/pointcloud_editor/AiInferenceProgressDialog.h
        src/ui/pointcloud_editor/AiModelConfigDialog.cpp
        src/ui/pointcloud_editor/AiModelConfigDialog.h
        src/ai/WslModelService.cpp
        src/ai/WslModelService.h
        src/core/Compensation.cpp
        src/core/SphereScan.cpp
        src/core/SphereMeasurement.cpp
        src/core/AsyncTask.h
        src/export/IOExport.cpp
        src/export/ExcelExport.cpp
        src/core/StructuredLight.cpp
        src/ui/preview/EditorScene.cpp
        src/ui/preview/EditorScene.h
        src/ui/preview/PointcloudPreviewInput.cpp
        src/ui/preview/PointcloudPreviewOverlays.cpp
        src/ui/preview/PointcloudPreviewWidget.cpp
        src/ui/preview/PointcloudPreviewWidget.h
        src/ui/pointcloud_editor/PointcloudLayersPanel.cpp
        src/ui/pointcloud_editor/PointcloudLayersPanel.h
        src/core/PointcloudOps.cpp
    )
    target_include_directories(SMCP_editor_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_definitions(SMCP_editor_tests PRIVATE NOMINMAX _USE_MATH_DEFINES _HAS_AUTO_PTR_ETC=1)
    target_compile_options(SMCP_editor_tests PRIVATE /W3 /utf-8 /external:W0)
    target_link_libraries(SMCP_editor_tests PRIVATE Qt5::Widgets Qt5::GuiPrivate opencv_core opencv_flann opencv_imgproc opencv_highgui opencv_calib3d OpenGL::GL)
    add_test(NAME editor_selection COMMAND SMCP_editor_tests -platform offscreen)
    set_tests_properties(editor_selection PROPERTIES ENVIRONMENT "QT_PLUGIN_PATH=${SMCP_QT_BIN_DIR}/../plugins")

    add_executable(SMCP_ai_tests
        tests/AiModelTests.cpp
        src/ai/WslModelService.cpp
        src/ai/WslModelService.h
    )
    target_include_directories(SMCP_ai_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_definitions(SMCP_ai_tests PRIVATE UNICODE _UNICODE)
    target_compile_options(SMCP_ai_tests PRIVATE /W3 /utf-8)
    target_link_libraries(SMCP_ai_tests PRIVATE Qt5::Core)
    add_test(NAME ai_model_contract COMMAND SMCP_ai_tests)

    add_executable(SMCP_pointcloud_tests
        tests/PointcloudOpsTests.cpp
        src/core/PointcloudOps.cpp
        src/core/PointcloudOps.h
    )
    target_include_directories(SMCP_pointcloud_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_definitions(SMCP_pointcloud_tests PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES _HAS_AUTO_PTR_ETC=1)
    target_compile_options(SMCP_pointcloud_tests PRIVATE /W3 /utf-8 /external:W0)
    target_link_libraries(SMCP_pointcloud_tests PRIVATE opencv_core opencv_flann)
    add_test(NAME pointcloud_registration COMMAND SMCP_pointcloud_tests)

    # Sub-pixel decoding and reconstruction, and the ways of solving the calibration, on synthetic
    # data with a known answer.
    add_executable(SMCP_subpixel_tests
        tests/SubpixelTests.cpp
        src/core/Calibration.cpp
        src/core/CalibrationData.cpp
        src/core/Scan3d.cpp
        src/core/StructuredLight.cpp
    )
    set_target_properties(SMCP_subpixel_tests PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
    target_include_directories(SMCP_subpixel_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_definitions(SMCP_subpixel_tests PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES
        _HAS_AUTO_PTR_ETC=1 UNICODE _UNICODE)
    target_compile_options(SMCP_subpixel_tests PRIVATE /W3 /utf-8 /external:W0)
    target_link_libraries(SMCP_subpixel_tests PRIVATE Qt5::Core opencv_core opencv_imgproc opencv_highgui opencv_calib3d
        opencv_features2d opencv_flann)
    add_test(NAME subpixel_reconstruction COMMAND SMCP_subpixel_tests)

    add_executable(SMCP_sphere_tests
        tests/SphereMeasurementTests.cpp
        src/core/SphereMeasurement.cpp
        src/core/SphereMeasurement.h
        src/core/PointcloudOps.cpp
        src/core/PointcloudOps.h
    )
    target_include_directories(SMCP_sphere_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_definitions(SMCP_sphere_tests PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES _HAS_AUTO_PTR_ETC=1)
    target_compile_options(SMCP_sphere_tests PRIVATE /W3 /utf-8 /external:W0)
    target_link_libraries(SMCP_sphere_tests PRIVATE opencv_core opencv_imgproc opencv_flann)
    add_test(NAME sphere_measurement COMMAND SMCP_sphere_tests)

    add_executable(SMCP_compensation_tests
        tests/CompensationTests.cpp
        src/core/Compensation.cpp
        src/core/Compensation.h
        src/core/PointcloudOps.cpp
        src/core/PointcloudOps.h
    )
    target_include_directories(SMCP_compensation_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_definitions(SMCP_compensation_tests PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES
        _HAS_AUTO_PTR_ETC=1 UNICODE _UNICODE SMCP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    target_compile_options(SMCP_compensation_tests PRIVATE /W3 /utf-8 /external:W0)
    target_link_libraries(SMCP_compensation_tests PRIVATE Qt5::Core opencv_core opencv_flann)
    add_test(NAME compensation_model COMMAND SMCP_compensation_tests)

    # Rendering check of the editor viewer on a real OpenGL context with a synthetic cloud (CTest
    # runs on the offscreen platform, which has no OpenGL). Run by hand: SMCP_viewer_render_check
    add_executable(SMCP_viewer_render_check
        tests/ViewerRenderCheck.cpp
        ${SMCP_RESOURCES}
    )
    target_link_libraries(SMCP_viewer_render_check PRIVATE smcp_app)

    # Renders the themed controls and some editor dialogs to PNG files, to review the visual
    # style without opening the application. Run by hand: SMCP_style_gallery <output directory>
    add_executable(SMCP_style_gallery
        tests/StyleGallery.cpp
        ${SMCP_RESOURCES}
    )
    target_link_libraries(SMCP_style_gallery PRIVATE smcp_app)

    # Takes the documentation screenshots (docs/screenshots) from the real application, on a
    # workspace with data. Run by hand, see the header of the source file:
    #   SMCP_doc_screenshots <workspace> <output directory>
    add_executable(SMCP_doc_screenshots
        tests/DocScreenshots.cpp
        ${SMCP_RESOURCES}
    )
    target_link_libraries(SMCP_doc_screenshots PRIVATE smcp_app dwmapi)

    # Regenerates resources/icons/app-icon.ico and app-icon.png from the SVG artwork (needs its font
    # installed). Run by hand when the artwork changes: SMCP_make_app_icon <svg> resources/icons
    add_executable(SMCP_make_app_icon tests/MakeAppIcon.cpp)
    set_target_properties(SMCP_make_app_icon PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
    target_link_libraries(SMCP_make_app_icon PRIVATE Qt5::Widgets)

    # Golden regression of decoding, calibration and reconstruction against the original serial
    # code kept in tests/legacy. It needs a real workspace, so it is run by hand:
    #   SMCP_pipeline_regress <workspace> --cols 24 --rows 17
    add_executable(SMCP_pipeline_regress
        tests/PipelineRegression.cpp
        tests/legacy/LegacyScan3d.cpp
        tests/legacy/LegacyScan3d.h
        tests/legacy/LegacyStructuredLight.cpp
        tests/legacy/LegacyStructuredLight.h
        src/core/Calibration.cpp
        src/core/CalibrationData.cpp
        src/core/Scan3d.cpp
        src/core/StructuredLight.cpp
    )
    set_target_properties(SMCP_pipeline_regress PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
    target_include_directories(SMCP_pipeline_regress PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tests)
    target_compile_definitions(SMCP_pipeline_regress PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES
        _HAS_AUTO_PTR_ETC=1 UNICODE _UNICODE)
    target_compile_options(SMCP_pipeline_regress PRIVATE /W3 /utf-8 /external:W0)
    target_link_libraries(SMCP_pipeline_regress PRIVATE Qt5::Widgets opencv_core opencv_imgproc opencv_highgui
        opencv_calib3d opencv_features2d opencv_flann)

    # Read-only statistics of the decoded Gray code of existing capture sets (camera pixels per
    # projector pixel, uncertain pixels, centroid noise), to judge reconstruction changes before
    # making them. Run by hand: SMCP_decode_stats <set directory>...
    # Plane-fit noise of the classic and the sub-pixel reconstruction on the chessboards of the
    # calibration sets of a workspace (read-only). Run by hand: SMCP_reconstruct_compare <workspace>
    add_executable(SMCP_reconstruct_compare
        tests/ReconstructCompare.cpp
        src/core/CalibrationData.cpp
        src/core/Scan3d.cpp
        src/core/StructuredLight.cpp
    )
    set_target_properties(SMCP_reconstruct_compare PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
    target_include_directories(SMCP_reconstruct_compare PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_definitions(SMCP_reconstruct_compare PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES
        _HAS_AUTO_PTR_ETC=1 UNICODE _UNICODE)
    target_compile_options(SMCP_reconstruct_compare PRIVATE /W3 /utf-8 /external:W0)
    target_link_libraries(SMCP_reconstruct_compare PRIVATE Qt5::Core opencv_core opencv_imgproc opencv_highgui
        opencv_calib3d)

    # One capture set reconstructed with a given calibration and method, from the command line
    # (read-only). Run by hand: SMCP_reconstruct_set <set> --calibration <yml> --out <cloud.xyz>
    add_executable(SMCP_reconstruct_set
        tests/ReconstructSet.cpp
        src/core/CalibrationData.cpp
        src/core/Scan3d.cpp
        src/core/StructuredLight.cpp
    )
    set_target_properties(SMCP_reconstruct_set PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
    target_include_directories(SMCP_reconstruct_set PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_definitions(SMCP_reconstruct_set PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES
        _HAS_AUTO_PTR_ETC=1 UNICODE _UNICODE)
    target_compile_options(SMCP_reconstruct_set PRIVATE /W3 /utf-8 /external:W0)
    target_link_libraries(SMCP_reconstruct_set PRIVATE Qt5::Core opencv_core opencv_imgproc opencv_highgui
        opencv_calib3d)

    # Ways of solving the calibration, judged on boards left out of it, from the corners saved in
    # a workspace (read-only). Run by hand: SMCP_calibration_compare <workspace>
    add_executable(SMCP_calibration_compare
        tests/CalibrationCompare.cpp
        src/core/Calibration.cpp
        src/core/CalibrationData.cpp
        src/core/Scan3d.cpp
        src/core/StructuredLight.cpp
    )
    set_target_properties(SMCP_calibration_compare PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
    target_include_directories(SMCP_calibration_compare PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_definitions(SMCP_calibration_compare PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES
        _HAS_AUTO_PTR_ETC=1 UNICODE _UNICODE)
    target_compile_options(SMCP_calibration_compare PRIVATE /W3 /utf-8 /external:W0)
    target_link_libraries(SMCP_calibration_compare PRIVATE Qt5::Core opencv_core opencv_imgproc opencv_highgui
        opencv_calib3d opencv_features2d opencv_flann)

    # Sphere diameters, center distances and form against a reference artifact, from the command
    # line (read-only), to compare processing options on existing clouds.
    # Run by hand: SMCP_measure_spheres --artifact <artifact.json> <cloud.xyz>...
    add_executable(SMCP_measure_spheres
        tests/MeasureSpheres.cpp
        src/core/SphereMeasurement.cpp
        src/core/PointcloudOps.cpp
    )
    set_target_properties(SMCP_measure_spheres PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
    target_include_directories(SMCP_measure_spheres PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_definitions(SMCP_measure_spheres PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES
        _HAS_AUTO_PTR_ETC=1 UNICODE _UNICODE)
    target_compile_options(SMCP_measure_spheres PRIVATE /W3 /utf-8 /external:W0)
    target_link_libraries(SMCP_measure_spheres PRIVATE Qt5::Core opencv_core opencv_imgproc opencv_flann)

    # Periodic errors of the decoded projector coordinates on a flat chessboard (read-only).
    # Run by hand: SMCP_band_analysis <workspace> <set index>
    add_executable(SMCP_band_analysis tests/BandAnalysis.cpp src/core/StructuredLight.cpp)
    set_target_properties(SMCP_band_analysis PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
    target_include_directories(SMCP_band_analysis PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_definitions(SMCP_band_analysis PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES
        _HAS_AUTO_PTR_ETC=1 UNICODE _UNICODE)
    target_compile_options(SMCP_band_analysis PRIVATE /W3 /utf-8 /external:W0)
    target_link_libraries(SMCP_band_analysis PRIVATE opencv_core opencv_imgproc opencv_highgui)

    add_executable(SMCP_decode_stats tests/DecodeStats.cpp src/core/StructuredLight.cpp)
    set_target_properties(SMCP_decode_stats PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
    target_include_directories(SMCP_decode_stats PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_definitions(SMCP_decode_stats PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES
        _HAS_AUTO_PTR_ETC=1 UNICODE _UNICODE)
    target_compile_options(SMCP_decode_stats PRIVATE /W3 /utf-8 /external:W0)
    target_link_libraries(SMCP_decode_stats PRIVATE opencv_core opencv_imgproc opencv_highgui)
endif()

# ---------------------------------------------------------------------------
# compile_commands.json for clangd (Zed): adds /vctoolsdir and the Windows SDK to
# every command so clang-cl finds the STL outside the Visual Studio environment.
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
        COMMENT "Completing compile_commands.json for clangd"
        VERBATIM)
endif()

# ---------------------------------------------------------------------------
# Post-build: self-contained executable in build/<preset>/bin
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
        COMMENT "Deploying Qt and OpenCV next to $<TARGET_FILE_NAME:SMCP>"
        VERBATIM)

    if(SMCP_WITH_SPINNAKER)
        add_custom_command(TARGET SMCP POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "$<IF:$<CONFIG:Debug>,${Spinnaker_RUNTIME_DEBUG},${Spinnaker_RUNTIME_RELEASE}>"
                    "$<TARGET_FILE_DIR:SMCP>"
            COMMENT "Deploying Spinnaker next to $<TARGET_FILE_NAME:SMCP>"
            COMMAND_EXPAND_LISTS
            VERBATIM)
    endif()
endif()
