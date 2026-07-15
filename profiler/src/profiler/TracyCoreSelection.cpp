#include <algorithm>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <dirent.h>
#endif

#include "TracyCoreSelection.hpp"
#include "TracyStorage.hpp"

namespace tracy::CoreSelection
{

static constexpr const char* Extension = ".coresel";

const char* LatestPath()
{
    return GetSavePath( "coreselection-latest.coresel" );
}

// Turn a file name into a friendly display label: drop the ".coresel"
// extension and the internal "coreselection-" prefix (used by the auto-save).
static std::string DisplayName( const std::string& fileName )
{
    std::string name = fileName;
    const size_t extLen = strlen( Extension );
    if( name.size() >= extLen && name.compare( name.size() - extLen, extLen, Extension ) == 0 )
    {
        name.erase( name.size() - extLen );
    }
    const char* prefix = "coreselection-";
    const size_t prefixLen = strlen( prefix );
    if( name.compare( 0, prefixLen, prefix ) == 0 )
    {
        name.erase( 0, prefixLen );
    }
    return name;
}

std::vector<std::pair<std::string, std::string>> List()
{
    std::vector<std::pair<std::string, std::string>> res;

    // The directory selections live in is the directory of LatestPath().
    std::string dir = LatestPath();
    const size_t slash = dir.find_last_of( '/' );
    if( slash == std::string::npos ) return res;
    dir.erase( slash + 1 );

    const size_t extLen = strlen( Extension );

#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA( ( dir + "*" + Extension ).c_str(), &fd );
    if( h != INVALID_HANDLE_VALUE )
    {
        do
        {
            if( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) continue;
            const std::string fn = fd.cFileName;
            res.emplace_back( DisplayName( fn ), dir + fn );
        }
        while( FindNextFileA( h, &fd ) );
        FindClose( h );
    }
#else
    if( DIR* d = opendir( dir.c_str() ) )
    {
        while( dirent* e = readdir( d ) )
        {
            const std::string fn = e->d_name;
            if( fn.size() < extLen ) continue;
            if( fn.compare( fn.size() - extLen, extLen, Extension ) != 0 ) continue;
            res.emplace_back( DisplayName( fn ), dir + fn );
        }
        closedir( d );
    }
#endif

    std::sort( res.begin(), res.end(), []( const auto& a, const auto& b ) { return a.first < b.first; } );
    return res;
}

bool Save( const char* path, const std::unordered_set<std::string>& labels )
{
    FILE* f = fopen( path, "wb" );
    if( !f ) return false;
    for( auto& l : labels )
    {
        fwrite( l.c_str(), 1, l.size(), f );
        fputc( '\n', f );
    }
    fclose( f );
    return true;
}

bool Load( const char* path, std::unordered_set<std::string>& labels )
{
    FILE* f = fopen( path, "rb" );
    if( !f ) return false;
    labels.clear();
    char line[4096];
    while( fgets( line, sizeof( line ), f ) )
    {
        size_t len = strlen( line );
        while( len > 0 && ( line[len-1] == '\n' || line[len-1] == '\r' ) ) line[--len] = '\0';
        if( len > 0 ) labels.emplace( line, len );
    }
    fclose( f );
    return true;
}

}
