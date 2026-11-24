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
    if( strncmp( url, "https://", 8 ) != 0 && strncmp( url, "http://", 7 ) != 0 ) return;
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
