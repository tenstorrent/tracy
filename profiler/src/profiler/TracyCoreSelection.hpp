#ifndef __TRACYCORESELECTION_HPP__
#define __TRACYCORESELECTION_HPP__

#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace tracy::CoreSelection
{

// A "core selection" is simply the set of GPU context labels that are visible
// on the timeline. It is persisted as a newline-separated text file so that it
// is portable across traces (a selection made on one chip can be loaded for
// another) and easy to inspect by hand.
//
// Selections are stored in the global tracy config dir with a ".coresel"
// extension so that previously saved selections can be listed and picked
// directly, without going through a file dialog.

// Path to the auto-saved "latest" selection in the global tracy config dir.
const char* LatestPath();

// The directory selection files are stored in (no trailing slash). This is
// where file dialogs should default to and where List() searches.
std::string DirPath();

// Selection files discoverable in the global tracy config dir, as
// {display name, full path} pairs sorted by display name.
std::vector<std::pair<std::string, std::string>> List();

bool Save( const char* path, const std::unordered_set<std::string>& labels );
bool Load( const char* path, std::unordered_set<std::string>& labels );

}

#endif
