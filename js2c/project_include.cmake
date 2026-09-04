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
set(JS2C_SECTIONS  "${JS2C_TOOL_DIR}/js2c_sections.py")

if(NOT EXISTS "${JS2C_GENERATOR}")
    message(FATAL_ERROR "js2c: vendored generator missing at ${JS2C_GENERATOR}")
endif()

# BUILD_DIR is readable during early expansion; CMAKE_CURRENT_BINARY_DIR is not, and
# it also differs between a local component and a namespaced managed_components one
# (esp-idf/CMakeLists.txt uses add_subdirectory(${dir} ${prefix}_${name}) for the
# latter). Deriving from BUILD_DIR + COMPONENT_NAME is stable in both placements.
idf_build_get_property(_js2c_build_dir BUILD_DIR)
set(JS2C_GEN_ROOT "${_js2c_build_dir}/js2c_gen")
file(MAKE_DIRECTORY "${JS2C_GEN_ROOT}")

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

    # Always non-empty: --authorized-paths takes one or more values, and the
    # schema's own directory is the natural floor (the generator allows it anyway).
    get_filename_component(_schema_dir "${_schema}" DIRECTORY)
    set(_auth "${_schema_dir}" ${ARG_AUTHORIZED_PATHS})

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
# js2c_generate_sections(<schema> BASENAME <name> [GEN_DIR <dir>]
#                        [AUTHORIZED_PATHS <dirs...>]
#                        [OUT_C <var>] [OUT_H <var>] [OUT_MACRO_H <var>])
#
# The output variables must not be named after this function's own keywords
# (BASENAME, GEN_DIR, ...): cmake_parse_arguments treats a value that matches a
# keyword as a keyword, so `OUT_C GEN_DIR` would parse as two empty options
# rather than as a name to write to.
#
# Generate a project's top-level config walker from its authored config schema.
#
# The project writes one file. Each top-level property that $refs another
# component's schema is a "section": the walker records its byte offset and
# length rather than parsing it, and the project hands that slice to the parser
# belonging to the component that owns it -- in place, without copying.
#
# It cannot simply be generated from the authored schema, because
# json_schema_to_c would emit a second definition of every section's struct,
# colliding with the one the owning component already generates. js2c_sections.py
# does the rewrite, and also emits <ID>_SECTIONS(X) so a section added to the
# schema but not dispatched in C fails to compile rather than being silently
# left zeroed.
#
# Like js2c_generate(), this must be called inside
# `if(NOT CMAKE_BUILD_EARLY_EXPANSION)`.
#
function(js2c_generate_sections SCHEMA)
    cmake_parse_arguments(ARG "" "BASENAME;GEN_DIR;OUT_C;OUT_H;OUT_MACRO_H"
                          "AUTHORIZED_PATHS" ${ARGN})

    get_filename_component(_schema "${SCHEMA}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_LIST_DIR}")
    if(NOT EXISTS "${_schema}")
        message(FATAL_ERROR "js2c_generate_sections: no such schema: ${_schema}")
    endif()

    if(NOT ARG_BASENAME)
        get_filename_component(ARG_BASENAME "${_schema}" NAME_WE)
    endif()
    if(NOT ARG_GEN_DIR)
        set(ARG_GEN_DIR "${JS2C_GEN_ROOT}/${COMPONENT_NAME}")
    endif()
    file(MAKE_DIRECTORY "${ARG_GEN_DIR}")

    set(_derived  "${ARG_GEN_DIR}/${ARG_BASENAME}_sections.json")
    set(_macro_h  "${ARG_GEN_DIR}/${ARG_BASENAME}_sections.h")

    idf_build_get_property(_py PYTHON)
    if(NOT _py)
        set(_py "python3")
    endif()

    get_filename_component(_schema_dir "${_schema}" DIRECTORY)
    set(_auth "${_schema_dir}" ${ARG_AUTHORIZED_PATHS})

    # Map every component in the build to its directory, so a section can name the
    # component that owns its fragment and be resolved wherever that component
    # physically lives -- components/, a namespaced directory under
    # managed_components/, or anywhere EXTRA_COMPONENT_DIRS points.
    #
    # These properties are populated while the component list is built, which is
    # before any component CMakeLists.txt runs, so this does not depend on
    # configuration order. A staging step would: ESP-IDF gives `main` an implicit
    # dependency on every component but still configures it before most of them,
    # so a fragment copied out by its owner is not there yet when main asks.
    idf_build_get_property(_components BUILD_COMPONENTS)
    set(_roots "")
    foreach(_name IN LISTS _components)
        if(_name MATCHES "::")
            continue()   # alias form, same component
        endif()
        idf_component_get_property(_dir ${_name} COMPONENT_DIR)
        if(_dir)
            list(APPEND _roots "--component-root" "${_name}=${_dir}")
        endif()
    endforeach()

    execute_process(
        COMMAND "${_py}" "${JS2C_SECTIONS}" "${_schema}" "${_derived}" "${_macro_h}"
                ${_roots}
                --authorized-paths ${_auth}
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE  _err)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "js2c_sections failed for ${_schema}\n${_err}${_out}")
    endif()
    message(STATUS "js2c: ${_out}")

    # The derived schema carries its own token budget in js2cSettings, so the
    # parser generation below needs no extra flags.
    js2c_generate("${_derived}"
                  BASENAME "${ARG_BASENAME}"
                  GEN_DIR "${ARG_GEN_DIR}"
                  AUTHORIZED_PATHS ${_auth}
                  OUT_C _c OUT_H _h)

    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                 "${_schema}" "${JS2C_SECTIONS}")

    if(ARG_OUT_C)
        set(${ARG_OUT_C} "${_c}" PARENT_SCOPE)
    endif()
    if(ARG_OUT_H)
        set(${ARG_OUT_H} "${_h}" PARENT_SCOPE)
    endif()
    if(ARG_OUT_MACRO_H)
        set(${ARG_OUT_MACRO_H} "${_macro_h}" PARENT_SCOPE)
    endif()
    # No OUT_DIR: derive it from OUT_C with get_filename_component(). An output
    # variable named GEN_DIR would collide with this function's own GEN_DIR
    # keyword -- cmake_parse_arguments treats a value that matches a keyword as a
    # keyword, so the pair would silently parse as two empty options.
endfunction()
