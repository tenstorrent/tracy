#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

#include "TracyImGui.hpp"
#include "TracyPrint.hpp"
#include "TracyView.hpp"
#include "../Fonts.hpp"
#include "../public/common/TracyTTDeviceData.hpp"
#include "../server/TracyVector.hpp"
#include "IconsFontAwesome6.h"

namespace tracy
{

namespace
{

constexpr int MaxHeatmapGridDim = 128;
constexpr int RiscBarCount = 5;

static constexpr const char* RiscBarNames[RiscBarCount] = {
    "BRISC", "NCRISC", "TRISC_0", "TRISC_1", "TRISC_2"
};

static float GetCoreHeatmapLegendBarHeight()
{
    return std::max( 6.f, ImGui::GetTextLineHeight() * 0.55f );
}

static float GetCoreHeatmapLegendRowsHeight()
{
    const float legendBarH = GetCoreHeatmapLegendBarHeight();
    const float legendGap = 4.f;
    return float( RiscBarCount ) * ( legendBarH + legendGap ) + 4.f;
}

static float GetCoreHeatmapFooterHeight()
{
    // Help line + RISC legend rows below the grid.
    return ImGui::GetTextLineHeight() + 2.f + GetCoreHeatmapLegendRowsHeight();
}

struct CoreKey
{
    uint64_t chipId;
    uint64_t coreX;
    uint64_t coreY;

