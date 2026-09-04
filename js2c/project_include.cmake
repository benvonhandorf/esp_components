#
# js2c -- JSON Schema to C code generation for ESP-IDF components.
#
# ESP-IDF includes this file into *project* scope before any component's
# CMakeLists.txt is added to the build (esp-idf/tools/cmake/build.cmake:518,
# called from :793, immediately before the add_subdirectory() loop). That is what
# lets a component living in another repository call js2c_generate() without the
# consuming project having to define anything.
#
# Note the variable trap: here CMAKE_CURRENT_SOURCE_DIR is the *project* root, not
# this component. Use COMPONENT_DIR, which IDF sets for each project_include.
#

set(JS2C_TOOL_DIR  "${COMPONENT_DIR}/tool")
set(JS2C_GENERATOR "${JS2C_TOOL_DIR}/json_schema_to_c.py")

if(NOT EXISTS "${JS2C_GENERATOR}")
    message(FATAL_ERROR "js2c: vendored generator missing at ${JS2C_GENERATOR}")
endif()

# BUILD_DIR is readable during early expansion; CMAKE_CURRENT_BINARY_DIR is not, and
# it also differs between a local component and a namespaced managed_components one
# (esp-idf/CMakeLists.txt uses add_subdirectory(${dir} ${prefix}_${name}) for the
# latter). Deriving from BUILD_DIR + COMPONENT_NAME is stable in both placements.
idf_build_get_property(_js2c_build_dir BUILD_DIR)
set(JS2C_GEN_ROOT    "${_js2c_build_dir}/js2c_gen")
set(JS2C_SCHEMA_ROOT "${_js2c_build_dir}/js2c_schemas")
file(MAKE_DIRECTORY "${JS2C_GEN_ROOT}" "${JS2C_SCHEMA_ROOT}")

#
# js2c_generate(<schema> [BASENAME <name>] [GEN_DIR <dir>] [EXTRA_ARGS <args...>]
#               [AUTHORIZED_PATHS <dirs...>] [OUT_C <var>] [OUT_H <var>])
#
# Generates a parser at *configure* time, not build time. Three reasons:
#   - The generated header is part of the owning component's public interface. A
#     build-time custom command gives a dependent's compile step no ordering edge
#     to the generator (ninja only orders the link), so a dependent that includes
#     the header races it on a clean parallel build.
#   - idf_component_register() rejects an INCLUDE_DIRS entry that is not an
#     existing directory (component.cmake:317).
#   - SRCS then names a file that actually exists.
# The cost is a re-configure when a schema changes, which CMAKE_CONFIGURE_DEPENDS
# below makes automatic.
#
# Output goes under BUILD_DIR, never the component source dir: a component pulled
# from managed_components/ is hash-verified and re-extracted on dependency changes.
#
# MUST be called inside `if(NOT CMAKE_BUILD_EARLY_EXPANSION)`. IDF re-runs every
# component CMakeLists.txt in `cmake -P` script mode to collect requirements, and
# in that pass this function does not exist.
#
function(js2c_generate SCHEMA)
    cmake_parse_arguments(ARG "" "BASENAME;GEN_DIR;OUT_C;OUT_H" "EXTRA_ARGS;AUTHORIZED_PATHS" ${ARGN})

    get_filename_component(_schema "${SCHEMA}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_LIST_DIR}")
    if(NOT EXISTS "${_schema}")
        message(FATAL_ERROR "js2c_generate: no such schema: ${_schema}")
    endif()

    if(NOT ARG_BASENAME)
        get_filename_component(ARG_BASENAME "${_schema}" NAME_WE)
    endif()
    if(NOT ARG_GEN_DIR)
        set(ARG_GEN_DIR "${JS2C_GEN_ROOT}/${COMPONENT_NAME}")
    endif()
    file(MAKE_DIRECTORY "${ARG_GEN_DIR}")

    set(_c "${ARG_GEN_DIR}/${ARG_BASENAME}.c")
    set(_h "${ARG_GEN_DIR}/${ARG_BASENAME}.h")

    # Use IDF's python, not a bare python3: the generator must run under the same
    # interpreter the rest of the build uses.
    idf_build_get_property(_py PYTHON)
    if(NOT _py)
        set(_py "python3")
    endif()

    # The schema's own directory is always allowed by the generator; add the staged
    # schema root so an aggregate can $ref fragments published by other components.
    set(_auth "${JS2C_SCHEMA_ROOT}" ${ARG_AUTHORIZED_PATHS})

    execute_process(
        COMMAND "${_py}" "${JS2C_GENERATOR}"
                --include-external-builtins-file "js2c_builtins.h"
                ${ARG_EXTRA_ARGS} "${_schema}" "${_c}" "${_h}"
                # --authorized-paths takes nargs="+", so it must come last or it
                # swallows the positional schema/c/h arguments.
                --authorized-paths ${_auth}
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE  _err)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "js2c: generation failed for ${_schema}\n${_err}${_out}")
    endif()

    # Re-run CMake when the schema or the generator changes.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                 "${_schema}" "${JS2C_GENERATOR}")

    if(ARG_OUT_C)
        set(${ARG_OUT_C} "${_c}" PARENT_SCOPE)
    endif()
    if(ARG_OUT_H)
        set(${ARG_OUT_H} "${_h}" PARENT_SCOPE)
    endif()
endfunction()

#
# js2c_publish_schema(<logical-name> <schema>)
#
# Stages a component's schema fragment at ${JS2C_SCHEMA_ROOT}/<logical-name>/<file>
# so a project can $ref it by a path that does not depend on where the component
# physically lives -- "networking/wifi_config_schema.json" resolves the same whether
# the component sits in components/ or managed_components/benvonhandorf__networking.
#
# The vendored generator DOES follow cross-file $refs (js2c/schema.py
# get_schema_from_path), gated by --authorized-paths for safety and resolved
# relative to the referring schema's own directory. js2c_generate() therefore
# authorizes JS2C_SCHEMA_ROOT by default, so a project aggregate staged there can
# $ref "networking/wifi_config_schema.json" and have it resolve.
#
# (An older revision of the generator supported only in-file "#/" refs. That is why
# the predecessor project's composed config_schema.json was documentation-only and
# its top-level parse was hand-written. It no longer has to be.)
#
function(js2c_publish_schema LOGICAL_NAME SCHEMA)
    get_filename_component(_abs "${SCHEMA}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_LIST_DIR}")
    if(NOT EXISTS "${_abs}")
        message(FATAL_ERROR "js2c_publish_schema: no such schema: ${_abs}")
    endif()
    get_filename_component(_name "${_abs}" NAME)
    # COPYONLY copies now and registers _abs as a configure dependency.
    configure_file("${_abs}" "${JS2C_SCHEMA_ROOT}/${LOGICAL_NAME}/${_name}" COPYONLY)
    idf_build_set_property(JS2C_PUBLISHED "${LOGICAL_NAME}=${_abs}" APPEND)
endfunction()
