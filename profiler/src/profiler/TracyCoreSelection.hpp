#ifndef __TRACYCORESELECTION_HPP__
#define __TRACYCORESELECTION_HPP__

#include <string>
#include <unordered_set>

namespace tracy::CoreSelection
{

// A "core selection" is simply the set of GPU context labels that are visible
// on the timeline. It is persisted as a newline-separated text file so that it
// is portable across traces (a selection made on one chip can be loaded for
// another) and easy to inspect by hand.

// Path to the auto-saved "latest" selection in the global tracy config dir.
const char* LatestPath();

bool Save( const char* path, const std::unordered_set<std::string>& labels );
bool Load( const char* path, std::unordered_set<std::string>& labels );

}

#endif
