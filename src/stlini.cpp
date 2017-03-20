//
//	INI File routines using the STL.
//
//	copyright August 18, 1999 by Robert Kesterson
//
//	License:
//			    You may use this code for any project you wish,
//				whether commercial or not, provided that
//				the following conditions are met:
//				(a) you drop me a line (email robertk@robertk.com) to let me know
//				     you're using it, and
//				(b) give me a mention in the credits someplace
//
//
//	Notes:
//
//	INI keys are not case sensitive in this implementation.
//
//  Lines in the INI that are > MAX_INI_LINE characters will not process correctly.
//  (Default is MAX_INI_LINE of 500, but you can change it if you need more...)
//
//	define TEST_INI to build a little test program.  Typical linuxcommand line:
//	g++ -DTEST_INI -O2 -Wall -o testini stlini.cpp -lstdc++
//
//  Provides the following functionality:
//
//		Loads an INI file into a single nested hash, such that for each named section
//		in the INI file, you can use the name of the section to retrieve a INISection of all
//		the key/value pairs in the section.  Within the hash of key/value pairs, you just
//		use the key name to retrieve the value.
//
//		Provides the capability to load and save the INI file to disk files.
//
//		To load/save the INI files, use the following:
//
//	 		INIFile LoadIni(const char *filename)
//			void SaveIni(INIFile &theINI, const char *filename)
//
//		To get/set individual keys, use the following:
//
//			string GetIniSetting(INIFile &theINI, const char *section, const char *key)
//			void PutIniSetting(INIFile &theINI, const char *section, const char *key, const char *value)
//
//  Typical usage is illustrated by the test main() function below.
//

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pwd.h>
#include <fstream>
#include <fcntl.h>
#include <vector>
#include "imgui/imgui.h"
#include "stlini.h"

const char *CIniFile::m_settings = "$settings$";

std::string GetIniStr( INIFile &theINI, const char *pszSection, const char *pszKey, const char *pszDefaultVal = "", bool *found = NULL );
int GetIniInt( INIFile &theINI, const char *pszSection, const char *pszKey, int nDefaultVal = 0 );
void PutIniSetting( INIFile &theINI, const char *pszSection, const char *pszKey = NULL, const char *pszValue = "" );
void RemoveIniSetting( INIFile &theINI, const char *pszSection, const char *pszKey );
void SaveIni( INIFile &theINI, const char *filename );
INIFile LoadIni( const char *filename );

std::vector< std::string > GetIniSections( INIFile &theINI );
std::vector< INIEntry > GetIniSectionEntries( INIFile &theINI, const char *section );

// These pragmas are to quiet VC++ about the expanded template identifiers exceeding 255 chars.
// You won't be able to see those variables in a debug session, but the code will run normally

std::vector< std::string > GetIniSections( INIFile &theINI )
{
    std::vector< std::string > sections;

    for ( const auto &i : theINI )
        sections.push_back( i.first );

    return sections;
}

std::vector< INIEntry > GetIniSectionEntries( INIFile &theINI, const char *section )
{
    std::vector< INIEntry > entries;
    INIFile::iterator iSection = theINI.find( std::string( section ) );

    if ( iSection != theINI.end() )
    {
        for ( const auto &entry : iSection->second )
            entries.push_back( entry );
    }

    return entries;
}

std::string GetIniStr( INIFile &theINI, const char *section, const char *key, const char *defaultval, bool *found )
{
    std::string result( defaultval ? defaultval : "" );

    INIFile::iterator iSection = theINI.find( std::string( section ) );
    if ( iSection != theINI.end() )
    {
        INISection::iterator apair = iSection->second.find( std::string( key ) );
        if ( apair != iSection->second.end() )
        {
            result = apair->second;

            if ( found )
                *found = true;
        }
    }
    return result;
}

int GetIniInt( INIFile &theINI, const char *section, const char *key, int nDefaultVal )
{
    INIFile::iterator iSection = theINI.find( std::string( section ) );
    if ( iSection != theINI.end() )
    {
        INISection::iterator apair = iSection->second.find( std::string( key ) );
        if ( apair != iSection->second.end() )
        {
            return atoi( apair->second.c_str() );
        }
    }

    return nDefaultVal;
}

void RemoveIniSetting( INIFile &theINI, const char *section, const char *key )
{
    INIFile::iterator iSection = theINI.find( std::string( section ) );
    if ( iSection != theINI.end() )
    {
        INISection::iterator apair = iSection->second.find( std::string( key ) );
        if ( apair != iSection->second.end() )
            iSection->second.erase( apair );
    }
}

