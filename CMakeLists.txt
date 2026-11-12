cmake_minimum_required(VERSION 3.16)

# ---- Pre-compress the SPA: 26 KB -> ~10 KB of flash ----
# CMAKE_CONFIGURE_DEPENDS makes CMake re-run when index.html changes, so the
# .gz is always regenerated before it gets embedded.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_LIST_DIR}/main/www/index.html")
execute_process(
    COMMAND sh -c "gzip -9fc '${CMAKE_CURRENT_LIST_DIR}/main/www/index.html' > '${CMAKE_CURRENT_LIST_DIR}/main/www/index.html.gz'"
    RESULT_VARIABLE _gz_rc)
if(NOT _gz_rc EQUAL 0)
    message(FATAL_ERROR "failed to gzip main/www/index.html (is gzip installed?)")
endif()
file(SIZE "${CMAKE_CURRENT_LIST_DIR}/main/www/index.html.gz" _gz_size)
message(STATUS "www/index.html.gz = ${_gz_size} bytes")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(beacon_gw)
