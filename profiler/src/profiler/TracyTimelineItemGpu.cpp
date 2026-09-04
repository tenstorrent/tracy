#include <algorithm>

#include "TracyColor.hpp"
#include "TracyGallop.hpp"
#include "TracyImGui.hpp"
#include "TracyPopcnt.hpp"
#include "TracyPrint.hpp"
#include "TracyTimelineContext.hpp"
#include "TracyTimelineItemGpu.hpp"
#include "TracyUtility.hpp"
#include "TracyView.hpp"
#include "TracyWorker.hpp"
#include "../server/TracyTaskDispatch.hpp"

namespace tracy
{

constexpr float MinVisSize = 3;

TimelineItemGpu::TimelineItemGpu( View& view, Worker& worker, GpuCtxData* gpu )
    : TimelineItem( view, worker, gpu, true )
    , m_gpu( gpu )
    , m_idx( view.GetNextGpuIdx() )
{
}

bool TimelineItemGpu::IsEmpty() const
{
    return m_gpu->threadData.empty();
}

const char* TimelineItemGpu::HeaderLabel() const
{
    static char buf[4096];
    if( m_gpu->name.Active() )
    {
        sprintf( buf, "%s", m_worker.GetString( m_gpu->name ) );
    }
    else
    {
        sprintf( buf, "%s context %i", GpuContextNames[(int)m_gpu->type], m_idx );
    }
    return buf;
}

void TimelineItemGpu::HeaderTooltip( const char* label ) const
{
    const bool dynamicColors = m_view.GetViewData().dynamicColors;
    const bool isMultithreaded =
        ( m_gpu->type == GpuContextType::Vulkan ) ||
        ( m_gpu->type == GpuContextType::OpenCL ) ||
        ( m_gpu->type == GpuContextType::Direct3D12 ) ||
        ( m_gpu->type == GpuContextType::Metal );

    char buf[64];
    sprintf( buf, "%s context %i", GpuContextNames[(int)m_gpu->type], m_idx );

    ImGui::BeginTooltip();
    if( m_gpu->name.Active() ) TextFocused( "Name:", m_worker.GetString( m_gpu->name ) );
    ImGui::TextUnformatted( buf );
    ImGui::Separator();
    if( !isMultithreaded )
    {
        SmallColorBox( GetThreadColor( m_gpu->thread, 0, dynamicColors ) );
        ImGui::SameLine();
        TextFocused( "Thread:", m_worker.GetThreadName( m_gpu->thread ) );
    }
    else
    {
        if( m_gpu->threadData.size() == 1 )
        {
            auto it = m_gpu->threadData.begin();
            auto tid = it->first;
            if( tid == 0 )
            {
                if( !it->second.timeline.empty() )
                {
                    if( it->second.timeline.is_magic() )
                    {
                        auto& tl = *(Vector<GpuEvent>*)&it->second.timeline;
                        tid = m_worker.DecompressThread( tl.begin()->Thread() );
                    }
                    else
                    {
                        tid = m_worker.DecompressThread( (*it->second.timeline.begin())->Thread() );
                    }
                }
            }
            SmallColorBox( GetThreadColor( tid, 0, dynamicColors ) );
            ImGui::SameLine();
            TextFocused( "Thread:", m_worker.GetThreadName( tid ) );
            ImGui::SameLine();
            ImGui::TextDisabled( "(%s)", RealToString( tid ) );
            if( m_worker.IsThreadFiber( tid ) )
            {
                ImGui::SameLine();
                TextColoredUnformatted( ImVec4( 0.2f, 0.6f, 0.2f, 1.f ), "Fiber" );
            }
        }
        else
        {
            ImGui::TextDisabled( "Threads:" );
            ImGui::Indent();
            for( auto& td : m_gpu->threadData )
            {
                SmallColorBox( GetThreadColor( td.first, 0, dynamicColors ) );
                ImGui::SameLine();
                ImGui::TextUnformatted( m_worker.GetThreadName( td.first ) );
                ImGui::SameLine();
                ImGui::TextDisabled( "(%s)", RealToString( td.first ) );
            }
            ImGui::Unindent();
        }
    }
    const auto t0 = RangeBegin();
    if( t0 != std::numeric_limits<int64_t>::max() )
    {
        TextFocused( "Appeared at", TimeToString( t0 ) );
    }
    TextFocused( "Zone count:", RealToString( m_gpu->count ) );
    uint64_t markerCount = 0;
    for( auto& td : m_gpu->threadData ) markerCount += td.second.markers.size();
    if( markerCount != 0 )
    {
        TextFocused( "Event count:", RealToString( markerCount ) );
    }
    if( m_gpu->period != 1.f )
    {
        TextFocused( "Timestamp accuracy:", TimeToString( m_gpu->period ) );
    }
    if( m_gpu->overflow != 0 )
    {
        ImGui::Separator();
        ImGui::TextUnformatted( "GPU timer overflow has been detected." );
        TextFocused( "Timer resolution:", RealToString( 63 - TracyLzcnt( m_gpu->overflow ) ) );
        ImGui::SameLine();
        TextDisabledUnformatted( "bits" );
    }
    ImGui::EndTooltip();
}

void TimelineItemGpu::HeaderExtraContents( const TimelineContext& ctx, int offset, float labelWidth )
{
    if( m_gpu->name.Active() )
    {
        auto draw = ImGui::GetWindowDrawList();
        const auto ty = ImGui::GetTextLineHeight();

        char buf[64];
        sprintf( buf, "%s context %i", GpuContextNames[(int)m_gpu->type], m_idx );
        draw->AddText( ctx.wpos + ImVec2( ty * 1.5f + labelWidth, offset ), HeaderColorInactive(), buf );
    }
}

int64_t TimelineItemGpu::RangeBegin() const
{
    int64_t t = std::numeric_limits<int64_t>::max();
    for( auto& td : m_gpu->threadData )
    {
        // A lane can hold only markers, in which case its zone timeline is empty.
        if( !td.second.timeline.empty() )
        {
            int64_t t0;
            if( td.second.timeline.is_magic() )
            {
                t0 = ((Vector<GpuEvent>*)&td.second.timeline)->front().GpuStart();
            }
            else
            {
                t0 = td.second.timeline.front()->GpuStart();
            }
            if( t0 >= 0 )
            {
                t = std::min( t, t0 );
            }
        }
        if( !td.second.markers.empty() )
        {
            t = std::min( t, td.second.markers.front()->gpuTime );
        }
    }
    return t;
}

int64_t TimelineItemGpu::RangeEnd() const
{
    int64_t t = std::numeric_limits<int64_t>::min();
    for( auto& td : m_gpu->threadData )
    {
        if( !td.second.timeline.empty() )
        {
            int64_t t0;
            if( td.second.timeline.is_magic() )
            {
                t0 = ((Vector<GpuEvent>*)&td.second.timeline)->front().GpuStart();
            }
            else
            {
                t0 = td.second.timeline.front()->GpuStart();
            }
            if( t0 >= 0 )
            {
                if( td.second.timeline.is_magic() )
                {
                    t = std::max( t, std::min( m_worker.GetLastTime(), m_worker.GetZoneEnd( ((Vector<GpuEvent>*)&td.second.timeline)->back() ) ) );
                }
                else
                {
                    t = std::max( t, std::min( m_worker.GetLastTime(), m_worker.GetZoneEnd( *td.second.timeline.back() ) ) );
                }
            }
        }
        if( !td.second.markers.empty() )
        {
            t = std::max( t, td.second.markers.back()->gpuTime );
        }
    }
    return t;
}

void TimelineItemGpu::Preprocess( const TimelineContext& ctx, TaskDispatch& td, bool visible, int yPos )
{
    // Lanes appear as their first zone arrives during a live capture: re-sync with threadData whenever it
    // grew, sorted by thread id so the rows keep a stable order.
    if( m_lanes.size() != m_gpu->threadData.size() )
    {
        m_lanes.clear();
        m_lanes.reserve( m_gpu->threadData.size() );
        for( auto& t : m_gpu->threadData ) m_lanes.emplace_back( GpuLaneDraw { t.first, -1, 0, false, {} } );
        std::sort( m_lanes.begin(), m_lanes.end(), [] ( const auto& l, const auto& r ) { return l.tid < r.tid; } );
    }
    // GpuDrift may insert into the view's drift map, so resolve it here on the main thread.
    const int drift = m_view.GetGpuDrift( m_gpu );
    for( auto& lane : m_lanes )
    {
        assert( lane.draw.empty() );
        auto it = m_gpu->threadData.find( lane.tid );
        if( it == m_gpu->threadData.end() ) continue;
        const GpuCtxThreadData* tdata = &it->second;
        td.Queue( [this, &ctx, tdata, visible, drift, &lane] {
            PreprocessLane( ctx, *tdata, visible, drift, lane );
        } );
    }
}

void TimelineItemGpu::PreprocessLane( const TimelineContext& ctx, const GpuCtxThreadData& td, bool visible, int drift, GpuLaneDraw& lane )
{
    auto& tl = td.timeline;
    lane.begin = -1;
    if( !tl.empty() )
    {
        lane.begin = tl.is_magic() ? ((Vector<GpuEvent>*)&tl)->front().GpuStart() : tl.front()->GpuStart();
    }
    lane.depth = lane.begin >= 0 ? PreprocessZoneLevel( ctx, tl, 0, visible, lane.begin, drift, 0, lane.draw ) : 0;

    const auto begin = lane.begin >= 0 ? lane.begin : 0;
    lane.markers = false;
    auto& mv = td.markers;
    if( !mv.empty() )
    {
        auto it = std::lower_bound( mv.begin(), mv.end(), ctx.vStart, [begin, drift] ( const auto& lhs, const auto& rhs ) { return View::AdjustGpuTime( lhs->gpuTime, begin, drift ) < rhs; } );
        if( it != mv.end() )
        {
            const auto zitend = std::lower_bound( it, mv.end(), ctx.vEnd+1, [begin, drift] ( const auto& lhs, const auto& rhs ) { return View::AdjustGpuTime( lhs->gpuTime, begin, drift ) < rhs; } );
            lane.markers = it != zitend;
        }
    }
}

void TimelineItemGpu::DrawFinished()
{
    for( auto& lane : m_lanes ) lane.draw.clear();
}

bool TimelineItemGpu::DrawContents( const TimelineContext& ctx, int& offset )
{
    return m_view.DrawGpu( ctx, *m_gpu, m_lanes, offset );
}

int TimelineItemGpu::PreprocessZoneLevel( const TimelineContext& ctx, const Vector<short_ptr<GpuEvent>>& vec, int depth, bool visible, int64_t begin, int drift, uint32_t inheritedColor, std::vector<TimelineDraw>& draw )
{
    if( vec.is_magic() )
    {
        return PreprocessZoneLevel<VectorAdapterDirect<GpuEvent>>( ctx, *(Vector<GpuEvent>*)( &vec ), depth, visible, begin, drift, inheritedColor, draw );
    }
    else
    {
        return PreprocessZoneLevel<VectorAdapterPointer<GpuEvent>>( ctx, vec, depth, visible, begin, drift, inheritedColor, draw );
    }
}

template<typename Adapter, typename V>
int TimelineItemGpu::PreprocessZoneLevel( const TimelineContext& ctx, const V& vec, int depth, bool visible, int64_t begin, int drift, uint32_t inheritedColor, std::vector<TimelineDraw>& draw )
{
    if( depth >= 256 ) return depth;

    const auto vStart = ctx.vStart;
    const auto vEnd = ctx.vEnd;
    const auto nspx = ctx.nspx;

    const auto MinVisNs = int64_t( round( ctx.scale * MinVisSize * nspx ) );

    // Ends compare as uint64_t so that unended zones (end = -1) sort last and are still drawn.
    auto zoneEnd = [this, begin, drift] ( const GpuEvent& ev ) { return (uint64_t)View::AdjustGpuTime( m_worker.GetZoneEnd( ev ), begin, drift ); };

    auto it = std::lower_bound( vec.begin(), vec.end(), std::max<int64_t>( 0, vStart ), [&zoneEnd] ( const auto& l, const auto& r ) { Adapter a; return zoneEnd( a(l) ) < (uint64_t)r; } );
    if( it == vec.end() ) return depth;

    const auto zitend = std::lower_bound( it, vec.end(), std::max<int64_t>( 0, vEnd ), [begin, drift] ( const auto& l, const auto& r ) { Adapter a; return (uint64_t)View::AdjustGpuTime( a(l).GpuStart(), begin, drift ) < (uint64_t)r; } );
    if( it == zitend ) return depth;
    Adapter a;
    if( View::AdjustGpuTime( m_worker.GetZoneEnd( a(*(zitend-1)) ), begin, drift ) < vStart ) return depth;

    int maxdepth = depth + 1;

    while( it < zitend )
    {
        auto& ev = a(*it);
        const auto end = View::AdjustGpuTime( m_worker.GetZoneEnd( ev ), begin, drift );
        const auto start = View::AdjustGpuTime( ev.GpuStart(), begin, drift );
        const auto zsz = end - start;
        if( zsz < MinVisNs )
        {
            auto nextTime = end + MinVisNs;
            auto next = it + 1;
            for(;;)
            {
                next = gallop_lower_bound( next, zitend, std::max<int64_t>( 0, nextTime ), [&zoneEnd] ( const auto& l, const auto& r ) { Adapter a; return zoneEnd( a(l) ) < (uint64_t)r; } );
                if( next == zitend ) break;
                const auto pt = View::AdjustGpuTime( m_worker.GetZoneEnd( a(*(next-1)) ), begin, drift );
                const auto nt = View::AdjustGpuTime( m_worker.GetZoneEnd( a(*next) ), begin, drift );
                if( nt - pt >= MinVisNs ) break;
                nextTime = nt + MinVisNs;
            }
            if( visible ) draw.emplace_back( TimelineDraw { TimelineDrawType::Folded, uint16_t( depth ), (void**)&ev, m_worker.GetZoneEnd( a(*(next-1)) ), uint32_t( next - it ), inheritedColor } );
            it = next;
        }
        else
        {
            auto currentInherited = inheritedColor;
            auto childrenInherited = inheritedColor;
            if( m_view.GetViewData().inheritParentColors )
            {
                const auto color = m_worker.GetSourceLocation( ev.SrcLoc() ).color;
                if( color != 0 )
                {
                    currentInherited = color | 0xFF000000;
                    if( ev.Child() >= 0 ) childrenInherited = DarkenColorSlightly( color );
                }
            }
            if( ev.Child() >= 0 )
            {
                const auto d = PreprocessZoneLevel( ctx, m_worker.GetGpuChildren( ev.Child() ), depth + 1, visible, begin, drift, childrenInherited, draw );
                if( d > maxdepth ) maxdepth = d;
            }
            if( visible ) draw.emplace_back( TimelineDraw { TimelineDrawType::Zone, uint16_t( depth ), (void**)&ev, 0, 0, currentInherited } );
            ++it;
        }
    }

    return maxdepth;
}


}
