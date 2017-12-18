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

#if defined( __APPLE__ )
// https://android.googlesource.com/platform/system/core/+/master/base/include/android-base/macros.h
#ifndef TEMP_FAILURE_RETRY
#define TEMP_FAILURE_RETRY(exp)            \
  ({                                       \
    decltype(exp) _rc;                     \
    do {                                   \
      _rc = (exp);                         \
    } while (_rc == -1 && errno == EINTR); \
    _rc;                                   \
  })
#endif
#endif

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

#define TEMP_FAILURE_RETRY( _x ) _x

#define ATTRIBUTE_PRINTF( _x, _y )

#define GCC_DIAG_PUSH_OFF( x )
#define GCC_DIAG_POP()

#endif

#ifndef PATH_MAX
  #ifndef MAX_PATH
    #define MAX_PATH 260
  #endif

  #define PATH_MAX ( MAX_PATH + 1 )
#endif

#if defined( WIN32 )

typedef __int64 ssize_t;
typedef __int64 off64_t;

#if defined( __cplusplus )
extern "C" {
#endif

int strcasecmp( const char *s1, const char *s2 );
int strncasecmp( const char *s1, const char *s2, size_t n );

char *strcasestr( const char *haystack, const char *needle );

int asprintf(char **strp, const char *fmt, ...);
int vasprintf( char **strp, const char *fmt, va_list ap );

char *strtok_r( char *str, const char *delim, char **saveptr );

#if defined( __cplusplus )
}
#endif

#endif

#if defined( __cplusplus )

template < typename K, typename V >
class util_umap
{
public:
    util_umap() {}
    ~util_umap() {}

    V *get_val( const K key, const V &defval )
    {
        auto res = m_map.emplace( key, defval );
        return &res.first->second;
    }

    V *get_val_create( const K key )
    {
        auto i = m_map.find( key );
        if ( i != m_map.end() )
            return &i->second;
        return get_val( key, V() );
    }
    V *get_val( const K key )
    {
        auto i = m_map.find( key );
        if ( i != m_map.end() )
            return &i->second;
        return NULL;
    }
    const V *get_val( const K key ) const
    {
        auto i = m_map.find( key );
        if ( i != m_map.end() )
            return &i->second;
        return NULL;
    }

    void set_val( const K key, const V &val )
    {
        auto res = m_map.emplace( key, val );

       /*
        * If the insertion takes place (because no other element existed with the
        * same key), the function returns a pair object, whose first component is an
        * iterator to the inserted element, and whose second component is true.
        *
        * Otherwise, the pair object returned has as first component an iterator
        * pointing to the element in the container with the same key, and false as its
        * second component.
        */
        if ( !res.second )
            res.first->second = val;
    }

    bool erase_key( const K key )
    {
        auto i = m_map.find( key );

        if ( i != m_map.end() )
        {
            m_map.erase( i );
            return true;
        }
        return false;
    }

public:
    typedef std::unordered_map< K, V > map_t;
    map_t m_map;
};

class StrPool
{
public:
    StrPool() {}
    ~StrPool() {}

    const char *getstr( const char *str, size_t len = ( size_t )-1 );
    const char *getstrf( const char *fmt, ... ) ATTRIBUTE_PRINTF( 2, 3 );
    const char *findstr( uint32_t hashval );

public:
    util_umap< uint32_t, std::string > m_pool;
};

extern "C" uint32_t fnv_hashstr32( const char *str, size_t len = ( size_t )-1 );

size_t get_file_size( const char *filename );
const char *get_path_filename( const char *filename );

std::string get_realpath( const char *filename );

bool copy_file( const char *filename, const char *newfilename );

std::string string_format( const char *fmt, ... ) ATTRIBUTE_PRINTF( 1, 2 );
std::string string_formatv( const char *fmt, va_list ap ) ATTRIBUTE_PRINTF( 1, 0 );

void string_replace_char( std::string &s, const char search, const char replace );
void string_replace_str( std::string &s, const std::string &search, const std::string &replace );

std::string gen_random_str( size_t len );

// trim from start (in place)
void string_ltrim( std::string &s );
// trim from end (in place)
void string_rtrim( std::string &s );
// trim from both ends (in place)
void string_trim( std::string &s );
// remove punctuation from string
std::string string_remove_punct( const std::string &s );

// disassemble a string into parts
std::vector< std::string > string_explode( std::string const &s, char delim );
// assembles a vector of string elements with a delimiter
std::string string_implode( std::vector< std::string > &elements, const std::string &delimiter );

// trim from start (copying)
std::string string_ltrimmed( std::string s );
// trim from end (copying)
std::string string_rtrimmed( std::string s );
// trim from both ends (copying)
std::string string_trimmed( std::string s );

// Remove leading and trailing whitespace from string (in place)
void str_strip_whitespace( char *str );
// Strstr routine which ignores whitespace
char *strstr_ignore_spaces( char *haystack, const char *needle, size_t *len = NULL );
// Remove substrings in place from a string
void remove_substrings( char *str, const char *fmt, ... ) ATTRIBUTE_PRINTF( 2, 3 );

inline char *strncasestr( const char *haystack, const char *needle, size_t needle_len )
{
   for ( ; *haystack; haystack++ )
   {
       if ( !strncasecmp( haystack, needle, needle_len ) )
           return ( char * )haystack;
   }
   return NULL;
}

template < typename T >
T Clamp( const T& val, const T& lower, const T& upper )
{
    return std::max< T >( lower, std::min< T >( val, upper ) );
}

template < typename T, size_t size >
constexpr size_t ARRAY_SIZE( const T (&)[ size ] )
{
    return size;
}

template < size_t T  >
int snprintf_safe( char ( &buf )[ T ], const char *fmt, ... ) ATTRIBUTE_PRINTF( 2, 3 );
template < size_t T  >
int snprintf_safe( char ( &buf )[ T ], const char *fmt, ... )
{
    va_list ap;
    int retval;

    va_start( ap, fmt );

    retval = vsnprintf( buf, T, fmt, ap );
    buf[ T - 1 ] = 0;

    va_end( ap );

    return retval;
}

template < size_t T  >
int vsnprintf_safe( char ( &buf )[ T ], const char *fmt, va_list ap ) ATTRIBUTE_PRINTF( 2, 0 );
template < size_t T  >
int vsnprintf_safe( char ( &buf )[ T ], const char *fmt, va_list ap )
{
    int retval;

    retval = vsnprintf( buf, T, fmt, ap );
    buf[ T - 1 ] = 0;

    return retval;
}

template < size_t T >
char *strcpy_safe( char ( &dest )[ T ], const char *src )
{
    size_t len = std::min< size_t >( T - 1, strlen( src ) );

    memcpy( dest, src, len );
    dest[ len ] = 0;

    return dest;
}

template < size_t T >
char *strcpy_safe( char ( &dest )[ T ], const std::string &src )
{
    size_t len = std::min< size_t >( T - 1, src.size() );

    memcpy( dest, src.data(), len );
    dest[ len ] = 0;

    return dest;
}

template < size_t T >
char *strcat_safe( char ( &dest )[ T ], const char *src )
{
    size_t i;

    for ( i = strlen( dest ); ( i < T - 1 ) && *src; )
        dest[ i++ ] = *src++;

    dest[ i ] = '\0';
    return dest;
}

#define STATIC_ASSERT( _x ) static_assert( _x, #_x )

#else

#define STATIC_ASSERT( _x )

#endif // __cplusplus

#endif // _GPUVIS_MACROS_H_
