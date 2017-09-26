/*
 * Copyright 2017 Valve Software
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

// Mask for sched_switch prev_state field
//   From include/linux/sched.h

/* Used in tsk->state: */
#define TASK_RUNNING            0
#define TASK_INTERRUPTIBLE      1
#define TASK_UNINTERRUPTIBLE    2
#define TASK_STOPPED            4
#define TASK_TRACED             8
/* Used in tsk->exit_state: */
#define EXIT_DEAD               16
#define EXIT_ZOMBIE             32
/* Used in tsk->state again: */
#define TASK_DEAD               64
#define TASK_WAKEKILL           128
#define TASK_WAKING             256
#define TASK_PARKED             512
#define TASK_STATE_MAX          1024

#define INVALID_ID ( ( uint32_t )-1 )

inline bool is_valid_id( uint32_t id )
{
    return ( id != INVALID_ID );
}

struct tgid_info_t
{
    int tgid;
    std::vector< int > pids;

    // Colored 'foobarapp-1234' string for tgid
    const char *commstr_clr;
    const char *commstr;

    uint32_t hashval = 0;
    uint32_t color = 0;
};

struct trace_info_t
{
    uint32_t cpus = 0;
    std::string file;
    std::string uname;
    bool timestamp_in_us;
    std::vector< std::string > cpustats;

    // Map tgid to vector of child pids and color
    util_umap< int, tgid_info_t > tgid_pids;
    // Map pid to tgid
    util_umap< int, int > pid_tgid_map;
    // Map pid to comm
    util_umap< int, const char * > pid_comm_map;
    // Map pid from sched_switch event prev_pid, next_pid fields to comm
    util_umap< int, const char * > sched_switch_pid_comm_map;
};

struct event_field_t
{
    const char *key;
    const char *value;
};

enum trace_flag_type_t {
    // TRACE_FLAG_IRQS_OFF = 0x01, // interrupts were disabled
    // TRACE_FLAG_IRQS_NOSUPPORT = 0x02,
    // TRACE_FLAG_NEED_RESCHED = 0x04,
    // TRACE_FLAG_HARDIRQ = 0x08, // inside an interrupt handler
    // TRACE_FLAG_SOFTIRQ = 0x10, // inside a softirq handler

    TRACE_FLAG_FTRACE_PRINT                 = 0x00100,
    TRACE_FLAG_VBLANK                       = 0x00200,
    TRACE_FLAG_TIMELINE                     = 0x00400,
    TRACE_FLAG_SW_QUEUE                     = 0x00800, // amdgpu_cs_ioctl
    TRACE_FLAG_HW_QUEUE                     = 0x01000, // amdgpu_sched_run_job
    TRACE_FLAG_FENCE_SIGNALED               = 0x02000, // *fence_signaled
    TRACE_FLAG_SCHED_SWITCH                 = 0x04000, // sched_switch
    TRACE_FLAG_SCHED_SWITCH_TASK_RUNNING    = 0x08000, // TASK_RUNNING
    TRACE_FLAG_AUTOGEN_COLOR                = 0x10000,
};

struct trace_event_t
{
    bool is_fence_signaled() const  { return !!( flags & TRACE_FLAG_FENCE_SIGNALED ); }
    bool is_ftrace_print() const    { return !!( flags & TRACE_FLAG_FTRACE_PRINT ); }
    bool is_vblank() const          { return !!( flags & TRACE_FLAG_VBLANK ); }
    bool is_timeline() const        { return !!( flags & TRACE_FLAG_TIMELINE ); }
    bool is_sched_switch() const    { return !!( flags & TRACE_FLAG_SCHED_SWITCH ); }

    bool has_duration() const       { return duration != INT64_MAX; }

    const char *get_timeline_name( const char *def = NULL ) const
    {
        if ( flags & TRACE_FLAG_SW_QUEUE )
            return "SW queue";
        else if ( flags & TRACE_FLAG_HW_QUEUE )
            return "HW queue";
        else if ( is_fence_signaled() )
            return "Execution";

        return def;
    }

    bool is_filtered_out;
    int pid;                    // event process id
    int crtc;                   // drm_vblank_event crtc (or -1)

    uint32_t id;                // event id
    uint32_t cpu;               // cpu this event was hit on
    uint32_t flags;             // TRACE_FLAGS_IRQS_OFF, TRACE_FLAG_HARDIRQ, TRACE_FLAG_SOFTIRQ
    uint32_t seqno;             // event seqno (from fields)
    uint32_t id_start;          // start event if this is a graph sequence event (ie amdgpu_sched_run_job, fence_signaled)
    uint32_t graph_row_id;

    uint32_t color;             // color of the event (or 0 for default)
    uint32_t color_index;       // -1 or colors_t enum value like col_Graph_Bari915SubmitDelay

    int64_t ts;                 // timestamp
    int64_t duration;           // how long this timeline event took (or INT64_MAX for not set)
    const char *comm;           // command name
    const char *system;         // event system (ftrace-print, etc.)
    const char *name;           // event name
    const char *user_comm;      // User space comm (if we can figure this out)

    std::vector< event_field_t > fields;
};

const char *get_event_field_val( const trace_event_t &event, const char *name, const char *defval = "" );

typedef std::function< int ( const trace_info_t &info, const trace_event_t &event ) > EventCallback;
int read_trace_file( const char *file, StrPool &strpool, EventCallback &cb );
