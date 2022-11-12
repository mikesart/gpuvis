/*
 * Copyright 2021 Valve Software
 *
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

#include <string>
#include <vector>
#include <forward_list>
#include <csetjmp>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <future>

#include "../gpuvis_macros.h"
#include "trace-read.h"

#define NSECS_PER_SEC  1000000000LL

extern "C"
{
    #include "trace-cmd-private.h"
    #include "../libtraceevent/src/event-parse.h"
    #include "../libtraceevent/src/event-parse-local.h"

    /* Be sure this syncs with event-parse.c! */
    struct cmdline_list
    {
        struct cmdline_list *next;
        char *comm;
        int pid;
    };

    struct tep_record *tracecmd_read_next_data(struct tracecmd_input *handle, int *rec_cpu);

    void print_str_arg( struct trace_seq *s, void *data, int size,
            struct tep_event *event, const char *format,
            int len_arg, struct tep_print_arg *arg );

    int tracecmd_ftrace_overrides(struct tracecmd_input *handle,
        struct tracecmd_ftrace *finfo)
    {
        return 0;
    }

    int tracecmd_blk_hack(struct tracecmd_input *handle)
    {
        return -1;
    }

    char **tracefs_tracers(const char *tracing_dir)
    {
        return nullptr;
    }

    /* From tracefs-events.c */
    char *trace_append_file(const char *dir, const char *name)
    {
        char *file;
        int ret;

        ret = asprintf(&file, "%s/%s", dir, name);

        return ret < 0 ? NULL : file;
    }
}

typedef struct tracecmd_input tracecmd_input_t;
typedef struct tep_handle     pevent_t;
typedef struct tep_record     pevent_record_t;

typedef struct file_info
{
    int done;
    tracecmd_input_t *handle;
    pevent_record_t *record;
    std::string file;
} file_info_t;

void logf( const char *fmt, ... ) ATTRIBUTE_PRINTF( 1, 2 );

//#ifdef WIN32
//#include <io.h>
//
//#define open _open
//#define close _close
//#define read _read
//#define lseek64 _lseeki64
//#define dup _dup
//
//#define __maybe_unused
//#else
//#define USE_MMAP
//
//#include <sys/mman.h>
//#include <sys/param.h>
//#include <unistd.h>
//
//#ifdef __APPLE__
//#define lseek64 lseek
//#define off64_t off_t
//#endif
//
//#endif
//
//#ifndef O_BINARY
//#define O_BINARY  0
//#endif

//  enum
//  {
//      //$$$ TODO
//      TRACECMD_OPTION_SAVED_TGIDS = 32,
//  };

//$$$ TODO?
// TRACECMD_FL_LATENCY = ( 1 << 2 ),

class trace_data_t
{
public:
    trace_data_t( EventCallback &_cb, trace_info_t &_trace_info, StrPool &_strpool ) :
        cb( _cb ), trace_info( _trace_info ), strpool( _strpool )
    {
        seqno_str = strpool.getstr( "seqno" );
        crtc_str = strpool.getstr( "crtc" );
        ip_str = strpool.getstr( "ip" );
        parent_ip_str = strpool.getstr( "parent_ip" );
        buf_str = strpool.getstr( "buf" );

        ftrace_print_str = strpool.getstr( "ftrace-print" );
        ftrace_function_str = strpool.getstr( "ftrace-function" );
        drm_vblank_event_str = strpool.getstr( "drm_vblank_event" );
        sched_switch_str = strpool.getstr( "sched_switch" );
        time_str = strpool.getstr( "time" );
        high_prec_str = strpool.getstr( "high_prec" );
    }

public:
    EventCallback &cb;
    trace_info_t &trace_info;
    StrPool &strpool;

    const char *seqno_str;
    const char *crtc_str;
    const char *ip_str;
    const char *parent_ip_str;
    const char *buf_str;

    const char *ftrace_print_str;
    const char *ftrace_function_str;
    const char *drm_vblank_event_str;
    const char *sched_switch_str;
    const char *time_str;
    const char *high_prec_str;
};

