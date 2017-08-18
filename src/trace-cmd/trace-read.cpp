/*
 * Copyright (C) 2009, 2010 Red Hat Inc, Steven Rostedt <srostedt@redhat.com>
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License (not later!)
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not,  see <http://www.gnu.org/licenses>
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 */
#define _LARGEFILE64_SOURCE
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <string>
#include <vector>
#include <forward_list>
#include <csetjmp>
#include <unordered_map>
#include <future>
#include <set>

#ifdef WIN32
#include <io.h>

#define open _open
#define close _close
#define read _read
#define lseek64 _lseeki64
#define dup _dup
#else
#define USE_MMAP

#include <sys/mman.h>
#include <sys/param.h>
#include <unistd.h>
#endif

extern "C"
{
    #include "event-parse.h"
    #include "kbuffer.h"
}

#include "../gpuvis_macros.h"
#include "trace-read.h"

enum
{
    TRACECMD_OPTION_DONE,
    TRACECMD_OPTION_DATE,
    TRACECMD_OPTION_CPUSTAT,
    TRACECMD_OPTION_BUFFER,
    TRACECMD_OPTION_TRACECLOCK,
    TRACECMD_OPTION_UNAME,
    TRACECMD_OPTION_HOOK,
    TRACECMD_OPTION_OFFSET,
    TRACECMD_OPTION_TGID,
};

enum
{
    TRACECMD_FL_BUFFER_INSTANCE = ( 1 << 1 ),
    TRACECMD_FL_LATENCY = ( 1 << 2 ),
};

typedef struct tracecmd_input tracecmd_input_t;
typedef struct pevent pevent_t;
typedef struct pevent_record pevent_record_t;
typedef struct kbuffer kbuffer_t;
typedef struct event_format event_format_t;

typedef struct file_info
{
    int done;
    tracecmd_input_t *handle;
    pevent_record_t *record;
} file_info_t;

typedef struct page
{
    off64_t offset;
    tracecmd_input_t *handle;
    void *map;
    int ref_count;
} page_t;

typedef struct cpu_data
{
    /* the first two never change */
    unsigned long long file_offset = 0;
    unsigned long long file_size = 0;
    unsigned long long offset = 0;
    unsigned long long size = 0;
    unsigned long long timestamp = 0;

    std::forward_list< page_t * > pages;
    pevent_record_t *next_record = nullptr;
    page_t *page = nullptr;
    kbuffer_t *kbuf = nullptr;
} cpu_data_t;

typedef struct input_buffer_instance
{
    char *name;
    size_t offset;
} input_buffer_instance_t;

typedef struct tracecmd_input
{
    pevent_t *pevent = nullptr;
    tracecmd_input_t *parent = nullptr;
    unsigned long flags = 0;
    int fd = -1;
    int long_size = 0;
    unsigned long page_size = 0;
    int cpus = 0;
    int ref = 0;
    int nr_buffers = 0; /* buffer instances */
    bool use_trace_clock = false;
#ifdef USE_MMAP
    bool read_page = false;
#endif
    cpu_data_t *cpu_data = nullptr;
    unsigned long long ts_offset = 0;
    input_buffer_instance_t *buffers = nullptr;

    std::string file;
    std::string uname;
    std::vector< std::string > cpustats;

    /* file information */
    size_t header_files_start = 0;
    size_t ftrace_files_start = 0;
    size_t event_files_start = 0;
    size_t total_file_size = 0;

    std::jmp_buf jump_buffer;
} tracecmd_input_t;

void logf( const char *fmt, ... ) ATTRIBUTE_PRINTF( 1, 2 );

[[noreturn]] static void die( tracecmd_input_t *handle, const char *fmt, ... ) ATTRIBUTE_PRINTF( 2, 3 );
[[noreturn]] static void die( tracecmd_input_t *handle, const char *fmt, ... )
{
    int ret;
    va_list ap;
    char *buf = NULL;

    va_start( ap, fmt );
    ret = vasprintf( &buf, fmt, ap );
    va_end( ap );

    if ( ret >= 0 )
    {
        logf( "%s", buf );
        free( buf );
    }

    std::longjmp( handle->jump_buffer, -1 );
}

static void *trace_malloc( tracecmd_input_t *handle, size_t size )
{
    void *ret = malloc( size );

    if ( !ret && handle )
        die( handle, "%s(%lu) failed\n", __func__, size );
    return ret;
}

static void *trace_realloc( tracecmd_input_t *handle, void *ptr, size_t size )
{
    void *ret = realloc( ptr, size );

    if ( !ret && handle )
        die( handle, "%s(%lu) failed\n", __func__, size );
    return ret;
}

static size_t do_read( tracecmd_input_t *handle, void *data, size_t size )
{
    ssize_t ret;

    ret = TEMP_FAILURE_RETRY( read( handle->fd, data, size ) );
    if ( ret < 0 )
    {
        die( handle, "%s(\"%s\") failed: %s (%d)\n", __func__, handle->file.c_str(),
             strerror( errno ), errno );
    }

    return ret;
}

static void do_read_check( tracecmd_input_t *handle, void *data, size_t size )
{
    size_t ret;

    ret = do_read( handle, data, size );
    if ( ret != size )
    {
        die( handle, "%s(\"%s\") failed: %s (%d)\n", __func__, handle->file.c_str(),
             strerror( errno ), errno );
    }
}

static char *read_string( tracecmd_input_t *handle )
{
    char *str = NULL;
    char buf[ BUFSIZ ];
    unsigned int i, r;
    unsigned int size = 0;

    for ( ;; )
    {
        r = do_read( handle, buf, BUFSIZ );
        if ( r <= 0 )
            goto fail;

        for ( i = 0; i < r; i++ )
        {
            if ( !buf[ i ] )
                break;
        }
        if ( i < r )
            break;

        if ( str )
        {
            size += BUFSIZ;
            str = ( char * )trace_realloc( handle, str, size );
            memcpy( str + ( size - BUFSIZ ), buf, BUFSIZ );
        }
        else
        {
            size = BUFSIZ;
            str = ( char * )trace_malloc( handle, size );
            memcpy( str, buf, size );
        }
    }

    /* move the file descriptor to the end of the string */
    off64_t val;
    val = lseek64( handle->fd, -( int )( r - ( i + 1 ) ), SEEK_CUR );
    if ( val < 0 )
        goto fail;

    if ( str )
    {
        size += i + 1;
        str = ( char * )trace_realloc( handle, str, size );
        memcpy( str + ( size - i ), buf, i );
        str[ size ] = 0;
    }
    else
    {
        size = i + 1;
        str = ( char * )trace_malloc( handle, size );
        memcpy( str, buf, i );
        str[ i ] = 0;
    }

    return str;

fail:
    if ( str )
        free( str );
    return NULL;
}

