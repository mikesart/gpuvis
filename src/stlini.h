#ifndef _STLINI_H
#define _STLINI_H 1

// From http://robertk.com/source/

#include <map>
#include <string>
#include <vector>

#define __STDC_FORMAT_MACROS
#include <inttypes.h>

struct StlIniCompareStringNoCase
{
    bool operator()( const std::string &x, const std::string &y ) const;
};

// return true or false depending on whether the first string is less than the second
inline bool StlIniCompareStringNoCase::operator()( const std::string &x, const std::string &y ) const
{
    return ( strcasecmp( x.c_str(), y.c_str() ) < 0 ) ? true : false;
}

// these typedefs just make the code a bit more readable
typedef std::pair< std::string, std::string > INIEntry; // key, value
typedef std::map< std::string, std::string, StlIniCompareStringNoCase > INISection;
typedef std::map< std::string, INISection, StlIniCompareStringNoCase > INIFile;

std::string util_get_config_dir( const char *dirname );

class CIniFile
{
public:
    CIniFile() {}
    ~CIniFile() { Close(); }

    void Open( const char *app, const char *filename );
    void Save();
    void Close();

    void PutInt( const char *key, int value, const char *section = NULL );
    int GetInt( const char *key, int defval, const char *section = NULL );

    void PutUint64( const char *key, uint64_t value, const char *section = NULL );
    uint64_t GetUint64( const char *key, uint64_t defval, const char *section = NULL );

    void PutFloat( const char *key, float value, const char *section = NULL );
    float GetFloat( const char *key, float defval, const char *section = NULL );

    void PutStr( const char *key, const char *value, const char *section = NULL );
    std::string GetStr( const char *key, const char *defval = NULL, const char *section = NULL );

    void PutVec4( const char *key, const ImVec4& value, const char *section = NULL );
    ImVec4 GetVec4( const char *key, const ImVec4 &defval, const char *section = NULL );

    void PutVec2( const char *key, const ImVec2& value, const char *section = NULL );
    ImVec2 GetVec2( const char *key, const ImVec2 &defval, const char *section = NULL );

    std::vector< std::string > GetSections();
    std::vector< INIEntry > GetSectionEntries( const char *section );

    void ClearSection( const char *section );

public:
    INIFile m_inifile;
    std::string m_filename;
    static const char *m_settings;
    static const char *m_colors;
};

#endif // _STLINI_H

