/*
 * Copyright 2019 Valve Software
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

#include <string>
#include <array>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <SDL.h>

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"   // BeginColumns(), EndColumns(), PushColumnClipRect()
#include "imgui/imgui_impl_sdl_gl3.h"

#define GPUVIS_TRACE_IMPLEMENTATION
#include "gpuvis_macros.h"

#include "tdopexpr.h"
#include "trace-cmd/trace-read.h"

#include "stlini.h"
#include "gpuvis_utils.h"
#include "gpuvis_wdat.h"
#include "gpuvis.h"

#include "gpuvis_wdat.h"

 /**
  * wdat_reader_t reads a wdat file and provides each event as a set of key/value pairs
  */
class wdat_reader_t
{
public:
    wdat_reader_t( const char *file ) :
        mFileStream( file )
    {
        mFileName = file;
    }

    // return false at end of stream
    bool parse_entry( std::istringstream &stream, std::string &key, std::string &val )
    {
        std::string garbage;

        std::getline( stream, key, '=' );
        if ( stream.fail() )
            return false;

        std::getline( stream, garbage, '`' );
        if ( stream.fail() )
            return false;

        std::getline( stream, val, '`' );
        if ( stream.fail() )
            return false;

        // Eat the last space
        std::getline( stream, garbage, ' ' );
        
        return true;
    }

    std::unordered_map<std::string, std::string> get_event()
    {
        std::unordered_map<std::string, std::string> map = {};

        if ( !mFileStream )
            return map;

        std::string entry;
        if ( !std::getline( mFileStream, entry ) )
            return map;

        std::istringstream event( entry );

        std::string key, val;
        while ( parse_entry( event, key, val ) )
        {
            map[key] = val;
        }

        // Save the original text for error handling
        map["wdat_line"] = entry;
        
        return map;
    }

private:
    std::string mFileName;
    std::ifstream mFileStream;
};

/**
 * Helper macros for x_entry_t classes
 */
#define WDAT_PARSE_STR( name ) name = entry[#name];
#define WDAT_PARSE_INT( name ) name = std::atoi( entry[#name].c_str() );
#define WDAT_PARSE_U64( name ) name = std::stoull( entry[#name].c_str() );
#define WDAT_PARSE_DBL( name ) name = std::stod( entry[#name] );

/**
 * The x_entry_t classes are helpers to interpret the wdat data as c++ types
 */
class header_entry_t
{
public:
    static const int entry_id = 0;

    int version;

    header_entry_t( std::unordered_map<std::string, std::string> &entry )
    {
        WDAT_PARSE_INT( version );
    }
};

class context_entry_t
{
public:
    static const int entry_id = 1;

    std::string file;
    std::string os_version;
    int num_cpu;
    uint64_t start_time;
    uint64_t end_time;

    context_entry_t( std::unordered_map<std::string, std::string> &entry )
    {
        WDAT_PARSE_STR( file );
        WDAT_PARSE_STR( os_version );
        WDAT_PARSE_INT( num_cpu );
        WDAT_PARSE_U64( start_time );
        WDAT_PARSE_U64( end_time );
    }
};

class event_entry_t
{
public:
    uint64_t ts;
    double ts_rms;
    int cpu;
    int pid;
    int tid;
    std::string pname;

    event_entry_t( std::unordered_map<std::string, std::string> &entry )
    {
        WDAT_PARSE_U64( ts );
        WDAT_PARSE_DBL( ts_rms );
        WDAT_PARSE_INT( cpu );
        WDAT_PARSE_INT( pid );
        WDAT_PARSE_INT( tid );
        WDAT_PARSE_STR( pname );
    }
};

class steamvr_entry_t : public event_entry_t
{
public:
    static const int entry_id = 2;

    std::string vrevent;

    steamvr_entry_t( std::unordered_map<std::string, std::string> &entry ) :
        event_entry_t( entry )
    {
        WDAT_PARSE_STR( vrevent );
    }
};

class vsync_entry_t : public event_entry_t
{
public:
    static const int entry_id = 3;

    uint64_t adapter;
    uint64_t display;
    uint64_t address;

    vsync_entry_t( std::unordered_map<std::string, std::string> &entry ) :
        event_entry_t( entry )
    {
        WDAT_PARSE_U64( adapter );
        WDAT_PARSE_U64( display );
        WDAT_PARSE_U64( address );
    }
};

