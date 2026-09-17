# Script (cmake -P) that completes compile_commands.json for clangd.
#
# clangd interprets cl.exe in clang-cl mode, but outside a Developer Command
# Prompt it cannot find the MSVC toolset (it fails with "'type_traits' file not
# found"). Every command receives /vctoolsdir and, when known, /winsdkdir and
# /winsdkversion. cl.exe never sees these options (they only exist in the
# compilation database), and clang-cl understands them.
#
# Expected variables:
#   DB           path of compile_commands.json
#   VCTOOLSDIR   toolset root (.../VC/Tools/MSVC/<version>)
#   WINSDKDIR    Windows SDK root (optional)
#   WINSDKVER    Windows SDK version (optional)

if(NOT EXISTS "${DB}")
    message(STATUS "clangd: ${DB} does not exist; nothing to do.")
    return()
endif()

file(READ "${DB}" _content)
if(_content MATCHES "/vctoolsdir")
    return()  # already patched
endif()

set(_extra " /vctoolsdir \\\"${VCTOOLSDIR}\\\"")
if(WINSDKDIR AND WINSDKVER)
    string(APPEND _extra " /winsdkdir \\\"${WINSDKDIR}\\\" /winsdkversion ${WINSDKVER}")
endif()

# Each command contains exactly one " -c <file>"; the options are inserted before it.
string(REPLACE " -c " "${_extra} -c " _patched "${_content}")
file(WRITE "${DB}" "${_patched}")
message(STATUS "clangd: compile_commands.json completed with ${_extra}")
