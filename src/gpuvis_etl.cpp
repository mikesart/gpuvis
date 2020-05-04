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

// Parsing ETL files is only supported on windows
// We heavily rely on the TDH windows library for the heavy lifting
#ifdef _WIN32

#define INITGUID

#include <windows.h>
#include <stdio.h>
#include <wbemidl.h>
#include <wmistr.h>
#include <evntrace.h>
#include <tdh.h>
#endif

// Include below always such that the read_etl_file stub can get compiled.
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
#include "imgui/imgui_impl_sdl.h"
#include "imgui/imgui_impl_opengl3.h"

#define GPUVIS_TRACE_IMPLEMENTATION
#include "gpuvis_macros.h"

#include "tdopexpr.h"
#include "trace-cmd/trace-read.h"

#ifdef _WIN32
#include "stlini.h"
#include "gpuvis_utils.h"
#include "etl_utils.h"
#include "gpuvis_etl.h"
#include "gpuvis.h"

/**
 * Extract a data member from an ETL trace using TDH
 *
 * This method will extract the property at index 'prop' from pEvent
 *
 * If this property is an array, 'idx' will specify which array element to index
 */
bool tdh_extract_property( PEVENT_RECORD pEvent, PTRACE_EVENT_INFO pInfo, int prop, int idx, int intype, void* &out )
{
    DWORD status = ERROR_SUCCESS;
    USHORT ArraySize = 0;
    PEVENT_MAP_INFO pMapInfo = nullptr;
    PROPERTY_DATA_DESCRIPTOR DataDescriptors[2];
    ULONG DescriptorsCount = 0;
    DWORD PropertySize = 0;
    PBYTE pData = nullptr;

    status = GetArraySize( pEvent, pInfo, prop, &ArraySize );
    if ( status != ERROR_SUCCESS )
    {
        logf( "Failed to extract property: error calculating array size\n" );
        goto error;
    }

    //wprintf( L"%s", (LPWSTR)( (PBYTE)(pInfo)+pInfo->EventPropertyInfoArray[prop].NameOffset ) );

    // We only support simple properties at the moment, no structs
    if ( ( pInfo->EventPropertyInfoArray[prop].Flags & PropertyStruct ) == PropertyStruct ||
         ArraySize != 1)
    {
        logf( "Failed to extract property: complex types unsupported\n" );
        goto error;
    }

    ZeroMemory( &DataDescriptors, sizeof( DataDescriptors ) );
    DataDescriptors[0].PropertyName = (ULONGLONG)( (PBYTE)(pInfo)+pInfo->EventPropertyInfoArray[prop].NameOffset );
    DataDescriptors[0].ArrayIndex = idx;
    DescriptorsCount = 1;

    status = TdhGetPropertySize( pEvent, 0, nullptr, DescriptorsCount, &DataDescriptors[0], &PropertySize );
    if ( status != ERROR_SUCCESS )
    {
        logf( "Failed to extract property: error calculating property size\n" );
        goto error;
    }

    pData = (PBYTE)malloc( PropertySize );
    if ( nullptr == pData )
    {
        logf( "Failed to extract property: error calculating property size\n" );
        goto error;
    }

    status = TdhGetProperty( pEvent, 0, nullptr, DescriptorsCount, &DataDescriptors[0], PropertySize, pData );
    if ( status != ERROR_SUCCESS )
    {
        logf( "Failed to extract property: error retriving property\n" );
        goto error;
    }

    status = GetMapInfo( pEvent,
        (PWCHAR)( (PBYTE)(pInfo)+pInfo->EventPropertyInfoArray[prop].nonStructType.MapNameOffset ),
        pInfo->DecodingSource,
        pMapInfo );
    if ( status != ERROR_SUCCESS )
    {
        logf( "Failed to extract property: retriving map info\n" );
        goto error;
    }

    // verify that the call matches our expected type
    _TDH_IN_TYPE eInfoType = (_TDH_IN_TYPE)pInfo->EventPropertyInfoArray[prop].nonStructType.InType;
    _TDH_IN_TYPE eRequestedType = ( _TDH_IN_TYPE)intype;
    if ( eInfoType != eRequestedType )
    {
        logf( "Failed to extract property: type mismatch\n" );
        goto error;
    }

    out = pData;

    if ( pMapInfo )
        free( pMapInfo );

    return true;

error:
    if ( pData )
        free( pData );
    if ( pMapInfo )
        free( pMapInfo );

    return false;
}

