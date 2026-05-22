#include <algorithm>
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
    std::vector<std::pair<int64_t, int64_t>> intervals;
    int64_t busyNs = 0;
    uint32_t zoneCount = 0;
};

struct DeviceGrid
{
    uint32_t sizeX = 0;
    uint32_t sizeY = 0;
    bool valid = false;
};

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

static void AddZoneInterval( CoreCell& cell, int64_t zoneStart, int64_t zoneEnd, int64_t tMin, int64_t tMax )
{
    if( zoneEnd < 0 ) zoneEnd = tMax;
    if( zoneStart >= zoneEnd ) return;

    const auto clipStart = std::max( zoneStart, tMin );
    const auto clipEnd = std::min( zoneEnd, tMax );
    if( clipStart >= clipEnd ) return;

    cell.intervals.emplace_back( clipStart, clipEnd );
    cell.zoneCount++;
}

static void FinalizeCoreCells( unordered_flat_map<CoreKey, CoreCell, CoreKeyHash>& cells )
{
    for( auto& kv : cells )
    {
        auto& cell = kv.second;
        auto& iv = cell.intervals;
        if( iv.empty() )
        {
            cell.busyNs = 0;
            continue;
        }

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

        cell.busyNs = total;
        iv.clear();
        iv.shrink_to_fit();
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
    Worker& worker, const V& vec, int64_t begin, int drift, int64_t tMin, int64_t tMax,
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
        AddZoneInterval( cell, start, end, tMin, tMax );

        ++it;
    }
}

static void ProcessGpuTimelineWork(
    Worker& worker, const GpuTimelineWork& work, int64_t begin, int drift, int64_t tMin, int64_t tMax,
    const CoreKey& key, unordered_flat_map<CoreKey, CoreCell, CoreKeyHash>& cells,
    std::vector<GpuTimelineWork>& stack )
{
    if( work.magicTimeline )
    {
        CollectGpuTimelineBusy<VectorAdapterDirect<GpuEvent>>(
            worker, *work.magicTimeline, begin, drift, tMin, tMax, key, cells, stack );
    }
    else if( work.ptrTimeline )
    {
        CollectGpuTimelineBusy<VectorAdapterPointer<GpuEvent>>(
            worker, *work.ptrTimeline, begin, drift, tMin, tMax, key, cells, stack );
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
            ProcessGpuTimelineWork( worker, work, begin, drift, tMin, tMax, key, cells, stack );
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
            const float util = ( it != cells.end() ) ? GetCoreUtilization( it->second, spanNs ) : 0.f;
            const auto color = HeatmapColor( util );

            const auto p0 = gridOrigin + ImVec2( x * cellSize, y * cellSize );
            const auto p1 = p0 + ImVec2( cellSize - 1, cellSize - 1 );
            draw->AddRectFilled( p0, p1, color );
            draw->AddRect( p0, p1, 0x44FFFFFF );

            if( ImGui::IsMouseHoveringRect( p0, p1 ) )
            {
                hasHover = true;
                hoverKey = key;
                hoverUtil = util;
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
        TextFocused( "Utilization:", utilBuf );
        TextFocused( "Busy time:", TimeToString( hoverCell.busyNs ) );
        TextFocused( "Zones:", RealToString( hoverCell.zoneCount ) );
        ImGui::EndTooltip();
    }

    ImGui::Dummy( ImVec2( 0, 4 ) );
    const auto legendY = ImGui::GetCursorScreenPos().y;
    const auto legendW = 200.f;
    const auto legendH = 12.f;
    ImGui::Dummy( ImVec2( legendW, legendH + 4 ) );
    const auto legendPos = ImVec2( ImGui::GetItemRectMin().x, legendY );
    for( int i = 0; i < 64; i++ )
    {
        const auto t = i / 63.f;
        const auto x0 = legendPos.x + legendW * t;
        const auto x1 = legendPos.x + legendW * ( i + 1 ) / 63.f;
        draw->AddRectFilled( ImVec2( x0, legendPos.y ), ImVec2( x1, legendPos.y + legendH ), HeatmapColor( t ) );
    }
    ImGui::SameLine();
    ImGui::TextUnformatted( "0%" );
    ImGui::SameLine( legendW - 32 );
    ImGui::TextUnformatted( "100%" );
}

}  // namespace

void View::DrawCoreHeatmap()
{
    const auto scale = GetScale();
    ImGui::SetNextWindowSize( ImVec2( 720 * scale, 520 * scale ), ImGuiCond_FirstUseEver );
    ImGui::Begin( "Core activity", &m_showCoreHeatmap, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse );
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
            const auto cellSize = std::clamp(
                std::min( ( avail.x - 40.f ) / grid.sizeX, ( avail.y - 80.f ) / grid.sizeY ),
                8.f * scale, 48.f * scale );

            DrawCoreHeatmapGrid( cells, grid, spanNs, cellSize, header );
        }

        ImGui::Separator();
    }

    ImGui::End();
}

}
