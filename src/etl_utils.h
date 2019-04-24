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

#ifndef ETL_UTILS_H_
#define ETL_UTILS_H_

#ifdef _WIN32

#include <windows.h>
#include <tdh.h>

DWORD DumpEventMetadataField( TRACE_EVENT_INFO* pinfo, DWORD i, USHORT indent );
DWORD DumpEventMetadata( PTRACE_EVENT_INFO info );
DWORD DumpEvent( PEVENT_RECORD pEvent, PTRACE_EVENT_INFO pInfo );
DWORD DumpPropertiesIndex( PEVENT_RECORD pEvent, PTRACE_EVENT_INFO pInfo, USHORT i, LPWSTR pStructureName, USHORT StructIndex );
DWORD DumpProperties( PEVENT_RECORD pEvent, PTRACE_EVENT_INFO pInfo );
DWORD FormatAndPrintData( PEVENT_RECORD pEvent, USHORT InType, USHORT OutType, PBYTE pData, DWORD DataSize, PEVENT_MAP_INFO pMapInfo );
void PrintMapString( PEVENT_MAP_INFO pMapInfo, PBYTE pData );
DWORD GetPropertyLength( PEVENT_RECORD pEvent, PTRACE_EVENT_INFO pInfo, USHORT i, PUSHORT PropertyLength );
DWORD GetArraySize( PEVENT_RECORD pEvent, PTRACE_EVENT_INFO pInfo, USHORT i, PUSHORT ArraySize );
DWORD GetMapInfo( PEVENT_RECORD pEvent, LPWSTR pMapName, DWORD DecodingSource, PEVENT_MAP_INFO & pMapInfo );
void RemoveTrailingSpace( PEVENT_MAP_INFO pMapInfo );
#endif //_WIN32

#endif // ETL_UTILS_H_