static unsigned int read4( tracecmd_input_t *handle )
{
    unsigned int data;
    pevent_t *pevent = handle->pevent;

    do_read_check( handle, &data, 4 );

    return __data2host4( pevent, data );
}

static unsigned long long read8( tracecmd_input_t *handle )
{
    unsigned long long data;
    pevent_t *pevent = handle->pevent;

    do_read_check( handle, &data, 8 );

    return __data2host8( pevent, data );
}

static void read_data_and_size( tracecmd_input_t *handle,
                               char **data, unsigned long long *size )
{
    *size = read8( handle );
    *data = ( char * )trace_malloc( handle, *size + 1 );

    do_read_check( handle, *data, *size );
}

static void read_header_files( tracecmd_input_t *handle )
{
    pevent_t *pevent = handle->pevent;
    long long size;
    char *header;
    char buf[ 64 ];

    do_read_check( handle, buf, 12 );

    if ( memcmp( buf, "header_page", 12 ) != 0 )
        die( handle, "%s: header_page not found.\n", __func__ );

    size = read8( handle );

    header = ( char * )trace_malloc( handle, size );

    do_read_check( handle, header, size );

    pevent_parse_header_page( pevent, header, size, handle->long_size );
    free( header );

    /*
	 * The size field in the page is of type long,
	 * use that instead, since it represents the kernel.
	 */
    handle->long_size = pevent->header_page_size_size;

    do_read_check( handle, buf, 13 );

    if ( memcmp( buf, "header_event", 13 ) != 0 )
        die( handle, "%s: header_event not found.\n", __func__ );

    size = read8( handle );

    header = ( char * )trace_malloc( handle, size );

    do_read_check( handle, header, size );

    free( header );

    handle->ftrace_files_start = lseek64( handle->fd, 0, SEEK_CUR );
}

static void read_ftrace_file( tracecmd_input_t *handle,
                             unsigned long long size )
{
    char *buf;
    pevent_t *pevent = handle->pevent;

    buf = ( char * )trace_malloc( handle, size );

    do_read_check( handle, buf, size );

    if ( pevent_parse_event( pevent, buf, size, "ftrace" ) )
        die( handle, "%s: pevent_parse_event failed.\n", __func__ );

    free( buf );
}

static void read_event_file( tracecmd_input_t *handle,
                            char *system, unsigned long long size )
{
    pevent_t *pevent = handle->pevent;
    char *buf;

    buf = ( char * )trace_malloc( handle, size );

    do_read_check( handle, buf, size );

    if ( pevent_parse_event( pevent, buf, size, system ) )
        die( handle, "%s: pevent_parse_event failed.\n", __func__ );

    free( buf );
}

static void read_ftrace_files( tracecmd_input_t *handle )
{
    unsigned int count;

    count = read4( handle );

    for ( unsigned int i = 0; i < count; i++ )
    {
        unsigned long long size;

        size = read8( handle );
        read_ftrace_file( handle, size );
    }

    handle->event_files_start = lseek64( handle->fd, 0, SEEK_CUR );
}

static void read_event_files( tracecmd_input_t *handle )
{
    unsigned int systems;

    systems = read4( handle );

    for ( unsigned int i = 0; i < systems; i++ )
    {
        char *system;
        unsigned int count;

        system = read_string( handle );
        if ( !system )
            die( handle, "%s: failed to read system string.\n", __func__ );

        count = read4( handle );

        for ( unsigned int x = 0; x < count; x++ )
        {
            unsigned long long size;

            size = read8( handle );
            read_event_file( handle, system, size );
        }

        free( system );
    }
}

static void parse_proc_kallsyms( pevent_t *pevent, char *file )
{
    char *line;
    char *next = NULL;

    line = strtok_r( file, "\n", &next );
    while ( line )
    {
        char *endptr;
        unsigned long long addr;

        // Parse lines of this form:
        //   addr             ch            func   mod
        //   ffffffffc07ec678 d descriptor.58652\t[bnep]

        addr = strtoull( line, &endptr, 16 );

        if ( endptr && *endptr == ' ' )
        {
            char ch;
            char *mod;
            char *func;

            line = endptr + 1;

            ch = *line;

            // Skip: x86-64 reports per-cpu variable offsets as absolute (A)
            if ( ch && ( ch != 'A' ) && ( line[ 1 ] == ' ' ) && line[ 2 ] )
            {
                func = line + 2;

                mod = strchr( func, '\t' );
                if ( mod && mod[ 1 ] == '[' )
                {
                    *mod = 0;
                    mod += 2;
                    mod[ strlen( mod ) - 1 ] = 0;
                }

                //$ TODO mikesart PERF: This is adding the item to a func_list
                // which gets converted to a sorted array afterwords.
                pevent_register_function( pevent, func, addr, mod );
            }
        }

        line = strtok_r( NULL, "\n", &next );
    }
}

static void read_proc_kallsyms( tracecmd_input_t *handle )
{
    char *buf;
    unsigned int size;
    pevent_t *pevent = handle->pevent;

    size = read4( handle );
    if ( !size  )
        return; /* OK? */

    buf = ( char * )trace_malloc( handle, size + 1 );

    do_read_check( handle, buf, size );

    buf[ size ] = 0;

    parse_proc_kallsyms( pevent, buf );

    free( buf );
}

static void parse_ftrace_printk( tracecmd_input_t *handle, pevent_t *pevent, char *file )
{
    char *line;
    char *next = NULL;

    line = strtok_r( file, "\n", &next );
    while ( line )
    {
        char *fmt;
        char *printk;
        char *addr_str;
        unsigned long long addr;

        addr_str = strtok_r( line, ":", &fmt );
        if ( !addr_str )
        {
            die( handle, "%s: printk format with empty entry.\n", __func__ );
            break;
        }

        addr = strtoull( addr_str, NULL, 16 );

        /* fmt still has a space, skip it */
        printk = strdup( fmt + 1 );
        line = strtok_r( NULL, "\n", &next );
        pevent_register_print_string( pevent, printk, addr );

        free( printk );
    }
}

static void read_ftrace_printk( tracecmd_input_t *handle )
{
    char *buf;
    unsigned int size;

    size = read4( handle );
    if ( !size )
        return; /* OK? */

    buf = ( char * )trace_malloc( handle, size + 1 );

    do_read_check( handle, buf, size );

    buf[ size ] = 0;

    parse_ftrace_printk( handle, handle->pevent, buf );

    free( buf );
}

