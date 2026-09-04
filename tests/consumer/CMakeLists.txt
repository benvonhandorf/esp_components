# Out-of-repo consumption test.
#
# This project deliberately lives outside any component's source tree and reaches
# esp_components only through EXTRA_COMPONENT_DIRS, so nothing here can depend on a
# project-scope variable that the components repo happens to set. That is the whole
# point: an in-tree build cannot prove reuse, because main sees everything.
cmake_minimum_required(VERSION 3.22)
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../..")

# On the linux target the build set has to be narrowed by hand. `main` deliberately
# declares no REQUIRES (project.cmake only grants it the implicit dependency on every
# component while REQUIRES and PRIV_REQUIRES are both unset), so it otherwise drags in
# the whole component tree -- and on IDF 6.0.2 esp_driver_ana_cmpr cannot resolve
# esp_hal_ana_cmpr for linux. COMPONENTS limits which components are built at all,
# without touching main's requirements.
if(IDF_TARGET STREQUAL "linux")
    set(COMPONENTS main js2c demo_net diag cli cli_web)
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(js2c_consumer)
