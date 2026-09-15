# Script (cmake -P) que completa compile_commands.json para clangd.
#
# clangd interpreta cl.exe en modo clang-cl, pero fuera de un "Developer
# Command Prompt" no sabe donde esta el toolset de MSVC (falla con
# "'type_traits' file not found"). Se anaden a cada comando las opciones
# /vctoolsdir y, si se conocen, /winsdkdir + /winsdkversion, que cl.exe nunca
# ve (solo viven en la base de compilacion) y clang-cl si entiende.
#
# Variables esperadas:
#   DB           ruta de compile_commands.json
#   VCTOOLSDIR   raiz del toolset (…/VC/Tools/MSVC/<version>)
#   WINSDKDIR    raiz del Windows SDK (opcional)
#   WINSDKVER    version del Windows SDK (opcional)

if(NOT EXISTS "${DB}")
    message(STATUS "clangd: no existe ${DB}; nada que hacer.")
    return()
endif()

file(READ "${DB}" _content)
if(_content MATCHES "/vctoolsdir")
    return()  # ya parcheado
endif()

set(_extra " /vctoolsdir \\\"${VCTOOLSDIR}\\\"")
if(WINSDKDIR AND WINSDKVER)
    string(APPEND _extra " /winsdkdir \\\"${WINSDKDIR}\\\" /winsdkversion ${WINSDKVER}")
endif()

# Cada comando contiene exactamente un " -c <archivo>"; se insertan las opciones delante.
string(REPLACE " -c " "${_extra} -c " _patched "${_content}")
file(WRITE "${DB}" "${_patched}")
message(STATUS "clangd: compile_commands.json completado con ${_extra}")