static void parse_cmdlines( pevent_t *pevent, char *file )
{
    char *line;
    char *next = NULL;

    line = strtok_r( file, "\n", &next );
    while ( line )
    {
        int pid;
        char *comm;

        // Parse "PID CMDLINE"
        pid = strtoul( line, &comm, 10 );
        if ( comm && *comm == ' ' )
            pevent_register_comm( pevent, comm + 1, pid );

        line = strtok_r( NULL, "\n", &next );
    }
}

static void read_and_parse_cmdlines( tracecmd_input_t *handle )
{
    char *cmdlines;
    unsigned long long size;
    pevent_t *pevent = handle->pevent;

    read_data_and_size( handle, &cmdlines, &size );

    cmdlines[ size ] = 0;
    parse_cmdlines( pevent, cmdlines );
    free( cmdlines );
}

static void parse_trace_clock( pevent_t *pevent, char *file )
{
    // "[local] global counter uptime perf mono mono_raw x86-tsc\n"
    char *clock = strchr( file, '[' );

    if ( clock )
    {
        char *end = strchr( clock, ']' );

        if ( end )
        {
            *end = 0;
            pevent_register_trace_clock( pevent, clock + 1 );
        }
    }
}

/**
 * tracecmd_read_headers - read the header information from trace.dat
 * @handle: input handle for the trace.dat file
 *
 * This reads the trace.dat file for various information. Like the
 * format of the ring buffer, event formats, ftrace formats, kallsyms
 * and printk.
 */
static void tracecmd_read_headers( tracecmd_input_t *handle )
{
    read_header_files( handle );

    read_ftrace_files( handle );

    read_event_files( handle );

    read_proc_kallsyms( handle );

    read_ftrace_printk( handle );

    read_and_parse_cmdlines( handle );

    pevent_set_long_size( handle->pevent, handle->long_size );
}

static int read_page( tracecmd_input_t *handle, off64_t offset,
                      int cpu, void *map )
{
    off64_t ret;
    off64_t save_seek;

    /* other parts of the code may expect the pointer to not move */
    save_seek = lseek64( handle->fd, 0, SEEK_CUR );

    ret = lseek64( handle->fd, offset, SEEK_SET );
    if ( ret < 0 )
        return -1;

    ret = TEMP_FAILURE_RETRY( read( handle->fd, map, handle->page_size ) );
    if ( ret < 0 )
        return -1;

    /* reset the file pointer back */
    lseek64( handle->fd, save_seek, SEEK_SET );
    return 0;
}

static page_t *allocate_page( tracecmd_input_t *handle, int cpu, off64_t offset )
{
    int ret;
    cpu_data_t *cpu_data = &handle->cpu_data[ cpu ];

    for ( page_t *page : cpu_data->pages )
    {
        if ( page->offset == offset )
        {
            page->ref_count++;
            return page;
        }
    }

    page_t *page = ( page_t * )trace_malloc( handle, sizeof( *page ) );

    memset( page, 0, sizeof( *page ) );

    page->offset = offset;
    page->handle = handle;

#ifdef USE_MMAP
    if ( handle->read_page )
#endif
    {
        page->map = trace_malloc( handle, handle->page_size );

        ret = read_page( handle, offset, cpu, page->map );
        if ( ret < 0 )
        {
            free( page->map );
            page->map = NULL;
        }
    }
#ifdef USE_MMAP
    else
    {
        page->map = mmap( NULL, handle->page_size, PROT_READ, MAP_PRIVATE,
                          handle->fd, offset );
        if ( page->map == MAP_FAILED )
            page->map = NULL;
    }
#endif

    if ( !page->map )
    {
        free( page );
        return NULL;
    }

    cpu_data->pages.push_front( page );

    page->ref_count = 1;
    return page;
}

static void __free_page( tracecmd_input_t *handle, int cpu, page_t *page )
{
    if ( !page->ref_count )
        die( handle, "%s: Page ref count is zero.\n", __func__ );

    page->ref_count--;
    if ( page->ref_count )
        return;

#ifdef USE_MMAP
    if ( handle->read_page )
#endif
        free( page->map );
#ifdef USE_MMAP
    else
        munmap( page->map, handle->page_size );
#endif

    handle->cpu_data[ cpu ].pages.remove( page );

    free( page );
}

static void free_page( tracecmd_input_t *handle, int cpu )
{
    if ( !handle->cpu_data ||
         cpu >= handle->cpus ||
         !handle->cpu_data[ cpu ].page )
        return;

    __free_page( handle, cpu, handle->cpu_data[ cpu ].page );

    handle->cpu_data[ cpu ].page = NULL;
}

static void __free_record( pevent_record_t *record )
{
    if ( record->priv )
    {
        page_t *page = ( page_t * )record->priv;

        __free_page( page->handle, record->cpu, page );
    }

    free( record );
}

static void free_record( tracecmd_input_t *handle, pevent_record_t *record )
{
    if ( !record )
        return;

    if ( !record->ref_count )
        die( handle, "%s: record ref count is zero.\n", __func__ );

    record->ref_count--;
    if ( record->ref_count )
        return;

    if ( record->locked )
        die( handle, "%s: freeing record when it is locked.\n", __func__ );

    record->data = NULL;

    __free_record( record );
}

static void free_next( tracecmd_input_t *handle, int cpu )
{
    pevent_record_t *record;

    if ( !handle->cpu_data || cpu >= handle->cpus )
        return;

    record = handle->cpu_data[ cpu ].next_record;
    if ( !record )
        return;

    handle->cpu_data[ cpu ].next_record = NULL;

    record->locked = 0;

    free_record( handle, record );
}

/*
 * Page is mapped, now read in the page header info.
 */
static void update_page_info( tracecmd_input_t *handle, int cpu )
{
    pevent_t *pevent = handle->pevent;
    void *ptr = handle->cpu_data[ cpu ].page->map;
    kbuffer_t *kbuf = handle->cpu_data[ cpu ].kbuf;

    /* FIXME: handle header page */
    if ( pevent->header_page_ts_size != 8 )
        die( handle, "%s: expected a long long type for timestamp.\n", __func__ );

    kbuffer_load_subbuffer( kbuf, ptr );
    if ( ( unsigned long )kbuffer_subbuffer_size( kbuf ) > handle->page_size )
    {
        die( handle, "%s: bad page read, with size of %d\n", __func__,
                 kbuffer_subbuffer_size( kbuf ) );
    }

    handle->cpu_data[ cpu ].timestamp = kbuffer_timestamp( kbuf ) + handle->ts_offset;
}

/*
 * get_page maps a page for a given cpu.
 *
 * Returns 1 if the page was already mapped,
 *         0 if it mapped successfully
 *        -1 on error
 */
