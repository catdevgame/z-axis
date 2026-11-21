# Every header of the readable game compiles on its own: one generated
# translation unit per header, built with the readable game (a header that
# leans on its includer's includes or using-declarations fails here, not in
# whichever file next includes it first). fncheck_traced.h is left out: it
# is only for the traced builds of the recompiled sources. Another game's
# folder (game/dm2, game/bmx) has its headers checked in that game's instance
# (game.cmake), where its sources are compiled.
#
# Included from game.cmake after zx_game_readable.

# zx_game_header_units(<target> <symbols dir> <header>...): an object library of one unit a header
# (none when there is no header).
function(zx_game_header_units _target _symbols)
    set(_units "")
    foreach(_header IN LISTS ARGN)
        file(RELATIVE_PATH _rel "${ZX_GAME_DIR}" "${_header}")
        string(REGEX REPLACE "[/.]" "_" _unit "${_rel}")
        set(_unit "${CMAKE_CURRENT_BINARY_DIR}/game_headers/${_unit}.cpp")
        set(_text "#include \"${_rel}\"\n")
        if(EXISTS "${_unit}")
            file(READ "${_unit}" _old)
        else()
            set(_old "")
        endif()
        if(NOT _old STREQUAL _text)
            file(WRITE "${_unit}" "${_text}")
        endif()
        list(APPEND _units "${_unit}")
    endforeach()
    if(NOT _units)
        return()
    endif()
    add_library(${_target} OBJECT ${_units})
    target_link_libraries(${_target} PUBLIC zx_runtime)
    target_include_directories(${_target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${ZX_GAME_DIR}" "${_symbols}")
endfunction()

file(GLOB_RECURSE _zx_game_headers CONFIGURE_DEPENDS "${ZX_GAME_DIR}/*.h")
list(REMOVE_ITEM _zx_game_headers "${ZX_GAME_DIR}/ee/fncheck_traced.h")
set(_zx_ai_headers ${_zx_game_headers})
foreach(_entry IN LISTS ZX_GAME_OTHER_GAMES)
    string(REGEX REPLACE "\\|.*" "" _game "${_entry}")
    set(ZX_GAME_FOLDER_HEADERS_${_game} ${_zx_game_headers})
    list(FILTER ZX_GAME_FOLDER_HEADERS_${_game} INCLUDE REGEX "^${ZX_GAME_DIR}/${_game}/")
    list(FILTER _zx_ai_headers EXCLUDE REGEX "^${ZX_GAME_DIR}/${_game}/")
endforeach()
zx_game_header_units(zx_game_headers "${ZX_SYMBOLS_DIR}/ai" ${_zx_ai_headers})
add_dependencies(zx_game_headers zx_game_symbols)
add_dependencies(zx_game_readable zx_game_headers)
