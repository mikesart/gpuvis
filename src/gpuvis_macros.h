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
#ifndef _GPUVIS_MACROS_H_
#define _GPUVIS_MACROS_H_

// Super handy macros from Jonathan Wakely / Patrick Horgan:
//   http://dbp-consulting.com/tutorials/SuppressingGCCWarnings.html
#if defined( __GNUC__ )

#define ATTRIBUTE_PRINTF( _x, _y ) __attribute__( ( __format__( __printf__, _x, _y ) ) )

#define GCC_DIAG_STR( s ) #s
#define GCC_DIAG_JOINSTR( x, y ) GCC_DIAG_STR( x##y )
#define GCC_DIAG_DO_PRAGMA( x ) _Pragma( #x )
#define GCC_DIAG_PRAGMA( x ) GCC_DIAG_DO_PRAGMA( GCC diagnostic x )

#define GCC_DIAG_PUSH_OFF( x ) \
    GCC_DIAG_PRAGMA( push )    \
    GCC_DIAG_PRAGMA( ignored GCC_DIAG_JOINSTR( -W, x ) )
#define GCC_DIAG_POP() GCC_DIAG_PRAGMA( pop )

#else

#define ATTRIBUTE_PRINTF( _x, _y )


#define GCC_DIAG_PUSH_OFF( x )
#define GCC_DIAG_POP()

#endif

#if defined( __GNUC__ )
__inline__ void __debugbreak() __attribute( ( always_inline ) );
__inline__ void __debugbreak()
{
    __asm__ volatile( "int $0x03" );
}
#endif

#if defined( __cplusplus )

template < class T >
T Min( T const &a, T const &b )
{
    return a < b ? a : b;
}

template < class T >
T Max( T const &a, T const &b )
{
    return a > b ? a : b;
}

// Should be constexpr, but VS2013 barfs on that.
template < typename T, int size >
constexpr size_t GWARRAYSIZE( T ( & )[ size ] )
{
    return size;
}

#define STATIC_ASSERT( _x ) static_assert( _x, #_x )

#else

#define STATIC_ASSERT( _x )

#endif // __cplusplus

#endif // _GPUVIS_MACROS_H_