static int get_page( tracecmd_input_t *handle, int cpu,
                     unsigned long long offset )
{
    /* Don't map if the page is already where we want */
    if ( handle->cpu_data[ cpu ].offset == offset &&
         handle->cpu_data[ cpu ].page )
        return 1;

    /* Do not map no data for CPU */
    if ( !handle->cpu_data[ cpu ].size )
        return -1;

    if ( offset & ( handle->page_size - 1 ) )
        die( handle, "%s: bad page offset %llx\n", __func__, offset );

    if ( offset < handle->cpu_data[ cpu ].file_offset ||
         offset > handle->cpu_data[ cpu ].file_offset +
                      handle->cpu_data[ cpu ].file_size )
    {
        die( handle, "%s: bad page offset %llx\n", __func__, offset );
        return -1;
    }

    handle->cpu_data[ cpu ].offset = offset;
    handle->cpu_data[ cpu ].size = ( handle->cpu_data[ cpu ].file_offset +
                                     handle->cpu_data[ cpu ].file_size ) - offset;

    free_page( handle, cpu );

    handle->cpu_data[ cpu ].page = allocate_page( handle, cpu, offset );
    if ( !handle->cpu_data[ cpu ].page )
        return -1;

    update_page_info( handle, cpu );
    return 0;
}

static int get_next_page( tracecmd_input_t *handle, int cpu )
{
    unsigned long long offset;

    if ( !handle->cpu_data[ cpu ].page )
        return 0;

    free_page( handle, cpu );

    if ( handle->cpu_data[ cpu ].size <= handle->page_size )
    {
        handle->cpu_data[ cpu ].offset = 0;
        return 0;
    }

    offset = handle->cpu_data[ cpu ].offset + handle->page_size;

    return get_page( handle, cpu, offset );
}

/**
 * tracecmd_peek_data - return the record at the current location.
 * @handle: input handle for the trace.dat file
 * @cpu: the CPU to pull from
 *
 * This returns the record at the current location of the CPU
 * iterator. It does not increment the CPU iterator.
 */
static pevent_record_t *tracecmd_peek_data( tracecmd_input_t *handle, int cpu )
{
    pevent_record_t *record;
    unsigned long long ts;
    kbuffer_t *kbuf;
    page_t *page;
    int index;
    void *data;

    if ( cpu >= handle->cpus )
        return NULL;

    page = handle->cpu_data[ cpu ].page;
    kbuf = handle->cpu_data[ cpu ].kbuf;

    if ( handle->cpu_data[ cpu ].next_record )
    {
        record = handle->cpu_data[ cpu ].next_record;
        if ( !record->data )
            die( handle, "%s: Something freed the record.\n", __func__ );

        if ( handle->cpu_data[ cpu ].timestamp == record->ts )
            return record;

        /*
		 * The timestamp changed, which means the cached
		 * record is no longer valid. Reread a new record.
		 */
        free_next( handle, cpu );
    }

read_again:
    if ( !page )
        return NULL;

    data = kbuffer_read_event( kbuf, &ts );
    if ( !data )
    {
        if ( get_next_page( handle, cpu ) )
            return NULL;

        page = handle->cpu_data[ cpu ].page;
        goto read_again;
    }

    handle->cpu_data[ cpu ].timestamp = ts + handle->ts_offset;

    index = kbuffer_curr_offset( kbuf );

    record = ( pevent_record_t * )trace_malloc( handle, sizeof( *record ) );

    memset( record, 0, sizeof( *record ) );

    record->ts = handle->cpu_data[ cpu ].timestamp;
    record->size = kbuffer_event_size( kbuf );
    record->cpu = cpu;
    record->data = data;
    record->offset = handle->cpu_data[ cpu ].offset + index;
    record->ref_count = 1;
    record->locked = 1;

    handle->cpu_data[ cpu ].next_record = record;

    record->record_size = kbuffer_curr_size( kbuf );
    record->priv = page;
    page->ref_count++;

    kbuffer_next_event( kbuf, NULL );

    return record;
}

/**
 * tracecmd_read_data - read the next record and increment
 * @handle: input handle for the trace.dat file
 * @cpu: the CPU to pull from
 *
 * This returns the record at the current location of the CPU
 * iterator and increments the CPU iterator.
 *
 * The record returned must be freed.
 */
static pevent_record_t *tracecmd_read_data( tracecmd_input_t *handle, int cpu )
{
    pevent_record_t *record;

    record = tracecmd_peek_data( handle, cpu );

    handle->cpu_data[ cpu ].next_record = NULL;

    if ( record )
        record->locked = 0;

    return record;
}

/**
 * tracecmd_peek_next_data - return the next record
 * @handle: input handle to the trace.dat file
 * @rec_cpu: return pointer to the CPU that the record belongs to
 *
 * This returns the next record by time. This is different than
 * tracecmd_peek_data in that it looks at all CPUs. It does a peek
 * at each CPU and the record with the earliest time stame is
 * returned. If @rec_cpu is not NULL it gets the CPU id the record was
 * on. It does not increment the CPU iterator.
 */
static pevent_record_t *tracecmd_peek_next_data( tracecmd_input_t *handle, int *rec_cpu )
{
    int cpu;
    int next_cpu;
    unsigned long long ts;
    pevent_record_t *record, *next_record = NULL;

    if ( rec_cpu )
        *rec_cpu = -1;

    next_cpu = -1;
    ts = 0;

    for ( cpu = 0; cpu < handle->cpus; cpu++ )
    {
        record = tracecmd_peek_data( handle, cpu );
        if ( record && ( !next_record || record->ts < ts ) )
        {
            ts = record->ts;
            next_cpu = cpu;
            next_record = record;
        }
    }

    if ( next_record )
    {
        if ( rec_cpu )
            *rec_cpu = next_cpu;
        return next_record;
    }

    return NULL;
}

/**
 * tracecmd_read_next_data - read the next record
 * @handle: input handle to the trace.dat file
 * @rec_cpu: return pointer to the CPU that the record belongs to
 *
 * This returns the next record by time. This is different than
 * tracecmd_read_data in that it looks at all CPUs. It does a peek
 * at each CPU and the record with the earliest time stame is
 * returned. If @rec_cpu is not NULL it gets the CPU id the record was
 * on. The CPU cursor of the returned record is moved to the
 * next record.
 *
 * Multiple reads of this function will return a serialized list
 * of all records for all CPUs in order of time stamp.
 *
 * The record returned must be freed.
 */
