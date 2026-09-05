# js2c -- runtime support for json_schema_to_c generated parsers.
#
# The CMake code-generation API lives in project_include.cmake, which IDF pulls
# into project scope before any component is added. This file builds only the
# small runtime the generated parsers link against.
#
# jsmn.h and js2c_builtins.h here are byte-identical copies of the ones inside
# tool/js2c/codegen/ -- the generator and its runtime are a matched pair with no
# version check, so they are kept identical by construction and verified by the
# check-runtime-identical target in test/Makefile, which every `make -C test` runs.

idf_component_register(
    SRCS         "src/js2c_error_capture.c"
    INCLUDE_DIRS "include")

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
