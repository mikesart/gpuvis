# CMakeLists.txt for gpuvis
cmake_minimum_required( VERSION 2.8 FATAL_ERROR )

project( "gpuvis" )

# bring in useful cmake macros
#   https://github.com/onqtam/ucm
include( "ucm.cmake" )

find_package( SDL2 REQUIRED )

ucm_set_runtime( STATIC )

# Same flags as the Linux Makefile
# set( CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -DDEBUG" )
# set( CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -DNDEBUG" )
ucm_add_flags( -O0 -DDEBUG CONFIG Debug )
ucm_add_flags( -O2 -DNDEBUG CONFIG Release )

if ( CMAKE_COMPILER_IS_GNUCC )
    ucm_add_flags( CXX -std=c++11 )
endif()

if ( WIN32 )
    set( WINRESOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/resources )
endif()

if ( WIN32 )
    LINK_LIBRARIES( shlwapi winhttp rpcrt4 crypt32 ole32 )
elseif (UNIX)
    LINK_LIBRARIES( dl )
endif()

ucm_print_flags()

# Main source list
set ( SRC_LIST
    src/gpuvis.cpp
    src/gpuvis_graph.cpp
    src/gpuvis_utils.cpp
    src/hash_fnv.c
    src/stlini.cpp
    src/imgui/imgui_impl_sdl_gl3.cpp
    src/imgui/imgui.cpp
    src/imgui/imgui_demo.cpp
    src/imgui/imgui_draw.cpp
    src/GL/gl3w.c
    src/trace-cmd/event-parse.c
    src/trace-cmd/trace-seq.c
    src/trace-cmd/kbuffer-parse.c
    src/trace-cmd/trace-read.cpp
    )

if ( WIN32 )
    set( SRC_LIST ${SRC_LIST} ${WINRESOURCE_DIR}/gpuvis.rc )
endif()

include_directories(
    ${SDL2_INCLUDE_DIRS}
    ${SQLITE3_INCLUDE}
    ${WINRESOURCE_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/src
    )

ucm_add_target( NAME gpuvis TYPE EXECUTABLE SOURCES ${SRC_LIST} )

target_link_libraries(
    ${PROJECT_NAME}
    ${LIBRARY_LIST}
    ${SDL2_LIBRARIES}
    )