static pevent_record_t *tracecmd_read_next_data( tracecmd_input_t *handle, int *rec_cpu )
{
    int next_cpu;
    pevent_record_t *record;

    record = tracecmd_peek_next_data( handle, &next_cpu );
    if ( !record )
        return NULL;

    if ( rec_cpu )
        *rec_cpu = next_cpu;

    return tracecmd_read_data( handle, next_cpu );
}

static int init_cpu( tracecmd_input_t *handle, int cpu )
{
    cpu_data_t *cpu_data = &handle->cpu_data[ cpu ];

    cpu_data->offset = cpu_data->file_offset;
    cpu_data->size = cpu_data->file_size;
    cpu_data->timestamp = 0;

    if ( !cpu_data->size )
    {
        //$ TODO: printf( "CPU %d is empty.\n", cpu );
        return 0;
    }

    cpu_data->page = allocate_page( handle, cpu, cpu_data->offset );
#ifdef USE_MMAP
    if ( !cpu_data->page && !handle->read_page )
    {
        //$ TODO mikesart: This just should never happen, yes?
        die( handle, "%s: Can't mmap file, will read instead.\n", __func__ );

        if ( cpu )
        {
            /*
			 * If the other CPUs had size and was able to mmap
			 * then bail.
			 */
            for ( int i = 0; i < cpu; i++ )
            {
                if ( handle->cpu_data[ i ].size )
                    return -1;
            }
        }

        /* try again without mmapping, just read it directly */
        handle->read_page = true;
        cpu_data->page = allocate_page( handle, cpu, cpu_data->offset );
        if ( !cpu_data->page )
            /* Still no luck, bail! */
            return -1;
    }
#endif

    update_page_info( handle, cpu );
    return 0;
}

static void tracecmd_parse_tgids(struct pevent *pevent,
                                 char *file, int size __maybe_unused)
{
    char *next = NULL;
    int pid, tgid;
    char *endptr;
    char *line;

    line = strtok_r(file, "\n", &next);
    while (line) {
        pid = strtol(line, &endptr, 10);
        if (endptr && *endptr == ' ') {
            tgid = strtol(endptr + 1, NULL, 10);
            pevent_register_tgid(pevent, tgid, pid);
        }
        line = strtok_r(NULL, "\n", &next);
    }
}


static int handle_options( tracecmd_input_t *handle )
{
    for ( ;; )
    {
        char *buf;
        unsigned int size;
        unsigned short option;
        unsigned long long offset;

        do_read_check( handle, &option, 2 );

        if ( option == TRACECMD_OPTION_DONE )
            break;

        /* next 4 bytes is the size of the option */
        do_read_check( handle, &size, 4 );

        size = __data2host4( handle->pevent, size );
        buf = ( char * )trace_malloc( handle, size );

        do_read_check( handle, buf, size );

        switch ( option )
        {
        case TRACECMD_OPTION_DATE:
            /*
			 * A time has been mapped that is the
			 * difference between the timestamps and
			 * gtod. It is stored as ASCII with '0x'
			 * appended.
			 */
            offset = strtoll( buf, NULL, 0 );

            /* Convert from micro to nano */
            offset *= 1000;
            handle->ts_offset += offset;
            break;
        case TRACECMD_OPTION_OFFSET:
            /*
			 * Similar to date option, but just adds an
			 * offset to the timestamp.
			 */
            offset = strtoll( buf, NULL, 0 );
            handle->ts_offset += offset;
            break;
        case TRACECMD_OPTION_CPUSTAT:
            handle->cpustats.push_back( buf );
            break;
        case TRACECMD_OPTION_BUFFER:
        {
            input_buffer_instance_t *buffer;

            /* A buffer instance is saved at the end of the file */
            handle->nr_buffers++;
            handle->buffers = ( input_buffer_instance_t * )trace_realloc( handle, handle->buffers,
                                       sizeof( *handle->buffers ) * handle->nr_buffers );
            buffer = &handle->buffers[ handle->nr_buffers - 1 ];
            buffer->name = strdup( buf + 8 );
            if ( !buffer->name )
            {
                free( handle->buffers );
                handle->buffers = NULL;
                return -ENOMEM;
            }

            offset = *( unsigned long long * )buf;
            buffer->offset = __data2host8( handle->pevent, offset );
            break;
        }
        case TRACECMD_OPTION_TRACECLOCK:
            handle->use_trace_clock = true;
            break;
        case TRACECMD_OPTION_UNAME:
            handle->uname = buf;
            break;
        case TRACECMD_OPTION_HOOK:
            // Used by trace-cmd report --profile. We don't need it.
            //   hook = tracecmd_create_event_hook( buf );
            break;
        case TRACECMD_OPTION_TGID:
            tracecmd_parse_tgids(handle->pevent, buf, size);
            break;
        default:
            die( handle, "%s: unknown option %d\n", __func__, option );
            break;
        }

        free( buf );
    }

    return 0;
}

static void read_cpu_data( tracecmd_input_t *handle )
{
    char buf[ 10 ];
    enum kbuffer_endian endian;
    enum kbuffer_long_size long_size;

    do_read_check( handle, buf, 10 );

    // check if this handles options
    if ( strncmp( buf, "options", 7 ) == 0 )
    {
        if ( handle_options( handle ) < 0 )
            die( handle, "%s: handle_options failed.\n", __func__ );

        do_read_check( handle, buf, 10 );
    }

    // Check if this is a latency report or not.
    if ( strncmp( buf, "latency", 7 ) == 0 )
    {
        handle->flags |= TRACECMD_FL_LATENCY;
        return;
    }

    /* We expect this to be flyrecord */
    if ( strncmp( buf, "flyrecord", 9 ) != 0 )
        die( handle, "%s: flyrecord not found.\n", __func__ );

    handle->cpu_data = new ( std::nothrow ) cpu_data_t [ handle->cpus ];
    if ( !handle->cpu_data )
        die( handle, "%s: new cpu_data_t failed.\n", __func__ );

    long_size = ( handle->long_size == 8 ) ? KBUFFER_LSIZE_8 : KBUFFER_LSIZE_4;

    endian = handle->pevent->file_bigendian ?
                KBUFFER_ENDIAN_BIG : KBUFFER_ENDIAN_LITTLE;

    for ( int cpu = 0; cpu < handle->cpus; cpu++ )
    {
        unsigned long long size;
        unsigned long long offset;

        handle->cpu_data[ cpu ].kbuf = kbuffer_alloc( long_size, endian );
        if ( !handle->cpu_data[ cpu ].kbuf )
            die( handle, "%s: kbuffer_alloc failed.\n", __func__ );

        if ( handle->pevent->old_format )
            kbuffer_set_old_format( handle->cpu_data[ cpu ].kbuf );

        offset = read8( handle );
        size = read8( handle );

        handle->cpu_data[ cpu ].file_offset = offset;
        handle->cpu_data[ cpu ].file_size = size;

        if ( size && ( offset + size > handle->total_file_size ) )
        {
            /* this happens if the file got truncated */
            die( handle, "%s: File possibly truncated. "
                    "Need at least %llu, but file size is %zu.\n",
                    __func__, offset + size, handle->total_file_size );
        }

        if ( init_cpu( handle, cpu ) < 0 )
            die( handle, "%s: init_cpu failed.\n", __func__ );
    }
}

