#include <stdio.h>
#include <string.h>

#include "TracyCoreSelection.hpp"
#include "TracyStorage.hpp"

namespace tracy::CoreSelection
{

const char* LatestPath()
{
    return GetSavePath( "coreselection-latest" );
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
