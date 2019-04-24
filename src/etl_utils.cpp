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

#ifdef _WIN32

#define INITGUID

#include <windows.h>
#include <stdio.h>
#include <strsafe.h>
#include <wbemidl.h>
#include <wmistr.h>
#include <evntrace.h>
#include <tdh.h>

#include "etl_utils.h"

/**
 * The following section contains some ETL helper functions based on the MS documentation examples
 *
 * Refer to:
 * https://docs.microsoft.com/en-us/windows/desktop/etw/using-tdhgetproperty-to-consume-event-data
 * https://docs.microsoft.com/en-us/windows/desktop/etw/using-tdhformatproperty-to-consume-event-data
 */


DWORD DumpEventMetadataField( TRACE_EVENT_INFO* pinfo, DWORD i, USHORT indent )
{
    DWORD status = ERROR_SUCCESS;
    USHORT j = 0;
    DWORD lastMember = 0;  // Last member of a structure

    // Print property name.

    wprintf( L"%*s%s[%d]", indent, L"", (LPWSTR)( (PBYTE)(pinfo)+pinfo->EventPropertyInfoArray[i].NameOffset ), i );

    // If the property is an array, the property can define the array size or it can
    // point to another property whose value defines the array size. The PropertyParamCount
    // flag tells you where the array size is defined.

    if ( ( pinfo->EventPropertyInfoArray[i].Flags & PropertyParamCount ) == PropertyParamCount )
    {
        j = pinfo->EventPropertyInfoArray[i].countPropertyIndex;
        wprintf( L" (array size is defined by %s)", (LPWSTR)( (PBYTE)(pinfo)+pinfo->EventPropertyInfoArray[j].NameOffset ) );
    }
    else
    {
        if ( pinfo->EventPropertyInfoArray[i].count > 1 )
            wprintf( L" (array size is %lu)", pinfo->EventPropertyInfoArray[i].count );
    }


    // If the property is a buffer, the property can define the buffer size or it can
    // point to another property whose value defines the buffer size. The PropertyParamLength
    // flag tells you where the buffer size is defined.

    if ( ( pinfo->EventPropertyInfoArray[i].Flags & PropertyParamLength ) == PropertyParamLength )
    {
        j = pinfo->EventPropertyInfoArray[i].lengthPropertyIndex;
        wprintf( L" (size is defined by %s)", (LPWSTR)( (PBYTE)(pinfo)+pinfo->EventPropertyInfoArray[j].NameOffset ) );
    }
    else
    {
        // Variable length properties such as structures and some strings do not have
        // length definitions.

        if ( pinfo->EventPropertyInfoArray[i].length > 0 )
            wprintf( L" (size is %lu bytes)", pinfo->EventPropertyInfoArray[i].length );
        else
            wprintf( L" (size  is unknown)" );
    }

    wprintf( L"\n" );


    // If the property is a structure, print the members of the structure.

    if ( ( pinfo->EventPropertyInfoArray[i].Flags & PropertyStruct ) == PropertyStruct )
    {
        wprintf( L"%*s(The property is a structure and has the following %hu members:)\n", 4, L"",
            pinfo->EventPropertyInfoArray[i].structType.NumOfStructMembers );

        lastMember = pinfo->EventPropertyInfoArray[i].structType.StructStartIndex +
            pinfo->EventPropertyInfoArray[i].structType.NumOfStructMembers;

        for ( j = pinfo->EventPropertyInfoArray[i].structType.StructStartIndex; j < lastMember; j++ )
        {
            DumpEventMetadataField( pinfo, j, indent + 4 );
        }
    }
    else
    {
        // You can use InType to determine the data type of the member and OutType
        // to determine the output format of the data.

        if ( pinfo->EventPropertyInfoArray[i].nonStructType.MapNameOffset )
        {
            // You can pass the name to the TdhGetEventMapInformation function to 
            // retrieve metadata about the value map.

            wprintf( L"%*s(Map attribute name is %s)\n", indent, L"",
                (PWCHAR)( (PBYTE)(pinfo)+pinfo->EventPropertyInfoArray[i].nonStructType.MapNameOffset ) );
        }
    }

    return status;
}

DWORD DumpEventMetadata( PTRACE_EVENT_INFO info )
{
    if ( info->TopLevelPropertyCount > 0 )
    {
        wprintf( L"\nThe following are the user data properties defined for this event:\n" );

        for ( USHORT i = 0; i < info->TopLevelPropertyCount; i++ )
        {
            DWORD status = DumpEventMetadataField( info, i, 0 );
            if ( ERROR_SUCCESS != status )
            {
                wprintf( L"Printing metadata for top-level properties failed.\n" );
                return -1;
            }
        }
    }
    else
    {
        wprintf( L"\nThe event does not define any user data properties.\n" );
    }
}

