//////////////////////////////////////////////////////////////////////////////
// gpuvis_trace_utils.h - v0.10 - public domain
//   no warranty is offered or implied; use this code at your own risk
//
// This is a single header file with useful utilities for gpuvis linux tracing
//
// ============================================================================
// You MUST define GPUVIS_IMPLEMENTATION in EXACTLY _one_ C or C++ file that
// includes this header, BEFORE the include, like this:
//
//   #define GPUVIS_IMPLEMENTATION
//   #include "gpuvis_trace_utils.h"
//
// All other files should just #include "gpuvis_trace_utils.h" w/o the #define.
// ============================================================================
//
// Credits
//
//    Michael Sartain
//
// LICENSE
//
//   This software is dual-licensed to the public domain and under the following
//   license: you are granted a perpetual, irrevocable license to copy, modify,
//   publish, and distribute this file as you see fit.

//////////////////////////////////////////////////////////////////////////////
//
//       INCLUDE SECTION
//

#ifndef _GPUVIS_TRACE_UTILS_H_
#define _GPUVIS_TRACE_UTILS_H_

#include <stdarg.h>

// printf-style warnings for user functions.
#if defined( __clang__ ) || defined( __GNUC__ )
#define GPUVIS_ATTR_PRINTF( _x, _y ) __attribute__( ( __format__( __printf__, _x, _y ) ) )
#else
#define GPUVIS_ATTR_PRINTF( _x, _y )
#endif

#if !defined( __linux__ )
#define GPUVIS_TRACE_UTILS_NOP
#endif

#ifdef GPUVIS_TRACE_UTILS_NOP
#undef GPUVIS_IMPLEMENTATION

static inline int gpuvis_trace_init() { return 0; }
static inline void gpuvis_trace_shutdown() {}
static inline const char *gpuvis_trace_errstr() { return ""; }

static inline int gpuvis_trace_printf( const char *fmt, ... ) { return 0; }
static inline int gpuvis_trace_vprintf( const char *fmt, va_list ap ) { return 0; }

static inline int gpuvis_trace_duration_printf( int duration, const char *fmt, ... ) { return 0; }
static inline int gpuvis_trace_duration_vprintf( int duration, const char *fmt, va_list ap ) { return 0; }

static inline int gpuvis_trace_begin_ctx_printf( int ctx, const char *fmt, ... ) { return 0; }
static inline int gpuvis_trace_begin_ctx_vprintf( int ctx, const char *fmt, va_list ap ) { return 0; }

static inline int gpuvis_trace_end_ctx_printf( int ctx, const char *fmt, ... ) { return 0; }
static inline int gpuvis_trace_end_ctx_vprintf( int ctx, const char *fmt, va_list ap ) { return 0; }

static inline int gpuvis_start_tracing() { return 0; }
static inline int gpuvis_trigger_capture_and_keep_tracing() { return 0; }
static inline int gpuvis_stop_tracing() { return 0; }

static inline int gpuvis_get_tracefs_dir() { return 0; }
static inline int gpuvis_get_trace_marker_path() { return 0; }

#else

#ifdef __cplusplus
  #define GPUVIS_EXTERN   extern "C"
#else
  #define GPUVIS_EXTERN   extern
#endif

GPUVIS_EXTERN int gpuvis_trace_init();
GPUVIS_EXTERN void gpuvis_trace_shutdown();
GPUVIS_EXTERN const char *gpuvis_trace_errstr();

GPUVIS_EXTERN int gpuvis_trace_printf( const char *fmt, ... ) GPUVIS_ATTR_PRINTF( 1, 2 );
GPUVIS_EXTERN int gpuvis_trace_vprintf( const char *fmt, va_list ap ) GPUVIS_ATTR_PRINTF( 1, 0 );

GPUVIS_EXTERN int gpuvis_trace_duration_printf( unsigned int duration, const char *fmt, ... ) GPUVIS_ATTR_PRINTF( 2, 3 );
GPUVIS_EXTERN int gpuvis_trace_duration_vprintf( unsigned int duration, const char *fmt, va_list ap ) GPUVIS_ATTR_PRINTF( 2, 0 );

GPUVIS_EXTERN int gpuvis_trace_begin_ctx_printf( unsigned int ctx, const char *fmt, ... ) GPUVIS_ATTR_PRINTF( 2, 3 );
GPUVIS_EXTERN int gpuvis_trace_begin_ctx_vprintf( unsigned int ctx, const char *fmt, va_list ap ) GPUVIS_ATTR_PRINTF( 2, 0 );

