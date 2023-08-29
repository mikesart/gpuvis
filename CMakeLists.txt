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
option(USE_I915_PERF "USE_I915_PERF" OFF)
option(USE_RAPIDJSON "USE_RAPIDJSON" ON)

set( CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake )

# bring in useful cmake macros
#   https://github.com/onqtam/ucm
include( "cmake/ucm.cmake" )

find_package( SDL2 REQUIRED )

if ( USE_FREETYPE )
    find_package( Freetype REQUIRED )
    ucm_add_flags( -DUSE_FREETYPE )
endif()

# https://github.com/mikesart/gpuvis/issues/17
# From Pierre-Loup: Static is better for our usecase, since we do ad-hoc builds
#   that we distribute in SteamVR.
ucm_set_runtime( STATIC )

# https://codingnest.com/basic-cmake/#fnref6
#  also: CMAKE_CXX_COMPILER_VERSION
## if ( CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang|GNU" )
##     message("foo")
##     # target_compile_options( vector-test PRIVATE -Wall -Wextra -Wunreachable-code -Wpedantic)
## endif()
## if ( CMAKE_CXX_COMPILER_ID MATCHES "Clang" )
##     message("foo2")
##     # target_compile_options( vector-test PRIVATE -Wweak-vtables -Wexit-time-destructors -Wglobal-constructors -Wmissing-noreturn )
## endif()
## if ( CMAKE_CXX_COMPILER_ID MATCHES "MSVC" )
##     message("foo3")
##     # target_compile_options( vector-test PRIVATE /W4 /w44265 /w44061 /w44062 )
## endif()

ucm_add_flags( -O0 -DDEBUG CONFIG Debug )
ucm_add_flags( -O2 -DNDEBUG CONFIG Release )

if ( CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang" )
    ucm_add_flags( CXX -std=c++11 )
endif()

if ( APPLE )
    ucm_add_flags( CXX -stdlib=libc++ )
    ucm_add_linker_flags( -stdlib=libc++ )
endif()

if ( WIN32 )
    ucm_add_flags( -D_CRT_SECURE_NO_WARNINGS /wd4996 /wd4244 /wd4090 /wd4018 /wd4267 )

    LINK_LIBRARIES( shlwapi opengl32 comctl32 tdh )
elseif ( UNIX AND NOT APPLE )
    find_package( PkgConfig REQUIRED )
    pkg_check_modules( GTK3 REQUIRED gtk+-3.0 )

    ucm_add_flags( -DUSE_GTK3 )
    if(CMAKE_SYSTEM_NAME MATCHES "Linux|SunOS" OR MINGW)
        ucm_add_flags( -D_LARGEFILE64_SOURCE=1 -D_FILE_OFFSET_BITS=64 )
    endif()

    set( GTK3_LIBRARIES "" )

    LINK_LIBRARIES( dl )
endif()

if ( USE_I915_PERF )
    pkg_check_modules( I915_PERF REQUIRED i915-perf>=1.5.0)
    ucm_add_flags( -DUSE_I915_PERF )
endif()

if ( USE_RAPIDJSON )
    find_package( RapidJSON REQUIRED )
    ucm_add_flags( -DHAVE_RAPIDJSON )
endif()

ucm_print_flags()

# Main source list
set ( SRC_LIST
    src/gpuvis.cpp
    src/gpuvis_graph.cpp
    src/gpuvis_framemarkers.cpp
    src/gpuvis_plots.cpp
    src/gpuvis_graphrows.cpp
    src/gpuvis_ftrace_print.cpp
    src/gpuvis_i915_perfcounters.cpp
    src/gpuvis_utils.cpp
	src/gpuvis_etl.cpp
	src/etl_utils.cpp
    src/tdopexpr.cpp
    src/ya_getopt.c
    src/MurmurHash3.cpp
    src/miniz.c
    src/stlini.cpp
    src/imgui/imgui_impl_sdl_gl3.cpp
    src/imgui/imgui.cpp
    src/imgui/imgui_demo.cpp
    src/imgui/imgui_draw.cpp
    src/imgui/imgui_freetype.cpp
    src/GL/gl3w.c
    src/i915-perf/i915-perf-read.cpp
    src/libtraceevent/src/event-parse.c
    src/libtraceevent/src/event-parse-api.c
    src/libtraceevent/src/event-plugin.c
    src/libtraceevent/src/kbuffer-parse.c
    src/libtraceevent/src/parse-utils.c
    src/libtraceevent/src/trace-seq.c
    src/trace-cmd/trace-read.cpp
    )

if ( APPLE )
    list ( APPEND SRC_LIST src/noc_file_dialog_osx.mm )
endif()

include_directories(
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${CMAKE_CURRENT_LIST_DIR}/src/libtraceevent/include
    ${FREETYPE_INCLUDE_DIRS}
    ${GTK3_INCLUDE_DIRS}
    ${SDL2_INCLUDE_DIR}
    ${RAPIDJSON_INCLUDE_DIRS}
    ${I915_PERF_INCLUDE_DIRS}
    )

ucm_add_target( NAME gpuvis TYPE EXECUTABLE SOURCES ${SRC_LIST} )

target_link_libraries(
    ${PROJECT_NAME}
    ${LIBRARY_LIST}
    ${SDL2_LIBRARY}
    ${FREETYPE_LIBRARIES}
    ${GTK3_LIBRARIES}
    ${I915_PERF_LIBRARIES}
    )