static int read_and_parse_trace_clock( tracecmd_input_t *handle,
                                       pevent_t *pevent )
{
    char *trace_clock;
    unsigned long long size;

    read_data_and_size( handle, &trace_clock, &size );

    trace_clock[ size ] = 0;
    parse_trace_clock( pevent, trace_clock );
    free( trace_clock );
    return 0;
}

/**
 * tracecmd_init_data - prepare reading the data from trace.dat
 * @handle: input handle for the trace.dat file
 *
 * This prepares reading the data from trace.dat. This is called
 * after tracecmd_read_headers() and before tracecmd_read_data().
 */
void tracecmd_init_data( tracecmd_input_t *handle )
{
    pevent_t *pevent = handle->pevent;

    handle->cpus = read4( handle );

    pevent_set_cpus( pevent, handle->cpus );

    read_cpu_data( handle );

    if ( handle->use_trace_clock )
    {
        /*
		 * There was a bug in the original setting of
		 * the trace_clock file which let it get
		 * corrupted. If it fails to read, force local
		 * clock.
		 */
        if ( read_and_parse_trace_clock( handle, pevent ) < 0 )
            pevent_register_trace_clock( pevent, "local" );
    }
}

static inline int tracecmd_host_bigendian( void )
{
    unsigned char str[] = { 0x1, 0x2, 0x3, 0x4 };
    unsigned int *ptr = ( unsigned int * )str;

    return *ptr == 0x01020304;
}

/**
 * tracecmd_alloc_fd - create a tracecmd_input handle from a file descriptor
 * @fd: the file descriptor for the trace.dat file
 *
 * Allocate a tracecmd_input handle from a file descriptor and open the
 * file. This tests if the file is of trace-cmd format and allocates
 * a parse event descriptor.
 *
 * The returned pointer is not ready to be read yet. A tracecmd_read_headers()
 * and tracecmd_init_data() still need to be called on the descriptor.
 */
static tracecmd_input_t *tracecmd_alloc_fd( const char *file, int fd )
{
    char buf[ 64 ];
    char *version;
    char test[] = { 23, 8, 68 };
    tracecmd_input_t *handle;

    handle = new ( std::nothrow ) tracecmd_input_t;
    if ( !handle )
        return NULL;

    handle->file = file;
    handle->fd = fd;
    handle->ref = 1;

    if ( setjmp( handle->jump_buffer ) )
    {
        logf( "%s: setjmp error code called for %s.\n", __func__, file );

        delete handle;
        close( fd );
        return NULL;
    }

    do_read_check( handle, buf, 3 );
    if ( memcmp( buf, test, 3 ) != 0 )
        die( handle, "%s: header memcheck failed.\n", __func__ );

    do_read_check( handle, buf, 7 );
    if ( memcmp( buf, "tracing", 7 ) != 0 )
        die( handle, "%s: failed to read tracing string.\n", __func__ );

    version = read_string( handle );
    if ( !version )
        die( handle, "%s: failed to read version string.\n", __func__ );

    free( version );

    do_read_check( handle, buf, 1 );

    handle->pevent = pevent_alloc();
    if ( !handle->pevent )
        die( handle, "%s: pevent_alloc failed.\n", __func__ );

    handle->pevent->file_bigendian = buf[ 0 ];
    handle->pevent->host_bigendian = tracecmd_host_bigendian();

    do_read_check( handle, buf, 1 );
    handle->long_size = buf[ 0 ];

    handle->page_size = read4( handle );

    handle->header_files_start = lseek64( handle->fd, 0, SEEK_CUR );
    handle->total_file_size = lseek64( handle->fd, 0, SEEK_END );
    handle->header_files_start = lseek64( handle->fd, handle->header_files_start, SEEK_SET );

    return handle;
}

/**
 * tracecmd_alloc_fd - create a tracecmd_input handle from a file name
 * @file: the file name of the file that is of tracecmd data type.
 *
 * Allocate a tracecmd_input handle from a given file name and open the
 * file. This tests if the file is of trace-cmd format and allocates
 * a parse event descriptor.
 *
 * The returned pointer is not ready to be read yet. A tracecmd_read_headers()
 * and tracecmd_init_data() still need to be called on the descriptor.
 */
static tracecmd_input_t *tracecmd_alloc( const char *file )
{
    int fd;

    fd = TEMP_FAILURE_RETRY( open( file, O_RDONLY ) );
    if ( fd < 0 )
    {
        logf( "%s: open(\"%s\") failed: %d\n", __func__, file, errno );
        return NULL;
    }

    return tracecmd_alloc_fd( file, fd );
}

/**
 * tracecmd_close - close and free the trace.dat handle
 * @handle: input handle for the trace.dat file
 *
 * Close the file descriptor of the handle and frees
 * the resources allocated by the handle.
 */
static void tracecmd_close( tracecmd_input_t *handle )
{
    int cpu;

    if ( !handle )
        return;

    if ( handle->ref <= 0 )
    {
        die( handle, "%s: bad ref count on handle.\n", __func__ );
        return;
    }

    if ( --handle->ref )
        return;

    for ( cpu = 0; cpu < handle->cpus; cpu++ )
    {
        /* The tracecmd_peek_data may have cached a record */
        free_next( handle, cpu );
        free_page( handle, cpu );

        if ( handle->cpu_data && handle->cpu_data[ cpu ].kbuf )
        {
            kbuffer_free( handle->cpu_data[ cpu ].kbuf );

            // if ( !list_empty( &handle->cpu_data[ cpu ].pages ) )
            if ( !handle->cpu_data[ cpu ].pages.empty() )
                die( handle, "%s: pages still allocated on cpu %d\n", __func__, cpu );
        }
    }

    close( handle->fd );

    delete [] handle->cpu_data;

    if ( handle->flags & TRACECMD_FL_BUFFER_INSTANCE )
    {
        tracecmd_close( handle->parent );
    }
    else
    {
        /* Only main handle frees pevent */
        pevent_free( handle->pevent );
    }

    delete handle;
}

