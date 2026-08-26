#include <numeric>

#include "imgui.h"

#include "TracyFilesystem.hpp"
#include "TracyImGui.hpp"
#include "TracyMouse.hpp"
#include "TracyPrint.hpp"
#include "TracySort.hpp"
#include "TracyView.hpp"
#include "tracy_pdqsort.h"
#include "../Fonts.hpp"

namespace tracy
{

extern double s_time;

void View::ShowFindZoneGpu( int16_t srcloc, const char* name )
{
    m_findZone.show = true;
    m_findZoneMode = 1;
    m_findZone.range.active = false;
    m_findZoneGpu.ShowZone( srcloc, name );
}

#ifndef TRACY_NO_STATISTICS

void View::FindZonesGpu()
{
    m_findZoneGpu.hasResults = true;
    m_findZoneGpu.match = m_worker.GetMatchingSourceLocation( m_findZoneGpu.pattern, m_findZoneGpu.ignoreCase );
    if( m_findZoneGpu.match.empty() ) return;

    auto& slz = m_worker.GetGpuSourceLocationZones();
    auto it = m_findZoneGpu.match.begin();
    while( it != m_findZoneGpu.match.end() )
    {
        auto sit = slz.find( *it );
        if( sit == slz.end() || sit->second.zones.empty() )
        {
            it = m_findZoneGpu.match.erase( it );
        }
        else
        {
            ++it;
        }
    }
}

void View::DrawGpuZoneList( int id, const Vector<GpuZoneRef>& zones )
{
    const auto zsz = zones.size();
    char buf[32];
    sprintf( buf, "%i##gpuzonelist", id );
    if( !ImGui::BeginTable( buf, 3, ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_Resizable | ImGuiTableFlags_Hideable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollY, ImVec2( 0, ImGui::GetTextLineHeightWithSpacing() * std::min<size_t>( zsz + 1, 15 ) ) ) )
    {
        ImGui::TreePop();
        return;
    }
    ImGui::TableSetupScrollFreeze( 0, 1 );
    ImGui::TableSetupColumn( "Time from start" );
    ImGui::TableSetupColumn( "GPU execution time", ImGuiTableColumnFlags_PreferSortDescending );
    ImGui::TableSetupColumn( "Context", ImGuiTableColumnFlags_NoSort );
    ImGui::TableHeadersRow();

    const Vector<GpuZoneRef>* zonesToIterate = &zones;
    Vector<GpuZoneRef> sortedZones;

    const auto& sortspec = *ImGui::TableGetSortSpecs()->Specs;
    if( sortspec.ColumnIndex != 0 || sortspec.SortDirection != ImGuiSortDirection_Ascending )
    {
        zonesToIterate = &sortedZones;
        sortedZones.reserve_and_use( zones.size() );
        memcpy( sortedZones.data(), zones.data(), zones.size() * sizeof( GpuZoneRef ) );

        switch( sortspec.ColumnIndex )
        {
        case 0:
            assert( sortspec.SortDirection != ImGuiSortDirection_Descending );
            std::reverse( sortedZones.begin(), sortedZones.end() );
            break;
        case 1:
            if( m_findZoneGpu.selfTime )
            {
                if( sortspec.SortDirection == ImGuiSortDirection_Descending )
                {
                    pdqsort_branchless( sortedZones.begin(), sortedZones.end(), [this]( const auto& lhs, const auto& rhs ) {
                        return GetZoneSelfTime( *lhs.zone ) > GetZoneSelfTime( *rhs.zone );
                        } );
                }
                else
                {
                    pdqsort_branchless( sortedZones.begin(), sortedZones.end(), [this]( const auto& lhs, const auto& rhs ) {
                        return GetZoneSelfTime( *lhs.zone ) < GetZoneSelfTime( *rhs.zone );
                        } );
                }
            }
            else
            {
                if( sortspec.SortDirection == ImGuiSortDirection_Descending )
                {
                    pdqsort_branchless( sortedZones.begin(), sortedZones.end(), []( const auto& lhs, const auto& rhs ) {
                        return lhs.zone->GpuEnd() - lhs.zone->GpuStart() > rhs.zone->GpuEnd() - rhs.zone->GpuStart();
                        } );
                }
                else
                {
                    pdqsort_branchless( sortedZones.begin(), sortedZones.end(), []( const auto& lhs, const auto& rhs ) {
                        return lhs.zone->GpuEnd() - lhs.zone->GpuStart() < rhs.zone->GpuEnd() - rhs.zone->GpuStart();
                        } );
                }
            }
            break;
        default:
            assert( false );
            break;
        }
    }

    const auto& gpuData = m_worker.GetGpuData();

    ImGuiListClipper clipper;
    clipper.Begin( zonesToIterate->size() );
    while( clipper.Step() )
    {
        for( auto i=clipper.DisplayStart; i<clipper.DisplayEnd; i++ )
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();

            auto& ref = (*zonesToIterate)[i];
            auto ev = (const GpuEvent*)ref.zone;
            const auto& owner = m_gpuZoneIdx.owners[ref.owner];
            const auto timespan = m_findZoneGpu.selfTime ? GetZoneSelfTime( *ev ) : ev->GpuEnd() - ev->GpuStart();

            ImGui::PushID( ev );
            if( ImGui::Selectable( TimeToStringExact( ev->GpuStart() ), m_gpuInfoWindow == ev, ImGuiSelectableFlags_SpanAllColumns ) )
            {
                ShowZoneInfo( *ev, owner.tid );
            }
            if( ImGui::IsItemHovered() )
            {
                m_gpuHighlight = ev;
                if( IsMouseClicked( 2 ) )
                {
                    ZoomToZone( *ev );
                }
                ZoneTooltip( *ev );
            }

            ImGui::TableNextColumn();
            ImGui::TextUnformatted( TimeToString( timespan ) );
            ImGui::TableNextColumn();
            ImGui::TextUnformatted( GetGpuContextLabel( gpuData[owner.ctx] ) );
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
    ImGui::TreePop();
}

void View::DrawFindZoneGpu()
{
    if( !m_worker.AreGpuSourceLocationZonesReady() )
    {
        const auto ty = ImGui::GetTextLineHeight();
        ImGui::PushFont( g_fonts.normal, FontBig );
        ImGui::Dummy( ImVec2( 0, ( ImGui::GetContentRegionAvail().y - ImGui::GetTextLineHeight() * 2 - ty ) * 0.5f ) );
        TextCentered( ICON_FA_CROW );
        TextCentered( "Please wait, computing data…" );
        ImGui::PopFont();
        DrawWaitingDotsCentered( s_time );
        return;
    }

    const auto scale = GetScale();
    bool findClicked = false;

    ImGui::PushItemWidth( -0.01f );
    if( m_shortcut == ShortcutAction::OpenFind )
    {
        ImGui::SetKeyboardFocusHere();
        m_shortcut = ShortcutAction::None;
    }
    findClicked |= ImGui::InputTextWithHint( "###findzonegpu", "Enter zone name to search for", m_findZoneGpu.pattern, 1024, ImGuiInputTextFlags_EnterReturnsTrue );
    ImGui::PopItemWidth();

    findClicked |= ImGui::Button( ICON_FA_MAGNIFYING_GLASS " Find" );
    ImGui::SameLine();

    if( ImGui::Button( ICON_FA_BAN " Clear" ) )
    {
        m_findZoneGpu.pattern[0] = '\0';
        m_findZoneGpu.Reset();
    }
    ImGui::SameLine();
    ImGui::Checkbox( "Ignore case", &m_findZoneGpu.ignoreCase );
    ImGui::SameLine();
    ImGui::Spacing();
    ImGui::SameLine();
    if( ImGui::Checkbox( "Limit range", &m_findZone.range.active ) )
    {
        if( m_findZone.range.active && m_findZone.range.min == 0 && m_findZone.range.max == 0 )
        {
            m_findZone.range.min = m_vd.zvStart;
            m_findZone.range.max = m_vd.zvEnd;
        }
    }
    if( m_findZone.range.active )
    {
        ImGui::SameLine();
        TextColoredUnformatted( 0xFF00FFFF, ICON_FA_TRIANGLE_EXCLAMATION );
        ImGui::SameLine();
        ToggleButton( ICON_FA_RULER " Limits", m_showRanges );
    }

    if( !m_worker.GetGpuData().empty() )
    {
        if( ImGui::Checkbox( ICON_FA_EYE " Only visible GPU contexts", &m_gpuCtxLimit ) )
        {
            m_findZoneGpu.scheduleResetMatch = true;
        }
        ImGui::SameLine();
        DrawHelpMarker( "Restrict the search to the GPU contexts that are enabled in Options \xe2\x86\x92 GPU zones. The same setting is shared with the GPU statistics view." );
    }

    if( m_findZoneGpu.rangeSlim != m_findZone.range )
    {
        m_findZoneGpu.ResetMatch();
        m_findZoneGpu.rangeSlim = m_findZone.range;
    }

    const auto ctxHash = GpuCtxVisibilityHash();
    if( m_findZoneGpu.ctxHash != ctxHash )
    {
        m_findZoneGpu.ctxHash = ctxHash;
        m_findZoneGpu.ResetMatch();
    }

    if( findClicked )
    {
        m_findZoneGpu.Reset();
        FindZonesGpu();
    }

    ImGui::Separator();
    ImGui::BeginChild( "##findzonegpu" );

    if( m_findZoneGpu.match.empty() )
    {
        ImGui::PushFont( g_fonts.normal, FontBig );
        ImGui::Dummy( ImVec2( 0, ( ImGui::GetContentRegionAvail().y - ImGui::GetTextLineHeight() * 2 ) * 0.5f ) );
        TextCentered( ICON_FA_CROW );
        if( m_findZoneGpu.hasResults )
        {
            TextCentered( "No matching zones found" );
        }
        else
        {
            TextCentered( "Please enter search pattern" );
        }
        ImGui::PopFont();
        ImGui::EndChild();
        return;
    }

    const auto rangeMin = m_findZone.range.min;
    const auto rangeMax = m_findZone.range.max;
    const auto limitRange = m_findZone.range.active;
    const auto limitCtx = m_gpuCtxLimit;

    auto& slz = m_worker.GetGpuSourceLocationZones();

    bool expand = ImGui::TreeNodeEx( "Matched source locations", ImGuiTreeNodeFlags_DefaultOpen );
    ImGui::SameLine();
    ImGui::TextDisabled( "(%zu)", m_findZoneGpu.match.size() );
    if( expand )
    {
        auto prev = m_findZoneGpu.selMatch;
        int idx = 0;
        for( auto& v : m_findZoneGpu.match )
        {
            auto& srcloc = m_worker.GetSourceLocation( v );
            auto& zones = slz.find( v )->second.zones;
            SmallColorBox( GetSrcLocColor( srcloc, 0 ) );
            ImGui::SameLine();
            ImGui::PushID( idx );
            ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 0, 0 ) );
            ImGui::RadioButton( m_worker.GetString( srcloc.name.active ? srcloc.name : srcloc.function ), &m_findZoneGpu.selMatch, idx++ );
            ImGui::PopStyleVar();
            if( m_findZoneBuzzAnim.Match( idx ) )
            {
                const auto time = m_findZoneBuzzAnim.Time();
                const auto indentVal = sin( time * 60.f ) * 10.f * time;
                ImGui::SameLine( 0, ImGui::GetStyle().ItemSpacing.x + indentVal );
            }
            else
            {
                ImGui::SameLine();
            }
            const auto fileName = m_worker.GetString( srcloc.file );
            ImGui::TextColored( ImVec4( 0.5, 0.5, 0.5, 1 ), "(%s) %s", RealToString( zones.size() ), LocationToString( fileName, srcloc.line ) );
            if( ImGui::IsItemHovered() )
            {
                DrawSourceTooltip( fileName, srcloc.line, srcloc.line );
                if( ImGui::IsItemClicked( 1 ) )
                {
                    if( SourceFileValid( fileName, m_worker.GetCaptureTime(), *this, m_worker ) )
                    {
                        ViewSourceCheckKeyMod( fileName, srcloc.line, m_worker.GetString( srcloc.function ) );
                    }
                    else
                    {
                        m_findZoneBuzzAnim.Enable( idx, 0.5f );
                    }
                }
            }
            ImGui::PopID();
        }
        ImGui::TreePop();

        if( m_findZoneGpu.selMatch != prev )
        {
            m_findZoneGpu.ResetMatch();
        }
    }
    if( m_findZoneGpu.scheduleResetMatch )
    {
        m_findZoneGpu.scheduleResetMatch = false;
        m_findZoneGpu.ResetMatch();
    }

