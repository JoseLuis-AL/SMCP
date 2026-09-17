#[=======================================================================[.rst:
FindSpinnaker
-------------

Finds the FLIR/Teledyne Spinnaker SDK and defines the imported target
``Spinnaker::Spinnaker`` (headers plus Debug/Release libraries).

Input
^^^^^

``SPINNAKER_DIR``
  Installation root (contains ``include``, ``lib64``, and ``bin64``).
  When it is not set, the usual installation paths and the ``SPINNAKER_DIR``
  environment variable are searched.

Result
^^^^^^

``Spinnaker_FOUND``
``Spinnaker_INCLUDE_DIR``
``Spinnaker_LIBRARY_RELEASE`` / ``Spinnaker_LIBRARY_DEBUG``
``Spinnaker_RUNTIME_DIR``
  ``bin64/vsXXXX`` directory containing the DLLs.
``Spinnaker_RUNTIME_RELEASE`` / ``Spinnaker_RUNTIME_DEBUG``
  Runtime DLLs (Spinnaker, GenICam, OpenMP) to copy next to the executable.

The SDK ships libraries per toolset (``lib64/vs2017`` -> ``*_v141``,
``lib64/vs2015`` -> ``*_v140``). vs2017 is preferred and vs2015 is the fallback;
both are ABI-compatible with MSVC v141..v145.
#]=======================================================================]

set(SPINNAKER_DIR "${SPINNAKER_DIR}" CACHE PATH "Spinnaker SDK root")

set(_spin_hints "${SPINNAKER_DIR}" "$ENV{SPINNAKER_DIR}")
set(_spin_paths
    "C:/Program Files/Teledyne/Spinnaker"
    "D:/Program Files/Teledyne/Spinnaker"
    "C:/Program Files/FLIR Systems/Spinnaker"
    "D:/Program Files/FLIR Systems/Spinnaker")

find_path(Spinnaker_INCLUDE_DIR
    NAMES Spinnaker.h
    HINTS ${_spin_hints}
    PATHS ${_spin_paths}
    PATH_SUFFIXES include)

set(_spin_root "")
if(Spinnaker_INCLUDE_DIR)
    get_filename_component(_spin_root "${Spinnaker_INCLUDE_DIR}" DIRECTORY)
    if(NOT SPINNAKER_DIR)
        set(SPINNAKER_DIR "${_spin_root}" CACHE PATH "Spinnaker SDK root" FORCE)
    endif()
endif()

find_library(Spinnaker_LIBRARY_RELEASE
    NAMES Spinnaker_v141 Spinnaker_v140
    HINTS ${_spin_hints} ${_spin_root}
    PATHS ${_spin_paths}
    PATH_SUFFIXES lib64/vs2017 lib64/vs2015
    NO_DEFAULT_PATH)

find_library(Spinnaker_LIBRARY_DEBUG
    NAMES Spinnakerd_v141 Spinnakerd_v140
    HINTS ${_spin_hints} ${_spin_root}
    PATHS ${_spin_paths}
    PATH_SUFFIXES lib64/vs2017 lib64/vs2015
    NO_DEFAULT_PATH)

# DLL directory matching the toolset of the library that was found.
if(Spinnaker_LIBRARY_RELEASE)
    get_filename_component(_spin_lib_dir "${Spinnaker_LIBRARY_RELEASE}" DIRECTORY)
    get_filename_component(_spin_toolset "${_spin_lib_dir}" NAME)   # vs2017 | vs2015
    get_filename_component(_spin_lib_name "${Spinnaker_LIBRARY_RELEASE}" NAME_WE)
    string(REGEX REPLACE "^Spinnaker_(v[0-9]+)$" "\\1" _spin_suffix "${_spin_lib_name}")  # v141 | v140
    string(REGEX REPLACE "^v" "VC" _spin_vc "${_spin_suffix}")                              # VC141 | VC140

    set(Spinnaker_RUNTIME_DIR "${_spin_root}/bin64/${_spin_toolset}")
    if(NOT EXISTS "${Spinnaker_RUNTIME_DIR}/Spinnaker_${_spin_suffix}.dll")
        # Some installations ship only GPU modules in lib64/vs2017; their DLLs live in vs2015.
        set(Spinnaker_RUNTIME_DIR "${_spin_root}/bin64/vs2015")
    endif()

    file(GLOB Spinnaker_RUNTIME_RELEASE
        "${Spinnaker_RUNTIME_DIR}/Spinnaker_${_spin_suffix}.dll"
        "${Spinnaker_RUNTIME_DIR}/*_MD_${_spin_vc}_v3_*.dll"
        "${Spinnaker_RUNTIME_DIR}/libiomp5md.dll")
    file(GLOB Spinnaker_RUNTIME_DEBUG
        "${Spinnaker_RUNTIME_DIR}/Spinnakerd_${_spin_suffix}.dll"
        "${Spinnaker_RUNTIME_DIR}/*_MDd_${_spin_vc}_v3_*.dll"
        "${Spinnaker_RUNTIME_DIR}/libiomp5md.dll")
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Spinnaker
    REQUIRED_VARS Spinnaker_INCLUDE_DIR Spinnaker_LIBRARY_RELEASE
    FAIL_MESSAGE "Spinnaker SDK not found. Set SPINNAKER_DIR (for example C:/Program Files/Teledyne/Spinnaker) or build with -DSMCP_WITH_SPINNAKER=OFF.")

if(Spinnaker_FOUND AND NOT TARGET Spinnaker::Spinnaker)
    add_library(Spinnaker::Spinnaker UNKNOWN IMPORTED)
    set_target_properties(Spinnaker::Spinnaker PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${Spinnaker_INCLUDE_DIR}"
        IMPORTED_LOCATION "${Spinnaker_LIBRARY_RELEASE}"
        IMPORTED_LOCATION_RELEASE "${Spinnaker_LIBRARY_RELEASE}")
    set_property(TARGET Spinnaker::Spinnaker APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
    if(Spinnaker_LIBRARY_DEBUG)
        set_property(TARGET Spinnaker::Spinnaker APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
        set_target_properties(Spinnaker::Spinnaker PROPERTIES
            IMPORTED_LOCATION_DEBUG "${Spinnaker_LIBRARY_DEBUG}")
    endif()
endif()

mark_as_advanced(Spinnaker_INCLUDE_DIR Spinnaker_LIBRARY_RELEASE Spinnaker_LIBRARY_DEBUG)
unset(_spin_hints)
unset(_spin_paths)
unset(_spin_root)
