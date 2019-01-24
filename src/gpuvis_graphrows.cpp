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
#include <stdio.h>
#include <string.h>

#include <array>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <string>

#include <SDL.h>

#include "imgui/imgui.h"
#include "gpuvis_macros.h"
#include "stlini.h"
#include "trace-cmd/trace-read.h"
#include "gpuvis_utils.h"
#include "gpuvis.h"

/*
 * GraphRows
 */
const std::vector< GraphRows::graph_rows_info_t > GraphRows::get_hidden_rows_list()
{
    std::vector< graph_rows_info_t > hidden_rows;

    for ( const graph_rows_info_t &rinfo : m_graph_rows_list )
    {
        if ( rinfo.hidden )
            hidden_rows.push_back( rinfo );
    }

    return hidden_rows;
}

void GraphRows::show_tgid_rows( const tgid_info_t *tgid_info, graph_rows_show_t show )
{
    for ( int pid : tgid_info->pids )
    {
        const char *comm = m_trace_events->comm_from_pid( pid );

        if ( comm )
            show_row( comm, show );
    }
}

void GraphRows::show_row( const std::string &name, graph_rows_show_t show )
{
    if ( show == GraphRows::SHOW_ALL_ROWS )
    {
        m_graph_rows_hide.clear();

        for ( graph_rows_info_t &row_info : m_graph_rows_list )
            row_info.hidden = false;
    }
    else if ( show == GraphRows::SHOW_ROW )
    {
        // Remove this name from the graph_rows_hide array
        auto idx = std::find( m_graph_rows_hide.begin(), m_graph_rows_hide.end(), name );
        if ( idx != m_graph_rows_hide.end() )
            m_graph_rows_hide.erase( idx );

        // Mark row as visible
        graph_rows_info_t *prow = get_row( name );
        if ( prow )
            prow->hidden = false;
    }
    else if ( ( show == GraphRows::HIDE_ROW ) ||
              ( show == GraphRows::HIDE_ROW_AND_ALL_BELOW ) )
    {
        for ( size_t i = find_row( name ); i < m_graph_rows_list.size(); i++ )
        {
            // Add entry to the graph_rows_hide array
            auto idx = std::find( m_graph_rows_hide.begin(),
                                  m_graph_rows_hide.end(), m_graph_rows_list[ i ].row_name );
            if ( idx == m_graph_rows_hide.end() )
                m_graph_rows_hide.push_back( m_graph_rows_list[ i ].row_name );

            // Mark this graph_row as hidden
            m_graph_rows_list[ i ].hidden = true;

            if ( show != GraphRows::HIDE_ROW_AND_ALL_BELOW )
                break;
        }
    }
}

static int pid_from_row_name( const std::string &row_name )
{
    const char *dash = strrchr( row_name.c_str(), '-' );

    return dash ? atoi( dash + 1 ) : -1;
}

class row_cmp_t
{
public:
    row_cmp_t( TraceEvents &trace_events ) : m_trace_events( trace_events ) {}
    ~row_cmp_t() {}

    uint64_t get_comm_index( const char *row_name ) const
    {
        static const char *comms_loprio[] =
        {
            "<...>",            // lowest priority
            "trace-cmd",
            "snapd",
            "sh-",
            "kill-",
            "pidof-",
            "sleep-",
            "xfce4-",
            "watchdog",
            "chrome",
            "ksoftirqd",
            "kworker",
            "gpu-trace",
            "mysqld",           // medium priority
        };
        static const char *comms_hiprio[] =
        {
            "steam",           // high priority
            "gfx",
            "amdgpu",
            "sdma",
            "vrcompositor",
            "vrdashboard",
            "vrserver",
            "vrmonitor",
            "GlobPool",
            "QXcbEventReader",
            "UIEngineAnimati",
            "UIEngineRenderT",
            "Connection",
            "LightHouse",
            "VKRenderThread",
            "RenderThread",     // highest priority
        };

        for ( size_t i = 0; i < ARRAY_SIZE( comms_hiprio ); i++ )
        {
            if ( !strncasecmp( comms_hiprio[ i ], row_name, strlen( comms_hiprio[ i ] ) ) )
                return ARRAY_SIZE( comms_loprio ) + i + 2;
        }
        for ( size_t i = 0; i < ARRAY_SIZE( comms_loprio ); i++ )
        {
            if ( !strncasecmp( comms_loprio[ i ], row_name, strlen( comms_loprio[ i ] ) ) )
                return i + 1;
        }
        return ARRAY_SIZE( comms_loprio ) + 1;
    }

