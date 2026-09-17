# Corrects the /showIncludes prefix that Ninja uses to track the header dependencies of cl.exe.
#
# A localized compiler prints that prefix in the console OEM code page (in Spanish,
# "Nota: inclusion del archivo:" with the accented o encoded as byte 0xA2). CMake decodes the
# output before storing CMAKE_CL_SHOWINCLUDES_PREFIX, so the stored text no longer matches the
# compiler bytes. Ninja then records no header dependencies: editing a header does not rebuild the
# sources that include it, and stale objects with different class layouts are linked together.
#
# This module queries cl.exe without decoding its output and replaces the prefix only when the one
# detected by CMake is not plain ASCII.

if(NOT MSVC OR NOT CMAKE_GENERATOR MATCHES "Ninja" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    return()
endif()

if(CMAKE_CL_SHOWINCLUDES_PREFIX MATCHES "^[ -~]+$")
    return()
endif()

set(_smcp_probe_dir "${CMAKE_BINARY_DIR}/CMakeFiles/SmcpShowIncludes")
file(MAKE_DIRECTORY "${_smcp_probe_dir}")
file(WRITE "${_smcp_probe_dir}/smcp_showincludes_probe.h" "#pragma once\n")
file(WRITE "${_smcp_probe_dir}/smcp_showincludes_probe.cpp" "#include \"smcp_showincludes_probe.h\"\n")

execute_process(
    COMMAND "${CMAKE_CXX_COMPILER}" /nologo /showIncludes /Zs smcp_showincludes_probe.cpp
    WORKING_DIRECTORY "${_smcp_probe_dir}"
    OUTPUT_VARIABLE _smcp_probe_output
    ERROR_VARIABLE _smcp_probe_error
    RESULT_VARIABLE _smcp_probe_result
    ENCODING NONE)

string(REGEX MATCH "([^\r\n]*:[ \t]+)[A-Za-z]:\\\\[^\r\n]*smcp_showincludes_probe\\.h"
    _smcp_probe_line "${_smcp_probe_output}")

if(_smcp_probe_result EQUAL 0 AND CMAKE_MATCH_1)
    set(CMAKE_CL_SHOWINCLUDES_PREFIX "${CMAKE_MATCH_1}")
    message(STATUS "Corrected the cl.exe /showIncludes prefix for the localized compiler")
else()
    message(FATAL_ERROR
        "Could not detect the /showIncludes prefix of the localized cl.exe. Ninja would not track "
        "header dependencies, and incremental builds could link stale objects. Use the vs2026 preset "
        "or install the English language pack for Visual Studio.\n${_smcp_probe_output}${_smcp_probe_error}")
endif()

unset(_smcp_probe_dir)
unset(_smcp_probe_output)
unset(_smcp_probe_error)
unset(_smcp_probe_result)
unset(_smcp_probe_line)