template< typename T, int intype >
bool tdh_extract_property_typed( PEVENT_RECORD pEvent, PTRACE_EVENT_INFO pInfo, int prop, int idx, T &out )
{
    void *pData = nullptr;

    if ( !tdh_extract_property( pEvent, pInfo, prop, idx, intype, pData ) || !pData )
    {
        goto error;
    }

    out = *( (T*)pData );
    free( pData );

    return true;

error:
    if ( pData )
        free( pData );

    return false;
}

template< int intype >
bool tdh_extract_property_typed( PEVENT_RECORD pEvent, PTRACE_EVENT_INFO pInfo, int prop, int idx, std::string &out )
{
    void *pData = nullptr;

    if ( !tdh_extract_property( pEvent, pInfo, prop, idx, intype, pData ) || !pData )
    {
        goto error;
    }

    out = (const char *)pData;
    free( pData );

    return true;

error:
    if ( pData )
        free( pData );

    return false;
}

template< int intype >
bool tdh_extract_property_typed( PEVENT_RECORD pEvent, PTRACE_EVENT_INFO pInfo, int prop, int idx, std::wstring &out )
{
    void *pData = nullptr;

    if ( !tdh_extract_property( pEvent, pInfo, prop, idx, intype, pData ) || !pData )
    {
        goto error;
    }

    out = (const wchar_t *)pData;
    free( pData );

    return true;

error:
    if ( pData )
        free( pData );

    return false;
}

/**
 * Helper for simple extraction from type information
 */
bool tdh_extract_a( PEVENT_RECORD pEvent, PTRACE_EVENT_INFO pInfo, int prop, int idx, std::string &out )
{
    return tdh_extract_property_typed<TDH_INTYPE_ANSISTRING>( pEvent, pInfo, prop, idx, out );
}

bool tdh_extract( PEVENT_RECORD pEvent, PTRACE_EVENT_INFO pInfo, int prop, std::string &out )
{
    return tdh_extract_a( pEvent, pInfo, prop, 0, out );
}

bool tdh_extract_a( PEVENT_RECORD pEvent, PTRACE_EVENT_INFO pInfo, int prop, int idx, std::wstring &out )
{
    return tdh_extract_property_typed<TDH_INTYPE_UNICODESTRING>( pEvent, pInfo, prop, idx, out );
}

bool tdh_extract( PEVENT_RECORD pEvent, PTRACE_EVENT_INFO pInfo, int prop, std::wstring &out )
{
    return tdh_extract_a( pEvent, pInfo, prop, 0, out );
}

bool tdh_extract_a( PEVENT_RECORD pEvent, PTRACE_EVENT_INFO pInfo, int prop, int idx, uint32_t &out )
{
    return tdh_extract_property_typed<uint32_t, TDH_INTYPE_UINT32>( pEvent, pInfo, prop, idx, out );
}

bool tdh_extract( PEVENT_RECORD pEvent, PTRACE_EVENT_INFO pInfo, int prop, uint32_t &out )
{
    return tdh_extract_a( pEvent, pInfo, prop, 0, out );
}

bool tdh_extract_a( PEVENT_RECORD pEvent, PTRACE_EVENT_INFO pInfo, int prop, int idx, uint64_t &out )
{
    return tdh_extract_property_typed<uint64_t, TDH_INTYPE_UINT64>( pEvent, pInfo, prop, idx, out );
}

bool tdh_extract( PEVENT_RECORD pEvent, PTRACE_EVENT_INFO pInfo, int prop, uint64_t &out )
{
    return tdh_extract_a( pEvent, pInfo, prop, 0, out );
}

bool tdh_extract_a( PEVENT_RECORD pEvent, PTRACE_EVENT_INFO pInfo, int prop, int idx, void* &out )
{
    return tdh_extract_property_typed<void*, TDH_INTYPE_POINTER>( pEvent, pInfo, prop, idx, out );
}