    uint64_t get_row_val( const GraphRows::graph_rows_info_t &ri ) const
    {
        // 0xffffff ffff ffffff
        //     tgid comm  count

        // 24 low bits for event count
        uint64_t event_count = std::min< uint32_t >( ri.event_count, 0xffffff );

        // If we have tgid info, group all those together at highest prio
        int pid = pid_from_row_name( ri.row_name );
        const tgid_info_t *tgid_info = m_trace_events.tgid_from_pid( pid );

        if ( tgid_info )
        {
            // 24 high bits for tgid
            uint32_t tgid = std::min< uint32_t >( tgid_info->tgid, 0x7fffff );
            // main process gets +1 so it's shown first
            uint64_t val = ( tgid << 1 ) + ( tgid == ( uint32_t )pid );

            return event_count + ( val << 40 );
        }

        // Group by whatever comm index we come up with
        return event_count + ( get_comm_index( ri.row_name.c_str() ) << 24 );
    }

    bool operator()( const GraphRows::graph_rows_info_t &lx,
                     const GraphRows::graph_rows_info_t &rx ) const
    {
        uint64_t lval = get_row_val( lx );
        uint64_t rval = get_row_val( rx );

        return ( rval < lval );
    }

public:
    TraceEvents &m_trace_events;
};

static void
save_umap_ini_entries( const util_umap< std::string, std::string > &blah,
                       const char *section )
{
    uint32_t num = 0;

    s_ini().ClearSection( section );

    for ( const auto &i : blah.m_map )
    {
        char key[ 32 ];
        const std::string &val = i.first + "\t" + i.second;

        snprintf_safe( key, "%02u", num++ );
        s_ini().PutStr( key, val.c_str(), section );
    }
}

static const std::vector< std::pair< std::string, std::string > >
read_umap_ini_entries( const char *section )
{
    std::vector< std::pair< std::string, std::string > > ret;
    std::vector< INIEntry > entries = s_ini().GetSectionEntries( section );

    for ( const INIEntry &entry : entries )
    {
        const std::vector< std::string > args = string_explode( entry.second, '\t' );

        if ( args.size() == 2 )
            ret.push_back( { args[ 0 ], args[ 1 ] } );
    }

    return ret;
}