    ImGui::Separator();

    const auto selSrcloc = m_findZoneGpu.match[m_findZoneGpu.selMatch];
    auto& zoneData = m_worker.GetGpuZonesForSourceLocation( selSrcloc );

    // Zones tagged with their owning context, resolved by walking the contexts' timelines.
    // Rebuilt only when the selected source location changes.
    if( !m_gpuZoneIdx.zonesValid || m_gpuZoneIdx.zonesSrcloc != selSrcloc )
    {
        BuildGpuZoneIndex( true, selSrcloc, false, Range {} );
    }
    if( m_findZoneGpu.idxGeneration != m_gpuZoneIdx.generation )
    {
        // Cached groups hold owner indices from the previous build; drop them.
        m_findZoneGpu.idxGeneration = m_gpuZoneIdx.generation;
        m_findZoneGpu.ResetMatch();
    }
    auto& zones = m_gpuZoneIdx.zones;

    // Which contexts are currently in scope, precomputed so the per-zone test is a lookup.
    const auto& gpuData = m_worker.GetGpuData();
    m_gpuCtxVisible.resize( gpuData.size() );
    for( size_t i=0; i<gpuData.size(); i++ ) m_gpuCtxVisible[i] = !limitCtx || IsGpuCtxVisible( gpuData[i] );

    // A zone is accepted into the search results only when it passes the optional time range
    // and the optional GPU context filter.
    auto Accept = [this, limitRange, limitCtx, rangeMin, rangeMax]( const GpuZoneRef& ref ) {
        auto& z = *(const GpuEvent*)ref.zone;
        if( limitRange && ( z.GpuStart() < rangeMin || z.GpuEnd() > rangeMax ) ) return false;
        if( limitCtx && !m_gpuCtxVisible[m_gpuZoneIdx.owners[ref.owner].ctx] ) return false;
        return true;
    };
    auto Timespan = [this]( const GpuZoneRef& ref ) {
        auto& z = *(const GpuEvent*)ref.zone;
        return m_findZoneGpu.selfTime ? GetZoneSelfTime( z ) : z.GpuEnd() - z.GpuStart();
    };
    const bool filtered = limitRange || limitCtx || m_findZoneGpu.selfTime;