// Get the size of the array. For MOF-based events, the size is specified in the declaration or using 
// the MAX qualifier. For manifest-based events, the property can specify the size of the array
// using the count attribute. The count attribue can specify the size directly or specify the name 
// of another property in the event data that contains the size.

DWORD GetArraySize( PEVENT_RECORD pEvent, PTRACE_EVENT_INFO pInfo, USHORT i, PUSHORT ArraySize )
{
    DWORD status = ERROR_SUCCESS;
    PROPERTY_DATA_DESCRIPTOR DataDescriptor;
    DWORD PropertySize = 0;

    if ( ( pInfo->EventPropertyInfoArray[i].Flags & PropertyParamCount ) == PropertyParamCount )
    {
        DWORD Count = 0;  // Expects the count to be defined by a UINT16 or UINT32
        DWORD j = pInfo->EventPropertyInfoArray[i].countPropertyIndex;
        ZeroMemory( &DataDescriptor, sizeof( PROPERTY_DATA_DESCRIPTOR ) );
        DataDescriptor.PropertyName = (ULONGLONG)( (PBYTE)(pInfo)+pInfo->EventPropertyInfoArray[j].NameOffset );
        DataDescriptor.ArrayIndex = ULONG_MAX;
        status = TdhGetPropertySize( pEvent, 0, NULL, 1, &DataDescriptor, &PropertySize );
        status = TdhGetProperty( pEvent, 0, NULL, 1, &DataDescriptor, PropertySize, (PBYTE)&Count );
        *ArraySize = (USHORT)Count;
    }
    else
    {
        *ArraySize = pInfo->EventPropertyInfoArray[i].count;
    }

    return status;
}


// Both MOF-based events and manifest-based events can specify name/value maps. The
// map values can be integer values or bit values. If the property specifies a value
// map, get the map.

DWORD GetMapInfo( PEVENT_RECORD pEvent, LPWSTR pMapName, DWORD DecodingSource, PEVENT_MAP_INFO & pMapInfo )
{
    DWORD status = ERROR_SUCCESS;
    DWORD MapSize = 0;

    // Retrieve the required buffer size for the map info.

    status = TdhGetEventMapInformation( pEvent, pMapName, pMapInfo, &MapSize );

    if ( ERROR_INSUFFICIENT_BUFFER == status )
    {
        pMapInfo = (PEVENT_MAP_INFO)malloc( MapSize );
        if ( pMapInfo == NULL )
        {
            wprintf( L"Failed to allocate memory for map info (size=%lu).\n", MapSize );
            status = ERROR_OUTOFMEMORY;
            goto cleanup;
        }

        // Retrieve the map info.

        status = TdhGetEventMapInformation( pEvent, pMapName, pMapInfo, &MapSize );
    }

    if ( ERROR_SUCCESS == status )
    {
        if ( DecodingSourceXMLFile == DecodingSource )
        {
            RemoveTrailingSpace( pMapInfo );
        }
    }
    else
    {
        if ( ERROR_NOT_FOUND == status )
        {
            status = ERROR_SUCCESS; // This case is okay.
        }
        else
        {
            wprintf( L"TdhGetEventMapInformation failed with 0x%x.\n", status );
        }
    }

cleanup:

    return status;
}


// The mapped string values defined in a manifest will contain a trailing space
// in the EVENT_MAP_ENTRY structure. Replace the trailing space with a null-
// terminating character, so that the bit mapped strings are correctly formatted.

void RemoveTrailingSpace( PEVENT_MAP_INFO pMapInfo )
{
    DWORD ByteLength = 0;

    for ( DWORD i = 0; i < pMapInfo->EntryCount; i++ )
    {
        ByteLength = ( wcslen( (LPWSTR)( (PBYTE)pMapInfo + pMapInfo->MapEntryArray[i].OutputOffset ) ) - 1 ) * 2;
        *( (LPWSTR)( (PBYTE)pMapInfo + ( pMapInfo->MapEntryArray[i].OutputOffset + ByteLength ) ) ) = L'\0';
    }
}