static bool is_timestamp_in_us( const std::string& trace_clock, bool use_trace_clock )
{
    if ( !use_trace_clock )
        return true;

    // trace_clock information:
    //  https://www.kernel.org/doc/Documentation/trace/ftrace.txt
    if ( trace_clock == "local" ||
         trace_clock == "global" ||
         trace_clock == "uptime" ||
         trace_clock == "perf" )
    {
        return true;
    }

    /* trace_clock is setting in tsc or counter mode */
    return false;
}


static void init_event_flags( trace_data_t &trace_data, trace_event_t &event )
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
    if ( event.system == trace_data.ftrace_print_str )
        event.flags |= TRACE_FLAG_FTRACE_PRINT;
    else if ( event.name == trace_data.drm_vblank_event_str )
        event.flags |= TRACE_FLAG_VBLANK;
    else if ( event.name == trace_data.sched_switch_str )
        event.flags |= TRACE_FLAG_SCHED_SWITCH;
    else if ( strstr( event.name, "fence_signaled" ) )
        event.flags |= TRACE_FLAG_FENCE_SIGNALED;
    else if ( strstr( event.name, "amdgpu_cs_ioctl" ) )
        event.flags |= TRACE_FLAG_SW_QUEUE;
    else if ( strstr( event.name, "amdgpu_sched_run_job" ) )
        event.flags |= TRACE_FLAG_HW_QUEUE;
}