/**
 * Parses the wdat information stream
 *
 * The wdat input stream is converted into a trace_info_t + a sequence of trace_event_t
 */
class wdat_parser_t
{
public:
    wdat_parser_t( const char *file, StrPool &strpool, trace_info_t &trace_info, EventCallback &cb )
        : mFileName( file )
        , mStrPool( strpool )
        , mTraceInfo( trace_info )
        , mCallback( cb )
        , mReader( file )
        , mCurrentEventId( 0 )
        , mStartTicks( 0 )
        , mAdapterCount( 0 )
        , mCrtcCount( 0 )
    {
        memset( mCrtcCurrentSeq, 0, sizeof( mCrtcCurrentSeq ) );
    }

    int process()
    {
        std::unordered_map<std::string, std::string> event;
        for ( event = mReader.get_event(); !event.empty(); event = mReader.get_event() )
        {
            int event_id = std::atoi( event["id"].c_str() );

            int ret;
            switch ( event_id )
            {
            case header_entry_t::entry_id:
                ret = process_header_entry( header_entry_t( event ) );
                break;
            case context_entry_t::entry_id:
                ret = process_context_entry( context_entry_t( event ) );
                break;
            case steamvr_entry_t::entry_id:
                ret = process_steamvr_entry( steamvr_entry_t( event ) );
                break;
            case vsync_entry_t::entry_id:
                ret = process_vsync_entry( vsync_entry_t( event ) );
                break;
            default:
                logf( "[Error] unrecognized wdat entry: %s", event["wdat_line"].c_str() );
                ret = 0;
                break;
            }
        }

        return 0;
    }

private:
    static const int kMaxCrtc = 32;

    const char *mFileName;
    StrPool &mStrPool;
    trace_info_t &mTraceInfo;
    EventCallback &mCallback;

    wdat_reader_t mReader;
    uint32_t mCurrentEventId;
    uint64_t mStartTicks;

    std::unordered_map<uint64_t, int> mAdapterMap;
    int mAdapterCount;

    std::unordered_map<uint64_t, int> mCrtcMap;
    int mCrtcCount;

    uint64_t mCrtcCurrentSeq[ kMaxCrtc ];

    int GetAdapterIdx( uint64_t key )
    {
        if ( mAdapterMap.find( key ) == mAdapterMap.end() )
        {
            mAdapterMap[key] = mAdapterCount++;
        }

        return mAdapterMap[key];
    }

    int GetCrtcIdx( uint64_t key )
    {
        if ( mCrtcMap.find( key ) == mCrtcMap.end() )
        {
            mCrtcMap[key] = mCrtcCount++;
            assert( mCrtcCount < kMaxCrtc );
        }

        return mCrtcMap[key];
    }

    int64_t ticks_to_relative_us( uint64_t ticks )
    {
        return ( ticks - mStartTicks ) * 100;
    }

    int process_header_entry( header_entry_t entry )
    {
        // Only version 1 is supported at the moment
        return entry.version == 1 ? 0 : -1;
    }

    int process_context_entry( context_entry_t entry )
    {
        mStartTicks = entry.start_time;

        mTraceInfo.cpus = entry.num_cpu;
        mTraceInfo.file = entry.file;
        mTraceInfo.uname = entry.os_version;
        mTraceInfo.timestamp_in_us = true; // nanoseconds?
        mTraceInfo.min_file_ts = ticks_to_relative_us( entry.start_time );
        mTraceInfo.cpu_info.resize( entry.num_cpu );

        for ( size_t cpu = 0; cpu < entry.num_cpu; cpu++ )
        {
            cpu_info_t &cpu_info = mTraceInfo.cpu_info[cpu];

            cpu_info.file_offset = 0;
            cpu_info.file_size = 0;

            cpu_info.entries = 0;
            cpu_info.overrun = 0;
            cpu_info.commit_overrun = 0;
            cpu_info.bytes = 0;
            cpu_info.oldest_event_ts = ticks_to_relative_us( entry.start_time );;
            cpu_info.now_ts = ticks_to_relative_us( entry.end_time );
            cpu_info.dropped_events = 0;
            cpu_info.read_events = 0;
        }
        return 0;
    }

    // In linux tgid is the process id
    bool is_process_known( int pid )
    {
        return mTraceInfo.tgid_pids.m_map.find( pid ) != mTraceInfo.tgid_pids.m_map.end();
    }

