#include "TracyColor.hpp"
#include "TracyImGui.hpp"
#include "TracyMouse.hpp"
#include "TracyPrint.hpp"
#include "TracyTimelineContext.hpp"
#include "TracyTimelineDraw.hpp"
#include "TracyUtility.hpp"
#include "TracyView.hpp"
#include "../Fonts.hpp"
#include "../public/common/TracyTTDeviceData.hpp"

namespace tracy
{

constexpr float MinVisSize = 3;

bool View::DrawGpu( const TimelineContext& ctx, const GpuCtxData& gpu, const std::vector<GpuLaneDraw>& lanes, int& offset )
{
    const auto w = ctx.w;
    const auto ty = ctx.ty;
    const auto ostep = ty + 1;
    const auto& wpos = ctx.wpos;
    const auto dpos = wpos + ImVec2( 0.5f, 0.5f );

    auto draw = ImGui::GetWindowDrawList();

    ImGui::PushFont( g_fonts.normal, FontSmall );
    const auto sty = ImGui::GetTextLineHeight();
    const auto sstep = sty + 1;
    ImGui::PopFont();

    const auto singleThread = gpu.threadData.size() == 1;
    const auto drift = GpuDrift( &gpu );
    int depth = 0;

    for( auto& lane : lanes )
    {
        auto td = gpu.threadData.find( lane.tid );
        if( td == gpu.threadData.end() ) continue;
        const auto begin = lane.begin >= 0 ? lane.begin : 0;
        if( !singleThread ) offset += sstep;

        // Marker row goes above the lane's zones, the way messages sit above a thread's zones.
        const int markerRows = DrawGpuMarkers( ctx, td->second.markers, offset, begin, drift ) ? 1 : 0;
        if( lane.depth != 0 ) DrawGpuZoneList( ctx, lane.draw, offset + ostep * markerRows, gpu.thread, begin, drift );

        const int rows = lane.depth + markerRows;
        if( rows != 0 )
        {
            if( !singleThread )
            {
                ImGui::PushFont( g_fonts.normal, FontSmall );
                DrawTextContrast( draw, wpos + ImVec2( ty, offset-1-sstep ), 0xFFFFAAAA, m_worker.GetThreadName( lane.tid ) );
                DrawLine( draw, dpos + ImVec2( 0, offset+sty-sstep ), dpos + ImVec2( w, offset+sty-sstep ), 0x22FFAAAA );
                ImGui::PopFont();
            }
            offset += ostep * rows;
            depth += rows;
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

void View::DrawGpuZoneList( const TimelineContext& ctx, const std::vector<TimelineDraw>& drawList, int _offset, uint64_t thread, int64_t begin, int drift )
{
    auto draw = ImGui::GetWindowDrawList();
    const auto w = ctx.w;
    const auto& wpos = ctx.wpos;
    const auto dpos = wpos + ImVec2( 0.5f, 0.5f );
    const auto ty = ctx.ty;
    const auto ostep = ty + 1;
    const auto yMin = ctx.yMin;
    const auto yMax = ctx.yMax;
    const auto pxns = ctx.pxns;
    const auto hover = ctx.hover;
    const auto vStart = ctx.vStart;
    // Below one glyph of width a zone shows no label and its name is not measured.
    const auto minLabelWidth = ty * 0.5f;

    for( auto& v : drawList )
    {
        const auto offset = _offset + ostep * v.depth;
        const auto yPos = wpos.y + offset;
        if( yPos > yMax || yPos + ostep < yMin ) continue;

        auto& ev = *(const GpuEvent*)v.ev.get();
        const auto start = AdjustGpuTime( ev.GpuStart(), begin, drift );

        switch( v.type )
        {
        case TimelineDrawType::Folded:
        {
            const auto color = v.inheritedColor ? v.inheritedColor : GetZoneColor( ev );
            const auto rend = AdjustGpuTime( v.rend.Val(), begin, drift );
            const auto px0 = ( start - vStart ) * pxns;
            const auto px1 = ( rend - vStart ) * pxns;
            draw->AddRectFilled( wpos + ImVec2( std::max( px0, -10.0 ), offset ), wpos + ImVec2( std::min( std::max( px1, px0+MinVisSize ), double( w + 10 ) ), offset + ty ), color );
            DrawZigZag( draw, wpos + ImVec2( 0, offset + ty/2 ), std::max( px0, -10.0 ), std::min( std::max( px1, px0+MinVisSize ), double( w + 10 ) ), ty/4, DarkenColor( color ) );
            if( hover && ImGui::IsMouseHoveringRect( wpos + ImVec2( std::max( px0, -10.0 ), offset ), wpos + ImVec2( std::min( std::max( px1, px0+MinVisSize ), double( w + 10 ) ), offset + ty + 1 ) ) )
            {
                if( IsMouseClickReleased( ImGuiMouseButton_Right ) ) m_setRangePopup = RangeSlim { start, rend, true };
                if( v.num > 1 )
                {
                    ImGui::BeginTooltip();
                    TextFocused( "Zones too small to display:", RealToString( v.num ) );
                    ImGui::Separator();
                    TextFocused( "Execution time:", TimeToString( rend - start ) );
                    ImGui::EndTooltip();

                    if( IsMouseClicked( ImGuiMouseButton_Middle ) && rend - start > 0 )
                    {
                        ZoomToRange( start, rend );
                    }
                }
                else
                {
                    const auto zoneThread = thread != 0 ? thread : m_worker.DecompressThread( ev.Thread() );
                    ZoneTooltip( ev );

                    if( IsMouseClicked( ImGuiMouseButton_Middle ) && rend - start > 0 )
                    {
                        ZoomToZone( ev );
                    }
                    if( IsMouseClicked( ImGuiMouseButton_Left ) )
                    {
                        if( ImGui::GetIO().KeyCtrl )
                        {
                            auto& srcloc = m_worker.GetSourceLocation( ev.SrcLoc() );
                            ShowFindZoneGpu( ev.SrcLoc(), m_worker.GetString( srcloc.name.active ? srcloc.name : srcloc.function ) );
                        }
                        else
                        {
                            ShowZoneInfo( ev, zoneThread );
                        }
                    }

                    m_gpuHover = &ev;
                    m_gpuThread = zoneThread;
                    m_gpuStart = ev.CpuStart();
                    m_gpuEnd = ev.CpuEnd();
                }
            }
            if( px1 - px0 >= minLabelWidth )
            {
                const auto tmp = RealToString( v.num );
                const auto tsz = ImGui::CalcTextSize( tmp );
                if( tsz.x < px1 - px0 )
                {
                    const auto x = px0 + ( px1 - px0 - tsz.x ) / 2;
                    DrawTextContrast( draw, wpos + ImVec2( x, offset ), 0xFF4488DD, tmp );
                }
            }
            break;
        }
        case TimelineDrawType::Zone:
        {
            const auto end = AdjustGpuTime( m_worker.GetZoneEnd( ev ), begin, drift );
            const auto zsz = std::max( ( end - start ) * pxns, pxns * 0.5 );
            const auto pr0 = ( start - vStart ) * pxns;
            const auto pr1 = ( end - vStart ) * pxns;
            const auto px0 = std::max( pr0, -10.0 );
            const auto px1 = std::max( { std::min( pr1, double( w + 10 ) ), px0 + pxns * 0.5, px0 + MinVisSize } );

            const char* zoneName = m_worker.GetZoneName( ev );
            const bool label = px1 - px0 >= minLabelWidth;
            auto tsz = label ? ImGui::CalcTextSize( zoneName ) : ImVec2( 0, ty );
            if( label && ( m_vd.shortenName == ShortenName::Always || ( ( m_vd.shortenName == ShortenName::NoSpace || m_vd.shortenName == ShortenName::NoSpaceAndNormalize ) && tsz.x > zsz ) ) )
            {
                zoneName = ShortenZoneName( m_vd.shortenName, zoneName, tsz, zsz );
            }
            const auto zoneColor = GetZoneColorData( ev, v.inheritedColor );
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
            if( label )
            {
                if( tsz.x < zsz )
                {
                    const auto x = ( start - vStart ) * pxns + ( ( end - start ) * pxns - tsz.x ) / 2;
                    if( x < 0 || x > w - tsz.x )
                    {
                        const auto tx = std::max( std::max( 0., px0 ), std::min( double( w - tsz.x ), x ) );
                        DrawTextContrastClipped( draw, wpos + ImVec2( tx, offset ), 0xFFFFFFFF, zoneName, px1 - tx );
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
                    const auto tx = std::max( pr0, 0. );
                    DrawTextContrastClipped( draw, wpos + ImVec2( tx, offset ), 0xFFFFFFFF, zoneName, px1 - tx );
                }
            }

            if( hover && ImGui::IsMouseHoveringRect( wpos + ImVec2( px0, offset ), wpos + ImVec2( px1, offset + tsz.y + 1 ) ) )
            {
                const auto zoneThread = thread != 0 ? thread : m_worker.DecompressThread( ev.Thread() );
                ZoneTooltip( ev );
                if( IsMouseClickReleased( ImGuiMouseButton_Right ) ) m_setRangePopup = RangeSlim { start, end, true };

                if( !m_zoomAnim.active && IsMouseClicked( ImGuiMouseButton_Middle ) )
                {
                    ZoomToZone( ev );
                }
                if( IsMouseClicked( ImGuiMouseButton_Left ) )
                {
                    if( ImGui::GetIO().KeyCtrl )
                    {
                        auto& srcloc = m_worker.GetSourceLocation( ev.SrcLoc() );
                        ShowFindZoneGpu( ev.SrcLoc(), m_worker.GetString( srcloc.name.active ? srcloc.name : srcloc.function ) );
                    }
                    else
                    {
                        ShowZoneInfo( ev, zoneThread );
                    }
                }

                m_gpuHover = &ev;
                m_gpuThread = zoneThread;
                m_gpuStart = ev.CpuStart();
                m_gpuEnd = ev.CpuEnd();
            }
            break;
        }
        default:
            break;
        }
    }
}

}