// Initialize m_graph_rows_list
void GraphRows::init( TraceEvents &trace_events )
{
    if ( !m_graph_rows_list.empty() )
        return;

    m_trace_events = &trace_events;

    // Order: gfx -> compute -> gfx hw -> compute hw -> sdma -> sdma hw
    loc_type_t type;
    const std::vector< uint32_t > *plocs;

    // AMD gpu events
    {
        if ( ( plocs = trace_events.get_locs( "gfx", &type ) ) )
            push_row( "gfx", type, plocs->size() );

        // Andres: full list of compute rings is comp_[1-2].[0-3].[0-8]
        for ( int c0 = 1; c0 < 3; c0++)
        {
            for ( int c1 = 0; c1 < 4; c1++ )
            {
                for ( int c2 = 0; c2 < 9; c2++ )
                {
                    std::string str = string_format( "comp_%d.%d.%d", c0, c1, c2 );

                    if ( ( plocs = trace_events.get_locs( str.c_str(), &type ) ) )
                        push_row( str, type, plocs->size() );
                }
            }
        }

        if ( ( plocs = trace_events.get_locs( "gfx hw", &type ) ) )
            push_row( "gfx hw", type, plocs->size() );

        for ( int c0 = 1; c0 < 3; c0++)
        {
            for ( int c1 = 0; c1 < 4; c1++ )
            {
                for ( int c2 = 0; c2 < 9; c2++ )
                {
                    std::string str = string_format( "comp_%d.%d.%d hw", c0, c1, c2 );

                    if ( ( plocs = trace_events.get_locs( str.c_str(), &type ) ) )
                        push_row( str, type, plocs->size() );
                }
            }
        }

        if ( ( plocs = trace_events.get_locs( "sdma0", &type ) ) )
            push_row( "sdma0", type, plocs->size() );
        if ( ( plocs = trace_events.get_locs( "sdma1", &type ) ) )
            push_row( "sdma1", type, plocs->size() );

        if ( ( plocs = trace_events.get_locs( "sdma0 hw", &type ) ) )
            push_row( "sdma0 hw", type, plocs->size() );
        if ( ( plocs = trace_events.get_locs( "sdma1 hw", &type ) ) )
            push_row( "sdma1 hw", type, plocs->size() );
    }

    // Intel gpu events
    {
        for ( auto &req_locs : trace_events.m_i915.req_locs.m_locs.m_map )
        {
            std::vector< uint32_t > &locs = req_locs.second;
            const char *name = trace_events.m_strpool.findstr( req_locs.first );

            push_row( name, LOC_TYPE_i915Request, locs.size() );
        }

        for ( auto &req_locs : trace_events.m_i915.reqwait_end_locs.m_locs.m_map )
        {
            std::vector< uint32_t > &locs = req_locs.second;
            const char *name = trace_events.m_strpool.findstr( req_locs.first );

            push_row( name, LOC_TYPE_i915RequestWait, locs.size() );
        }
    }

    if ( ( plocs = trace_events.get_locs( "cpu graph", &type ) ) )
    {
        push_row( "cpu graph", type, plocs->size(), false );
    }

    if ( ( plocs = trace_events.get_locs( "print", &type ) ) )
    {
        push_row( "print", type, plocs->size(), true );

        auto add_print_row_lambda = [&]( const char *str, int pid, bool hidden_def )
        {
            std::string name = string_format( "print %s:%d", str, pid );
            const char *comm = trace_events.comm_from_pid( pid );

            if ( comm )
            {
                const char *pidstr = strrchr( comm, '-' );
                int len = pidstr ? ( pidstr - comm ) : 64;

                name += string_format( " (%.*s)", len, comm );
            }

            bool hidden = !s_ini().GetInt( name.c_str(), hidden_def ? 0.0f : 1.0f, "$row_sizes$" );

            push_row( name, type, plocs->size(), hidden );
        };

        for ( auto item : m_trace_events->m_ftrace.row_info.m_map )
        {
            if ( item.second.pid == -1 )
                continue;

            if ( item.second.pid )
                add_print_row_lambda( "pid", item.second.pid, false );
            else
                add_print_row_lambda( "tgid", item.second.tgid, true );
        }
    }

    {
        std::vector< INIEntry > entries = s_ini().GetSectionEntries( "$graph_plots$" );

        for ( const INIEntry &entry : entries )
        {
            const std::string &plot_name = entry.first;
            const std::vector< std::string > plot_args = string_explode( entry.second, '\t' );

            if ( plot_args.size() == 2 )
            {
                const std::string &plot_filter = plot_args[ 0 ];
                const std::string &plot_scanf = plot_args[ 1 ];

                plocs = trace_events.get_locs( plot_filter.c_str() );
                if ( plocs )
                {
                    GraphPlot &plot = trace_events.get_plot( plot_name.c_str() );

                    if ( plot.init( trace_events, plot_name, plot_filter, plot_scanf ) )
                        push_row( plot_name, LOC_TYPE_Plot, plot.m_plotdata.size() );
                }
            }
        }
    }

    std::vector< graph_rows_info_t > comms;
    for ( auto item : trace_events.m_comm_locs.m_locs.m_map )
    {
        uint32_t hashval = item.first;
        const char *comm = trace_events.m_strpool.findstr( hashval );

        comms.push_back( { false, LOC_TYPE_Comm, comm, comm, item.second.size() } );
    }

    // Sort by tgids, count of events, and comm name...
    row_cmp_t row_cmp( trace_events );
    std::sort( comms.begin(), comms.end(), row_cmp );

    // Add the sorted comm events to our m_graph_rows_list array
    m_graph_rows_list.insert( m_graph_rows_list.end(), comms.begin(), comms.end() );

    std::string graph_rows_hide_str = s_ini().GetStr( "graph_rows_hide_str", "" );
    if ( !graph_rows_hide_str.empty() )
    {
        m_graph_rows_hide = string_explode( graph_rows_hide_str, '\t' );

        for ( graph_rows_info_t &row_info : m_graph_rows_list )
        {
            auto idx = std::find( m_graph_rows_hide.begin(), m_graph_rows_hide.end(), row_info.row_name );

            if ( idx != m_graph_rows_hide.end() )
                row_info.hidden = true;
        }
    }

    // Check for added rows
    {
        auto inientries = read_umap_ini_entries( "$graph_rows_add$" );

        for ( const auto &entry : inientries )
        {
            add_row( entry.first, entry.second, 1.0f );
        }
    }

    // Check for row order
    {
        auto inientries = read_umap_ini_entries( "$graph_rows_move_after$" );

        // Do the moves twice to handle move dependencies
        for ( int i = 0; i < 2; i++ )
        {
            for ( const auto &entry : inientries )
            {
                move_row( entry.first, entry.second );
            }
        }
    }

    // check for row time scaling
    {
        auto inientries = read_umap_ini_entries( "$graph_rows_scale_ts$" );

        for ( const auto &entry : inientries )
        {
            m_graph_row_scale_ts.m_map[ entry.first ] = entry.second;
        }
    }
}