bool tdh_extract( PEVENT_RECORD pEvent, PTRACE_EVENT_INFO pInfo, int prop, void* &out )
{
    return tdh_extract_a( pEvent, pInfo, prop, 0, out );
}

 /**
 * etl_reader_t reads a etl file and provides each event as a set of key/value pairs
 *
 * Refer to:
 * https://docs.microsoft.com/en-us/windows/desktop/etw/event-trace-logfile
 * https://docs.microsoft.com/en-us/windows/desktop/etw/using-tdhformatproperty-to-consume-event-data
 */
class etl_reader_t
{
public:
    struct etl_reader_cb_data_t
    {
        void* ctx;
        PEVENT_RECORD event;
        PTRACE_EVENT_INFO info;
    };

    typedef void( *EventCallback )( etl_reader_cb_data_t *cbdata );

    etl_reader_t( const char *file, EventCallback cb, void *ctx )
        : mFileName( file )
        , mTraceHandle( 0 )
        , mParserCallback( cb )
        , mParserCtx( ctx )
    {
    }

    DWORD get_event_info( PEVENT_RECORD pEvent, PTRACE_EVENT_INFO & pInfo )
    {
        DWORD status = ERROR_SUCCESS;
        DWORD BufferSize = 0;

        // Retrieve the required buffer size for the event metadata.

        status = TdhGetEventInformation( pEvent, 0, nullptr, pInfo, &BufferSize );
        if ( ERROR_INSUFFICIENT_BUFFER == status )
        {
            pInfo = (TRACE_EVENT_INFO*)malloc( BufferSize );
            if ( pInfo == nullptr )
            {
                logf( "Failed to allocate memory for event info (size=%lu).\n", BufferSize );
                return ERROR_OUTOFMEMORY;
            }

            // Retrieve the event metadata.

            status = TdhGetEventInformation( pEvent, 0, nullptr, pInfo, &BufferSize );
        }

        if ( ERROR_SUCCESS != status )
        {
            logf( "TdhGetEventInformation failed with 0x%x.\n", status );
        }

        return status;
    }

    bool is_parseable_event( PTRACE_EVENT_INFO info )
    {
        switch ( info->DecodingSource )
        {
        case DecodingSourceWbem:
        case DecodingSourceXMLFile:
            return true;
        default:
            return false;
        }
    }

    static void WINAPI process_event_cb( PEVENT_RECORD event )
    {
        etl_reader_t *ctx = (etl_reader_t *)event->UserContext;
        ctx->process_event( event );
    }

    void process_event( PEVENT_RECORD event )
    {
        DWORD status = ERROR_SUCCESS;
        PTRACE_EVENT_INFO info = nullptr;

        status = get_event_info( event, info );
        if ( ERROR_SUCCESS != status )
        {
            logf( "Failed to get event information failed with %lu\n", status );
            return;
        }

        if ( !is_parseable_event( info ) )
        {
            return;
        }

        etl_reader_cb_data_t cbdata = { 0 };
        cbdata.ctx = mParserCtx;
        cbdata.event = event;
        cbdata.info = info;
        mParserCallback( &cbdata );

        free( info );
    }

    int process()
    {
        TDHSTATUS status = ERROR_SUCCESS;
        EVENT_TRACE_LOGFILE trace;
        TRACE_LOGFILE_HEADER* pHeader = &trace.LogfileHeader;

        ZeroMemory( &trace, sizeof( EVENT_TRACE_LOGFILE ) );
        trace.LogFileName = (char *)mFileName;
        trace.EventRecordCallback = (PEVENT_RECORD_CALLBACK)( process_event_cb );
        trace.ProcessTraceMode = PROCESS_TRACE_MODE_EVENT_RECORD;
        trace.Context = this;

        mTraceHandle = OpenTrace( &trace );
        if ( INVALID_PROCESSTRACE_HANDLE == mTraceHandle )
        {
            logf( "Failed to open etl trace %s: %lu\n", mFileName, GetLastError() );
            return -1;
        }

        mIsUserTrace = pHeader->LogFileMode & EVENT_TRACE_PRIVATE_LOGGER_MODE;
        logf( "Number of events lost:  %lu\n", pHeader->EventsLost );
        logf( "Number of buffers lost: %lu\n", pHeader->BuffersLost );

        status = ProcessTrace( &mTraceHandle, 1, 0, 0 );
        if ( status != ERROR_SUCCESS && status != ERROR_CANCELLED )
        {
            logf( "Failed to process trace: %lu\n", status );
            return -1;
        }

        logf( "Loading OK\n" );

        return 0;
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

        //if ( !mFileStream )
        return map;

        std::string entry;
        //if ( !std::getline( mFileStream, entry ) )
        return map;

        std::istringstream event( entry );

        std::string key, val;
        while ( parse_entry( event, key, val ) )
        {
            map[key] = val;
        }

        // Save the original text for error handling
        map["etl_line"] = entry;

        return map;
    }

private:
    const char *mFileName;
    TRACEHANDLE mTraceHandle;
    bool mIsUserTrace;
    EventCallback mParserCallback;
    void * mParserCtx;
};