static int trace_enum_events( trace_data_t &trace_data, tracecmd_input_t *handle, pevent_record_t *record )
{
    int ret = 0;
    tep_event *event;
    pevent_t *pevent = tracecmd_get_tep(handle);
    StrPool &strpool = trace_data.strpool;

    event = tep_find_event_by_record( pevent, record );
    if ( event )
    {
        struct trace_seq seq;
        trace_event_t trace_event;
        struct tep_format_field *format;
        int pid = tep_data_pid( pevent, record );
        const char *comm = tep_data_comm_from_pid( pevent, pid );
        bool is_ftrace_function = !strcmp( "ftrace", event->system ) && !strcmp( "function", event->name );
        bool is_printk_function = !strcmp( "ftrace", event->system ) && !strcmp( "print", event->name );

        trace_seq_init( &seq );

        trace_event.pid = pid;
        trace_event.cpu = record->cpu;
        trace_event.ts = record->ts;

        trace_event.comm = strpool.getstrf( "%s-%u", comm, pid );

        trace_event.system = strpool.getstr( event->system );
        trace_event.name = strpool.getstr( event->name );
        trace_event.user_comm = trace_event.comm;

        // Get count of fields for this event.
        uint32_t field_count = 0;
        format = event->format.fields;
        for ( ; format; format = format->next )
            field_count++;

        // Alloc space for our fields array.
        trace_event.numfields = 0;
        trace_event.fields = new event_field_t[ field_count ];

        format = event->format.common_fields;
        for ( ; format; format = format->next )
        {
            if ( !strcmp( format->name, "common_flags" ) )
            {
                unsigned long long val = tep_read_number( pevent,
                        ( char * )record->data + format->offset, format->size );

                // TRACE_FLAG_IRQS_OFF | TRACE_FLAG_HARDIRQ | TRACE_FLAG_SOFTIRQ
                trace_event.flags = val;
                break;
            }
        }

        format = event->format.fields;
        for ( ; format; format = format->next )
        {
            const char *format_name = strpool.getstr( format->name );

            trace_seq_reset( &seq );

            if ( is_printk_function && ( format_name == trace_data.buf_str ) )
            {
                struct tep_print_arg *args = event->print_fmt.args;

                // We are assuming print_fmt for ftrace/print function is:
                //   print fmt: "%ps: %s", (void *)REC->ip, REC->buf
                if ( args->type != TEP_PRINT_FIELD )
                    args = args->next;

                print_str_arg( &seq, record->data, record->size,
                               event, "%s", -1, args );

                // pretty_print prints IP and print string (buf).
                //   pretty_print( &seq, record->data, record->size, event );

                trace_event.system = trace_data.ftrace_print_str;

                // Convert all LFs to spaces.
                for ( unsigned int i = 0; i < seq.len; i++ )
                {
                    if ( seq.buffer[ i ] == '\n' )
                        seq.buffer[ i ] = ' ';
                }
            }
            else
            {
                const char *trace_clock_str = tracecmd_get_trace_clock(handle);
                tep_print_field( &seq, record->data, format );

                if ( format_name == trace_data.seqno_str )
                {
                    unsigned long long val = tep_read_number( pevent,
                               ( char * )record->data + format->offset, format->size );

                    trace_event.seqno = val;
                }
                else if ( format_name == trace_data.crtc_str )
                {
                    unsigned long long val = tep_read_number( pevent,
                               ( char * )record->data + format->offset, format->size );

                    trace_event.crtc = val;
                }
                else if ( trace_event.name == trace_data.drm_vblank_event_str &&
                          format_name == trace_data.time_str &&
                          !strcmp(trace_clock_str, "mono" ))
                {
                    // for drm_vblank_event, if "time" field is available,
                    // and the trace-clock is monotonic, store the timestamp
                    // passed along with the vblank event
                    unsigned long long val = tep_read_number( pevent,
                               ( char * )record->data + format->offset, format->size );

                    trace_event.vblank_ts = val;
                }
                else if ( trace_event.name == trace_data.drm_vblank_event_str &&
                          format_name == trace_data.high_prec_str &&
                          !strcmp(trace_clock_str, "mono" ))
                {
                    // for drm_vblank_event, if "high_prec" field is available,
                    // and the trace-lock is monotonic, store the field whether or not
                    // the passed timestamp is actually from a high-precision source
                    unsigned long long val = tep_read_number( pevent,
                               ( char * )record->data + format->offset, format->size );

                    trace_event.vblank_ts_high_prec = val != 0;
                }
                else if ( is_ftrace_function )
                {
                    bool is_ip = ( format_name == trace_data.ip_str );

                    if ( is_ip || ( format_name == trace_data.parent_ip_str ) )
                    {
                        unsigned long long val = tep_read_number( pevent,
                                ( char * )record->data + format->offset, format->size );
                        const char *func = tep_find_function( pevent, val );

                        if ( func )
                        {
                            trace_seq_printf( &seq, " (%s)", func );

                            if ( is_ip )
                            {
                                // If this is a ftrace:function event, set the name
                                //  to be the function name we just found.
                                trace_event.system = trace_data.ftrace_function_str;
                                trace_event.name = strpool.getstr( func );
                            }
                        }
                    }
                }
            }

            // Trim trailing whitespace
            while ( ( seq.len > 0 ) &&
                    isspace( (unsigned char)seq.buffer[ seq.len - 1 ] ) )
            {
                seq.len--;
            }

            trace_seq_terminate( &seq );

            trace_event.fields[ trace_event.numfields ].key = format_name;
            trace_event.fields[ trace_event.numfields ].value = strpool.getstr( seq.buffer, seq.len );
            trace_event.numfields++;
        }

        init_event_flags( trace_data, trace_event );

        ret = trace_data.cb( trace_event );

        trace_seq_destroy( &seq );
    }

    return ret;
}

void tgid_info_t::add_pid( int pid )
{
    auto idx = std::find( pids.begin(), pids.end(), pid );

    if ( idx == pids.end() )
    {
        // Add pid to the tgid array of pids: main thread at start, others at back
        if ( pid == tgid )
            pids.insert( pids.begin(), pid );
        else
            pids.push_back( pid );
    }
}

static void add_file( std::vector< file_info_t > &file_list, tracecmd_input_t *handle, const char *file )
{
    file_info_t item;

    item.done = 0;
    item.record = nullptr;
    item.handle = handle;
    item.file = file;

    file_list.push_back( item );
}

static int64_t geti64( const char *str, const char *var )
{
    const char *val = strstr( str, var );

    if ( val )
        return strtoull( val + strlen( var ), NULL, 10 );
    return 0;
}

static int64_t getf64( const char *str, const char *var )
{
    const char *val = strstr( str, var );

    if ( val )
        return strtold( val + strlen( var ), NULL ) * NSECS_PER_SEC;
    return 0;
}