void PutIniSetting( INIFile &theINI, const char *section, const char *key, const char *value )
{
    INIFile::iterator iniSection;
    INISection::iterator apair;

    if ( ( iniSection = theINI.find( std::string( section ) ) ) == theINI.end() )
    {
        // no such section?  Then add one..
        INISection newsection;
        if ( key )
            newsection.insert( std::pair< std::string, std::string >( std::string( key ), std::string( value ) ) );
        theINI.insert( std::pair< std::string, INISection >( std::string( section ), newsection ) );
    }
    else if ( key )
    { // found section, make sure key isn't in there already,
        // if it is, just drop and re-add
        apair = iniSection->second.find( std::string( key ) );
        if ( apair != iniSection->second.end() )
            iniSection->second.erase( apair );
        iniSection->second.insert( std::pair< std::string, std::string >( std::string( key ), std::string( value ) ) );
    }
}

INIFile LoadIni( const char *filename )
{
    INIFile theINI;
    char *value, *temp;
    std::string section;
    char buffer[ MAX_INI_LINE ];
    std::fstream file( filename, std::ios::in );

    while ( file.good() )
    {
        memset( buffer, 0, sizeof( buffer ) );
        file.getline( buffer, sizeof( buffer ) );
        if ( ( temp = strchr( buffer, '\n' ) ) )
            *temp = '\0'; // cut off at newline
        if ( ( temp = strchr( buffer, '\r' ) ) )
            *temp = '\0'; // cut off at linefeeds
        if ( ( buffer[ 0 ] == '[' ) && ( temp = strrchr( buffer, ']' ) ) )
        {                 // if line is like -->   [section name]
            *temp = '\0'; // chop off the trailing ']';
            section = &buffer[ 1 ];
            PutIniSetting( theINI, &buffer[ 1 ] ); // start new section
        }
        else if ( buffer[ 0 ] && ( value = strchr( buffer, '=' ) ) )
        {
            *value++ = '\0';                                         // assign whatever follows = sign to value, chop at "="
            PutIniSetting( theINI, section.c_str(), buffer, value ); // and add both sides to INISection
        }
        else if ( buffer[ 0 ] )
            PutIniSetting( theINI, section.c_str(), buffer, "" ); // must be a comment or something
    }
    return theINI;
}

void SaveIni( INIFile &theINI, const char *filename )
{
    std::fstream file( filename, std::ios::out );
    if ( !file.good() )
        return;

    // just iterate the hashes and values and dump them to a file.
    INIFile::iterator section = theINI.begin();
    while ( section != theINI.end() )
    {
        if ( section->first > "" )
            file << std::endl
                 << "[" << section->first << "]" << std::endl;
        INISection::iterator pair = section->second.begin();

        while ( pair != section->second.end() )
        {
            if ( pair->second > "" )
                file << pair->first << "=" << pair->second << std::endl;
            else
                file << pair->first << "=" << std::endl;
            pair++;
        }
        section++;
    }
    file.close();
}

void CIniFile::Open( const char *app, const char *filename )
{
    if ( m_filename.empty() )
    {
        m_filename = util_get_config_dir( app );
        m_filename += "/";
        m_filename += filename;

        m_inifile = LoadIni( m_filename.c_str() );
    }
}

void CIniFile::Close()
{
    if ( !m_filename.empty() )
    {
        Save();
        m_filename.clear();
        m_inifile.clear();
    }
}

void CIniFile::Save()
{
    if ( !m_filename.empty() )
    {
        SaveIni( m_inifile, m_filename.c_str() );
    }
}

void CIniFile::PutInt( const char *key, int value, const char *section )
{
    std::string val = std::to_string( value );

    PutStr( key, val.c_str(), section );
}

int CIniFile::GetInt( const char *key, int defval, const char *section )
{
    std::string retstr = GetIniStr( m_inifile, section ? section : m_settings, key, NULL );

    return retstr.size() ? atoi( retstr.c_str() ) : defval;
}

void CIniFile::PutUint64( const char *key, uint64_t value, const char *section )
{
    char buf[ 64 ];

    snprintf( buf, sizeof( buf ), "0x%" PRIx64, value );
    PutStr( key, buf, section );
}

uint64_t CIniFile::GetUint64( const char *key, uint64_t defval, const char *section )
{
    std::size_t pos = 0;
    std::string val = GetIniStr( m_inifile, section ? section : m_settings, key, "" );
    return val.empty() ? defval : std::stoull( val, &pos, 0 );
}

void CIniFile::PutFloat( const char *key, float value, const char *section )
{
    std::string val = std::to_string( value );

    PutStr( key, val.c_str(), section );
}

float CIniFile::GetFloat( const char *key, float defval, const char *section )
{
    std::string retstr = GetIniStr( m_inifile, section ? section : m_settings, key, NULL );

    return retstr.size() ? ( float )atof( retstr.c_str() ) : defval;
}

void CIniFile::PutStr( const char *key, const char *value, const char *section )
{
    PutIniSetting( m_inifile, section ? section : m_settings, key, value );
}

std::string CIniFile::GetStr( const char *key, const char *defval, const char *section )
{
    bool found = false;
    std::string ret = GetIniStr( m_inifile, section ? section : m_settings, key, defval, &found );

#if 0
    // Not found - check if there is a default value for this key.
    if ( ret.empty() && !defval && !found )
    {
        for ( size_t i = 0; i < GWARRAYSIZE( g_defvals ); i++ )
        {
            if ( !strcmp( g_defvals[ i ].key, key ) )
            {
                PutStr( key, g_defvals[ i ].defval, section );
                return g_defvals[ i ].defval;
            }
        }
    }
#endif

    return ret;
}