/**
 * Extract the i'th property into a variable by reference
 */
#define ETL_EXTRACT( i, ref ) tdh_extract( cbdata->event, cbdata->info, i, ref )

/**
 * Dump all properties to figure out what is needed
 */
#define ETL_DUMP() DumpEventMetadata( cbdata->info ); DumpProperties( cbdata->event, cbdata->info )

/**
 * The x_entry_t classes are helpers to interpret the etl data as c++ types
 */
class context_entry_t
{
public:
    std::wstring file;
    std::string os_version;
    uint32_t num_cpu;
    uint64_t start_time;
    uint64_t end_time;

    context_entry_t( etl_reader_t::etl_reader_cb_data_t *cbdata )
    {
        os_version = "windows";
        ETL_EXTRACT( 22, file );
        ETL_EXTRACT(  3, num_cpu );
        ETL_EXTRACT( 18, start_time );
        ETL_EXTRACT(  4, end_time );
    }
};

class event_entry_t
{
public:
    uint64_t ts;
    int cpu;
    int pid;
    int tid;
    std::string pname;

    event_entry_t( etl_reader_t::etl_reader_cb_data_t *cbdata )
    {
        PTRACE_EVENT_INFO pinfo = cbdata->info;
        EVENT_HEADER *header = &cbdata->event->EventHeader;
        ts = header->TimeStamp.QuadPart;
        cpu = cbdata->event->BufferContext.ProcessorNumber;
        pid = header->ProcessId;
        tid = header->ThreadId;
        pname = "process"; //TODO
    }
};

class steamvr_entry_t : public event_entry_t
{
public:
    std::string vrevent;

    steamvr_entry_t( etl_reader_t::etl_reader_cb_data_t *cbdata ) :
        event_entry_t( cbdata )
    {
        ETL_EXTRACT( 0, vrevent );
    }
};

class vsync_entry_t : public event_entry_t
{
public:
    void *adapter;
    uint32_t display;
    uint64_t address;

    vsync_entry_t( etl_reader_t::etl_reader_cb_data_t *cbdata ) :
        event_entry_t( cbdata )
    {
        ETL_EXTRACT( 0, adapter );
        ETL_EXTRACT( 1, display );
        ETL_EXTRACT( 2, address );
    }
};

class queue_packet_header_entry_t : public event_entry_t
{
public:
    void *ctx;
    uint32_t ptype;
    uint32_t seq;

    queue_packet_header_entry_t( etl_reader_t::etl_reader_cb_data_t *cbdata ) :
        event_entry_t( cbdata )
    {
        ETL_EXTRACT( 0, ctx );
        ETL_EXTRACT( 1, ptype );
        ETL_EXTRACT( 2, seq );
    }
};

class dma_packet_header_entry_t : public event_entry_t
{
public:
    void *ctx;
    void *qctx;
    uint32_t ptype;
    uint32_t submit_seq;
    uint32_t seq;

    dma_packet_header_entry_t( etl_reader_t::etl_reader_cb_data_t *cbdata ) :
        event_entry_t( cbdata )
    {
        int i = 0;
        UCHAR opcode = cbdata->event->EventHeader.EventDescriptor.Opcode;

        ETL_EXTRACT( i++, ctx );

        // Field only present in the start packet
        if ( opcode == EVENT_TRACE_TYPE_START )
            ETL_EXTRACT( i++, qctx );
        else
            qctx = nullptr;

        ETL_EXTRACT( i++, ptype );
        ETL_EXTRACT( i++, submit_seq );
        ETL_EXTRACT( i++, seq );
    }
};