void GraphRows::shutdown()
{
    for ( auto it = m_graph_rows_hide.begin(); it != m_graph_rows_hide.end(); )
    {
        // Erase any added rows that the user has hidden. We
        //   store the filters so they should be easy to recreate.
        if ( m_graph_rows_add.erase_key( *it ) )
        {
            m_graph_row_scale_ts.erase_key( *it );

            // Don't erase move information...
            // m_graph_rows_move.erase_key( *it );

            it = m_graph_rows_hide.erase( it );
        }
        else
        {
            it++;
        }
    }

    std::string str = string_implode( m_graph_rows_hide, "\t" );
    s_ini().PutStr( "graph_rows_hide_str", str.c_str() );

    save_umap_ini_entries( m_graph_rows_add, "$graph_rows_add$" );
    save_umap_ini_entries( m_graph_rows_move, "$graph_rows_move_after$" );
    save_umap_ini_entries( m_graph_row_scale_ts, "$graph_rows_scale_ts$" );
}

void GraphRows::add_row( const std::string &name_in, const std::string &filter_expr, float scale )
{
    loc_type_t type;
    std::string name = name_in;
    const std::vector< uint32_t > *plocs = m_trace_events->get_locs( filter_expr.c_str(), &type );
    size_t event_count = plocs ? plocs->size() : 0;

    if ( type == LOC_TYPE_Tdopexpr )
    {
        graph_rows_info_t *row = get_row( name );

        // If this is a user created row, make sure they're not overwriting an existing row
        if ( row && ( row->type != type ) )
            name += "_2";
    }

    if ( type == LOC_TYPE_Plot )
    {
        GraphPlot &plot = m_trace_events->get_plot( name.c_str() );
        std::string val = string_format( "%s\t%s",
                plot.m_filter_str.c_str(), plot.m_scanf_str.c_str() );

        s_ini().PutStr( name.c_str(), val.c_str(), "$graph_plots$" );
    }
    else
    {
        // Add expression to our added rows list
        m_graph_rows_add.m_map[ name ] = filter_expr;
    }

    // Set time scale
    m_graph_row_scale_ts.m_map[ name ] = string_format( "%.2f", scale );

    size_t idx = find_row( name );
    if ( idx != ( size_t )-1 )
    {
        graph_rows_info_t &row = m_graph_rows_list[ idx ];

        row.row_filter_expr = filter_expr;
        row.type = type;
        row.event_count = event_count;
        row.hidden = false;
    }
    else if ( type == LOC_TYPE_Plot )
    {
        size_t print_row_index = find_row( "print", m_graph_rows_list.size() - 1 );
        auto it = m_graph_rows_list.begin() + print_row_index + 1;

        m_graph_rows_list.insert( it,
            { false, LOC_TYPE_Plot, name, filter_expr, event_count } );
    }
    else
    {
        for ( size_t i = 0; i < m_graph_rows_list.size(); i++ )
        {
            // Add this new filter expression before the first comm / tdop expression event we find
            if ( m_graph_rows_list[ i ].type == LOC_TYPE_Tdopexpr ||
                 m_graph_rows_list[ i ].type == LOC_TYPE_Comm )
            {
                m_graph_rows_list.insert( m_graph_rows_list.begin() + i,
                    { false, type, name, filter_expr, event_count } );
                return;
            }
        }

        // Just add to the end.
        m_graph_rows_list.push_back( { false, type, name, filter_expr, event_count } );
    }
}

void GraphRows::move_row( const std::string &name_src, const std::string &name_dest )
{
    size_t index_src = find_row( name_src );
    size_t index_dest = find_row( name_dest );

    if ( ( index_dest != ( size_t )-1 ) &&
         ( index_src != ( size_t )-1 ) &&
         ( index_src != index_dest ) )
    {
        m_graph_rows_move.m_map[ name_src ] = name_dest;

        m_graph_rows_list.insert( m_graph_rows_list.begin() + index_dest + 1,
                                  m_graph_rows_list[ index_src ] );

        m_graph_rows_list.erase( m_graph_rows_list.begin() + index_src + ( index_src > index_dest ) );
    }
}

// Search in m_graph_rows_list for name. Returns index or -1 if not found.
size_t GraphRows::find_row( const std::string &name, size_t not_found_val )
{
    auto lambda_name_cmp = [ &name ]( const GraphRows::graph_rows_info_t& row_info )
                                        { return row_info.row_name == name; };
    auto idx = std::find_if( m_graph_rows_list.begin(), m_graph_rows_list.end(), lambda_name_cmp );

    return ( idx != m_graph_rows_list.end() ) ? ( idx - m_graph_rows_list.begin() ) : not_found_val;
}

GraphRows::graph_rows_info_t *GraphRows::get_row( const std::string &name )
{
    auto idx = find_row( name );

    return ( idx == ( size_t )-1 ) ? NULL : &m_graph_rows_list[ idx ];
}

float GraphRows::get_row_scale_ts( const std::string &name )
{
    const std::string *scale_ts_str = m_graph_row_scale_ts.get_val( name );

    return scale_ts_str ? atof( scale_ts_str->c_str() ) : 1.0f;
}
