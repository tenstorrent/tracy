#ifdef _WIN32
#  include <windows.h>
#  include <shellapi.h>
#elif defined __EMSCRIPTEN__
#  include <emscripten.h>
#else
#  include <spawn.h>
#  include <sys/wait.h>
#  include <unistd.h>
extern char** environ;
#endif

#include <string.h>

#include "TracyWeb.hpp"

namespace tracy
{

// URL schemes are case-insensitive (RFC 3986), so "HTTPS://" is as valid as "https://".
// Written out instead of strncasecmp(), which Windows does not have.
static bool HasPrefixNoCase( const char* s, const char* prefix )
{
    while( *prefix )
    {
        char a = *s++;
        const char b = *prefix++;
        if( a >= 'A' && a <= 'Z' ) a += 'a' - 'A';
        if( a != b ) return false;
    }
    return true;
}

#if !defined _WIN32 && !defined __EMSCRIPTEN__
static void Launch( const char* opener, const char* url )
{
    // No shell is involved: the url is passed as a single argv entry, so shell metacharacters in it
    // (e.g. from a link in an LLM response) cannot be interpreted as commands.
    char* const argv[] = { const_cast<char*>( opener ), const_cast<char*>( url ), nullptr };
    pid_t pid;
    if( posix_spawnp( &pid, opener, nullptr, nullptr, argv, environ ) == 0 )
    {
        int status;
        waitpid( pid, &status, 0 );
    }
}
#endif

void OpenWebpage( const char* url )
{
    // Only web links are opened. This also stops "-option" style arguments and file:/custom schemes,
    // and in the browser build keeps javascript: links out of window.open().
    if( !HasPrefixNoCase( url, "https://" ) && !HasPrefixNoCase( url, "http://" ) ) return;
#ifdef _WIN32
    ShellExecuteA( nullptr, nullptr, url, nullptr, nullptr, 0 );
#elif defined __APPLE__
    Launch( "open", url );
#elif defined __EMSCRIPTEN__
    EM_ASM( { window.open( UTF8ToString( $0 ), '_blank' ) }, url );
#else
    Launch( "xdg-open", url );
#endif
}

}