/**
 * Parses the ETL information stream
 *
 * The ETL input stream is converted into a trace_info_t + a sequence of trace_event_t
 */
class etl_parser_t
{
private:
    class __declspec( uuid( "{3baa334f-c49b-4a90-b796-f7642dac0656}" ) ) kSteamVrProvider;
    class __declspec( uuid( "{802ec45a-1e99-4b83-9920-87c98277ba9d}" ) ) kDxcProvider;

    // Get these from Microsoft-Windows-DxgKrnl.manifest.xml
    static const int kDxcVsyncTaskId = 10;
    static const int kDxcQueuePacketTaskId = 9;
    static const int kDxcDmaPacketTaskId = 8;

public:
    // Forward the callback to the right object
    static void process_event_cb_proxy( etl_reader_t::etl_reader_cb_data_t *cbdata )
    {
        etl_parser_t *pthis = (etl_parser_t *)cbdata->ctx;
        pthis->process_event_cb( cbdata );
    }

    etl_parser_t( const char *file, StrPool &strpool, trace_info_t &trace_info, EventCallback &cb )
        : mFileName( file )
        , mStrPool( strpool )
        , mTraceInfo( trace_info )
        , mCallback( cb )
        , mReader( file,  process_event_cb_proxy, this )
        , mStartTicks( 0 )
        , mAdapterCount( 0 )
        , mCrtcCount( 0 )
    {
        memset( mCrtcCurrentSeq, 0, sizeof( mCrtcCurrentSeq ) );
    }

    int process()
    {
        int err;

        err = mReader.process();
        if ( err )
        {
            return err;
        }

         return 0;
    }

    int process_event_cb( etl_reader_t::etl_reader_cb_data_t *cbdata )
    {
        PEVENT_RECORD event = cbdata->event;
        PTRACE_EVENT_INFO info = cbdata->info;
        GUID *providerGuid = &event->EventHeader.ProviderId;
        UCHAR opcode = event->EventHeader.EventDescriptor.Opcode;
        USHORT task = event->EventHeader.EventDescriptor.Task;
        int ret = -1;

        // Trace events provide context information
        if ( IsEqualGUID( *providerGuid, EventTraceGuid ) )
        {    
            switch ( opcode )
            {
            case EVENT_TRACE_TYPE_INFO:
                ret = process_context_entry( context_entry_t( cbdata ) );
                break;
            }
        }
        // SteamVR is a known user provider that generates events
        else if ( IsEqualGUID( *providerGuid, __uuidof( kSteamVrProvider ) ) )
        {
            switch ( opcode )
            {
            case EVENT_TRACE_TYPE_INFO:
                ret = process_steamvr_entry( steamvr_entry_t( cbdata ) );
                break;
            }
        }
        // The DX driver has a lot of interesting information
        else if ( IsEqualGUID( *providerGuid, __uuidof( kDxcProvider ) ) )
        {
            switch ( task )
            {
            case kDxcVsyncTaskId:
                switch ( opcode )
                {
                case EVENT_TRACE_TYPE_INFO:
                    ret = process_vsync_entry( vsync_entry_t( cbdata ) );
                    break;
                }
                break;
            case kDxcQueuePacketTaskId:
                ret = process_queue_packet_entry( cbdata );
                break;
            case kDxcDmaPacketTaskId:
                ret = process_dma_packet_entry( cbdata );
                break;
            }
        }
        else
        {
            //DumpEventMetadata( info );
            //DumpProperties( event, info );
        }

        return ret;
    }

private:
    static const int kMaxCrtc = 32;

    const char *mFileName;
    StrPool &mStrPool;
    trace_info_t &mTraceInfo;
    EventCallback &mCallback;

    etl_reader_t mReader;
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

    std::string sfromws( std::wstring ws)
    {
        return std::string( ws.begin(), ws.end() );
    }

