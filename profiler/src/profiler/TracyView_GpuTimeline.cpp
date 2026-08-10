#include "TracyColor.hpp"
#include "TracyImGui.hpp"
#include "TracyMouse.hpp"
#include "TracyPrint.hpp"
#include "TracyTimelineContext.hpp"
#include "TracyView.hpp"
#include "../Fonts.hpp"
#include "../public/common/TracyTTDeviceData.hpp"

namespace tracy
{

constexpr float MinVisSize = 3;

std::string GetRiscName(RiscType risc) {
    switch (risc) {
        case RiscType::BRISC: return "BRISC";
        case RiscType::NCRISC: return "NCRISC";
        case RiscType::TRISC_0: return "TRISC_0";
        case RiscType::TRISC_1: return "TRISC_1";
        case RiscType::TRISC_2: return "TRISC_2";
        case RiscType::ERISC: return "ERISC";
        case RiscType::TENSIX_RISC_AGG: return "TENSIX_RISC_AGG";
        case RiscType::NONE: return "";
        default: return "UNKNOWN";
    }
}

bool View::DrawGpu( const TimelineContext& ctx, const GpuCtxData& gpu, int& offset )
{
    const auto w = ctx.w;
    const auto ty = ctx.ty;
    const auto ostep = ty + 1;
    const auto pxns = ctx.pxns;
    const auto nspx = ctx.nspx;
    const auto& wpos = ctx.wpos;
    const auto dpos = wpos + ImVec2( 0.5f, 0.5f );
    const auto hover = ctx.hover;
    const auto yMin = ctx.yMin;
    const auto yMax = ctx.yMax;

    auto draw = ImGui::GetWindowDrawList();

    ImGui::PushFont( g_fonts.normal, FontSmall );
    const auto sty = ImGui::GetTextLineHeight();
    const auto sstep = sty + 1;
    ImGui::PopFont();

    const auto singleThread = gpu.threadData.size() == 1;
    int depth = 0;
    constexpr int threadNameSize = 30;
    char buf[threadNameSize];

    Vector<uint32_t> tds;
    for( auto& td : gpu.threadData )
    {
        tds.push_back(td.first);
    }
    std::sort (tds.begin(), tds.end());

    for( auto& tn :  tds)
    {
        auto & td = gpu.threadData.at(tn);
        TTDeviceMarker marker = TTDeviceMarker (tn);
        snprintf(buf, threadNameSize, "%s", GetRiscName(marker.risc).c_str());
        auto& tl = td.timeline;

        // A lane may hold only markers and no zones, so the timeline can legitimately be empty.
        int64_t begin = -1;
        if( !tl.empty() )
        {
            begin = tl.is_magic() ? ((Vector<GpuEvent>*)&tl)->front().GpuStart() : tl.front()->GpuStart();
        }
        const bool hasZones = begin >= 0;
        if( !hasZones && td.markers.empty() ) continue;

        const auto drift = GpuDrift( &gpu );
        offset += sstep;

        // Marker row goes above the lane's zones, the way messages sit above a thread's zones.
        const bool drewMarkers = DrawGpuMarkers( ctx, td.markers, offset, hasZones ? begin : 0, drift );
        const int markerRows = drewMarkers ? 1 : 0;

        const auto partDepth = hasZones
            ? DispatchGpuZoneLevel( tl, hover, pxns, int64_t( nspx ), wpos, offset + ostep * markerRows, 0, gpu.thread, yMin, yMax, begin, drift )
            : 0;

        if( partDepth != 0 || drewMarkers )
        {
            if( !singleThread )
            {
                ImGui::PushFont( g_fonts.normal, FontSmall );
                DrawTextContrast( draw, wpos + ImVec2( ty, offset-1-sstep ), 0xFFFFAAAA, buf );
                DrawLine( draw, dpos + ImVec2( 0, offset+sty-sstep ), dpos + ImVec2( w, offset+sty-sstep ), 0x22FFAAAA );
                ImGui::PopFont();
            }

            offset += ostep * ( partDepth + markerRows );
            depth += partDepth + markerRows;
        }
        else if( !singleThread )
        {
            offset -= sstep;
        }
    }
    return depth != 0;
}

static const char* GpuMarkerTypeName( uint8_t type )
{
    switch( (TTDeviceMarkerType)type )
    {
    case TTDeviceMarkerType::DATA: return "data";
    case TTDeviceMarkerType::FLAG: return "flag";
    case TTDeviceMarkerType::RUNTIME_EVENT: return "runtime event";
    // Legacy DRAM-readback names, still produced by that path.
    case TTDeviceMarkerType::TS_EVENT: return "TS_EVENT";
    case TTDeviceMarkerType::TS_DATA: return "TS_DATA";
    case TTDeviceMarkerType::TS_DATA_16B: return "TS_DATA_16B";
    default: return "event";
    }
}

// Muted, desaturated tones: a marker row sits directly above a lane's zones, so a saturated glyph reads as
// an alarm and fights the zone colors. Distinct hue per kind, similar value so none dominates.
static uint32_t GpuMarkerColor( uint8_t type )
{
    switch( (TTDeviceMarkerType)type )
    {
    case TTDeviceMarkerType::DATA:          return 0xFFB49678;  // slate blue
    case TTDeviceMarkerType::FLAG:          return 0xFF82A582;  // sage green
    case TTDeviceMarkerType::RUNTIME_EVENT: return 0xFF6EA0BE;  // muted amber
    default:                                return 0xFFA08C8C;  // legacy DRAM markers: neutral grey
    }
}

bool View::DrawGpuMarkers( const TimelineContext& ctx, const Vector<short_ptr<GpuMarkerData>>& vec, int offset, int64_t begin, int drift )
{
    if( vec.empty() ) return false;

    const auto vStart = ctx.vStart;
    const auto vEnd = ctx.vEnd;
    const auto pxns = ctx.pxns;
    const auto nspx = ctx.nspx;
    const auto hover = ctx.hover;
    const auto& wpos = ctx.wpos;
    const auto ty = ctx.ty;

    auto it = std::lower_bound( vec.begin(), vec.end(), vStart, [begin, drift] ( const auto& lhs, const auto& rhs ) { return AdjustGpuTime( lhs->gpuTime, begin, drift ) < rhs; } );
    if( it == vec.end() ) return false;
    const auto zitend = std::lower_bound( it, vec.end(), vEnd+1, [begin, drift] ( const auto& lhs, const auto& rhs ) { return AdjustGpuTime( lhs->gpuTime, begin, drift ) < rhs; } );
    if( it == zitend ) return false;

    if( wpos.y + offset + ty < ctx.yMin || wpos.y + offset > ctx.yMax ) return true;

    auto draw = ImGui::GetWindowDrawList();
    const auto to = 9.f * GetScale();
    const auto th = ( ty - to ) * sqrt( 3 ) * 0.5;
    const auto MinVisNs = int64_t( round( GetScale() * MinVisSize * nspx ) );

    while( it < zitend )
    {
        const auto t0 = AdjustGpuTime( (*it)->gpuTime, begin, drift );
        const auto next = std::upper_bound( it, zitend, t0 + MinVisNs, [begin, drift] ( const auto& lhs, const auto& rhs ) { return lhs < AdjustGpuTime( rhs->gpuTime, begin, drift ); } );
        const auto num = next - it;
        const auto px = ( t0 - vStart ) * pxns;

        // A folded cluster can hold mixed kinds; color it by the first, which is the one the tooltip anchors on.
        const uint32_t color = GpuMarkerColor( (*it)->markerType );
        if( num == 1 )
        {
            draw->AddTriangle( wpos + ImVec2( px - (ty - to) * 0.5, offset + to ), wpos + ImVec2( px + (ty - to) * 0.5, offset + to ), wpos + ImVec2( px, offset + to + th ), color, 2.0f );
        }
        else
        {
            draw->AddTriangleFilled( wpos + ImVec2( px - (ty - to) * 0.5, offset + to ), wpos + ImVec2( px + (ty - to) * 0.5, offset + to ), wpos + ImVec2( px, offset + to + th ), color );
            draw->AddTriangle( wpos + ImVec2( px - (ty - to) * 0.5, offset + to ), wpos + ImVec2( px + (ty - to) * 0.5, offset + to ), wpos + ImVec2( px, offset + to + th ), color, 2.0f );
        }

        if( hover && ImGui::IsMouseHoveringRect( wpos + ImVec2( px - (ty - to) * 0.5 - 1, offset ), wpos + ImVec2( px + (ty - to) * 0.5 + 1, offset + ty ) ) )
        {
            const auto tEnd = AdjustGpuTime( (*(next-1))->gpuTime, begin, drift );
            ImGui::BeginTooltip();
            if( num > 1 )
            {
                TextFocused( "Device events:", RealToString( num ) );
                ImGui::Separator();
                TextFocused( "Time span:", TimeToString( tEnd - t0 ) );
                ImGui::TextDisabled( "Zoom in to separate them" );
            }
            else
            {
                auto& ev = **it;
                auto& srcloc = m_worker.GetSourceLocation( ev.srcloc );
                TextFocused( "Device event:", m_worker.GetString( srcloc.name ) );
                ImGui::SameLine();
                ImGui::TextDisabled( "(%s)", GpuMarkerTypeName( ev.markerType ) );
                TextFocused( "Time:", TimeToStringExact( t0 ) );
                const auto file = m_worker.GetString( srcloc.file );
                if( file && *file )
                {
                    ImGui::TextDisabled( "%s:%i", file, srcloc.line );
                }
                if( ev.meta.Active() )
                {
                    ImGui::Separator();
                    ImGui::TextUnformatted( m_worker.GetString( ev.meta ) );
                }
            }
            ImGui::EndTooltip();

            if( IsMouseClicked( 2 ) )
            {
                if( num > 1 && tEnd > t0 )
                {
                    ZoomToRange( t0, tEnd );
                }
                else
                {
                    CenterAtTime( t0 );
                }
            }
        }

        it = next;
    }
    return true;
}

int View::DispatchGpuZoneLevel( const Vector<short_ptr<GpuEvent>>& vec, bool hover, double pxns, int64_t nspx, const ImVec2& wpos, int _offset, int depth, uint64_t thread, float yMin, float yMax, int64_t begin, int drift )
{
    const auto ty = ImGui::GetTextLineHeight();
    const auto ostep = ty + 1;
    const auto offset = _offset + ostep * depth;

    const auto yPos = wpos.y + offset;
    if( yPos + ostep >= yMin && yPos <= yMax )
    {
        if( vec.is_magic() )
        {
            return DrawGpuZoneLevel<VectorAdapterDirect<GpuEvent>>( *(Vector<GpuEvent>*)&vec, hover, pxns, nspx, wpos, _offset, depth, thread, yMin, yMax, begin, drift );
        }
        else
        {
            return DrawGpuZoneLevel<VectorAdapterPointer<GpuEvent>>( vec, hover, pxns, nspx, wpos, _offset, depth, thread, yMin, yMax, begin, drift );
        }
    }
    else
    {
        if( vec.is_magic() )
        {
            return SkipGpuZoneLevel<VectorAdapterDirect<GpuEvent>>( *(Vector<GpuEvent>*)&vec, hover, pxns, nspx, wpos, _offset, depth, thread, yMin, yMax, begin, drift );
        }
        else
        {
            return SkipGpuZoneLevel<VectorAdapterPointer<GpuEvent>>( vec, hover, pxns, nspx, wpos, _offset, depth, thread, yMin, yMax, begin, drift );
        }
    }
}

template<typename Adapter, typename V>
int View::DrawGpuZoneLevel( const V& vec, bool hover, double pxns, int64_t nspx, const ImVec2& wpos, int _offset, int depth, uint64_t thread, float yMin, float yMax, int64_t begin, int drift )
{
    // cast to uint64_t, so that unended zones (end = -1) are still drawn
    auto it = std::lower_bound( vec.begin(), vec.end(), std::max<int64_t>( 0, m_vd.zvStart ), [begin, drift] ( const auto& l, const auto& r ) { Adapter a; return (uint64_t)AdjustGpuTime( a(l).GpuEnd(), begin, drift ) < (uint64_t)r; } );
    if( it == vec.end() ) return depth;

    Adapter a;

    const auto zitend = std::lower_bound( it, vec.end(), std::max<int64_t>( 0, m_vd.zvEnd ), [begin, drift] ( const auto& l, const auto& r ) { Adapter a; return (uint64_t)AdjustGpuTime( a(l).GpuStart(), begin, drift ) < (uint64_t)r; } );
    if( it == zitend ) return depth;
    if( AdjustGpuTime( a(*(zitend-1)).GpuEnd(), begin, drift ) < m_vd.zvStart ) return depth;

    const auto w = ImGui::GetContentRegionAvail().x - 1;
    const auto ty = ImGui::GetTextLineHeight();
    const auto ostep = ty + 1;
    const auto offset = _offset + ostep * depth;
    auto draw = ImGui::GetWindowDrawList();
    const auto dpos = wpos + ImVec2( 0.5f, 0.5f );

    depth++;
    int maxdepth = depth;

    while( it < zitend )
    {
        auto& ev = a(*it);
        auto end = m_worker.GetZoneEnd( ev );
        if( end == std::numeric_limits<int64_t>::max() ) break;
        const auto start = AdjustGpuTime( ev.GpuStart(), begin, drift );
        end = AdjustGpuTime( end, begin, drift );
        const auto zsz = std::max( ( end - start ) * pxns, pxns * 0.5 );
        if( zsz < MinVisSize )
        {
            const auto color = GetZoneColor( ev );
            const auto MinVisNs = MinVisSize * nspx;
            int num = 0;
            const auto px0 = ( start - m_vd.zvStart ) * pxns;
            auto px1ns = end - m_vd.zvStart;
            auto rend = end;
            auto nextTime = end + MinVisNs;
            for(;;)
            {
                const auto prevIt = it;
                it = std::lower_bound( it, zitend, std::max<int64_t>( 0, nextTime ), [begin, drift] ( const auto& l, const auto& r ) { Adapter a; return (uint64_t)AdjustGpuTime( a(l).GpuEnd(), begin, drift ) < (uint64_t)r; } );
                if( it == prevIt ) ++it;
                num += std::distance( prevIt, it );
                if( it == zitend ) break;
                const auto nend = AdjustGpuTime( m_worker.GetZoneEnd( a(*it) ), begin, drift );
                const auto nsnext = nend - m_vd.zvStart;
                if( nsnext < 0 || nsnext - px1ns >= MinVisNs * 2 ) break;
                px1ns = nsnext;
                rend = nend;
                nextTime = nend + nspx;
            }
            const auto px1 = px1ns * pxns;
            draw->AddRectFilled( wpos + ImVec2( std::max( px0, -10.0 ), offset ), wpos + ImVec2( std::min( std::max( px1, px0+MinVisSize ), double( w + 10 ) ), offset + ty ), color );
            DrawZigZag( draw, wpos + ImVec2( 0, offset + ty/2 ), std::max( px0, -10.0 ), std::min( std::max( px1, px0+MinVisSize ), double( w + 10 ) ), ty/4, DarkenColor( color ) );
            if( hover && ImGui::IsMouseHoveringRect( wpos + ImVec2( std::max( px0, -10.0 ), offset ), wpos + ImVec2( std::min( std::max( px1, px0+MinVisSize ), double( w + 10 ) ), offset + ty + 1 ) ) )
            {
                if( num > 1 )
                {
                    ImGui::BeginTooltip();
                    TextFocused( "Zones too small to display:", RealToString( num ) );
                    ImGui::Separator();
                    TextFocused( "Execution time:", TimeToString( rend - start ) );
                    ImGui::EndTooltip();

                    if( IsMouseClicked( 2 ) && rend - start > 0 )
                    {
                        ZoomToRange( start, rend );
                    }
                }
                else
                {
                    const auto zoneThread = thread != 0 ? thread : m_worker.DecompressThread( ev.Thread() );
                    ZoneTooltip( ev );

                    if( IsMouseClicked( 2 ) && rend - start > 0 )
                    {
                        ZoomToZone( ev );
                    }
                    if( IsMouseClicked( 0 ) )
                    {
                        ShowZoneInfo( ev, zoneThread );
                    }

                    m_gpuThread = zoneThread;
                    m_gpuStart = ev.CpuStart();
                    m_gpuEnd = ev.CpuEnd();
                }
            }
            const auto tmp = RealToString( num );
            const auto tsz = ImGui::CalcTextSize( tmp );
            if( tsz.x < px1 - px0 )
            {
                const auto x = px0 + ( px1 - px0 - tsz.x ) / 2;
                DrawTextContrast( draw, wpos + ImVec2( x, offset ), 0xFF4488DD, tmp );
            }
        }
        else
        {
            if( ev.Child() >= 0 )
            {
                const auto d = DispatchGpuZoneLevel( m_worker.GetGpuChildren( ev.Child() ), hover, pxns, nspx, wpos, _offset, depth, thread, yMin, yMax, begin, drift );
                if( d > maxdepth ) maxdepth = d;
            }

            const char* zoneName = m_worker.GetZoneName( ev );
            auto tsz = ImGui::CalcTextSize( zoneName );

            const auto pr0 = ( start - m_vd.zvStart ) * pxns;
            const auto pr1 = ( end - m_vd.zvStart ) * pxns;
            const auto px0 = std::max( pr0, -10.0 );
            const auto px1 = std::max( { std::min( pr1, double( w + 10 ) ), px0 + pxns * 0.5, px0 + MinVisSize } );
            const auto zoneColor = GetZoneColorData( ev );
            draw->AddRectFilled( wpos + ImVec2( px0, offset ), wpos + ImVec2( px1, offset + tsz.y ), zoneColor.color );
            if( zoneColor.highlight )
            {
                if( zoneColor.thickness > 1.f )
                {
                    draw->AddRect( wpos + ImVec2( px0 + 1, offset + 1 ), wpos + ImVec2( px1 - 1, offset + tsz.y - 1 ), zoneColor.accentColor, 0.f, zoneColor.thickness );
                }
                else
                {
                    draw->AddRect( wpos + ImVec2( px0, offset ), wpos + ImVec2( px1, offset + tsz.y ), zoneColor.accentColor, 0.f, zoneColor.thickness );
                }
            }
            else
            {
                const auto darkColor = DarkenColor( zoneColor.color );
                DrawLine( draw, dpos + ImVec2( px0, offset + tsz.y ), dpos + ImVec2( px0, offset ), dpos + ImVec2( px1-1, offset ), zoneColor.accentColor, zoneColor.thickness );
                DrawLine( draw, dpos + ImVec2( px0, offset + tsz.y ), dpos + ImVec2( px1-1, offset + tsz.y ), dpos + ImVec2( px1-1, offset ), darkColor, zoneColor.thickness );
            }
            if( tsz.x < zsz )
            {
                const auto x = ( start - m_vd.zvStart ) * pxns + ( ( end - start ) * pxns - tsz.x ) / 2;
                if( x < 0 || x > w - tsz.x )
                {
                    ImGui::PushClipRect( wpos + ImVec2( px0, offset ), wpos + ImVec2( px1, offset + tsz.y * 2 ), true );
                    DrawTextContrast( draw, wpos + ImVec2( std::max( std::max( 0., px0 ), std::min( double( w - tsz.x ), x ) ), offset ), 0xFFFFFFFF, zoneName );
                    ImGui::PopClipRect();
                }
                else if( ev.GpuStart() == ev.GpuEnd() )
                {
                    DrawTextContrast( draw, wpos + ImVec2( px0 + ( px1 - px0 - tsz.x ) * 0.5, offset ), 0xFFFFFFFF, zoneName );
                }
                else
                {
                    DrawTextContrast( draw, wpos + ImVec2( x, offset ), 0xFFFFFFFF, zoneName );
                }
            }
            else
            {
                ImGui::PushClipRect( wpos + ImVec2( px0, offset ), wpos + ImVec2( px1, offset + tsz.y * 2 ), true );
                DrawTextContrast( draw, wpos + ImVec2( ( start - m_vd.zvStart ) * pxns, offset ), 0xFFFFFFFF, zoneName );
                ImGui::PopClipRect();
            }

            if( hover && ImGui::IsMouseHoveringRect( wpos + ImVec2( px0, offset ), wpos + ImVec2( px1, offset + tsz.y + 1 ) ) )
            {
                const auto zoneThread = thread != 0 ? thread : m_worker.DecompressThread( ev.Thread() );
                ZoneTooltip( ev );

                if( !m_zoomAnim.active && IsMouseClicked( 2 ) )
                {
                    ZoomToZone( ev );
                }
                if( IsMouseClicked( 0 ) )
                {
                    ShowZoneInfo( ev, zoneThread );
                }

                m_gpuThread = zoneThread;
                m_gpuStart = ev.CpuStart();
                m_gpuEnd = ev.CpuEnd();
            }

            ++it;
        }
    }
    return maxdepth;
}

template<typename Adapter, typename V>
int View::SkipGpuZoneLevel( const V& vec, bool hover, double pxns, int64_t nspx, const ImVec2& wpos, int _offset, int depth, uint64_t thread, float yMin, float yMax, int64_t begin, int drift )
{
    // cast to uint64_t, so that unended zones (end = -1) are still drawn
    auto it = std::lower_bound( vec.begin(), vec.end(), std::max<int64_t>( 0, m_vd.zvStart ), [begin, drift] ( const auto& l, const auto& r ) { Adapter a; return (uint64_t)AdjustGpuTime( a(l).GpuEnd(), begin, drift ) < (uint64_t)r; } );
    if( it == vec.end() ) return depth;

    Adapter a;

    const auto zitend = std::lower_bound( it, vec.end(), std::max<int64_t>( 0, m_vd.zvEnd ), [begin, drift] ( const auto& l, const auto& r ) { Adapter a; return (uint64_t)AdjustGpuTime( a(l).GpuStart(), begin, drift ) < (uint64_t)r; } );
    if( it == zitend ) return depth;
    if( AdjustGpuTime( a(*(zitend-1)).GpuEnd(), begin, drift ) < m_vd.zvStart ) return depth;

    depth++;
    int maxdepth = depth;

    while( it < zitend )
    {
        auto& ev = a(*it);
        auto end = m_worker.GetZoneEnd( ev );
        if( end == std::numeric_limits<int64_t>::max() ) break;
        const auto start = AdjustGpuTime( ev.GpuStart(), begin, drift );
        end = AdjustGpuTime( end, begin, drift );
        const auto zsz = std::max( ( end - start ) * pxns, pxns * 0.5 );
        if( zsz < MinVisSize )
        {
            const auto MinVisNs = MinVisSize * nspx;
            auto px1ns = end - m_vd.zvStart;
            auto nextTime = end + MinVisNs;
            for(;;)
            {
                const auto prevIt = it;
                it = std::lower_bound( it, zitend, nextTime, [begin, drift] ( const auto& l, const auto& r ) { Adapter a; return (uint64_t)AdjustGpuTime( a(l).GpuEnd(), begin, drift ) < (uint64_t)r; } );
                if( it == prevIt ) ++it;
                if( it == zitend ) break;
                const auto nend = AdjustGpuTime( m_worker.GetZoneEnd( a(*it) ), begin, drift );
                const auto nsnext = nend - m_vd.zvStart;
                if( nsnext - px1ns >= MinVisNs * 2 ) break;
                px1ns = nsnext;
                nextTime = nend + nspx;
            }
        }
        else
        {
            if( ev.Child() >= 0 )
            {
                const auto d = DispatchGpuZoneLevel( m_worker.GetGpuChildren( ev.Child() ), hover, pxns, nspx, wpos, _offset, depth, thread, yMin, yMax, begin, drift );
                if( d > maxdepth ) maxdepth = d;
            }
            ++it;
        }
    }
    return maxdepth;
}

}
