#ifndef __TRACYGALLOP_HPP__
#define __TRACYGALLOP_HPP__

#include <algorithm>
#include <iterator>

namespace tracy
{

// lower_bound for a target expected a short distance past `first`: probes 1, 2, 4, ... elements ahead and
// binary-searches only the last doubling span. Consecutive searches over a dense range then touch neighbouring
// elements, where a full binary search over [first, last) touches a scattered element per step.
template<typename It, typename T, typename Cmp>
It gallop_lower_bound( It first, It last, const T& value, Cmp cmp )
{
    if( first == last || !cmp( *first, value ) ) return first;
    typename std::iterator_traits<It>::difference_type step = 1;
    auto lo = first;
    for(;;)
    {
        auto probe = last - lo > step ? lo + step : last;
        if( probe == last || !cmp( *probe, value ) ) return std::lower_bound( lo + 1, probe, value, cmp );
        lo = probe;
        step *= 2;
    }
}

}

#endif