GPUVIS_EXTERN int gpuvis_trace_end_ctx_printf( unsigned int ctx, const char *fmt, ... ) GPUVIS_ATTR_PRINTF( 2, 3 );
GPUVIS_EXTERN int gpuvis_trace_end_ctx_vprintf( unsigned int ctx, const char *fmt, va_list ap ) GPUVIS_ATTR_PRINTF( 2, 0 );

GPUVIS_EXTERN int gpuvis_start_tracing();
GPUVIS_EXTERN int gpuvis_trigger_capture_and_keep_tracing();
GPUVIS_EXTERN int gpuvis_stop_tracing();

GPUVIS_EXTERN const char *gpuvis_get_tracefs_dir();
GPUVIS_EXTERN const char *gpuvis_get_trace_marker_path();

//////////////////////////////////////////////////////////////////////////////
//
//     IMPLEMENTATION SECTION
//

#endif // !GPUVIS_TRACE_UTILS_NOP

#ifdef GPUVIS_IMPLEMENTATION

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/vfs.h>
#include <linux/magic.h>

#ifndef TRACEFS_MAGIC
#define TRACEFS_MAGIC      0x74726163
#endif

// From kernel/trace/trace.h
#ifndef TRACE_BUF_SIZE
#define TRACE_BUF_SIZE     1024
#endif

#define GPUVIS_STR( x ) #x
#define GPUVIS_STR_VALUE( x ) GPUVIS_STR( x )

static int trace_fd = -1;
static const char *trace_err = NULL;

GPUVIS_EXTERN int gpuvis_trace_init()
{
    if ( ( trace_fd < 0 ) && !trace_err )
    {
        const char *trace_marker_path = gpuvis_get_trace_marker_path();

        if ( !trace_marker_path[ 0 ] )
        {
            trace_err = strerror( EACCES );
#if defined( DEBUG ) || defined( _DEBUG )
            printf( "[ERROR] gpuvis_get_trace_marker_path() failed: %s\n", trace_err );
#endif
        }
        else
        {
            trace_fd = open( trace_marker_path, O_WRONLY );
            if ( trace_fd < 0 )
            {
                trace_err = strerror( errno );
#if defined( DEBUG ) || defined( _DEBUG )
                printf( "[ERROR] %s open( %s ) failed: %s\n", __func__, trace_marker_path, trace_err );
#endif
            }
        }
    }

    return trace_fd;
}

GPUVIS_EXTERN const char *gpuvis_trace_errstr()
{
    return trace_err;
}

GPUVIS_EXTERN void gpuvis_trace_shutdown()
{
    if ( trace_fd >= 0 )
    {
        close( trace_fd );
        trace_fd = -1;
    }

    trace_err = NULL;
}

static int trace_printf_impl( const char *key, unsigned int val, const char *fmt, va_list ap ) GPUVIS_ATTR_PRINTF( 3, 0 );
static int trace_printf_impl( const char *key, unsigned int val, const char *fmt, va_list ap )
{
    int ret = -1;

    if ( gpuvis_trace_init() >= 0 )
    {
        int n;
        char buf[ TRACE_BUF_SIZE ];

        n = vsnprintf( buf, sizeof( buf ), fmt, ap );

        if ( ( n > 0 ) || ( !n && key ) )
        {
            if ( ( size_t )n >= sizeof( buf ) )
                n = sizeof( buf ) - 1;

            if ( key )
            {
                int keystrlen;
                char keystr[ 128 ];

                keystrlen = snprintf( keystr, sizeof( keystr ), " (%s=%u)", key, val );

                if ( keystrlen > 0 )
                {
                    if ( ( size_t )n + keystrlen >= sizeof( buf ) )
                        n = sizeof( buf ) - keystrlen - 1;

                    strcpy( buf + n, keystr );

                    n += keystrlen;
                }
            }

            ret = write( trace_fd, buf, n );
        }
    }

    return ret;
}

GPUVIS_EXTERN int gpuvis_trace_printf( const char *fmt, ... )
{
    int ret;
    va_list ap;

    va_start( ap, fmt );
    ret = gpuvis_trace_vprintf( fmt, ap );
    va_end( ap );

    return ret;
}

GPUVIS_EXTERN int gpuvis_trace_vprintf( const char *fmt, va_list ap )
{
    return trace_printf_impl( NULL, 0, fmt, ap );
}

GPUVIS_EXTERN int gpuvis_trace_duration_printf( unsigned int duration, const char *fmt, ... )
{
    int ret;
    va_list ap;

    va_start( ap, fmt );
    ret = gpuvis_trace_duration_vprintf( duration, fmt, ap );
    va_end( ap );

    return ret;
}

