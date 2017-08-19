# CMakeLists.txt for gpuvis

# To build on Windows:
#
# Download SDL2 development libraries and runtime binary:
#   https://www.libsdl.org/download-2.0.php
#
# set SDL2DIR=c:\dev\SDL2-2.0.5
#
# git clone https://github.com/mikesart/gpuvis
# mkdir gpuvis\build
# cd gpuvis\build
# ; copy SDL2.dll here...
#
# cmake -G "NMake Makefiles JOM" -DCMAKE_BUILD_TYPE=RelWithDebInfo ..
# jom
#

cmake_minimum_required( VERSION 2.8 FATAL_ERROR )

project( "gpuvis" )

option(USE_FREETYPE "USE_FREETYPE" ON)

set( CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake )

# bring in useful cmake macros
#   https://github.com/onqtam/ucm
include( "cmake/ucm.cmake" )

find_package( SDL2 REQUIRED )

if ( USE_FREETYPE )
    find_package( Freetype REQUIRED )
    ucm_add_flags( -DUSE_FREETYPE )
endif()

ucm_set_runtime( STATIC )

ucm_add_flags( -O0 -DDEBUG CONFIG Debug )
ucm_add_flags( -O2 -DNDEBUG CONFIG Release )

if ( CMAKE_COMPILER_IS_GNUCC )
    ucm_add_flags( CXX -std=c++11 )
endif()

if ( WIN32 )
    ucm_add_flags( -D_CRT_SECURE_NO_WARNINGS /wd4996 /wd4244 /wd4090 /wd4018 /wd4267 )

    LINK_LIBRARIES( shlwapi opengl32 comctl32 )
elseif ( UNIX )
    find_package( PkgConfig REQUIRED )
    pkg_check_modules( GTK3 REQUIRED gtk+-3.0 )

    LINK_LIBRARIES( dl )
endif()

ucm_print_flags()

# Main source list
set ( SRC_LIST
    src/gpuvis.cpp
    src/gpuvis_graph.cpp
    src/gpuvis_framemarkers.cpp
    src/gpuvis_plots.cpp
    src/gpuvis_graphrows.cpp
    src/gpuvis_utils.cpp
    src/tdopexpr.cpp
    src/ya_getopt.c
    src/hash_fnv.c
    src/stlini.cpp
    src/imgui/imgui_impl_sdl_gl3.cpp
    src/imgui/imgui.cpp
    src/imgui/imgui_demo.cpp
    src/imgui/imgui_draw.cpp
    src/imgui/imgui_freetype.cpp
    src/GL/gl3w.c
    src/trace-cmd/event-parse.c
    src/trace-cmd/trace-seq.c
    src/trace-cmd/kbuffer-parse.c
    src/trace-cmd/trace-read.cpp
    )

include_directories(
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${FREETYPE_INCLUDE_DIRS}
    ${GTK3_INCLUDE_DIRS}
    ${SDL2_INCLUDE_DIR}
    )

ucm_add_target( NAME gpuvis TYPE EXECUTABLE SOURCES ${SRC_LIST} )

target_link_libraries(
    ${PROJECT_NAME}
    ${LIBRARY_LIST}
    ${SDL2_LIBRARY}
    ${FREETYPE_LIBRARIES}
    ${GTK3_LIBRARIES}
    )
