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

#ifndef GPUVIS_TRACE_UTILS_NOP

#ifdef __cplusplus
  #define GPUVIS_EXTERN   extern "C"
#else
  #define GPUVIS_EXTERN   extern
#endif

// Try to open tracefs trace_marker file for writing. Returns -1 on error.
GPUVIS_EXTERN int gpuvis_trace_init();
// Close tracefs trace_marker file.
GPUVIS_EXTERN void gpuvis_trace_shutdown();

GPUVIS_EXTERN int gpuvis_trace_printf( const char *fmt, ... ) GPUVIS_ATTR_PRINTF( 1, 2 );
GPUVIS_EXTERN int gpuvis_trace_vprintf( const char *fmt, va_list ap ) GPUVIS_ATTR_PRINTF( 1, 0 );

GPUVIS_EXTERN int gpuvis_trace_duration_printf( unsigned int duration, const char *fmt, ... ) GPUVIS_ATTR_PRINTF( 2, 3 );
GPUVIS_EXTERN int gpuvis_trace_duration_vprintf( unsigned int duration, const char *fmt, va_list ap ) GPUVIS_ATTR_PRINTF( 2, 0 );

GPUVIS_EXTERN int gpuvis_trace_begin_ctx_printf( unsigned int ctx, const char *fmt, ... ) GPUVIS_ATTR_PRINTF( 2, 3 );
GPUVIS_EXTERN int gpuvis_trace_begin_ctx_vprintf( unsigned int ctx, const char *fmt, va_list ap ) GPUVIS_ATTR_PRINTF( 2, 0 );

GPUVIS_EXTERN int gpuvis_trace_end_ctx_printf( unsigned int ctx, const char *fmt, ... ) GPUVIS_ATTR_PRINTF( 2, 3 );
GPUVIS_EXTERN int gpuvis_trace_end_ctx_vprintf( unsigned int ctx, const char *fmt, va_list ap ) GPUVIS_ATTR_PRINTF( 2, 0 );

// Execute "trace-cmd start -b 2000 -D -i -e sched:sched_switch -e ..."
GPUVIS_EXTERN int gpuvis_start_tracing();
// Execute "trace-cmd extract"
GPUVIS_EXTERN int gpuvis_trigger_capture_and_keep_tracing();
// Execute "trace-cmd reset"
GPUVIS_EXTERN int gpuvis_stop_tracing();

// -1: tracing not setup, 0: tracing disabled, 1: tracing enabled.
GPUVIS_EXTERN int gpuvis_tracing_on();

// Get tracefs directory. Ie: /sys/kernel/tracing. Returns "" on error.
GPUVIS_EXTERN const char *gpuvis_get_tracefs_dir();

// Get tracefs file path in buf. Ie: /sys/kernel/tracing/trace_marker. Returns NULL on error.
GPUVIS_EXTERN const char *gpuvis_get_tracefs_filename( char *buf, size_t buflen, const char *file );

#else

#undef GPUVIS_IMPLEMENTATION

static inline int gpuvis_trace_init() { return 0; }
static inline void gpuvis_trace_shutdown() {}

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

static inline int gpuvis_tracing_on() { return -1; }

static inline int gpuvis_get_tracefs_dir() { return 0; }
static inline const char *gpuvis_get_tracefs_filename( char *buf, size_t buflen, const char *file ) { return NULL; }

#endif // !GPUVIS_TRACE_UTILS_NOP

#ifdef GPUVIS_IMPLEMENTATION

//////////////////////////////////////////////////////////////////////////////
//
//     IMPLEMENTATION SECTION
//

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

static int exec_tracecmd( const char *cmd )
{
    int ret;

    FILE *fh = popen( cmd, "r" );
    if ( !fh )
    {
        //$ TODO: popen() failed: errno
        ret = -1;
    }
    else
    {
        char buf[ 8192 ];

        while ( fgets( buf, sizeof( buf ), fh ) )
        {
            //$ TODO
            printf( "%s: %s", __func__, buf );
        }

        if ( feof( fh ) )
        {
            int pclose_ret = pclose( fh );

            ret = WEXITSTATUS( pclose_ret );
        }
        else
        {
            //$ TODO: Failed to read pipe to end: errno
            pclose( fh );
            ret = -1;
        }
    }

    return ret;
}