GPUVIS_EXTERN int gpuvis_trace_duration_vprintf( unsigned int duration, const char *fmt, va_list ap )
{
    return trace_printf_impl( "duration", duration, fmt, ap );
}

GPUVIS_EXTERN int gpuvis_trace_begin_ctx_printf( unsigned int ctx, const char *fmt, ... )
{
    int ret;
    va_list ap;

    va_start( ap, fmt );
    ret = gpuvis_trace_begin_ctx_vprintf( ctx, fmt, ap );
    va_end( ap );

    return ret;
}

GPUVIS_EXTERN int gpuvis_trace_begin_ctx_vprintf( unsigned int ctx, const char *fmt, va_list ap )
{
    return trace_printf_impl( "begin_ctx", ctx, fmt, ap );
}

GPUVIS_EXTERN int gpuvis_trace_end_ctx_printf( unsigned int ctx, const char *fmt, ... )
{
    int ret;
    va_list ap;

    va_start( ap, fmt );
    ret = gpuvis_trace_end_ctx_vprintf( ctx, fmt, ap );
    va_end( ap );

    return ret;
}

GPUVIS_EXTERN int gpuvis_trace_end_ctx_vprintf( unsigned int ctx, const char *fmt, va_list ap )
{
    return trace_printf_impl( "end_ctx", ctx, fmt, ap );
}

GPUVIS_EXTERN int gpuvis_start_tracing()
{
    //$ TODO
    // system( ESCAPE_STEAM_RUNTIME "kill -10 `" ESCAPE_STEAM_RUNTIME "pgrep -f gpu-trace`" );
    // sprintf( buf, ESCAPE_STEAM_RUNTIME "./gpu-trace -g -r -s 10 -p %d &", getpid() );
    return 0;
}

GPUVIS_EXTERN int gpuvis_trigger_capture_and_keep_tracing()
{
    //$ TODO
    return 0;
}

GPUVIS_EXTERN int gpuvis_stop_tracing()
{
    //$ TODO
    return 0;
}

static int is_tracefs_dir( const char *dir )
{
    struct statfs stat;

    return !statfs( dir, &stat ) && ( stat.f_type == TRACEFS_MAGIC );
}

GPUVIS_EXTERN const char *gpuvis_get_tracefs_dir()
{
    static int inited = 0;
    static char tracefs_dir[ PATH_MAX ];

    if ( !inited )
    {
        size_t i;
        static const char *tracefs_dirs[] =
        {
            "/sys/kernel/tracing",
            "/sys/kernel/debug/tracing",
            "/tracing",
            "/trace",
        };

        for ( i = 0; i < sizeof( tracefs_dirs ) / sizeof( tracefs_dirs[ 0 ] ); i++ )
        {
            if ( is_tracefs_dir( tracefs_dirs[ i ] ) )
            {
                strncpy( tracefs_dir, tracefs_dirs[ i ], PATH_MAX );
                tracefs_dir[ PATH_MAX - 1 ] = 0;
                break;
            }
        }

        if ( !tracefs_dir[ 0 ] )
        {
            FILE *fp;
            char type[ 128 ];
            char dir[ PATH_MAX + 1 ];

            fp = fopen( "/proc/mounts", "r" );
            if ( fp )
            {
                while ( fscanf( fp, "%*s %" GPUVIS_STR_VALUE( PATH_MAX ) "s %127s %*s %*d %*d\n", dir, type ) == 2 )
                {
                    if ( !strcmp( type, "tracefs" ) && is_tracefs_dir( dir ) )
                    {
                        strncpy( tracefs_dir, dir, PATH_MAX );
                        tracefs_dir[ PATH_MAX - 1 ] = 0;
                        break;
                    }
                }

                fclose( fp );
            }
        }

        inited = 1;
    }

    return tracefs_dir;
}

GPUVIS_EXTERN const char *gpuvis_get_trace_marker_path()
{
    static int inited = 0;
    static char tracefs_marker_path[ PATH_MAX ];

    if ( !inited )
    {
        const char *tracefs_dir = gpuvis_get_tracefs_dir();

        if ( tracefs_dir[ 0 ] )
        {
            // The "trace_marker" file allows userspace to write into the ftrace buffer.
            snprintf( tracefs_marker_path, PATH_MAX, "%s/trace_marker", tracefs_dir );
            tracefs_marker_path[ PATH_MAX - 1 ] = 0;
        }

        inited = 1;
    }

    return tracefs_marker_path;
}

#endif // GPUVIS_IMPLEMENTATION

#endif // _GPUVIS_TRACE_UTILS_H_
