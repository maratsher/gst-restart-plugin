cmake_minimum_required(VERSION 3.10)
project(gstrestartsrc VERSION 1.0)

set(CMAKE_C_STANDARD 11)

set(BUILD_TYPE SHARED)

find_package(PkgConfig)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET gstreamer-1.0>=1.4)
exec_program(${PKG_CONFIG_EXECUTABLE} ARGS --variable pluginsdir gstreamer-1.0 OUTPUT_VARIABLE PKG_GSTREAMER_PLUGIN_DIR)

if (CMAKE_BUILD_TYPE EQUAL "DEBUG")
    add_definitions(-g)
else()
    add_definitions(-O2)
endif()

add_library(gstrestartsrc ${BUILD_TYPE}
         gstrestartsrc.c
         gstrestartsrc.h)

target_link_libraries(gstrestartsrc PkgConfig::GST)

install(TARGETS gstrestartsrc LIBRARY DESTINATION ${PKG_GSTREAMER_PLUGIN_DIR})
	