DWORD DumpProperties( PEVENT_RECORD pEvent, PTRACE_EVENT_INFO pInfo )
{
    DWORD status;

    if ( EVENT_HEADER_FLAG_STRING_ONLY == ( pEvent->EventHeader.Flags & EVENT_HEADER_FLAG_STRING_ONLY ) )
    {
        wprintf( L"%s\n", (LPWSTR)pEvent->UserData );
        return ERROR_SUCCESS;
    }

    for ( USHORT i = 0; i < pInfo->TopLevelPropertyCount; i++ )
    {
        status = DumpPropertiesIndex( pEvent, pInfo, i, NULL, 0 );
        if ( ERROR_SUCCESS != status )
        {
            wprintf( L"Printing top level properties failed.\n" );
            goto cleanup;
        }
    }

cleanup:
    return status;
}

DWORD DumpPropertiesIndex( PEVENT_RECORD pEvent, PTRACE_EVENT_INFO pInfo, USHORT i, LPWSTR pStructureName, USHORT StructIndex )
{
    DWORD status = ERROR_SUCCESS;
    DWORD LastMember = 0;  // Last member of a structure
    USHORT ArraySize = 0;
    PEVENT_MAP_INFO pMapInfo = NULL;
    PROPERTY_DATA_DESCRIPTOR DataDescriptors[2];
    ULONG DescriptorsCount = 0;
    DWORD PropertySize = 0;
    PBYTE pData = NULL;

    // Get the size of the array if the property is an array.

    status = GetArraySize( pEvent, pInfo, i, &ArraySize );

    for ( USHORT k = 0; k < ArraySize; k++ )
    {
        wprintf( L"%*s%s[%d][%d]: ", ( pStructureName ) ? 4 : 0, L"", (LPWSTR)( (PBYTE)(pInfo)+pInfo->EventPropertyInfoArray[i].NameOffset ), i, k );

        // If the property is a structure, print the members of the structure.

        if ( ( pInfo->EventPropertyInfoArray[i].Flags & PropertyStruct ) == PropertyStruct )
        {
            wprintf( L"\n" );

            LastMember = pInfo->EventPropertyInfoArray[i].structType.StructStartIndex +
                pInfo->EventPropertyInfoArray[i].structType.NumOfStructMembers;

            for ( USHORT j = pInfo->EventPropertyInfoArray[i].structType.StructStartIndex; j < LastMember; j++ )
            {
                status = DumpPropertiesIndex( pEvent, pInfo, j, (LPWSTR)( (PBYTE)(pInfo)+pInfo->EventPropertyInfoArray[i].NameOffset ), k );
                if ( ERROR_SUCCESS != status )
                {
                    wprintf( L"Printing the members of the structure failed.\n" );
                    goto cleanup;
                }
            }
        }
        else
        {
            ZeroMemory( &DataDescriptors, sizeof( DataDescriptors ) );

            // To retrieve a member of a structure, you need to specify an array of descriptors. 
            // The first descriptor in the array identifies the name of the structure and the second 
            // descriptor defines the member of the structure whose data you want to retrieve. 

            if ( pStructureName )
            {
                DataDescriptors[0].PropertyName = (ULONGLONG)pStructureName;
                DataDescriptors[0].ArrayIndex = StructIndex;
                DataDescriptors[1].PropertyName = (ULONGLONG)( (PBYTE)(pInfo)+pInfo->EventPropertyInfoArray[i].NameOffset );
                DataDescriptors[1].ArrayIndex = k;
                DescriptorsCount = 2;
            }
            else
            {
                DataDescriptors[0].PropertyName = (ULONGLONG)( (PBYTE)(pInfo)+pInfo->EventPropertyInfoArray[i].NameOffset );
                DataDescriptors[0].ArrayIndex = k;
                DescriptorsCount = 1;
            }

            // The TDH API does not support IPv6 addresses. If the output type is TDH_OUTTYPE_IPV6,
            // you will not be able to consume the rest of the event. If you try to consume the
            // remainder of the event, you will get ERROR_EVT_INVALID_EVENT_DATA.

            if ( TDH_INTYPE_BINARY == pInfo->EventPropertyInfoArray[i].nonStructType.InType &&
                TDH_OUTTYPE_IPV6 == pInfo->EventPropertyInfoArray[i].nonStructType.OutType )
            {
                wprintf( L"The event contains an IPv6 address. Skipping event.\n" );
                status = ERROR_EVT_INVALID_EVENT_DATA;
                break;
            }
            else
            {
                status = TdhGetPropertySize( pEvent, 0, NULL, DescriptorsCount, &DataDescriptors[0], &PropertySize );

                if ( ERROR_SUCCESS != status )
                {
                    wprintf( L"TdhGetPropertySize failed with %lu\n", status );
                    goto cleanup;
                }

                pData = (PBYTE)malloc( PropertySize );

                if ( NULL == pData )
                {
                    wprintf( L"Failed to allocate memory for property data\n" );
                    status = ERROR_OUTOFMEMORY;
                    goto cleanup;
                }

                status = TdhGetProperty( pEvent, 0, NULL, DescriptorsCount, &DataDescriptors[0], PropertySize, pData );

                // Get the name/value mapping if the property specifies a value map.

                status = GetMapInfo( pEvent,
                    (PWCHAR)( (PBYTE)(pInfo)+pInfo->EventPropertyInfoArray[i].nonStructType.MapNameOffset ),
                    pInfo->DecodingSource,
                    pMapInfo );

                if ( ERROR_SUCCESS != status )
                {
                    wprintf( L"GetMapInfo failed\n" );
                    goto cleanup;
                }

                status = FormatAndPrintData( pEvent,
                    pInfo->EventPropertyInfoArray[i].nonStructType.InType,
                    pInfo->EventPropertyInfoArray[i].nonStructType.OutType,
                    pData,
                    PropertySize,
                    pMapInfo
                );

                if ( ERROR_SUCCESS != status )
                {
                    wprintf( L"GetMapInfo failed\n" );
                    goto cleanup;
                }

                if ( pData )
                {
                    free( pData );
                    pData = NULL;
                }

                if ( pMapInfo )
                {
                    free( pMapInfo );
                    pMapInfo = NULL;
                }
            }
        }
    }

cleanup:

    if ( pData )
    {
        free( pData );
        pData = NULL;
    }

    if ( pMapInfo )
    {
        free( pMapInfo );
        pMapInfo = NULL;
    }

    return status;
}