static tracecmd_input_t *tracecmd_buffer_instance_handle( tracecmd_input_t *handle, int indx )
{
    tracecmd_input_t *new_handle;
    input_buffer_instance_t *buffer = &handle->buffers[ indx ];
    size_t offset;
    ssize_t ret;

    if ( indx >= handle->nr_buffers )
        return NULL;

    /*
	 * We make a copy of the current handle, but we substitute
	 * the cpu data with the cpu data for this buffer.
	 */
    new_handle = new tracecmd_input_t;

    *new_handle = *handle;
    new_handle->cpu_data = NULL;
    new_handle->nr_buffers = 0;
    new_handle->buffers = NULL;
    new_handle->ref = 1;
    new_handle->parent = handle;

    handle->ref++;

    new_handle->fd = dup( handle->fd );

    new_handle->flags |= TRACECMD_FL_BUFFER_INSTANCE;

    /* Save where we currently are */
    offset = lseek64( handle->fd, 0, SEEK_CUR );

    ret = lseek64( handle->fd, buffer->offset, SEEK_SET );
    if ( ret < 0 )
    {
        die( handle, "%s: could not seek to buffer %s offset %lu.\n",
                 __func__, buffer->name, buffer->offset );
    }

    read_cpu_data( new_handle );

    ret = lseek64( handle->fd, offset, SEEK_SET );
    if ( ret < 0 )
        die( handle, "%s: could not seek to back to offset %ld\n", __func__, offset );

    return new_handle;
}

static bool is_timestamp_in_us( char *trace_clock, bool use_trace_clock )
{
    if ( !use_trace_clock )
        return true;

    // trace_clock information:
    //  https://www.kernel.org/doc/Documentation/trace/ftrace.txt
    if ( !strcmp( trace_clock, "local" ) ||
         !strcmp( trace_clock, "global" ) ||
         !strcmp( trace_clock, "uptime" ) ||
         !strcmp( trace_clock, "perf" ) )
        return true;

    /* trace_clock is setting in tsc or counter mode */
    return false;
}

extern "C" void print_str_arg( struct trace_seq *s, void *data, int size,
              struct event_format *event, const char *format,
              int len_arg, struct print_arg *arg );

static void init_event_flags( trace_event_t &event )
{
    // Make sure our event type bits are cleared
    event.flags &= ~( TRACE_FLAG_FENCE_SIGNALED |
                      TRACE_FLAG_FTRACE_PRINT |
                      TRACE_FLAG_VBLANK |
                      TRACE_FLAG_TIMELINE |
                      TRACE_FLAG_SW_QUEUE |
                      TRACE_FLAG_HW_QUEUE |
                      TRACE_FLAG_SCHED_SWITCH |
                      TRACE_FLAG_SCHED_SWITCH_TASK_RUNNING |
                      TRACE_FLAG_AUTOGEN_COLOR );

    // fence_signaled was renamed to dma_fence_signaled post v4.9
    if ( strstr( event.name, "fence_signaled" ) )
        event.flags |= TRACE_FLAG_FENCE_SIGNALED;
    else if ( !strcmp( event.system, "ftrace-print" ) )
        event.flags |= TRACE_FLAG_FTRACE_PRINT;
    else if ( !strcmp( event.name, "drm_vblank_event" ) )
        event.flags |= TRACE_FLAG_VBLANK;
    else if ( strstr( event.name, "amdgpu_cs_ioctl" ) )
        event.flags |= TRACE_FLAG_SW_QUEUE;
    else if ( strstr( event.name, "amdgpu_sched_run_job" ) )
        event.flags |= TRACE_FLAG_HW_QUEUE;
    else if ( !strcmp( event.name, "sched_switch" ) )
        event.flags |= TRACE_FLAG_SCHED_SWITCH;
}

static int trace_enum_events( EventCallback &cb, StrPool &strpool, const trace_info_t &trace_info,
                             tracecmd_input_t *handle, pevent_record_t *record )
{
    int ret = 0;
    event_format_t *event;
    pevent_t *pevent = handle->pevent;

    event = pevent_find_event_by_record( pevent, record );
    if ( event )
    {
        struct trace_seq seq;
        trace_event_t trace_event;
        struct format_field *format;
        int pid = pevent_data_pid( pevent, record );
        const char *comm = pevent_data_comm_from_pid( pevent, pid );
        bool is_ftrace_function = !strcmp( "ftrace", event->system ) && !strcmp( "function", event->name );
        bool is_printk_function = !strcmp( "ftrace", event->system ) && !strcmp( "print", event->name );

        trace_seq_init( &seq );

        memset( &trace_event, 0, sizeof( trace_event ) );

        trace_event.id = 0;
        trace_event.pid = pid;
        trace_event.cpu = record->cpu;

        trace_seq_printf( &seq, "%s-%u", comm, pid );
        trace_event.comm = strpool.getstr( seq.buffer );

        trace_event.ts = record->ts;

        trace_event.system = strpool.getstr( event->system );
        trace_event.name = strpool.getstr( event->name );

        trace_event.timeline = "";
        trace_event.context = 0;
        trace_event.seqno = 0;
        trace_event.crtc = -1;
        trace_event.user_comm = trace_event.comm;
        trace_event.id_start = ( uint32_t )-1;
        trace_event.graph_row_id = 0;
        trace_event.duration = ( uint32_t )-1;
        trace_event.is_filtered_out = false;

        // Get count of fields for this event.
        int field_count = 0;
        format = event->format.fields;
        for ( ; format; format = format->next )
            field_count++;

        // Reserve space in our fields array.
        trace_event.fields.reserve( field_count );

        format = event->format.common_fields;
        for ( ; format; format = format->next )
        {
            if ( !strcmp( format->name, "common_flags" ) )
            {
                unsigned long long val = pevent_read_number( pevent,
                        ( char * )record->data + format->offset, format->size );

                // TRACE_FLAG_IRQS_OFF | TRACE_FLAG_HARDIRQ | TRACE_FLAG_SOFTIRQ
                trace_event.flags = val;
                break;
            }
        }

        format = event->format.fields;
        for ( ; format; format = format->next )
        {
            trace_seq_reset( &seq );
            pevent_print_field( &seq, record->data, format );

            if ( !strcmp( format->name, "timeline" ) )
            {
                trace_event.timeline = strpool.getstr( seq.buffer );
            }
            else if ( !strcmp( format->name, "context" ) )
            {
                unsigned long long val = pevent_read_number( pevent,
                        ( char * )record->data + format->offset, format->size );

                trace_event.context = val;
            }
            else if ( !strcmp( format->name, "seqno" ) )
            {
                unsigned long long val = pevent_read_number( pevent,
                        ( char * )record->data + format->offset, format->size );

                trace_event.seqno = val;
            }
            else if ( !strcmp( format->name, "crtc" ) )
            {
                unsigned long long val = pevent_read_number( pevent,
                        ( char * )record->data + format->offset, format->size );

                trace_event.crtc = val;
            }

            if ( is_ftrace_function )
            {
                bool is_ip = !strcmp( format->name, "ip" );

                if ( is_ip || !strcmp( format->name, "parent_ip" ) )
                {
                    unsigned long long val = pevent_read_number( pevent,
                        ( char * )record->data + format->offset, format->size );
                    const char *func = pevent_find_function( pevent, val );

                    if ( func )
                    {
                        trace_seq_printf( &seq, " (%s)", func );

                        if ( is_ip )
                        {
                            // If this is a ftrace:function event, set the name
                            //  to be the function name we just found.
                            trace_event.system = "ftrace-function";
                            trace_event.name = strpool.getstr( func );
                        }
                    }
                }
            }
            else if ( is_printk_function && !strcmp( format->name, "buf" ) )
            {
                struct print_arg *args = event->print_fmt.args;

                // We are assuming print_fmt for ftrace/print function is:
                //   print fmt: "%ps: %s", (void *)REC->ip, REC->buf
                if ( args->type != PRINT_FIELD )
                    args = args->next;

                trace_seq_reset( &seq );
                print_str_arg( &seq, record->data, record->size,
                               event, "%s", -1, args );

                // pretty_print prints IP and print string (buf).
                //   pretty_print( &seq, record->data, record->size, event );

                trace_event.system = "ftrace-print";

                // Convert all LFs to spaces.
                for ( unsigned int i = 0; i < seq.len; i++ )
                {
                    if ( seq.buffer[ i ] == '\n' )
                        seq.buffer[ i ] = ' ';
                }
            }

            // Trim trailing whitespace
            while ( ( seq.len > 0 ) &&
                    isspace( (unsigned char)seq.buffer[ seq.len - 1 ] ) )
            {
                seq.len--;
            }

            trace_seq_terminate( &seq );

            event_field_t field;
            field.key = strpool.getstr( format->name );
            field.value = strpool.getstr( seq.buffer );
            trace_event.fields.push_back( field );
        }

        init_event_flags( trace_event );

        ret = cb( trace_info, trace_event );

        trace_seq_destroy( &seq );
    }

    return ret;
}