GPUVIS_EXTERN int gpuvis_trace_init()
{
    if ( ( trace_fd < 0 ) && !trace_err )
    {
        char filename[ PATH_MAX ];

        // The "trace_marker" file allows userspace to write into the ftrace buffer.
        if ( !gpuvis_get_tracefs_filename( filename, sizeof( filename ), "trace_marker" ) )
        {
            trace_err = strerror( EACCES );
        }
        else
        {
            trace_fd = open( filename, O_WRONLY );
            if ( trace_fd < 0 )
                trace_err = strerror( errno );
        }
    }

    return trace_fd;
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
    const char cmd[] =
            "trace-cmd start -b 2000 -D -i "
            // https://github.com/mikesart/gpuvis/wiki/TechDocs-Linux-Scheduler
            " -e sched:sched_switch"
            " -e sched:sched_process_fork"
            " -e sched:sched_process_exec"
            " -e sched:sched_process_exit"
            " -e drm:drm_vblank_event"
            " -e drm:drm_vblank_event_queued"
            // https://github.com/mikesart/gpuvis/wiki/TechDocs-AMDGpu
            " -e amdgpu:amdgpu_vm_flush"
            " -e amdgpu:amdgpu_cs_ioctl"
            " -e amdgpu:amdgpu_sched_run_job"
            " -e *fence:*fence_signaled"
            // https://github.com/mikesart/gpuvis/wiki/TechDocs-Intel
            " -e i915:i915_flip_request"
            " -e i915:i915_flip_complete"
            " -e i915:intel_gpu_freq_change"
            " -e i915:i915_gem_request_add"
            " -e i915:i915_gem_request_submit"  // Require CONFIG_DRM_I915_LOW_LEVEL_TRACEPOINTS
            " -e i915:i915_gem_request_in"      // Kconfig option to be enabled.
            " -e i915:i915_gem_request_out"     //
            " -e i915:intel_engine_notify"
            " -e i915:i915_gem_request_wait_begin"
            " -e i915:i915_gem_request_wait_end 2>&1";

    return exec_tracecmd( cmd );
}

GPUVIS_EXTERN int gpuvis_trigger_capture_and_keep_tracing()
{
    if ( gpuvis_tracing_on() )
    {
        char datetime[ 128 ];
        char cmd[ PATH_MAX ];
        time_t t = time( NULL );
        struct tm *tmp = localtime( &t );

        strftime( datetime, sizeof( datetime ), "%Y-%m-%d_%H-%M-%S", tmp );
        datetime[ sizeof( datetime ) - 1 ] = 0;

        snprintf( cmd, sizeof( cmd ), "trace-cmd extract -o \"trace_%s.dat\" 2>&1", datetime );
        cmd[ sizeof( cmd ) - 1 ] = 0;

        printf( "cmd: %s\n", cmd );
        return exec_tracecmd( cmd );
    }

    return -1;
}

GPUVIS_EXTERN int gpuvis_stop_tracing()
{
    const char cmd[] = "trace-cmd reset 2>&1";

    return exec_tracecmd( cmd );
}

GPUVIS_EXTERN int gpuvis_tracing_on()
{
    int ret = -1;
    char buf[ 32 ];
    char filename[ PATH_MAX ];

    if ( gpuvis_get_tracefs_filename( filename, PATH_MAX, "tracing_on" ) )
    {
        int fd = open( filename, O_RDONLY );

        if ( fd >= 0 )
        {
            if ( read( fd, buf, sizeof( buf ) ) > 0 )
                ret = atoi( buf );

            close( fd );
        }
    }

    return ret;
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

GPUVIS_EXTERN const char *gpuvis_get_tracefs_filename( char *buf, size_t buflen, const char *file )
{
    const char *tracefs_dir = gpuvis_get_tracefs_dir();

    if ( tracefs_dir[ 0 ] )
    {
        snprintf( buf, buflen, "%s/%s", tracefs_dir, file );
        buf[ buflen - 1 ] = 0;

        return buf;
    }

    return NULL;
}

#endif // GPUVIS_IMPLEMENTATION

#endif // _GPUVIS_TRACE_UTILS_H_