DWORD FormatAndPrintData( PEVENT_RECORD pEvent, USHORT InType, USHORT OutType, PBYTE pData, DWORD DataSize, PEVENT_MAP_INFO pMapInfo )
{
    UNREFERENCED_PARAMETER( pEvent );

    DWORD status = ERROR_SUCCESS;

    switch ( InType )
    {
    case TDH_INTYPE_UNICODESTRING:
    case TDH_INTYPE_COUNTEDSTRING:
    case TDH_INTYPE_REVERSEDCOUNTEDSTRING:
    case TDH_INTYPE_NONNULLTERMINATEDSTRING:
    {
        size_t StringLength = 0;

        if ( TDH_INTYPE_COUNTEDSTRING == InType )
        {
            StringLength = *(PUSHORT)pData;
        }
        else if ( TDH_INTYPE_REVERSEDCOUNTEDSTRING == InType )
        {
            StringLength = MAKEWORD( HIBYTE( (PUSHORT)pData ), LOBYTE( (PUSHORT)pData ) );
        }
        else if ( TDH_INTYPE_NONNULLTERMINATEDSTRING == InType )
        {
            StringLength = DataSize;
        }
        else
        {
            StringLength = wcslen( (LPWSTR)pData );
        }

        wprintf( L"%.*s\n", StringLength, (LPWSTR)pData );
        break;
    }

    case TDH_INTYPE_ANSISTRING:
    case TDH_INTYPE_COUNTEDANSISTRING:
    case TDH_INTYPE_REVERSEDCOUNTEDANSISTRING:
    case TDH_INTYPE_NONNULLTERMINATEDANSISTRING:
    {
        size_t StringLength = 0;

        if ( TDH_INTYPE_COUNTEDANSISTRING == InType )
        {
            StringLength = *(PUSHORT)pData;
        }
        else if ( TDH_INTYPE_REVERSEDCOUNTEDANSISTRING == InType )
        {
            StringLength = MAKEWORD( HIBYTE( (PUSHORT)pData ), LOBYTE( (PUSHORT)pData ) );
        }
        else if ( TDH_INTYPE_NONNULLTERMINATEDANSISTRING == InType )
        {
            StringLength = DataSize;
        }
        else
        {
            StringLength = strlen( (LPSTR)pData );
        }

        wprintf( L"%.*S\n", StringLength, (LPSTR)pData );
        break;
    }

    case TDH_INTYPE_INT8:
    {
        wprintf( L"%hd\n", *(PCHAR)pData );
        break;
    }

    case TDH_INTYPE_UINT8:
    {
        if ( TDH_OUTTYPE_HEXINT8 == OutType )
        {
            wprintf( L"0x%x\n", *(PBYTE)pData );
        }
        else
        {
            wprintf( L"%hu\n", *(PBYTE)pData );
        }

        break;
    }

    case TDH_INTYPE_INT16:
    {
        wprintf( L"%hd\n", *(PSHORT)pData );
        break;
    }

    case TDH_INTYPE_UINT16:
    {
        if ( TDH_OUTTYPE_HEXINT16 == OutType )
        {
            wprintf( L"0x%x\n", *(PUSHORT)pData );
        }
        //else if ( TDH_OUTTYPE_PORT == OutType )
        //{
        //    wprintf( L"%hu\n", ntohs( *(PUSHORT)pData ) );
        //}
        else
        {
            wprintf( L"%hu\n", *(PUSHORT)pData );
        }

        break;
    }

    case TDH_INTYPE_INT32:
    {
        if ( TDH_OUTTYPE_HRESULT == OutType )
        {
            wprintf( L"0x%x\n", *(PLONG)pData );
        }
        else
        {
            wprintf( L"%d\n", *(PLONG)pData );
        }

        break;
    }

    case TDH_INTYPE_UINT32:
    {
        if ( TDH_OUTTYPE_HRESULT == OutType ||
            TDH_OUTTYPE_WIN32ERROR == OutType ||
            TDH_OUTTYPE_NTSTATUS == OutType ||
            TDH_OUTTYPE_HEXINT32 == OutType )
        {
            wprintf( L"0x%x\n", *(PULONG)pData );
        }
        else if ( TDH_OUTTYPE_IPV4 == OutType )
        {
            wprintf( L"%d.%d.%d.%d\n", ( *(PLONG)pData >> 0 ) & 0xff,
                ( *(PLONG)pData >> 8 ) & 0xff,
                ( *(PLONG)pData >> 16 ) & 0xff,
                ( *(PLONG)pData >> 24 ) & 0xff );
        }
        else
        {
            if ( pMapInfo )
            {
                PrintMapString( pMapInfo, pData );
            }
            else
            {
                wprintf( L"%lu\n", *(PULONG)pData );
            }
        }

        break;
    }

    case TDH_INTYPE_INT64:
    {
        wprintf( L"%I64d\n", *(PLONGLONG)pData );

        break;
    }

    case TDH_INTYPE_UINT64:
    {
        if ( TDH_OUTTYPE_HEXINT64 == OutType )
        {
            wprintf( L"0x%x\n", *(PULONGLONG)pData );
        }
        else
        {
            wprintf( L"%I64u\n", *(PULONGLONG)pData );
        }

        break;
    }

    case TDH_INTYPE_FLOAT:
    {
        wprintf( L"%f\n", *(PFLOAT)pData );

        break;
    }

    case TDH_INTYPE_DOUBLE:
    {
        wprintf( L"%I64f\n", *(DOUBLE*)pData );

        break;
    }

    case TDH_INTYPE_BOOLEAN:
    {
        wprintf( L"%s\n", ( 0 == (PBOOL)pData ) ? L"false" : L"true" );

        break;
    }

    case TDH_INTYPE_BINARY:
    {
        for ( DWORD i = 0; i < DataSize; i++ )
        {
            wprintf( L"%.2x", pData[i] );
        }

        wprintf( L"\n" );

        break;
    }

    case TDH_INTYPE_GUID:
    {
        WCHAR szGuid[50];

        StringFromGUID2( *(GUID*)pData, szGuid, sizeof( szGuid ) - 1 );
        wprintf( L"%s\n", szGuid );

        break;
    }

    case TDH_INTYPE_POINTER:
    case TDH_INTYPE_SIZET:
    {
        if ( 4 == 8 )
        {
            wprintf( L"0x%x\n", *(PULONG)pData );
        }
        else
        {
            wprintf( L"0x%x\n", *(PULONGLONG)pData );
        }

        break;
    }

    case TDH_INTYPE_FILETIME:
    {
        break;
    }

    case TDH_INTYPE_SYSTEMTIME:
    {
        break;
    }

    case TDH_INTYPE_SID:
    {
        // unimplemented
        break;
    }

    case TDH_INTYPE_HEXINT32:
    {
        wprintf( L"0x%x\n", (PULONG)pData );
        break;
    }

    case TDH_INTYPE_HEXINT64:
    {
        wprintf( L"0x%x\n", (PULONGLONG)pData );
        break;
    }

    case TDH_INTYPE_UNICODECHAR:
    {
        wprintf( L"%c\n", *(PWCHAR)pData );
        break;
    }

    case TDH_INTYPE_ANSICHAR:
    {
        wprintf( L"%C\n", *(PCHAR)pData );
        break;
    }

    case TDH_INTYPE_WBEMSID:
    {
        // unimplemented
        break;
    }

    default:
        status = ERROR_NOT_FOUND;
    }

cleanup:

    return status;
}