static struct tep_record *get_next_record( file_info_t *file_info )
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

static void free_handle_record( file_info_t *file_info )
{
    if (!file_info->record)
        return;

    tracecmd_free_record(file_info->record);
    file_info->record = NULL;
}

static void parse_cmdlist( StrPool &strpool, trace_info_t &trace_info, std::vector< file_info_t > &file_list )
{
    // Explicitly add idle thread at pid 0
    trace_info.pid_comm_map.get_val( 0, strpool.getstr( "<idle>" ) );

    for ( file_info_t &file_info : file_list )
    {
        struct tep_handle *pevent = tracecmd_get_tep(file_info.handle);

        // Add comms for other pids
        for ( struct cmdline_list *cmdlist = pevent->cmdlist;
                cmdlist;
                cmdlist = cmdlist->next )
        {
            int pid = cmdlist->pid;
            const char *comm = cmdlist->comm;
            int tgid = tep_data_tgid_from_pid( pevent, pid );

            // Add to our pid --> comm map
            trace_info.pid_comm_map.get_val( pid, strpool.getstr( comm ) );

            if ( tgid > 0 )
            {
                tgid_info_t *tgid_info = trace_info.tgid_pids.get_val_create( tgid );

                if ( !tgid_info->tgid )
                {
                    tgid_info->tgid = tgid;
                    tgid_info->hashval += hashstr32( comm );
                }
                tgid_info->add_pid( pid );

                // Pid --> tgid
                trace_info.pid_tgid_map.get_val( pid, tgid );
            }
        }
    }
}

static void set_min_file_ts( trace_info_t &trace_info, std::vector< file_info_t> &file_list )
{
    // Find the lowest ts value in the trace file
    for ( file_info_t &file_info : file_list )
    {
        int64_t ts = tracecmd_get_first_ts( file_info.handle );

        if ( ts )
            trace_info.min_file_ts = std::min< int64_t >( trace_info.min_file_ts, ts );
    }

    // Should never happen, but sure is bad if it ever does as our min_ts is set to INT64 MAX
    if (trace_info.min_file_ts == INT64_MAX)
        trace_info.min_file_ts = 0;
}

static std::string cstr_to_string(const char *cstr)
{
    return std::string(cstr ? cstr : "");
}

static uint64_t parse_cpu_stats(trace_info_t &trace_info, std::vector< file_info_t > &file_list)
{
    std::vector< std::string > cpustats;
    tracecmd_input_t *handle = file_list[0].handle;
    int cpus = tracecmd_cpus(handle);
    const char *cpustats_str = tracecmd_get_cpustats(handle);

    // Latest ts value where a cpu data starts
    uint64_t trim_ts = 0;

    if (cpustats_str)
        cpustats = string_explode( cpustats_str, '\n' );

    trace_info.cpu_info.resize( cpus );
    for ( size_t cpu = 0; cpu < ( size_t )cpus; cpu++ )
    {
        cpu_info_t &cpu_info = trace_info.cpu_info[ cpu ];

        cpu_info.file_size = tracecmd_get_cpu_file_size(handle, cpu);
        if (cpu_info.file_size == (uint64_t)-1)
            cpu_info.file_size = 0;

        if (cpu < cpustats.size())
        {
            const char *stats = cpustats[ cpu ].c_str();

            cpu_info.entries = geti64( stats, "entries:" );
            cpu_info.overrun = geti64( stats, "overrun:" );
            cpu_info.commit_overrun = geti64( stats, "commit overrun:" );
            cpu_info.bytes = geti64( stats, "bytes:" );
            cpu_info.oldest_event_ts = getf64( stats, "oldest event ts:" );
            cpu_info.now_ts = getf64( stats, "now ts:" );
            cpu_info.dropped_events = geti64( stats, "dropped events:" );
            cpu_info.read_events = geti64( stats, "read events:" );

            if ( cpu_info.oldest_event_ts )
                cpu_info.oldest_event_ts -= trace_info.min_file_ts;
            if ( cpu_info.now_ts )
                cpu_info.now_ts -= trace_info.min_file_ts;
        }

        for ( file_info_t &file_info : file_list )
        {
            pevent_record_t *record = tracecmd_peek_data( file_info.handle, cpu );

            if ( record )
            {
                cpu_info.min_ts = record->ts - trace_info.min_file_ts;

                if ( cpu_info.overrun && trace_info.trim_trace )
                    trim_ts = std::max< uint64_t >( trim_ts, record->ts );
            }
        }
    }

    return trim_ts;
}

