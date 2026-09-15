#ifndef __TRACYTIMELINEITEMGPU_HPP__
#define __TRACYTIMELINEITEMGPU_HPP__

#include <vector>

#include "TracyEvent.hpp"
#include "TracyTimelineDraw.hpp"
#include "TracyTimelineItem.hpp"

namespace tracy
{

class TimelineItemGpu final : public TimelineItem
{
public:
    TimelineItemGpu( View& view, Worker& worker, GpuCtxData* gpu );

    int GetIdx() const { return m_idx; }

protected:
    uint32_t HeaderColor() const override { return 0xFFFFAAAA; }
    uint32_t HeaderColorInactive() const override { return 0xFF886666; }
    uint32_t HeaderLineColor() const override { return 0x33FFFFFF; }
    const char* HeaderLabel() const override;

    int64_t RangeBegin() const override;
    int64_t RangeEnd() const override;

    void HeaderTooltip( const char* label ) const override;
    void HeaderExtraContents( const TimelineContext& ctx, int offset, float labelWidth ) override;

    bool DrawContents( const TimelineContext& ctx, int& offset ) override;
    void DrawFinished() override;

    bool IsEmpty() const override;

    void Preprocess( const TimelineContext& ctx, TaskDispatch& td, bool visible, int yPos ) override;
    bool MeasureOffscreenLazily() const override { return true; }
    bool MeasureIsCurrent( const TimelineContext& ctx ) const override;

private:
    void PreprocessLane( const TimelineContext& ctx, const GpuCtxThreadData& td, bool visible, int drift, GpuLaneDraw& lane );
    int PreprocessZoneLevel( const TimelineContext& ctx, const Vector<short_ptr<GpuEvent>>& vec, int depth, bool visible, int64_t begin, int drift, uint32_t inheritedColor, std::vector<TimelineDraw>& draw );

    template<typename Adapter, typename V>
    int PreprocessZoneLevel( const TimelineContext& ctx, const V& vec, int depth, bool visible, int64_t begin, int drift, uint32_t inheritedColor, std::vector<TimelineDraw>& draw );

    GpuCtxData* m_gpu;
    int m_idx;
    std::vector<GpuLaneDraw> m_lanes;
    int64_t m_measuredStart = 0;
    int64_t m_measuredEnd = 0;
    double m_measuredNspx = 0;
    uint64_t m_measuredCount = 0;
};

}

#endif