    // In linux pid is the thread id
    bool is_thread_known( int tid )
    {
        return mTraceInfo.pid_comm_map.m_map.find( tid ) != mTraceInfo.pid_comm_map.m_map.end();
    }

    // Process the common information for all events
    int process_event_entry( event_entry_t entry, trace_event_t &event )
    {
        const char *comm = mStrPool.getstrf( "%s-%u", entry.pname.c_str(), entry.tid );

        if ( !is_thread_known( entry.tid ) )
        {
            mTraceInfo.pid_comm_map.get_val( entry.tid, mStrPool.getstr( comm ) );
        }

        if ( !is_process_known( entry.pid ) )
        {
            tgid_info_t *tgid_info = mTraceInfo.tgid_pids.get_val_create( entry.pid );

            if ( !tgid_info->tgid )
            {
                tgid_info->tgid = entry.pid;
                tgid_info->hashval += hashstr32( comm );
            }
            tgid_info->add_pid( entry.tid );

            // Pid --> tgid
            mTraceInfo.pid_tgid_map.get_val( entry.tid, entry.pid );
        }

        event.pid = entry.tid;
        event.id = mCurrentEventId++;
        event.cpu = entry.cpu;
        event.ts = ticks_to_relative_us( entry.ts );
        event.comm = comm;
        event.user_comm = comm;
        event.seqno = 0;

        return 0;
    }

    // Process steamvr event specific information
    int process_steamvr_entry( steamvr_entry_t entry )
    {
        int err;

        trace_event_t event;

        err = process_event_entry( entry, event );
        if ( err )
            return err;

        event.system = mStrPool.getstr( "ftrace-print" ); // For dat compatibility
        event.name = mStrPool.getstr( "steamvr" );
        event.numfields = 1;
        event.fields = new event_field_t[event.numfields];
        event.fields[0].key = mStrPool.getstr( "buf" );
        event.fields[0].value = mStrPool.getstr( entry.vrevent.c_str() );
        event.flags = TRACE_FLAG_FTRACE_PRINT;

        return mCallback( event );
    }

    // Process vsync event specific information
    int process_vsync_entry( vsync_entry_t entry )
    {
        int err;

        trace_event_t event;

        err = process_event_entry( entry, event );
        if ( err )
            return err;

        int crtc = GetCrtcIdx( entry.display );
        int adapter = GetAdapterIdx( entry.adapter );
        uint64_t seq = mCrtcCurrentSeq[crtc]++;

        event.system = mStrPool.getstr( "drm" ); // For dat compatibility
        event.name = mStrPool.getstr( "drm_vblank_event" ); // For dat compatibility

        event.crtc = crtc;
        event.numfields = 2;
        event.fields = new event_field_t[event.numfields];
        event.fields[0].key = mStrPool.getstr( "crtc" );
        event.fields[0].value = mStrPool.getstrf( "%d", crtc );
        event.fields[1].key = mStrPool.getstr( "seq" );
        event.fields[1].value = mStrPool.getstrf( "%ull", seq );
        event.flags = TRACE_FLAG_VBLANK;

        return mCallback( event );
    }
};

int read_wdat_file( const char *file, StrPool &strpool, trace_info_t &trace_info, EventCallback &cb )
{
    wdat_parser_t parser( file, strpool, trace_info, cb );
    return parser.process();

    /*
    // Find the lowest ts value in the trace file
    trace_info.min_file_ts = std::min< int64_t >( trace_info.min_file_ts, record->ts );

    trace_info.cpu_info.resize( handle->cpus );
    for ( size_t cpu = 0; cpu < (size_t)handle->cpus; cpu++ )
    {
        cpu_info_t &cpu_info = trace_info.cpu_info[cpu];
        pevent_record_t *record = tracecmd_peek_data( handle, cpu );

        cpu_info.file_offset = handle->cpu_data[cpu].file_offset;
        cpu_info.file_size = handle->cpu_data[cpu].file_size;

        if ( cpu < handle->cpustats.size() )
        {
            const char *stats = handle->cpustats[cpu].c_str();

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

        if ( record )
        {
            cpu_info.min_ts = record->ts - trace_info.min_file_ts;

            if ( cpu_info.overrun && trace_info.trim_trace )
                trim_ts = std::max< unsigned long long >( trim_ts, record->ts );
        }
    }

    // Scoot to tracestart time if it was set
    trim_ts = std::max< unsigned long long >( trim_ts, trace_info.min_file_ts + trace_info.m_tracestart );
    */
    return 0;
}