    int process_context_entry( context_entry_t entry )
    {
        mStartTicks = entry.start_time;

        mTraceInfo.cpus = entry.num_cpu;
        mTraceInfo.file = sfromws(entry.file);
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
        int adapter = GetAdapterIdx( (uint64_t)entry.adapter );
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

    int process_queue_packet_entry( etl_reader_t::etl_reader_cb_data_t *cbdata )
    {
        int err = -1;
        trace_event_t event;
        std::string timeline = "";
        queue_packet_header_entry_t header( cbdata );
        UCHAR opcode = cbdata->event->EventHeader.EventDescriptor.Opcode;

        switch ( header.ptype )
        {
        case DXGKETW_RENDER_COMMAND_BUFFER:
        case DXGKETW_DEFERRED_COMMAND_BUFFER:
        case DXGKETW_SYSTEM_COMMAND_BUFFER:
            timeline = "gfx";
            break;
        default:
            //ETL_DUMP();
            return -1;
        }

        switch ( opcode )
        {
        case EVENT_TRACE_TYPE_START:
            // Packet was received by the scheduler
            event.name = mStrPool.getstr( "amdgpu_cs_ioctl" ); // For dat compatibility
            event.flags = TRACE_FLAG_SW_QUEUE;
            break;
        case EVENT_TRACE_TYPE_INFO:
            // Begin move to HW queue? Use DmaPacket/Start instead
            return 0;
        case EVENT_TRACE_TYPE_STOP:
            // Packet is no longer in use by the driver, don't care
            return 0;
        default:
            return 0;
        }

        err = process_event_entry( header, event );
        if ( err )
            return err;

        event.system = mStrPool.getstr( "QueuePacket" );

        event.numfields = 3;
        event.fields = new event_field_t[event.numfields];
        event.fields[0].key = mStrPool.getstr( "timeline" );
        event.fields[0].value = mStrPool.getstr( timeline.c_str() );
        event.fields[1].key = mStrPool.getstr( "context" );
        event.fields[1].value = mStrPool.getstrf( "0x%xll", header.ctx );
        event.fields[ 2 ].key = mStrPool.getstr( "seq" );
        event.fields[ 2 ].value = mStrPool.getstrf( "%u", header.seq );
        event.seqno = header.seq;

        return mCallback( event );
    }

    int process_dma_packet_entry( etl_reader_t::etl_reader_cb_data_t *cbdata )
    {
        int err = -1;
        trace_event_t event;
        dma_packet_header_entry_t header( cbdata );
        std::string timeline = "gfx";
        UCHAR opcode = cbdata->event->EventHeader.EventDescriptor.Opcode;

        switch ( opcode )
        {
        case EVENT_TRACE_TYPE_START:
            // Submit to the HW engine
            event.name = mStrPool.getstr( "amdgpu_sched_run_job" ); // For dat compatibility
            event.flags = TRACE_FLAG_HW_QUEUE;
            break;
        case EVENT_TRACE_TYPE_INFO:
            // Finished processing by the GPU ISR
            event.name = mStrPool.getstr( "fence_signaled" ); // For dat compatibility
            event.flags = TRACE_FLAG_FENCE_SIGNALED;
            break;
        default:
            return 0;
        }

        err = process_event_entry( header, event );
        if ( err )
            return err;

        event.system = mStrPool.getstr( "QueuePacket" );

        event.numfields = 3;
        event.fields = new event_field_t[ event.numfields ];
        event.fields[ 0 ].key = mStrPool.getstr( "timeline" );
        event.fields[ 0 ].value = mStrPool.getstr( timeline.c_str() );
        event.fields[ 1 ].key = mStrPool.getstr( "context" );
        event.fields[ 1 ].value = mStrPool.getstrf( "0x%xll", header.ctx );
        event.fields[ 2 ].key = mStrPool.getstr( "seq" );
        event.fields[ 2 ].value = mStrPool.getstrf( "%u", header.seq );
        event.seqno = header.seq;

        return mCallback( event );
    }
};

int read_etl_file( const char *file, StrPool &strpool, trace_info_t &trace_info, EventCallback &cb )
{
    etl_parser_t parser( file, strpool, trace_info, cb );
    return parser.process();
}

#else

// Stub implementation for non-windows OSs
int read_etl_file( const char *file, StrPool &strpool, trace_info_t &trace_info, EventCallback &cb )
{
    return -1;
}
#endif
