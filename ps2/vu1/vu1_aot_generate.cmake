# cmake -DTOOL=<zx-vu1aot> -DOUT=<folder> -DKEY=<key> -DIMAGES=<a|b|...> -P vu1_aot_generate.cmake
#
# The ahead-of-time VU1 code of the micro memory images IMAGES, made by TOOL into the folder OUT for the
# generator key KEY (ps2/CMakeLists.txt): made aside, then renamed into place, so a folder is never
# written over and two builds making the same one at once both end with it whole. The images go in with
# the code (the next generator makes its own folder from them), and vu1_aot.key says which generator
# made it.
#
# The rename needs OUT absent: Windows does not move a folder onto an existing one (Ninja makes OUT, its
# outputs' folder, before it runs this; on Windows the rename was "Access is denied"). So an OUT with no
# key (Ninja's empty folder, or what a killed run left) is taken away first; an OUT with this key is
# another run's finished code, adopted as it is.
string(REPLACE "|" ";" IMAGES "${IMAGES}")
# OUT's key: "" when it has none.
function(_vu1_made out result)
    set(_key "")
    if(EXISTS "${out}/vu1_aot.key")
        file(READ "${out}/vu1_aot.key" _key)
        string(STRIP "${_key}" _key)
    endif()
    set(${result} "${_key}" PARENT_SCOPE)
endfunction()
_vu1_made("${OUT}" _made)
if(_made STREQUAL KEY)
    return() # (another build made it first)
elseif(NOT _made STREQUAL "")
    message(FATAL_ERROR "${OUT} holds another generator's VU1 code (key ${_made}, not ${KEY})")
endif()
# (the aside folder this run's alone: a random tag and the time, two runs at once never share one)
string(RANDOM LENGTH 12 _tag)
string(TIMESTAMP _now "%Y%m%d%H%M%S%f")
set(_aside "${OUT}.making-${_now}-${_tag}")
file(REMOVE_RECURSE "${_aside}")
file(MAKE_DIRECTORY "${_aside}")
execute_process(COMMAND "${TOOL}" "${_aside}" ${IMAGES} RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
    file(REMOVE_RECURSE "${_aside}")
    message(FATAL_ERROR "zx-vu1aot failed (${_rc}): ${_err}")
endif()
foreach(_image IN LISTS IMAGES)
    get_filename_component(_stem "${_image}" NAME_WE)
    if(NOT EXISTS "${_aside}/vu1_aot_${_stem}.cpp")
        file(REMOVE_RECURSE "${_aside}")
        message(FATAL_ERROR "zx-vu1aot made no vu1_aot_${_stem}.cpp from ${_image} (an image is named by its hash)")
    endif()
    file(COPY "${_image}" DESTINATION "${_aside}")
endforeach()
file(WRITE "${_aside}/vu1_aot.key" "${KEY}\n")
# Into place: OUT taken away when it has no key, then the aside renamed onto the free name. Another run can
# take the name between the two: then its code (the same key) is adopted, and this run's aside dropped. A
# few tries, as a folder just emptied can stay busy a moment on Windows (a scanner holding a handle).
set(_renamed "")
foreach(_try RANGE 1 5)
    _vu1_made("${OUT}" _made)
    if(_made STREQUAL KEY)
        set(_renamed "0")
        break() # (another run's, whole)
    elseif(NOT _made STREQUAL "")
        file(REMOVE_RECURSE "${_aside}")
        message(FATAL_ERROR "${OUT} holds another generator's VU1 code (key ${_made}, not ${KEY})")
    endif()
    if(EXISTS "${OUT}")
        file(REMOVE_RECURSE "${OUT}")
    endif()
    file(RENAME "${_aside}" "${OUT}" RESULT _renamed)
    if(_renamed STREQUAL "0")
        break()
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1)
endforeach()
if(EXISTS "${_aside}")
    file(REMOVE_RECURSE "${_aside}")
endif()
_vu1_made("${OUT}" _made)
if(NOT _made STREQUAL KEY)
    message(FATAL_ERROR "cannot move the VU1 code into ${OUT}: ${_renamed}")
endif()