static pevent_record_t *get_next_record( file_info_t *file_info )
{
    if ( file_info->record )
        return file_info->record;

    if ( file_info->done )
        return NULL;

    file_info->record = tracecmd_read_next_data( file_info->handle, NULL );
    if ( !file_info->record )
        file_info->done = 1;

    return file_info->record;
}

static void add_file( std::vector< file_info_t * > &file_list, tracecmd_input_t *handle, const char *file )
{
    file_info_t *item = ( file_info_t * )trace_malloc( handle, sizeof( *item ) );

    memset( item, 0, sizeof( *item ) );

    item->handle = handle;
    handle->file = file;

    file_list.push_back( item );
}

int read_trace_file( const char *file, StrPool &strpool, EventCallback &cb )
{
    trace_info_t trace_info;
    tracecmd_input_t *handle;
    std::vector< file_info_t * > file_list;

    handle = tracecmd_alloc( file );
    if ( !handle )
    {
        logf( "%s: Open trace file \"%s\" failed.\n", __func__, file );
        return -1;
    }

    add_file( file_list, handle, file );

    // Read header information from trace.dat file.
    tracecmd_read_headers( handle );

    // Prepare reading the data from trace.dat.
    tracecmd_init_data( handle );

    // We don't support reading latency trace files.
    if ( handle->flags & TRACECMD_FL_LATENCY )
        die( handle, "%s: Latency traces not supported.\n", __func__ );

    /* Find the kernel_stacktrace if available */
    // pevent = handle->pevent;
    // event = pevent_find_event_by_name(pevent, "ftrace", "kernel_stack");

    /* If this file has buffer instances, get the file_info for them */
    for ( int i = 0; i < handle->nr_buffers; i++ )
    {
        tracecmd_input_t *new_handle;
        const char *name = handle->buffers[ i ].name;

        new_handle = tracecmd_buffer_instance_handle( handle, i );
        if ( !new_handle )
            die( handle, "%s: could not retrieve handle %s.\n", __func__, name );

        add_file( file_list, new_handle, name );
    }

    trace_info.cpus = handle->cpus;
    trace_info.cpustats = handle->cpustats;
    trace_info.file = handle->file;
    trace_info.uname = handle->uname;
    trace_info.timestamp_in_us = is_timestamp_in_us( handle->pevent->trace_clock, handle->use_trace_clock );

    // Explicitly add idle thread at pid 0 
    trace_info.pid_comm_map.get_val( 0, strpool.getstr( "<idle>" ) );

    // Add comms for other pids
    for ( cmdline_list *cmdlist = handle->pevent->cmdlist;
          cmdlist;
          cmdlist = cmdlist->next )
    {
        int pid = cmdlist->pid;
        const char *comm = cmdlist->comm;
        int tgid = pevent_data_tgid_from_pid( handle->pevent, pid );

        // Add to our pid --> comm map
        trace_info.pid_comm_map.get_val( pid, strpool.getstr( comm ) );

        if ( tgid > 0 )
        {
            tgid_info_t *tgid_info = trace_info.tgid_pids.get_val( tgid, tgid_info_t() );

            // Add pid to the tgid array of pids: main thread at start, others at back
            if ( pid == tgid )
                tgid_info->pids.insert( tgid_info->pids.begin(), pid );
            else
                tgid_info->pids.push_back( pid );

            tgid_info->tgid = tgid;
            tgid_info->hashval += fnv_hashstr32( comm );

            // Pid --> tgid
            trace_info.pid_tgid_map.get_val( pid, tgid );
        }
    }

    for ( ;; )
    {
        file_info_t *last_file_info = NULL;
        pevent_record_t *last_record = NULL;

        for ( file_info_t *file_info : file_list )
        {
            pevent_record_t *record = get_next_record( file_info );

            if ( !last_record ||
                 ( record && record->ts < last_record->ts ) )
            {
                last_record = record;
                last_file_info = file_info;
            }
        }

        if ( !last_record )
            break;

        int ret = trace_enum_events( cb, strpool, trace_info, last_file_info->handle, last_record );

        free_record( last_file_info->handle, last_file_info->record );
        last_file_info->record = NULL;

        if ( ret )
            break;
    }

    for ( file_info_t *file_info : file_list )
    {
        tracecmd_close( file_info->handle );
        free( file_info );
    }
    file_list.clear();

    return 0;
}