void CIniFile::PutVec4( const char *key, const ImVec4& value, const char *section )
{
    char buf[ 512 ];
    snprintf( buf, sizeof( buf ), "%f,%f,%f,%f", value.x, value.y, value.z, value.w );
    PutStr( key, buf, section );
}

ImVec4 CIniFile::GetVec4( const char *key, const ImVec4 &defval, const char *section )
{
    ImVec4 vec4 = defval;
    std::string str = GetStr( key, NULL, section );

    if ( !str.empty() )
    {
        sscanf( str.c_str(), "%f,%f,%f,%f", &vec4.x, &vec4.y, &vec4.z, &vec4.w );
    }

    return vec4;
}

std::vector< std::string > CIniFile::GetSections()
{
    return GetIniSections( m_inifile );
}

std::vector< INIEntry > CIniFile::GetSectionEntries( const char *section )
{
    return GetIniSectionEntries( m_inifile, section );
}

std::string util_get_config_dir( const char *dirname )
{
    std::string config_dir;
    static const char *xdg_config_home = getenv( "XDG_CONFIG_HOME" );

    if ( xdg_config_home && xdg_config_home[ 0 ] )
    {
        config_dir = xdg_config_home;
    }
    else 
    {
        static const char *home = getenv( "HOME" );

        if ( !home || !home[ 0 ] )
        {
            passwd *pw = getpwuid( geteuid() );
            home = pw->pw_dir;
        }

        if ( home && home[ 0 ] )
        {
            config_dir = home;
            config_dir += "/.config";
        }
    }

    if ( !config_dir.size() )
    {
        // Egads, can't find home dir - just fall back to using tmp dir.
        config_dir = P_tmpdir;
    }

    config_dir += "/";
    config_dir += dirname;

    mkdir( config_dir.c_str(), S_IRWXU | S_IRWXG | S_IRWXO );
    return config_dir;
}


#ifdef TEST_INI

void DumpIni( INIFile &ini )
{
    // This is essentially SaveIni() except it just dumps to stdout
    INIFile::iterator section = ini.begin();
    while ( section != ini.end() )
    {
        cout << std::endl
             << "[" << section->first << "]" << std::endl;
        INISection sectionvals = section->second;
        INISection::iterator pair = sectionvals.begin();

        while ( pair != sectionvals.end() )
        {
            cout << pair->first;
            if ( pair->second > "" )
                cout << "=" << pair->second;
            cout << std::endl;
            pair++;
        }
        section++;
    }
}

int main( int argc, char **argv )
{
    // read an INI.  If file doesn't exist, that's OK.
    INIFile ini = LoadIni( "test.ini" );
    if ( ini.size() )
    {
        // Note that existing INIs will be added to, though if any of the keys
        // listed below already exist, this program will modify them.
        cout << "About to modify test.ini, which presently contains:\n";
        DumpIni( ini );
    }

    cout << "\nLoading INI with the following information, plus comments\n\n";
    cout << "[Favorites]\ncolor=blue\nfood=pizza\nbeer=homebrew\n\n";
    cout << "[Computing]\nOperating System=Linux\nToolkit=FLTK\nComment=Now isn't this fun?\n\n";

    PutIniSetting( ini, "", "; This is a comment about the whole INI file" );
    PutIniSetting( ini, "Favorites", "; This is a list of favorites" );
    PutIniSetting( ini, "Favorites", "color", "blue" );
    PutIniSetting( ini, "Favorites", "food", "pizza" );
    PutIniSetting( ini, "Favorites", "beer", "homebrew" );
    PutIniSetting( ini, "Computing", "; Information about computing preferences" );
    PutIniSetting( ini, "Computing", "Operating System", "Linux" );
    PutIniSetting( ini, "Computing", "Toolkit", "FLTK" );
    PutIniSetting( ini, "Computing", "Comment", "This will be replaced in next line." );
    PutIniSetting( ini, "Computing", "Comment", "Now isn't this fun?" );

    cout << "\nINI Ready, saving to disk\n\n";
    SaveIni( ini, "test.ini" );

    cout << "Loading from disk to verify.\n\n";
    INIFile ini2 = LoadIni( "test.ini" );

    cout << "Contents of ini just read\n\n";
    DumpIni( ini2 );

    cout << "\nChecking single value for section Computing, key Comment:\n";
    cout << "Value is: " << GetIniStr( ini2, "Computing", "Comment" ) << std::endl;

    cout << "\nChecking unset value for section Computing, \nkey Distribution, with default of \"RedHat\"\n";
    cout << "Value is: " << GetIniStr( ini2, "Computing", "Distribution", "RedHat" ) << "\n\nDone\n\n";
    return ( 0 );
}
#endif