    bool operator==( const CoreKey& o ) const
    {
        return chipId == o.chipId && coreX == o.coreX && coreY == o.coreY;
    }
};

struct CoreKeyHash
{
    size_t operator()( const CoreKey& k ) const
    {
        return size_t( k.chipId ) ^ ( size_t( k.coreX ) << 16 ) ^ ( size_t( k.coreY ) << 24 );
    }
};

struct CoreCell
{
    std::array<std::vector<std::pair<int64_t, int64_t>>, RiscBarCount> riscIntervals;
    std::array<int64_t, RiscBarCount> riscBusyNs {};
    int64_t busyNs = 0;
    uint32_t zoneCount = 0;
};

struct DeviceGrid
{
    uint32_t sizeX = 0;
    uint32_t sizeY = 0;
    bool valid = false;
};

static float ComputeCoreHeatmapCellSize( const DeviceGrid& grid, const ImVec2& avail, float scale )
{
    const float footerH = GetCoreHeatmapFooterHeight();
    const float axisRowH = ImGui::GetTextLineHeight();
    const float vertForCells = std::max( 32.f, avail.y - footerH - axisRowH );
    const float horizForCells = std::max( 32.f, avail.x - 16.f );

    return std::clamp(
        std::min( horizForCells / ( float( grid.sizeX ) + 2.f ), vertForCells / ( float( grid.sizeY ) + 2.f ) ),
        8.f * scale, 48.f * scale );
}

struct GpuTimelineWork
{
    const Vector<short_ptr<GpuEvent>>* ptrTimeline = nullptr;
    const Vector<GpuEvent>* magicTimeline = nullptr;
};

struct ParsedGpuCtx
{
    GpuCtxData* gpu = nullptr;
    uint64_t chipId = 0;
    uint64_t logicalX = 0;
    uint64_t logicalY = 0;
    bool hasLogical = false;
};

struct Rgb8
{
    uint8_t r, g, b;
};

static Rgb8 LerpRgb( const Rgb8& a, const Rgb8& b, float t )
{
    return {
        uint8_t( a.r + ( b.r - a.r ) * t ),
        uint8_t( a.g + ( b.g - a.g ) * t ),
        uint8_t( a.b + ( b.b - a.b ) * t )
    };
}

static uint32_t PackRgb( const Rgb8& c )
{
    return 0xFF000000 | ( uint32_t( c.r ) << 16 ) | ( uint32_t( c.g ) << 8 ) | uint32_t( c.b );
}

// Absolute utilization in [0, 1] — modern thermal ramp (dark → blue → teal → gold → coral).
static uint32_t HeatmapColor( float utilization )
{
    utilization = std::clamp( utilization, 0.f, 1.f );

    if( utilization < 0.004f )
    {
        return PackRgb( { 30, 32, 40 } );
    }

    static constexpr struct { float t; Rgb8 c; } stops[] = {
        { 0.00f, { 30,  32,  40 } },
        { 0.12f, { 40,  62,  95 } },
        { 0.30f, { 52,  99,  141 } },
        { 0.50f, { 58,  140, 162 } },
        { 0.70f, { 120, 178, 155 } },
        { 0.85f, { 210, 188, 110 } },
        { 1.00f, { 232, 118, 88 } },
    };

    for( size_t i = 1; i < sizeof( stops ) / sizeof( stops[0] ); ++i )
    {
        if( utilization <= stops[i].t )
        {
            const auto seg = ( utilization - stops[i - 1].t ) / ( stops[i].t - stops[i - 1].t );
            return PackRgb( LerpRgb( stops[i - 1].c, stops[i].c, seg ) );
        }
    }

    return PackRgb( stops[sizeof( stops ) / sizeof( stops[0] ) - 1].c );
}

static float GetCoreUtilization( const CoreCell& cell, int64_t spanNs )
{
    if( spanNs <= 0 ) return 0.f;
    return std::clamp( float( cell.busyNs ) / float( spanNs ), 0.f, 1.f );
}

static float GetRiscUtilization( const CoreCell& cell, int riscBar, int64_t spanNs )
{
    if( riscBar < 0 || riscBar >= RiscBarCount || spanNs <= 0 ) return 0.f;
    return std::clamp( float( cell.riscBusyNs[riscBar] ) / float( spanNs ), 0.f, 1.f );
}

static int RiscToBarIndex( RiscType risc )
{
    switch( risc )
    {
    case RiscType::BRISC: return 0;
    case RiscType::NCRISC: return 1;
    case RiscType::TRISC_0: return 2;
    case RiscType::TRISC_1: return 3;
    case RiscType::TRISC_2: return 4;
    default: return -1;
    }
}

// Color::ColorType values from TracyTTDevice::getMarkerColor (public/common/TracyColor.hpp).
static constexpr uint32_t RiscTimelineColorRaw[RiscBarCount] = {
    0xee9a00,  // Orange2 — BRISC
    0x43cd80,  // SeaGreen3 — NCRISC
    0x6ca6cd,  // SkyBlue3 — TRISC_0
    0x00e5ee,  // Turquoise2 — TRISC_1
    0x98f5ff,  // CadetBlue1 — TRISC_2
};

// Pack like Worker::AddSourceLocationPayload so bars match GPU timeline zone colors.
static uint32_t PackTimelineColor( uint32_t color )
{
    const uint32_t packed =
        ( ( color & 0x00FF0000 ) >> 16 ) |
        ( ( color & 0x0000FF00 ) ) |
        ( ( color & 0x000000FF ) << 16 );
    return 0xFF000000 | packed;
}

static uint32_t RiscBarColor( int riscBar )
{
    if( riscBar < 0 || riscBar >= RiscBarCount ) return 0xFF555555;
    return PackTimelineColor( RiscTimelineColorRaw[riscBar] );
}

static constexpr uint32_t kCellBg = 0xFF1E2028;

static float UtilBarWidth( float util, float innerW, bool hasActivity )
{
    if( !hasActivity ) return 0.f;
    return std::max( util * innerW, 1.f );
}

static int64_t MergeIntervals( std::vector<std::pair<int64_t, int64_t>>& iv )
{
    if( iv.empty() ) return 0;

    std::sort( iv.begin(), iv.end(), [] ( const auto& a, const auto& b ) { return a.first < b.first; } );

    int64_t total = 0;
    int64_t curStart = iv.front().first;
    int64_t curEnd = iv.front().second;

    for( size_t i = 1; i < iv.size(); ++i )
    {
        if( iv[i].first <= curEnd )
        {
            curEnd = std::max( curEnd, iv[i].second );
        }
        else
        {
            total += curEnd - curStart;
            curStart = iv[i].first;
            curEnd = iv[i].second;
        }
    }
    total += curEnd - curStart;

    iv.clear();
    iv.shrink_to_fit();
    return total;
}

static int64_t GetGpuTimelineBegin( const GpuCtxData& gpu )
{
    for( auto& td : gpu.threadData )
    {
        auto& tl = td.second.timeline;
        if( tl.empty() ) continue;
        if( tl.is_magic() )
        {
            auto& tlm = *(Vector<GpuEvent>*)&tl;
            if( tlm.front().GpuStart() >= 0 ) return tlm.front().GpuStart();
        }
        else if( tl.front()->GpuStart() >= 0 )
        {
            return tl.front()->GpuStart();
        }
    }
    return 0;
}

static int64_t AdjustGpuTime( int64_t time, int64_t begin, int drift )
{
    if( time < 0 ) return time;
    const auto t = time - begin;
    return time + t / 1000000000 * drift;
}

static int64_t GpuEndForSearch( const GpuEvent& ev, int64_t begin, int drift, int64_t tMax )
{
    auto end = ev.GpuEnd();
    if( end < 0 ) return tMax;
    return AdjustGpuTime( end, begin, drift );
}

static void AddZoneInterval(
    CoreCell& cell, int riscBar, int64_t zoneStart, int64_t zoneEnd, int64_t tMin, int64_t tMax )
{
    if( riscBar < 0 || riscBar >= RiscBarCount ) return;
    if( zoneEnd < 0 ) zoneEnd = tMax;
    if( zoneStart >= zoneEnd ) return;

    const auto clipStart = std::max( zoneStart, tMin );
    const auto clipEnd = std::min( zoneEnd, tMax );
    if( clipStart >= clipEnd ) return;

    cell.riscIntervals[riscBar].emplace_back( clipStart, clipEnd );
    cell.zoneCount++;
}

static void FinalizeCoreCells( unordered_flat_map<CoreKey, CoreCell, CoreKeyHash>& cells )
{
    for( auto& kv : cells )
    {
        auto& cell = kv.second;
        std::vector<std::pair<int64_t, int64_t>> combined;

        for( int i = 0; i < RiscBarCount; ++i )
        {
            combined.insert(
                combined.end(), cell.riscIntervals[i].begin(), cell.riscIntervals[i].end() );
        }

        cell.busyNs = MergeIntervals( combined );

        for( int i = 0; i < RiscBarCount; ++i )
        {
            cell.riscBusyNs[i] = MergeIntervals( cell.riscIntervals[i] );
        }
    }
}

static void PushGpuChildren( Worker& worker, int32_t childIdx, std::vector<GpuTimelineWork>& stack )
{
    if( childIdx < 0 ) return;
    if( (size_t)childIdx >= worker.GetGpuChildrenCount() ) return;

    auto& ch = worker.GetGpuChildren( childIdx );
    if( ch.empty() ) return;

    if( ch.is_magic() )
    {
        stack.push_back( { nullptr, (Vector<GpuEvent>*)&ch } );
    }
    else
    {
        stack.push_back( { &ch, nullptr } );
    }
}

template<typename Adapter, typename V>
static void CollectGpuTimelineBusy(
    Worker& worker, const V& vec, int64_t begin, int drift, int64_t tMin, int64_t tMax, int riscBar,
    const CoreKey& key, unordered_flat_map<CoreKey, CoreCell, CoreKeyHash>& cells,
    std::vector<GpuTimelineWork>& stack )
{
    Adapter a;

    auto it = std::lower_bound( vec.begin(), vec.end(), std::max<int64_t>( 0, tMin ),
        [begin, drift, tMax] ( const auto& l, const auto& r )
        {
            Adapter ad;
            return GpuEndForSearch( ad( l ), begin, drift, tMax ) < r;
        } );
    if( it == vec.end() ) return;

    const auto zitend = std::lower_bound( it, vec.end(), std::max<int64_t>( 0, tMax ),
        [begin, drift] ( const auto& l, const auto& r )
        {
            Adapter ad;
            return AdjustGpuTime( ad( l ).GpuStart(), begin, drift ) < r;
        } );
    if( it == zitend ) return;

    auto& cell = cells[key];

    while( it < zitend )
    {
        auto& ev = a( *it );

        if( ev.Child() >= 0 )
        {
            PushGpuChildren( worker, ev.Child(), stack );
            ++it;
            continue;
        }

        auto end = ev.GpuEnd();
        if( end < 0 )
        {
            end = tMax;
        }
        else
        {
            end = AdjustGpuTime( end, begin, drift );
        }

        const auto start = AdjustGpuTime( ev.GpuStart(), begin, drift );
        AddZoneInterval( cell, riscBar, start, end, tMin, tMax );

        ++it;
    }
}

static void ProcessGpuTimelineWork(
    Worker& worker, const GpuTimelineWork& work, int64_t begin, int drift, int64_t tMin, int64_t tMax, int riscBar,
    const CoreKey& key, unordered_flat_map<CoreKey, CoreCell, CoreKeyHash>& cells,
    std::vector<GpuTimelineWork>& stack )
{
    if( work.magicTimeline )
    {
        CollectGpuTimelineBusy<VectorAdapterDirect<GpuEvent>>(
            worker, *work.magicTimeline, begin, drift, tMin, tMax, riscBar, key, cells, stack );
    }
    else if( work.ptrTimeline )
    {
        CollectGpuTimelineBusy<VectorAdapterPointer<GpuEvent>>(
            worker, *work.ptrTimeline, begin, drift, tMin, tMax, riscBar, key, cells, stack );
    }
}

static void CollectGpuCtxBusy(
    Worker& worker, const GpuCtxData& gpu, int drift, int64_t tMin, int64_t tMax, const CoreKey& key,
    unordered_flat_map<CoreKey, CoreCell, CoreKeyHash>& cells )
{
    const auto begin = GetGpuTimelineBegin( gpu );
    std::vector<GpuTimelineWork> stack;
    stack.reserve( 64 );

    for( auto& td : gpu.threadData )
    {
        const TTDeviceMarker marker( uint32_t( td.first ) );
        const auto riscBar = RiscToBarIndex( marker.risc );
        if( riscBar < 0 ) continue;

        auto& tl = td.second.timeline;
        if( tl.empty() ) continue;

        stack.clear();
        if( tl.is_magic() )
        {
            stack.push_back( { nullptr, (Vector<GpuEvent>*)&tl } );
        }
        else
        {
            stack.push_back( { &tl, nullptr } );
        }

        while( !stack.empty() )
        {
            const auto work = stack.back();
            stack.pop_back();
            ProcessGpuTimelineWork( worker, work, begin, drift, tMin, tMax, riscBar, key, cells, stack );
        }
    }
}

static bool ParseDeviceGridAppInfo( const char* text, uint64_t& chipId, uint32_t& sizeX, uint32_t& sizeY )
{
    if( !text ) return false;
    if( strncmp( text, "tt_device_grid ", 15 ) != 0 ) return false;

    unsigned long long chip = 0;
    unsigned int gx = 0, gy = 0;
    if( sscanf( text + 15, "chip=%llu size=%ux%u", &chip, &gx, &gy ) != 3 ) return false;

    chipId = chip;
    sizeX = gx;
    sizeY = gy;
    return sizeX > 0 && sizeY > 0 && sizeX <= MaxHeatmapGridDim && sizeY <= MaxHeatmapGridDim;
}

static unordered_flat_map<uint64_t, DeviceGrid> GetDeviceGrids( const Worker& worker )
{
    unordered_flat_map<uint64_t, DeviceGrid> grids;

    for( auto& info : worker.GetAppInfo() )
    {
        uint64_t chipId = 0;
        uint32_t sizeX = 0, sizeY = 0;
        if( ParseDeviceGridAppInfo( worker.GetString( info ), chipId, sizeX, sizeY ) )
        {
            grids[chipId] = DeviceGrid { sizeX, sizeY, true };
        }
    }

    return grids;
}

static bool ParseGpuContextLabel( const char* label, ParsedGpuCtx& out )
{
    if( !label ) return false;

    unsigned long long chip = 0;
    unsigned int lx = 0, ly = 0, px = 0, py = 0;
    if( sscanf( label, "Device: %llu, Logical (%u,%u) Physical (%u,%u)", &chip, &lx, &ly, &px, &py ) == 5 )
    {
        out.chipId = chip;
        out.logicalX = lx;
        out.logicalY = ly;
        out.hasLogical = true;
        return true;
    }

    if( sscanf( label, "Device %llu:", &chip ) == 1 )
    {
        out.chipId = chip;
        out.hasLogical = false;
        return true;
    }

    return false;
}

static void InferDeviceGridFromContexts(
    const std::vector<ParsedGpuCtx>& contexts, uint64_t chipId, DeviceGrid& grid )
{
    uint32_t maxX = 0;
    uint32_t maxY = 0;
    bool found = false;

    for( auto& ctx : contexts )
    {
        if( ctx.chipId != chipId || !ctx.hasLogical ) continue;
        found = true;
        maxX = std::max( maxX, uint32_t( ctx.logicalX ) );
        maxY = std::max( maxY, uint32_t( ctx.logicalY ) );
    }

    if( found )
    {
        grid.sizeX = maxX + 1;
        grid.sizeY = maxY + 1;
        grid.valid = grid.sizeX <= MaxHeatmapGridDim && grid.sizeY <= MaxHeatmapGridDim;
    }
}

static void DrawCoreHeatmapGrid(
    const unordered_flat_map<CoreKey, CoreCell, CoreKeyHash>& cells,
    const DeviceGrid& grid, int64_t spanNs, float cellSize, const char* hoverLabel )
{
    if( !grid.valid ) return;

    const auto cols = int( grid.sizeX );
    const auto rows = int( grid.sizeY );
    const auto gridW = cols * cellSize;
    const auto gridH = rows * cellSize;

    const auto cursor = ImGui::GetCursorScreenPos();
    auto draw = ImGui::GetWindowDrawList();

    ImGui::Dummy( ImVec2( gridW + cellSize * 2, gridH + cellSize * 2 ) );
    const auto gridOrigin = cursor + ImVec2( cellSize, cellSize );

    ImGui::PushFont( g_fonts.normal, FontSmall );
    for( int x = 0; x < cols; x++ )
    {
        char label[16];
        snprintf( label, sizeof( label ), "%d", x );
        const auto tsz = ImGui::CalcTextSize( label );
        draw->AddText(
            gridOrigin + ImVec2( x * cellSize + ( cellSize - tsz.x ) * 0.5f, -tsz.y - 2 ),
            0xFFAAAAAA, label );
    }
    for( int y = 0; y < rows; y++ )
    {
        char label[16];
        snprintf( label, sizeof( label ), "%d", y );
        draw->AddText(
            gridOrigin + ImVec2( -ImGui::CalcTextSize( label ).x - 4, y * cellSize + ( cellSize - ImGui::GetTextLineHeight() ) * 0.5f ),
            0xFFAAAAAA, label );
    }
    ImGui::PopFont();

    TextDisabledUnformatted( "logical X →" );
    ImGui::SameLine( gridW );
    TextDisabledUnformatted( "logical Y ↓" );

    const auto chipId = cells.empty() ? 0 : cells.begin()->first.chipId;

    const float pad = std::max( 1.f, cellSize * 0.08f );
    const float barGap = std::max( 0.5f, cellSize * 0.04f );
    const float innerW = cellSize - pad * 2.f;
    const float innerH = cellSize - pad * 2.f;
    const float aggBarH = std::max( 3.f, innerH * 0.24f );
    const float riscAreaH = innerH - aggBarH - barGap;
    const float barH = ( riscAreaH - barGap * float( RiscBarCount - 1 ) ) / float( RiscBarCount );

    CoreKey hoverKey {};
    bool hasHover = false;
    float hoverUtil = 0.f;
    CoreCell hoverCell {};

    for( int y = 0; y < rows; y++ )
    {
        for( int x = 0; x < cols; x++ )
        {
            const CoreKey key { chipId, uint64_t( x ), uint64_t( y ) };
            const auto it = cells.find( key );

            const auto p0 = gridOrigin + ImVec2( x * cellSize, y * cellSize );
            const auto p1 = p0 + ImVec2( cellSize - 1, cellSize - 1 );
            draw->AddRectFilled( p0, p1, kCellBg );
            draw->AddRect( p0, p1, 0x44FFFFFF );

            if( it != cells.end() )
            {
                const auto& cell = it->second;
                const float coreUtil = GetCoreUtilization( cell, spanNs );
                const bool coreActive = cell.busyNs > 0;
                const float coreBarW = UtilBarWidth( coreUtil, innerW, coreActive );
                const auto aggY = p0.y + pad;
                draw->AddRectFilled(
                    ImVec2( p0.x + pad, aggY ),
                    ImVec2( p0.x + pad + coreBarW, aggY + aggBarH ),
                    HeatmapColor( coreUtil ) );

                const auto riscBaseY = aggY + aggBarH + barGap;
                for( int ri = 0; ri < RiscBarCount; ++ri )
                {
                    const float riscUtil = GetRiscUtilization( cell, ri, spanNs );
                    const bool riscActive = cell.riscBusyNs[ri] > 0;
                    const float barW = UtilBarWidth( riscUtil, innerW, riscActive );
                    const auto barY = riscBaseY + float( ri ) * ( barH + barGap );
                    draw->AddRectFilled(
                        ImVec2( p0.x + pad, barY ),
                        ImVec2( p0.x + pad + barW, barY + barH ),
                        RiscBarColor( ri ) );
                }
            }

            if( ImGui::IsMouseHoveringRect( p0, p1 ) )
            {
                hasHover = true;
                hoverKey = key;
                hoverUtil = ( it != cells.end() ) ? GetCoreUtilization( it->second, spanNs ) : 0.f;
                hoverCell = it != cells.end() ? it->second : CoreCell {};
            }
        }
    }

    if( hasHover )
    {
        char coreBuf[64];
        char utilBuf[32];
        snprintf( coreBuf, sizeof( coreBuf ), "%" PRIu64 ", %" PRIu64, hoverKey.coreX, hoverKey.coreY );
        snprintf( utilBuf, sizeof( utilBuf ), "%.1f%%", hoverUtil * 100.f );

        ImGui::BeginTooltip();
        ImGui::TextUnformatted( hoverLabel );
        TextFocused( "Logical core (x, y):", coreBuf );
        TextFocused( "Core utilization:", utilBuf );
        TextFocused( "Busy time:", TimeToString( hoverCell.busyNs ) );
        TextFocused( "Zones:", RealToString( hoverCell.zoneCount ) );
        for( int ri = 0; ri < RiscBarCount; ++ri )
        {
            char riscLabel[32];
            char riscBuf[32];
            snprintf( riscLabel, sizeof( riscLabel ), "%s:", RiscBarNames[ri] );
            snprintf(
                riscBuf, sizeof( riscBuf ), "%.1f%%",
                GetRiscUtilization( hoverCell, ri, spanNs ) * 100.f );
            TextFocused( riscLabel, riscBuf );
        }
        ImGui::EndTooltip();
    }

    TextDisabledUnformatted( "Top bar = core utilization; lower bars = per-RISC utilization (timeline colors)" );

    const float legendBarW = 48.f;
    const float legendBarH = GetCoreHeatmapLegendBarHeight();
    const float legendGap = 4.f;

    ImGui::Dummy( ImVec2( std::max( gridW + cellSize * 2.f, legendBarW + 120.f ), GetCoreHeatmapLegendRowsHeight() ) );
    const auto legendOrigin = ImGui::GetItemRectMin();

    for( int ri = 0; ri < RiscBarCount; ++ri )
    {
        const auto rowY = legendOrigin.y + float( ri ) * ( legendBarH + legendGap );
        const auto rowPos = ImVec2( legendOrigin.x, rowY );
        draw->AddRectFilled(
            rowPos, rowPos + ImVec2( legendBarW, legendBarH ), RiscBarColor( ri ) );
        draw->AddText(
            rowPos + ImVec2( legendBarW + 6.f, rowY + ( legendBarH - ImGui::GetTextLineHeight() ) * 0.5f ),
            0xFFCCCCCC, RiscBarNames[ri] );
    }
}

}  // namespace

void View::DrawCoreHeatmap()
{
    const auto scale = GetScale();
    ImGui::SetNextWindowSize( ImVec2( 760 * scale, 700 * scale ), ImGuiCond_FirstUseEver );
    ImGui::SetNextWindowSizeConstraints(
        ImVec2( 480 * scale, 420 * scale ), ImVec2( FLT_MAX, FLT_MAX ) );
    ImGui::Begin( "Core activity", &m_showCoreHeatmap );
    if( ImGui::GetCurrentWindowRead()->SkipItems ) { ImGui::End(); return; }

    const int64_t tMin = m_vd.zvStart;
    const int64_t tMax = m_vd.zvEnd;
    const int64_t spanNs = tMax - tMin;

    if( spanNs <= 0 )
    {
        ImGui::TextWrapped( "Visible time range is empty. Pan or zoom the timeline to select a range." );
        ImGui::End();
        return;
    }

    {
        char spanBuf[128];
        snprintf( spanBuf, sizeof( spanBuf ), "%s – %s", TimeToString( tMin ), TimeToString( tMax ) );
        TextFocused( "Time span:", spanBuf );
    }
    TextFocused( "Duration:", TimeToString( spanNs ) );
    ImGui::Separator();

    const auto deviceGrids = GetDeviceGrids( m_worker );

    std::vector<ParsedGpuCtx> parsedContexts;
    parsedContexts.reserve( m_worker.GetGpuData().size() );
    for( auto* gpu : m_worker.GetGpuData() )
    {
        if( gpu->type != GpuContextType::tt_device ) continue;

        ParsedGpuCtx parsed;
        parsed.gpu = gpu;
        if( !ParseGpuContextLabel( GetGpuContextLabel( gpu ), parsed ) ) continue;
        parsedContexts.push_back( parsed );
    }

    if( parsedContexts.empty() )
    {
        ImGui::PushFont( g_fonts.normal, FontBig );
        ImGui::Dummy( ImVec2( 0, ( ImGui::GetContentRegionAvail().y - ImGui::GetTextLineHeight() * 2 ) * 0.5f ) );
        TextCentered( ICON_FA_TABLE_CELLS );
        TextCentered( "No TT device data in this trace" );
        ImGui::PopFont();
        ImGui::End();
        return;
    }

    unordered_flat_map<uint64_t, std::vector<ParsedGpuCtx*>> contextsByChip;
    for( auto& ctx : parsedContexts )
    {
        contextsByChip[ctx.chipId].push_back( &ctx );
    }

    for( auto& entry : contextsByChip )
    {
        const auto chipId = entry.first;
        auto& contexts = entry.second;

        DeviceGrid grid;
        auto git = deviceGrids.find( chipId );
        if( git != deviceGrids.end() )
        {
            grid = git->second;
        }
        else
        {
            InferDeviceGridFromContexts( parsedContexts, chipId, grid );
            if( grid.valid )
            {
                ImGui::TextWrapped(
                    "Device %llu: using inferred grid %ux%u (re-capture with current tt-metal for compute_with_storage_grid_size).",
                    (unsigned long long)chipId, grid.sizeX, grid.sizeY );
            }
        }

        if( !grid.valid )
        {
            ImGui::TextWrapped(
                "Device %llu: no compute grid metadata. Expected app info: tt_device_grid chip=N size=XxY",
                (unsigned long long)chipId );
            ImGui::Separator();
            continue;
        }

        unordered_flat_map<CoreKey, CoreCell, CoreKeyHash> cells;

        for( auto* parsed : contexts )
        {
            if( !parsed->hasLogical ) continue;

            const CoreKey key { chipId, parsed->logicalX, parsed->logicalY };
            CollectGpuCtxBusy( m_worker, *parsed->gpu, GpuDrift( parsed->gpu ), tMin, tMax, key, cells );
        }

        FinalizeCoreCells( cells );

        char header[128];
        snprintf( header, sizeof( header ), "Device %llu  (%ux%u compute grid)", (unsigned long long)chipId, grid.sizeX, grid.sizeY );
        ImGui::PushFont( g_fonts.normal, FontBig );
        ImGui::TextUnformatted( header );
        ImGui::PopFont();

        if( cells.empty() )
        {
            ImGui::TextUnformatted( "No device zones in the visible range." );
        }
        else
        {
            const auto avail = ImGui::GetContentRegionAvail();
            const auto cellSize = ComputeCoreHeatmapCellSize( grid, avail, scale );
            DrawCoreHeatmapGrid( cells, grid, spanNs, cellSize, header );
        }

        ImGui::Separator();
    }

    ImGui::End();
}

}