static void process_records( StrPool &strpool, trace_info_t &trace_info, std::vector< file_info_t > &file_list,
        EventCallback &cb, uint64_t trim_ts )
{
    trace_data_t trace_data( cb, trace_info, strpool );

    for ( ;; )
    {
        int ret = 0;
        file_info_t *last_file_info = NULL;
        pevent_record_t *last_record = NULL;

        for ( file_info_t &file_info : file_list )
        {
            pevent_record_t *record = get_next_record( &file_info );

            if ( !last_record ||
                 ( record && record->ts < last_record->ts ) )
            {
                last_record = record;
                last_file_info = &file_info;
            }
        }

        if ( last_record )
        {
            cpu_info_t &cpu_info = trace_info.cpu_info[ last_record->cpu ];

            // Bump up total event count for this cpu
            cpu_info.tot_events++;

            // Store the max ts value we've seen for this cpu
            cpu_info.max_ts = last_record->ts - trace_info.min_file_ts;

            // If this ts is greater than our trim value, add it.
            if ( last_record->ts >= trim_ts )
            {
                cpu_info.events++;
                ret = trace_enum_events( trace_data, last_file_info->handle, last_record );

                // Bail if user specified read length and we hit it
                if ( trace_info.m_tracelen && ( last_record->ts - trim_ts > trace_info.m_tracelen ) )
                    last_record = NULL;
            }

            free_handle_record( last_file_info );
        }

        if ( !last_record || ret )
            break;
    }

    if ( trim_ts )
        trace_info.trimmed_ts = trim_ts - trace_info.min_file_ts;
}

int read_trace_file( const char *file, StrPool &strpool, trace_info_t &trace_info, EventCallback &cb )
{
    GPUVIS_TRACE_BLOCK( __func__ );

    tracecmd_input_t *handle;
    std::vector< file_info_t > file_list;

    handle = tracecmd_open( file, TRACECMD_FL_LOAD_NO_PLUGINS | TRACECMD_FL_LOAD_NO_SYSTEM_PLUGINS );
    if (!handle)
    {
        logf( "[Error] %s: tracecmd_open %s failed.\n", __func__, file );
        return -1;
    }

    add_file( file_list, handle, file );

    /* If this file has buffer instances, get the file_info for them */
    int instances = tracecmd_buffer_instances(handle);
    if ( instances )
    {
        for ( int i = 0; i < instances; i++)
        {
            const char *name = tracecmd_buffer_instance_name(handle, i);
            struct tracecmd_input *new_handle = tracecmd_buffer_instance_handle(handle, i);

            if ( name && new_handle )
            {
                add_file( file_list, new_handle, name );
            }
        }
    }

    trace_info.cpus = tracecmd_cpus(handle);
    trace_info.file = file_list[0].file;
    trace_info.uname = cstr_to_string(tracecmd_get_uname(handle));
    trace_info.opt_version = cstr_to_string(tracecmd_get_version(handle));
    trace_info.timestamp_in_us = is_timestamp_in_us( tracecmd_get_trace_clock(handle), tracecmd_get_use_trace_clock(handle) );

    parse_cmdlist( strpool, trace_info, file_list );

    set_min_file_ts( trace_info, file_list );

    // Latest ts value where a cpu data starts
    uint64_t trim_ts = parse_cpu_stats( trace_info, file_list );

    // Scoot to tracestart time if it was set
    trim_ts = std::max< int64_t >( trim_ts, trace_info.min_file_ts + trace_info.m_tracestart );

    process_records( strpool, trace_info, file_list, cb, trim_ts );

    for ( file_info_t &file_info : file_list )
    {
        tracecmd_close( file_info.handle );
    }
    file_list.clear();

    return 0;
}

