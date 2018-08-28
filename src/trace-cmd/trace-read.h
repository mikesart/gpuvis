/*
 * Copyright 2018 Valve Software
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

// From kernel/trace/trace.h
#ifndef TRACE_BUF_SIZE
#define TRACE_BUF_SIZE     1024
#endif

// About process states:
//   https://www.ibm.com/developerworks/linux/library/l-task-killable/

// R Running
// S Sleeping in an interruptible wait
// D Waiting in uninterruptible disk sleep
// T Stopped (on a signal) or (before Linux 2.6.33) trace stopped
// t Tracing stop (Linux 2.6.33 onward)
// X Dead (from Linux 2.6.0 onward)
// Z Zombie
// P Parked (Linux 3.9 to 3.13 only)

// Bits for sched_switch prev_state field:
//   From task_index_to_char() in include/linux/sched.h

/* Used in tsk->state: */
#define TASK_RUNNING            0x0000 // R
#define TASK_INTERRUPTIBLE      0x0001 // S
#define TASK_UNINTERRUPTIBLE    0x0002 // D
#define TASK_STOPPED            0x0004 // T
#define TASK_TRACED             0x0008 // t
/* Used in tsk->exit_state: */
#define EXIT_DEAD               0x0010 // X
#define EXIT_ZOMBIE             0x0020 // Z
/* Used in tsk->state again: */
#define TASK_PARKED             0x0040 // P
#define TASK_DEAD               0x0080 // I

#define TASK_REPORT_MAX         0x0100 // (0x7f + 1) << 1

#define INVALID_ID ( ( uint32_t )-1 )

inline bool is_valid_id( uint32_t id )
{
    return ( id != INVALID_ID );
}

struct tgid_info_t
{
    int tgid = 0;
    std::vector< int > pids;

    // Colored 'foobarapp-1234' string for tgid
    const char *commstr_clr;
    const char *commstr;

    uint32_t hashval = 0;
    uint32_t color = 0;

    void add_pid( int pid );
};

struct cpu_info_t
{
    // per_cpu/cpu0/stats
    //
    // This displays certain stats about the ring buffer:
    //  entries: The number of events that are still in the buffer.
    //  overrun: The number of lost events due to overwriting when·
    //       the buffer was full.
    //  commit overrun: Should always be zero.
    //     This gets set if so many events happened within a nested
    //     event (ring buffer is re-entrant), that it fills the
    //     buffer and starts dropping events.
    //  bytes: Bytes actually read (not overwritten).
    //  oldest event ts: The oldest timestamp in the buffer
    //  now ts: The current timestamp
    //  dropped events: Events lost due to overwrite option being off.·
    //  read events: The number of events read.
    int64_t entries = 0;
    int64_t overrun = 0;
    int64_t commit_overrun = 0;
    int64_t bytes = 0;
    int64_t oldest_event_ts = 0;
    int64_t now_ts = 0;
    int64_t dropped_events = 0;
    int64_t read_events = 0;

    uint64_t file_offset = 0;
    uint64_t file_size = 0;

    int64_t min_ts = 0;
    int64_t max_ts = 0;

    // Non-trimmed events read for this cpu
    uint64_t events = 0;
    // Total events read for this cpu
    uint64_t tot_events = 0;
};

struct trace_info_t
{
    uint32_t cpus = 0;
    std::string file;
    std::string uname;
    bool timestamp_in_us;

    std::vector< cpu_info_t > cpu_info;

    // ts of the first event in the file
    int64_t min_file_ts = INT64_MAX;

    // ts where we trimmed from
    bool trim_trace = false;
    int64_t trimmed_ts = 0;

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
    TRACE_FLAG_SCHED_SWITCH_SYSTEM_EVENT    = 0x10000,
    TRACE_FLAG_AUTOGEN_COLOR                = 0x20000,
};

struct trace_event_t
{
public:
    bool is_filtered_out = false;

    int pid;                        // event process id
    uint32_t id;                    // event id
    uint32_t cpu;                   // cpu this event was hit on
    int64_t ts;                     // timestamp

    uint32_t flags = 0;             // TRACE_FLAGS_IRQS_OFF, TRACE_FLAG_HARDIRQ, TRACE_FLAG_SOFTIRQ
    uint32_t seqno = 0;             // event seqno (from fields)
    uint32_t id_start = INVALID_ID; // start event if this is a graph sequence event (ie amdgpu_sched_run_job, fence_signaled)
    uint32_t graph_row_id = 0;
    int crtc = -1;                  // drm_vblank_event crtc (or -1)

    uint32_t color = 0;             // color of the event (or 0 for default)

    // i915 events: col_Graph_Bari915SubmitDelay, etc
    // ftrace print events: buf hashval for colors
    // otherwise: -1
    uint32_t color_index = ( uint32_t )-1;

    int64_t duration = INT64_MAX;   // how long this timeline event took (or INT64_MAX for not set)

    const char *comm;               // command name
    const char *system;             // event system (ftrace-print, etc.)
    const char *name;               // event name
    const char *user_comm;          // User space comm (if we can figure this out)

    uint32_t numfields = 0;
    event_field_t *fields = nullptr;

public:
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
};

const char *get_event_field_val( const trace_event_t &event, const char *name, const char *defval = "" );
event_field_t *get_event_field( trace_event_t &event, const char *name );

typedef std::function< int ( const trace_event_t &event ) > EventCallback;
int read_trace_file( const char *file, StrPool &strpool, trace_info_t &trace_info, EventCallback &cb );