void PrintMapString( PEVENT_MAP_INFO pMapInfo, PBYTE pData )
{
    BOOL MatchFound = FALSE;

    if ( ( pMapInfo->Flag & EVENTMAP_INFO_FLAG_MANIFEST_VALUEMAP ) == EVENTMAP_INFO_FLAG_MANIFEST_VALUEMAP ||
        ( ( pMapInfo->Flag & EVENTMAP_INFO_FLAG_WBEM_VALUEMAP ) == EVENTMAP_INFO_FLAG_WBEM_VALUEMAP &&
        ( pMapInfo->Flag & ( ~EVENTMAP_INFO_FLAG_WBEM_VALUEMAP ) ) != EVENTMAP_INFO_FLAG_WBEM_FLAG ) )
    {
        if ( ( pMapInfo->Flag & EVENTMAP_INFO_FLAG_WBEM_NO_MAP ) == EVENTMAP_INFO_FLAG_WBEM_NO_MAP )
        {
            wprintf( L"%s\n", (LPWSTR)( (PBYTE)pMapInfo + pMapInfo->MapEntryArray[*(PULONG)pData].OutputOffset ) );
        }
        else
        {
            for ( DWORD i = 0; i < pMapInfo->EntryCount; i++ )
            {
                if ( pMapInfo->MapEntryArray[i].Value == *(PULONG)pData )
                {
                    wprintf( L"%s\n", (LPWSTR)( (PBYTE)pMapInfo + pMapInfo->MapEntryArray[i].OutputOffset ) );
                    MatchFound = TRUE;
                    break;
                }
            }

            if ( FALSE == MatchFound )
            {
                wprintf( L"%lu\n", *(PULONG)pData );
            }
        }
    }
    else if ( ( pMapInfo->Flag & EVENTMAP_INFO_FLAG_MANIFEST_BITMAP ) == EVENTMAP_INFO_FLAG_MANIFEST_BITMAP ||
        ( pMapInfo->Flag & EVENTMAP_INFO_FLAG_WBEM_BITMAP ) == EVENTMAP_INFO_FLAG_WBEM_BITMAP ||
        ( ( pMapInfo->Flag & EVENTMAP_INFO_FLAG_WBEM_VALUEMAP ) == EVENTMAP_INFO_FLAG_WBEM_VALUEMAP &&
        ( pMapInfo->Flag & ( ~EVENTMAP_INFO_FLAG_WBEM_VALUEMAP ) ) == EVENTMAP_INFO_FLAG_WBEM_FLAG ) )
    {
        if ( ( pMapInfo->Flag & EVENTMAP_INFO_FLAG_WBEM_NO_MAP ) == EVENTMAP_INFO_FLAG_WBEM_NO_MAP )
        {
            DWORD BitPosition = 0;

            for ( DWORD i = 0; i < pMapInfo->EntryCount; i++ )
            {
                if ( ( *(PULONG)pData & ( BitPosition = ( 1 << i ) ) ) == BitPosition )
                {
                    wprintf( L"%s%s",
                        ( MatchFound ) ? L" | " : L"",
                        (LPWSTR)( (PBYTE)pMapInfo + pMapInfo->MapEntryArray[i].OutputOffset ) );

                    MatchFound = TRUE;
                }
            }

        }
        else
        {
            for ( DWORD i = 0; i < pMapInfo->EntryCount; i++ )
            {
                if ( ( pMapInfo->MapEntryArray[i].Value & *(PULONG)pData ) == pMapInfo->MapEntryArray[i].Value )
                {
                    wprintf( L"%s%s",
                        ( MatchFound ) ? L" | " : L"",
                        (LPWSTR)( (PBYTE)pMapInfo + pMapInfo->MapEntryArray[i].OutputOffset ) );

                    MatchFound = TRUE;
                }
            }
        }

        if ( MatchFound )
        {
            wprintf( L"\n" );
        }
        else
        {
            wprintf( L"%lu\n", *(PULONG)pData );
        }
    }
}

#endif //_WIN32