    if( ImGui::TreeNodeEx( "Histogram", ImGuiTreeNodeFlags_DefaultOpen ) )
    {
        const auto ty = ImGui::GetTextLineHeight();

        int64_t tmin = m_findZoneGpu.tmin;
        int64_t tmax = m_findZoneGpu.tmax;
        int64_t total = m_findZoneGpu.total;
        const auto zsz = zones.size();
        if( m_findZoneGpu.sortedNum != zsz )
        {
            auto& vec = m_findZoneGpu.sorted;
            const auto vszorig = vec.size();
            vec.reserve( zsz );
            size_t i;
            if( !filtered )
            {
                tmin = zoneData.min;
                tmax = zoneData.max;
            }
            auto sumSq = m_findZoneGpu.sumSq;
            for( i=m_findZoneGpu.sortedNum; i<zsz; i++ )
            {
                if( !Accept( zones[i] ) ) continue;
                const auto t = Timespan( zones[i] );
                if( t <= 0 ) continue;
                vec.push_back_no_space_check( t );
                total += t;
                sumSq += double( t ) * t;
                if( t < tmin ) tmin = t;
                if( t > tmax ) tmax = t;
            }
            auto mid = vec.begin() + vszorig;
            pdqsort_branchless( mid, vec.end() );
            std::inplace_merge( vec.begin(), mid, vec.end() );

            const auto vsz = vec.size();
            if( vsz != 0 )
            {
                m_findZoneGpu.average = float( total ) / vsz;
                m_findZoneGpu.median = vec[vsz/2];
                m_findZoneGpu.p75 = vec[3 * (vsz / 4)];
                m_findZoneGpu.p90 = vec[vsz / 10 * 9];
                m_findZoneGpu.p99 = vec[size_t(float(vsz * 0.99))];
                m_findZoneGpu.p99_9 = vec[size_t(float(vsz * 0.999))];
                m_findZoneGpu.total = total;
                m_findZoneGpu.sumSq = sumSq;
                m_findZoneGpu.sortedNum = i;
                m_findZoneGpu.tmin = tmin;
                m_findZoneGpu.tmax = tmax;
            }
            else
            {
                m_findZoneGpu.sortedNum = i;
            }
        }

        if( m_findZoneGpu.selGroup != m_findZoneGpu.Unselected )
        {
            if( m_findZoneGpu.selSortNum != m_findZoneGpu.sortedNum )
            {
                const auto selGroup = m_findZoneGpu.selGroup;
                const auto groupBy = m_findZoneGpu.groupBy;

                auto& vec = m_findZoneGpu.selSort;
                vec.reserve( zsz );
                auto act = m_findZoneGpu.selSortActive;
                int64_t selTotal = m_findZoneGpu.selTotal;
                for( size_t i=m_findZoneGpu.selSortNum; i<m_findZoneGpu.sortedNum; i++ )
                {
                    auto& ev = zones[i];
                    if( !Accept( ev ) ) continue;
                    if( selGroup != GetGpuSelectionTarget( ev, groupBy ) ) continue;
                    const auto t = Timespan( ev );
                    if( t <= 0 ) continue;
                    vec.push_back_no_space_check( t );
                    act++;
                    selTotal += t;
                }

                m_findZoneGpu.selSortNum = m_findZoneGpu.sortedNum;
                m_findZoneGpu.selSortActive = act;
                m_findZoneGpu.selTotal = selTotal;
                if( !vec.empty() )
                {
                    pdqsort_branchless( vec.begin(), vec.end() );
                    m_findZoneGpu.selAverage = float( selTotal ) / act;
                    m_findZoneGpu.selMedian = vec[vec.size()/2];
                }
            }
        }

        if( m_findZoneGpu.sorted.size() > 1 )
        {
            const auto sz = m_findZoneGpu.sorted.size();
            const auto avg = m_findZoneGpu.average;
            const auto ss = m_findZoneGpu.sumSq - 2. * m_findZoneGpu.total * avg + avg * avg * sz;
            const auto sd = sqrt( ss / ( sz - 1 ) );

            TextFocused( "Count:", RealToString( sz ) );
            ImGui::SameLine();
            ImGui::Spacing();
            ImGui::SameLine();
            TextFocused( "Total time:", TimeToString( m_findZoneGpu.total ) );
            ImGui::SameLine();
            ImGui::Spacing();
            ImGui::SameLine();
            TextFocused( "\xcf\x83:", TimeToString( sd ) );
            TooltipIfHovered( "Standard deviation" );
        }
        else
        {
            TextFocused( "Count:", RealToString( m_findZoneGpu.sorted.size() ) );
            ImGui::SameLine();
            ImGui::Spacing();
            ImGui::SameLine();
            TextFocused( "Total time:", TimeToString( m_findZoneGpu.total ) );
        }

        SmallCheckbox( "Log values", &m_findZoneGpu.logVal );
        ImGui::SameLine();
        if( SmallCheckbox( "Log time", &m_findZoneGpu.logTime ) )
        {
            m_findZoneGpu.binCache.numBins = -1;
        }
        ImGui::SameLine();
        SmallCheckbox( "Cumulate time", &m_findZoneGpu.cumulateTime );
        ImGui::SameLine();
        DrawHelpMarker( "Show total time taken by calls in each bin instead of call counts." );
        ImGui::SameLine();
        if( SmallCheckbox( "Self time", &m_findZoneGpu.selfTime ) )
        {
            m_findZoneGpu.scheduleResetMatch = true;
        }

        const auto cumulateTime = m_findZoneGpu.cumulateTime;

        if( tmax - tmin > 0 && !m_findZoneGpu.sorted.empty() )
        {
            const auto w = ImGui::GetContentRegionAvail().x;

            const auto numBins = int64_t( w - 4 );
            if( numBins > 1 )
            {
                const auto s = std::min( m_findZoneGpu.highlight.start, m_findZoneGpu.highlight.end );
                const auto e = std::max( m_findZoneGpu.highlight.start, m_findZoneGpu.highlight.end );

                const auto& sorted = m_findZoneGpu.sorted;

                auto sortedBegin = sorted.begin();
                auto sortedEnd = sorted.end();
                while( sortedBegin != sortedEnd && *sortedBegin == 0 ) ++sortedBegin;

                if( m_findZoneGpu.minBinVal > 1 || filtered )
                {
                    if( m_findZoneGpu.logTime )
                    {
                        const auto tMinLog = log10( tmin );
                        const auto zmax = ( log10( tmax ) - tMinLog ) / numBins;
                        int64_t i;
                        for( i=0; i<numBins; i++ )
                        {
                            const auto nextBinVal = int64_t( pow( 10.0, tMinLog + ( i+1 ) * zmax ) );
                            auto nit = std::lower_bound( sortedBegin, sortedEnd, nextBinVal );
                            const auto distance = std::distance( sortedBegin, nit );
                            if( distance >= m_findZoneGpu.minBinVal ) break;
                            sortedBegin = nit;
                        }
                        for( int64_t j=numBins-1; j>i; j-- )
                        {
                            const auto nextBinVal = int64_t( pow( 10.0, tMinLog + ( j-1 ) * zmax ) );
                            auto nit = std::lower_bound( sortedBegin, sortedEnd, nextBinVal );
                            const auto distance = std::distance( nit, sortedEnd );
                            if( distance >= m_findZoneGpu.minBinVal ) break;
                            sortedEnd = nit;
                        }
                    }
                    else
                    {
                        const auto zmax = tmax - tmin;
                        int64_t i;
                        for( i=0; i<numBins; i++ )
                        {
                            const auto nextBinVal = tmin + ( i+1 ) * zmax / numBins;
                            auto nit = std::lower_bound( sortedBegin, sortedEnd, nextBinVal );
                            const auto distance = std::distance( sortedBegin, nit );
                            if( distance >= m_findZoneGpu.minBinVal ) break;
                            sortedBegin = nit;
                        }
                        for( int64_t j=numBins-1; j>i; j-- )
                        {
                            const auto nextBinVal = tmin + ( j-1 ) * zmax / numBins;
                            auto nit = std::lower_bound( sortedBegin, sortedEnd, nextBinVal );
                            const auto distance = std::distance( nit, sortedEnd );
                            if( distance >= m_findZoneGpu.minBinVal ) break;
                            sortedEnd = nit;
                        }
                    }

                    if( sortedBegin != sorted.end() && sortedBegin != sortedEnd )
                    {
                        tmin = *sortedBegin;
                        tmax = *(sortedEnd-1);
                        total = 0;
                        for( auto ptr = sortedBegin; ptr != sortedEnd; ptr++ ) total += *ptr;
                    }
                }

                if( numBins > m_findZoneGpu.numBins )
                {
                    m_findZoneGpu.numBins = numBins;
                    m_findZoneGpu.bins = std::make_unique<int64_t[]>( numBins );
                    m_findZoneGpu.binTime = std::make_unique<int64_t[]>( numBins );
                    m_findZoneGpu.selBin = std::make_unique<int64_t[]>( numBins );
                    m_findZoneGpu.binCache.numBins = -1;
                }

                const auto& bins = m_findZoneGpu.bins;
                const auto& binTime = m_findZoneGpu.binTime;
                const auto& selBin = m_findZoneGpu.selBin;

                const auto distBegin = std::distance( sorted.begin(), sortedBegin );
                const auto distEnd = std::distance( sorted.begin(), sortedEnd );
                if( distBegin != distEnd )
                {

                if( m_findZoneGpu.binCache.numBins != numBins ||
                    m_findZoneGpu.binCache.distBegin != distBegin ||
                    m_findZoneGpu.binCache.distEnd != distEnd )
                {
                    m_findZoneGpu.binCache.numBins = numBins;
                    m_findZoneGpu.binCache.distBegin = distBegin;
                    m_findZoneGpu.binCache.distEnd = distEnd;

                    memset( bins.get(), 0, sizeof( int64_t ) * numBins );
                    memset( binTime.get(), 0, sizeof( int64_t ) * numBins );
                    memset( selBin.get(), 0, sizeof( int64_t ) * numBins );

                    int64_t selectionTime = 0;

                    if( m_findZoneGpu.logTime )
                    {
                        const auto tMinLog = log10( tmin );
                        const auto zmax = ( log10( tmax ) - tMinLog ) / numBins;
                        {
                            auto zit = sortedBegin;
                            for( int64_t i=0; i<numBins; i++ )
                            {
                                const auto nextBinVal = int64_t( pow( 10.0, tMinLog + ( i+1 ) * zmax ) );
                                auto nit = std::lower_bound( zit, sortedEnd, nextBinVal );
                                const auto distance = std::distance( zit, nit );
                                const auto timeSum = std::accumulate( zit, nit, int64_t( 0 ) );
                                bins[i] = distance;
                                binTime[i] = timeSum;
                                if( m_findZoneGpu.highlight.active && zit != nit )
                                {
                                    auto end = nit - 1;
                                    if( *zit >= s && *end <= e ) selectionTime += timeSum;
                                }
                                zit = nit;
                            }
                            const auto timeSum = std::accumulate( zit, sortedEnd, int64_t( 0 ) );
                            bins[numBins-1] += std::distance( zit, sortedEnd );
                            binTime[numBins-1] += timeSum;
                            if( m_findZoneGpu.highlight.active && zit != sortedEnd && *zit >= s && *(sortedEnd-1) <= e ) selectionTime += timeSum;
                        }
                        if( m_findZoneGpu.selGroup != m_findZoneGpu.Unselected )
                        {
                            auto zit = m_findZoneGpu.selSort.begin();
                            const auto zend = m_findZoneGpu.selSort.end();
                            while( zit != zend && *zit == 0 ) ++zit;
                            for( int64_t i=0; i<numBins; i++ )
                            {
                                const auto nextBinVal = int64_t( pow( 10.0, tMinLog + ( i+1 ) * zmax ) );
                                auto nit = std::lower_bound( zit, zend, nextBinVal );
                                if( cumulateTime )
                                {
                                    selBin[i] = std::accumulate( zit, nit, int64_t( 0 ) );
                                }
                                else
                                {
                                    selBin[i] = std::distance( zit, nit );
                                }
                                zit = nit;
                            }
                        }
                    }
                    else
                    {
                        const auto zmax = tmax - tmin;
                        {
                            auto zit = sortedBegin;
                            for( int64_t i=0; i<numBins; i++ )
                            {
                                const auto nextBinVal = tmin + ( i+1 ) * zmax / numBins;
                                auto nit = std::lower_bound( zit, sortedEnd, nextBinVal );
                                const auto distance = std::distance( zit, nit );
                                const auto timeSum = std::accumulate( zit, nit, int64_t( 0 ) );
                                bins[i] = distance;
                                binTime[i] = timeSum;
                                if( m_findZoneGpu.highlight.active && zit != nit )
                                {
                                    auto end = nit - 1;
                                    if( *zit >= s && *end <= e ) selectionTime += timeSum;
                                }
                                zit = nit;
                            }
                            const auto timeSum = std::accumulate( zit, sortedEnd, int64_t( 0 ) );
                            bins[numBins-1] += std::distance( zit, sortedEnd );
                            binTime[numBins-1] += timeSum;
                            if( m_findZoneGpu.highlight.active && zit != sortedEnd && *zit >= s && *(sortedEnd-1) <= e ) selectionTime += timeSum;
                        }
                        if( m_findZoneGpu.selGroup != m_findZoneGpu.Unselected )
                        {
                            auto zit = m_findZoneGpu.selSort.begin();
                            const auto zend = m_findZoneGpu.selSort.end();
                            while( zit != zend && *zit == 0 ) ++zit;
                            for( int64_t i=0; i<numBins; i++ )
                            {
                                const auto nextBinVal = tmin + ( i+1 ) * zmax / numBins;
                                auto nit = std::lower_bound( zit, zend, nextBinVal );
                                if( cumulateTime )
                                {
                                    selBin[i] = std::accumulate( zit, nit, int64_t( 0 ) );
                                }
                                else
                                {
                                    selBin[i] = std::distance( zit, nit );
                                }
                                zit = nit;
                            }
                        }
                    }

                    m_findZoneGpu.selTime = selectionTime;
                }

                int64_t maxVal;
                if( cumulateTime )
                {
                    maxVal = binTime[0];
                    for( int i=1; i<numBins; i++ )
                    {
                        maxVal = std::max( maxVal, binTime[i] );
                    }
                }
                else
                {
                    maxVal = bins[0];
                    for( int i=1; i<numBins; i++ )
                    {
                        maxVal = std::max( maxVal, bins[i] );
                    }
                }

                int64_t maxBin = 0;
                for( int i=0; i<numBins; i++ )
                {
                    if( bins[i] > bins[maxBin] ) maxBin = i;
                }

                TextFocused( "Total time:", TimeToString( total ) );
                ImGui::SameLine();
                ImGui::Spacing();
                ImGui::SameLine();
                TextFocused( "Max counts:", cumulateTime ? TimeToString( maxVal ) : RealToString( maxVal ) );
                TextFocused( "Mean:", TimeToString( m_findZoneGpu.average ) );
                ImGui::SameLine();
                ImGui::Spacing();
                ImGui::SameLine();
                TextFocused( "Median:", TimeToString( m_findZoneGpu.median ) );
                ImGui::SameLine();
                ImGui::Spacing();
                ImGui::SameLine();
                {
                    int64_t t0, t1;
                    if( m_findZoneGpu.logTime )
                    {
                        const auto ltmin = log10( tmin );
                        const auto ltmax = log10( tmax );
                        t0 = int64_t( pow( 10, ltmin + double( maxBin )   / numBins * ( ltmax - ltmin ) ) );
                        t1 = int64_t( pow( 10, ltmin + double( maxBin+1 ) / numBins * ( ltmax - ltmin ) ) );
                    }
                    else
                    {
                        t0 = int64_t( tmin + double( maxBin )   / numBins * ( tmax - tmin ) );
                        t1 = int64_t( tmin + double( maxBin+1 ) / numBins * ( tmax - tmin ) );
                    }
                    TextFocused( "Mode:", TimeToString( ( t0 + t1 ) / 2 ) );
                }
                TextFocused( "P75:", TimeToString( m_findZoneGpu.p75 ) );
                ImGui::SameLine();
                ImGui::Spacing();
                ImGui::SameLine();
                TextFocused( "P90:", TimeToString( m_findZoneGpu.p90 ) );
                ImGui::SameLine();
                ImGui::Spacing();
                ImGui::SameLine();
                TextFocused( "P99:", TimeToString( m_findZoneGpu.p99 ) );
                ImGui::SameLine();
                ImGui::Spacing();
                ImGui::SameLine();
                TextFocused( "P99.9:", TimeToString( m_findZoneGpu.p99_9 ) );

                TextDisabledUnformatted( "Selection range:" );
                ImGui::SameLine();
                if( m_findZoneGpu.highlight.active )
                {
                    const auto hs = std::min( m_findZoneGpu.highlight.start, m_findZoneGpu.highlight.end );
                    const auto he = std::max( m_findZoneGpu.highlight.start, m_findZoneGpu.highlight.end );
                    ImGui::Text( "%s - %s (%s)", TimeToString( hs ), TimeToString( he ), TimeToString( he - hs ) );
                }
                else
                {
                    ImGui::TextUnformatted( "none" );
                }
                ImGui::SameLine();
                DrawHelpMarker( "Left draw on histogram to select range. Right click to clear selection." );
                if( m_findZoneGpu.highlight.active )
                {
                    TextFocused( "Selection time:", TimeToString( m_findZoneGpu.selTime ) );
                }
                else
                {
                    TextFocused( "Selection time:", "none" );
                }
                if( m_findZoneGpu.selGroup != m_findZoneGpu.Unselected )
                {
                    TextFocused( "Zone group time:", TimeToString( m_findZoneGpu.groups[m_findZoneGpu.selGroup].time ) );
                    TextFocused( "Group mean:", TimeToString( m_findZoneGpu.selAverage ) );
                    ImGui::SameLine();
                    ImGui::Spacing();
                    ImGui::SameLine();
                    TextFocused( "Group median:", TimeToString( m_findZoneGpu.selMedian ) );
                }
                else
                {
                    TextFocused( "Zone group time:", "none" );
                    TextFocused( "Group mean:", "none" );
                    ImGui::SameLine();
                    ImGui::Spacing();
                    ImGui::SameLine();
                    TextFocused( "Group median:", "none" );
                }

                ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 0, 0 ) );
                ImGui::Checkbox( "###draw1gpu", &m_findZoneGpu.drawAvgMed );
                ImGui::SameLine();
                ImGui::ColorButton( "c1gpu", ImVec4( 0xFF/255.f, 0x44/255.f, 0x44/255.f, 1.f ), ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop );
                ImGui::SameLine();
                ImGui::TextUnformatted( "Mean time" );
                ImGui::SameLine();
                ImGui::Spacing();
                ImGui::SameLine();
                ImGui::ColorButton( "c2gpu", ImVec4( 0x44/255.f, 0xAA/255.f, 0xFF/255.f, 1.f ), ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop );
                ImGui::SameLine();
                ImGui::TextUnformatted( "Median time" );
                ImGui::Checkbox( "###draw2gpu", &m_findZoneGpu.drawSelAvgMed );
                ImGui::SameLine();
                ImGui::ColorButton( "c3gpu", ImVec4( 0xFF/255.f, 0xAA/255.f, 0x44/255.f, 1.f ), ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop );
                ImGui::SameLine();
                if( m_findZoneGpu.selGroup != m_findZoneGpu.Unselected )
                {
                    ImGui::TextUnformatted( "Group mean" );
                }
                else
                {
                    TextDisabledUnformatted( "Group mean" );
                }
                ImGui::SameLine();
                ImGui::Spacing();
                ImGui::SameLine();
                ImGui::ColorButton( "c4gpu", ImVec4( 0x44/255.f, 0xDD/255.f, 0x44/255.f, 1.f ), ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop );
                ImGui::SameLine();
                if( m_findZoneGpu.selGroup != m_findZoneGpu.Unselected )
                {
                    ImGui::TextUnformatted( "Group median" );
                }
                else
                {
                    TextDisabledUnformatted( "Group median" );
                }
                ImGui::PopStyleVar();

                const auto Height = 200 * scale;
                const auto wpos = ImGui::GetCursorScreenPos();
                const auto dpos = wpos + ImVec2( 0.5f, 0.5f );

                ImGui::InvisibleButton( "##histogramgpu", ImVec2( w, Height + round( ty * 2.5 ) ) );
                const bool hover = ImGui::IsItemHovered();

                auto draw = ImGui::GetWindowDrawList();
                draw->AddRectFilled( wpos, wpos + ImVec2( w, Height ), 0x22FFFFFF );
                draw->AddRect( wpos, wpos + ImVec2( w, Height ), 0x88FFFFFF );

                if( m_findZoneGpu.logVal )
                {
                    const auto hAdj = double( Height - 4 ) / log10( maxVal + 1 );
                    for( int i=0; i<numBins; i++ )
                    {
                        const auto val = cumulateTime ? binTime[i] : bins[i];
                        if( val > 0 )
                        {
                            DrawLine( draw, dpos + ImVec2( 2+i, Height-3 ), dpos + ImVec2( 2+i, Height-3 - log10( val + 1 ) * hAdj ), 0xFF22DDDD );
                            if( selBin[i] > 0 )
                            {
                                DrawLine( draw, dpos + ImVec2( 2+i, Height-3 ), dpos + ImVec2( 2+i, Height-3 - log10( selBin[i] + 1 ) * hAdj ), 0xFFDD7777 );
                            }
                        }
                    }
                }
                else
                {
                    const auto hAdj = double( Height - 4 ) / maxVal;
                    for( int i=0; i<numBins; i++ )
                    {
                        const auto val = cumulateTime ? binTime[i] : bins[i];
                        if( val > 0 )
                        {
                            DrawLine( draw, dpos + ImVec2( 2+i, Height-3 ), dpos + ImVec2( 2+i, Height-3 - val * hAdj ), 0xFF22DDDD );
                            if( selBin[i] > 0 )
                            {
                                DrawLine( draw, dpos + ImVec2( 2+i, Height-3 ), dpos + ImVec2( 2+i, Height-3 - selBin[i] * hAdj ), 0xFFDD7777 );
                            }
                        }
                    }
                }

                const auto xoff = 2;
                const auto yoff = Height + 1;

                DrawHistogramMinMaxLabel( draw, tmin, tmax, wpos + ImVec2( 0, yoff ), w, ty );

                const auto ty05 = round( ty * 0.5f );
                const auto ty025 = round( ty * 0.25f );
                if( m_findZoneGpu.logTime )
                {
                    const auto ltmin = log10( tmin );
                    const auto ltmax = log10( tmax );
                    const auto start = int( floor( ltmin ) );
                    const auto end = int( ceil( ltmax ) );

                    const auto range = ltmax - ltmin;
                    const auto step = w / range;
                    auto offset = start - ltmin;
                    int tw = 0;
                    int tx = 0;

                    auto tt = int64_t( pow( 10, start ) );

                    static const double logticks[] = { log10( 2 ), log10( 3 ), log10( 4 ), log10( 5 ), log10( 6 ), log10( 7 ), log10( 8 ), log10( 9 ) };

                    for( int i=start; i<=end; i++ )
                    {
                        const auto x = ( i - start + offset ) * step;

                        if( x >= 0 )
                        {
                            DrawLine( draw, dpos + ImVec2( x, yoff ), dpos + ImVec2( x, yoff + ty05 ), 0x66FFFFFF );
                            if( tw == 0 || x > tx + tw + ty * 1.1 )
                            {
                                tx = x;
                                auto txt = TimeToString( tt );
                                draw->AddText( wpos + ImVec2( x, yoff + ty05 ), 0x66FFFFFF, txt );
                                tw = ImGui::CalcTextSize( txt ).x;
                            }
                        }

                        for( int j=0; j<8; j++ )
                        {
                            const auto xoff = x + logticks[j] * step;
                            if( xoff >= 0 )
                            {
                                DrawLine( draw, dpos + ImVec2( xoff, yoff ), dpos + ImVec2( xoff, yoff + ty025 ), 0x66FFFFFF );
                            }
                        }

                        tt *= 10;
                    }
                }
                else
                {
                    const auto pxns = numBins / double( tmax - tmin );
                    const auto nspx = 1.0 / pxns;
                    const auto tscale = std::max<float>( 0.0f, round( log10( nspx ) + 2 ) );
                    const auto step = pow( 10, tscale );

                    const auto dx = step * pxns;
                    double x = 0;
                    int tw = 0;
                    int tx = 0;

                    const auto sstep = step / 10.0;
                    const auto sdx = dx / 10.0;

                    static const double linelen[] = { 0.5, 0.25, 0.25, 0.25, 0.25, 0.375, 0.25, 0.25, 0.25, 0.25 };

                    int64_t tt = int64_t( ceil( tmin / sstep ) * sstep );
                    const auto diff = tmin / sstep - int64_t( tmin / sstep );
                    const auto xo = ( diff == 0 ? 0 : ( ( 1 - diff ) * sstep * pxns ) ) + xoff;
                    int iter = int( ceil( ( tmin - int64_t( tmin / step ) * step ) / sstep ) );

                    while( x < numBins )
                    {
                        DrawLine( draw, dpos + ImVec2( xo + x, yoff ), dpos + ImVec2( xo + x, yoff + round( ty * linelen[iter] ) ), 0x66FFFFFF );
                        if( iter == 0 && ( tw == 0 || x > tx + tw + ty * 1.1 ) )
                        {
                            tx = x;
                            auto txt = TimeToString( tt );
                            draw->AddText( wpos + ImVec2( xo + x, yoff + ty05 ), 0x66FFFFFF, txt );
                            tw = ImGui::CalcTextSize( txt ).x;
                        }

                        iter = ( iter + 1 ) % 10;
                        x += sdx;
                        tt += sstep;
                    }
                }

                float ta, tm, tga, tgm;
                if( m_findZoneGpu.logTime )
                {
                    const auto ltmin = log10( tmin );
                    const auto ltmax = log10( tmax );

                    ta = ( log10( m_findZoneGpu.average ) - ltmin ) / float( ltmax - ltmin ) * numBins;
                    tm = ( log10( m_findZoneGpu.median ) - ltmin ) / float( ltmax - ltmin ) * numBins;
                    tga = ( log10( m_findZoneGpu.selAverage ) - ltmin ) / float( ltmax - ltmin ) * numBins;
                    tgm = ( log10( m_findZoneGpu.selMedian ) - ltmin ) / float( ltmax - ltmin ) * numBins;
                }
                else
                {
                    ta = ( m_findZoneGpu.average - tmin ) / float( tmax - tmin ) * numBins;
                    tm = ( m_findZoneGpu.median - tmin ) / float( tmax - tmin ) * numBins;
                    tga = ( m_findZoneGpu.selAverage - tmin ) / float( tmax - tmin ) * numBins;
                    tgm = ( m_findZoneGpu.selMedian - tmin ) / float( tmax - tmin ) * numBins;
                }
                ta = round( ta );
                tm = round( tm );
                tga = round( tga );
                tgm = round( tgm );

                if( m_findZoneGpu.drawAvgMed )
                {
                    if( ta == tm )
                    {
                        DrawLine( draw, ImVec2( dpos.x + ta, dpos.y ), ImVec2( dpos.x + ta, dpos.y+Height-2 ), 0xFFFF88FF );
                    }
                    else
                    {
                        DrawLine( draw, ImVec2( dpos.x + ta, dpos.y ), ImVec2( dpos.x + ta, dpos.y+Height-2 ), 0xFF4444FF );
                        DrawLine( draw, ImVec2( dpos.x + tm, dpos.y ), ImVec2( dpos.x + tm, dpos.y+Height-2 ), 0xFFFFAA44 );
                    }
                }
                if( m_findZoneGpu.drawSelAvgMed && m_findZoneGpu.selGroup != m_findZoneGpu.Unselected )
                {
                    DrawLine( draw, ImVec2( dpos.x + tga, dpos.y ), ImVec2( dpos.x + tga, dpos.y+Height-2 ), 0xFF44AAFF );
                    DrawLine( draw, ImVec2( dpos.x + tgm, dpos.y ), ImVec2( dpos.x + tgm, dpos.y+Height-2 ), 0xFF44DD44 );
                }

                if( hover && ImGui::IsMouseHoveringRect( wpos + ImVec2( 2, 2 ), wpos + ImVec2( w-2, Height + round( ty * 1.5 ) ) ) )
                {
                    const auto ltmin = log10( tmin );
                    const auto ltmax = log10( tmax );

                    auto& io = ImGui::GetIO();
                    DrawLine( draw, ImVec2( io.MousePos.x + 0.5f, dpos.y ), ImVec2( io.MousePos.x + 0.5f, dpos.y+Height-2 ), 0x33FFFFFF );

                    const auto bin = std::clamp( int64_t( io.MousePos.x - wpos.x - 2 ), int64_t( 0 ), numBins - 1 );
                    int64_t t0, t1;
                    if( m_findZoneGpu.logTime )
                    {
                        t0 = int64_t( pow( 10, ltmin + double( bin ) / numBins * ( ltmax - ltmin ) ) );

                        // Hackfix for inability to select data in last bin.
                        if( bin+1 == numBins )
                        {
                            t1 = tmax;
                        }
                        else
                        {
                            t1 = int64_t( pow( 10, ltmin + double( bin+1 ) / numBins * ( ltmax - ltmin ) ) );
                        }
                    }
                    else
                    {
                        t0 = int64_t( tmin + double( bin )   / numBins * ( tmax - tmin ) );
                        t1 = int64_t( tmin + double( bin+1 ) / numBins * ( tmax - tmin ) );
                    }

                    int64_t tBefore = 0;
                    int64_t cntBefore = 0;
                    for( int i=0; i<bin; i++ )
                    {
                        tBefore += binTime[i];
                        cntBefore += bins[i];
                    }

                    int64_t tAfter = 0;
                    int64_t cntAfter = 0;
                    for( int i=bin+1; i<numBins; i++ )
                    {
                        tAfter += binTime[i];
                        cntAfter += bins[i];
                    }

                    ImGui::BeginTooltip();
                    TextDisabledUnformatted( "Time range:" );
                    ImGui::SameLine();
                    ImGui::Text( "%s - %s", TimeToString( t0 ), TimeToString( t1 ) );
                    TextFocused( "Count:", RealToString( bins[bin] ) );
                    TextFocused( "Count in the left bins:", RealToString( cntBefore ) );
                    TextFocused( "Count in the right bins:", RealToString( cntAfter ) );
                    TextFocused( "Time spent in bin:", TimeToString( binTime[bin] ) );
                    TextFocused( "Time spent in the left bins:", TimeToString( tBefore ) );
                    TextFocused( "Time spent in the right bins:", TimeToString( tAfter ) );
                    ImGui::EndTooltip();

                    if( IsMouseClicked( 1 ) )
                    {
                        m_findZoneGpu.highlight.active = false;
                        m_findZoneGpu.ResetGroups();
                    }
                    else if( IsMouseClicked( 0 ) )
                    {
                        m_findZoneGpu.highlight.active = true;
                        m_findZoneGpu.highlight.start = t0;
                        m_findZoneGpu.highlight.end = t1;
                        m_findZoneGpu.hlOrig_t0 = t0;
                        m_findZoneGpu.hlOrig_t1 = t1;
                    }
                    else if( IsMouseDragging( 0 ) )
                    {
                        if( t0 < m_findZoneGpu.hlOrig_t0 )
                        {
                            m_findZoneGpu.highlight.start = t0;
                            m_findZoneGpu.highlight.end = m_findZoneGpu.hlOrig_t1;
                        }
                        else
                        {
                            m_findZoneGpu.highlight.start = m_findZoneGpu.hlOrig_t0;
                            m_findZoneGpu.highlight.end = t1;
                        }
                        m_findZoneGpu.ResetGroups();
                    }
                }

                if( m_findZoneGpu.highlight.active && m_findZoneGpu.highlight.start != m_findZoneGpu.highlight.end )
                {
                    const auto hs = std::min( m_findZoneGpu.highlight.start, m_findZoneGpu.highlight.end );
                    const auto he = std::max( m_findZoneGpu.highlight.start, m_findZoneGpu.highlight.end );

                    float t0, t1;
                    if( m_findZoneGpu.logTime )
                    {
                        const auto ltmin = log10( tmin );
                        const auto ltmax = log10( tmax );

                        t0 = ( log10( hs ) - ltmin ) / float( ltmax - ltmin ) * numBins;
                        t1 = ( log10( he ) - ltmin ) / float( ltmax - ltmin ) * numBins;
                    }
                    else
                    {
                        t0 = ( hs - tmin ) / float( tmax - tmin ) * numBins;
                        t1 = ( he - tmin ) / float( tmax - tmin ) * numBins;
                    }

                    draw->PushClipRect( wpos, wpos + ImVec2( w, Height ), true );
                    draw->AddRectFilled( wpos + ImVec2( 2 + t0, 1 ), wpos + ImVec2( 2 + t1, Height-1 ), 0x22DD8888 );
                    draw->AddRect( wpos + ImVec2( 2 + t0, 1 ), wpos + ImVec2( 2 + t1, Height-1 ), 0x44DD8888 );
                    draw->PopClipRect();
                }

                if( m_gpuHover && m_findZoneGpu.match[m_findZoneGpu.selMatch] == m_gpuHover->SrcLoc() && m_gpuHover->GpuEnd() >= 0 )
                {
                    const auto zoneTime = m_gpuHover->GpuEnd() - m_gpuHover->GpuStart();
                    float zonePos;
                    if( m_findZoneGpu.logTime )
                    {
                        const auto ltmin = log10( tmin );
                        const auto ltmax = log10( tmax );
                        zonePos = round( ( log10( zoneTime ) - ltmin ) / float( ltmax - ltmin ) * numBins );
                    }
                    else
                    {
                        zonePos = round( ( zoneTime - tmin ) / float( tmax - tmin ) * numBins );
                    }
                    const auto c = uint32_t( ( sin( s_time * 10 ) * 0.25 + 0.75 ) * 255 );
                    const auto color = 0xFF000000 | ( c << 16 ) | ( c << 8 ) | c;
                    DrawLine( draw, ImVec2( dpos.x + zonePos, dpos.y ), ImVec2( dpos.x + zonePos, dpos.y+Height-2 ), color );
                    m_wasActive.store( true, std::memory_order_release );
                }

                }
            }
        }

        ImGui::TreePop();
    }

    ImGui::Separator();

    ImGui::TextUnformatted( "Found zones:" );
    ImGui::SameLine();
    DrawHelpMarker( "Left click to highlight entry." );
    if( m_findZoneGpu.selGroup != m_findZoneGpu.Unselected )
    {
        ImGui::SameLine();
        if( ImGui::SmallButton( ICON_FA_DELETE_LEFT " Clear" ) )
        {
            m_findZoneGpu.selGroup = m_findZoneGpu.Unselected;
            m_findZoneGpu.ResetSelection();
        }
    }

    bool groupChanged = false;
    ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 0, 0 ) );
    ImGui::TextUnformatted( "Group by:" );
    ImGui::SameLine();
    groupChanged |= ImGui::RadioButton( "Context", (int*)( &m_findZoneGpu.groupBy ), (int)FindZoneGpu::GroupBy::Context );
    ImGui::SameLine();
    groupChanged |= ImGui::RadioButton( "Thread", (int*)( &m_findZoneGpu.groupBy ), (int)FindZoneGpu::GroupBy::Thread );
    ImGui::SameLine();
    groupChanged |= ImGui::RadioButton( "No grouping", (int*)( &m_findZoneGpu.groupBy ), (int)FindZoneGpu::GroupBy::NoGrouping );
    if( groupChanged )
    {
        m_findZoneGpu.selGroup = m_findZoneGpu.Unselected;
        m_findZoneGpu.ResetGroups();
    }

    ImGui::TextUnformatted( "Sort by:" );
    ImGui::SameLine();
    ImGui::RadioButton( "Order", (int*)( &m_findZoneGpu.sortBy ), (int)FindZoneGpu::SortBy::Order );
    ImGui::SameLine();
    ImGui::RadioButton( "Count", (int*)( &m_findZoneGpu.sortBy ), (int)FindZoneGpu::SortBy::Count );
    ImGui::SameLine();
    ImGui::RadioButton( "Time", (int*)( &m_findZoneGpu.sortBy ), (int)FindZoneGpu::SortBy::Time );
    ImGui::SameLine();
    ImGui::RadioButton( "MTPC", (int*)( &m_findZoneGpu.sortBy ), (int)FindZoneGpu::SortBy::Mtpc );
    ImGui::PopStyleVar();
    ImGui::SameLine();
    DrawHelpMarker( "Mean time per call" );

    const auto hmin = std::min( m_findZoneGpu.highlight.start, m_findZoneGpu.highlight.end );
    const auto hmax = std::max( m_findZoneGpu.highlight.start, m_findZoneGpu.highlight.end );
    const auto groupBy = m_findZoneGpu.groupBy;
    const auto highlightActive = m_findZoneGpu.highlight.active;

    FindZoneGpu::Group* group = nullptr;
    constexpr uint64_t invalidGid = std::numeric_limits<uint64_t>::max() - 1;
    uint64_t lastGid = invalidGid;
    auto zptr = zones.data() + m_findZoneGpu.processed;
    const auto zend = zones.data() + zones.size();
    while( zptr < zend )
    {
        auto& ev = *zptr;
        if( !Accept( ev ) )
        {
            zptr++;
            continue;
        }

        const auto timespan = Timespan( ev );
        if( timespan <= 0 )
        {
            zptr++;
            continue;
        }

        if( highlightActive )
        {
            if( timespan < hmin || timespan > hmax )
            {
                zptr++;
                continue;
            }
        }

        zptr++;
        const uint64_t gid = GetGpuSelectionTarget( ev, groupBy );
        if( lastGid != gid )
        {
            lastGid = gid;
            auto it = m_findZoneGpu.groups.find( gid );
            if( it == m_findZoneGpu.groups.end() )
            {
                it = m_findZoneGpu.groups.emplace( gid, FindZoneGpu::Group { m_findZoneGpu.groupId++ } ).first;
                it->second.zones.reserve( 1024 );
            }
            group = &it->second;
        }
        group->time += timespan;
        group->zones.push_back_non_empty( ev );
    }
    m_findZoneGpu.processed = zptr - zones.data();

    Vector<decltype( m_findZoneGpu.groups )::iterator> groups;
    groups.reserve_and_use( m_findZoneGpu.groups.size() );
    int gidx = 0;
    for( auto it = m_findZoneGpu.groups.begin(); it != m_findZoneGpu.groups.end(); ++it )
    {
        groups[gidx++] = it;
    }

    switch( m_findZoneGpu.sortBy )
    {
    case FindZoneGpu::SortBy::Order:
        pdqsort_branchless( groups.begin(), groups.end(), []( const auto& lhs, const auto& rhs ) { return lhs->second.id < rhs->second.id; } );
        break;
    case FindZoneGpu::SortBy::Count:
        pdqsort_branchless( groups.begin(), groups.end(), []( const auto& lhs, const auto& rhs ) { return lhs->second.zones.size() > rhs->second.zones.size(); } );
        break;
    case FindZoneGpu::SortBy::Time:
        pdqsort_branchless( groups.begin(), groups.end(), []( const auto& lhs, const auto& rhs ) { return lhs->second.time > rhs->second.time; } );
        break;
    case FindZoneGpu::SortBy::Mtpc:
        pdqsort_branchless( groups.begin(), groups.end(), []( const auto& lhs, const auto& rhs ) { return double( lhs->second.time ) / lhs->second.zones.size() > double( rhs->second.time ) / rhs->second.zones.size(); } );
        break;
    default:
        assert( false );
        break;
    }

    TextFocused( "Number of groups:", RealToString( groups.size() ) );
    for( auto& v : groups )
    {
        const char* hdrString;
        char thdBuf[64];
        switch( groupBy )
        {
        case FindZoneGpu::GroupBy::Context:
            hdrString = v->first < gpuData.size() ? GetGpuContextLabel( gpuData[v->first] ) : "Unknown context";
            break;
        case FindZoneGpu::GroupBy::Thread:
        {
            // Device zones carry a synthetic submitting thread id, not a host thread, so there
            // is no thread name to look up.
            const auto tid = v->first;
            SmallColorBox( GetThreadColor( tid, 0 ) );
            ImGui::SameLine();
            const auto tname = m_worker.GetThreadName( tid );
            if( tname && *tname && strcmp( tname, "???" ) != 0 )
            {
                hdrString = tname;
            }
            else
            {
                snprintf( thdBuf, sizeof( thdBuf ), "Thread %s", RealToString( tid ) );
                hdrString = thdBuf;
            }
            break;
        }
        case FindZoneGpu::GroupBy::NoGrouping:
            hdrString = "Zone list";
            break;
        default:
            hdrString = nullptr;
            assert( false );
            break;
        }
        ImGui::PushID( v->first );
        const bool expandGroup = ImGui::TreeNodeEx( hdrString, ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ( v->first == m_findZoneGpu.selGroup ? ImGuiTreeNodeFlags_Selected : 0 ) );
        if( ImGui::IsItemClicked() )
        {
            m_findZoneGpu.selGroup = v->first;
            m_findZoneGpu.ResetSelection();
        }
        ImGui::PopID();
        ImGui::SameLine();
        ImGui::TextColored( ImVec4( 0.5f, 0.5f, 0.5f, 1.0f ), "(%s) %s", RealToString( v->second.zones.size() ), TimeToString( v->second.time ) );
        if( expandGroup )
        {
            DrawGpuZoneList( v->second.id, v->second.zones );
        }
    }

    ImGui::EndChild();
}

uint64_t View::GetGpuSelectionTarget( const GpuZoneRef& ev, FindZoneGpu::GroupBy groupBy ) const
{
    const auto& owner = m_gpuZoneIdx.owners[ev.owner];
    switch( groupBy )
    {
    case FindZoneGpu::GroupBy::Context:
        return owner.ctx;
    case FindZoneGpu::GroupBy::Thread:
        return owner.tid;
    case FindZoneGpu::GroupBy::NoGrouping:
        return 0;
    default:
        assert( false );
        return 0;
    }
}

#endif

}
