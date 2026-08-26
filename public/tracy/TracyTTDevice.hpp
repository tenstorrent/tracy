#ifndef __TRACYTTDEVICE_HPP__
#define __TRACYTTDEVICE_HPP__

#if !defined TRACY_ENABLE

#define TracyTTContext() nullptr
#define TracyTTDestroy(c)
#define TracyTTContextName(c, x, y)
#define TracyTTContextNameLockfree(c, x, y)
#define TracyTTContextPopulate(c, x, y, z)
#define TracyTTContextPopulateCalibrated(c, x, y, z)
#define TracyTTContextPopulateCalibratedLockfree(c, x, y, z)
#define TracyTTPushStartZone(c, e)
#define TracyTTPushEndZone(c, e)
#define TracyTTPushMarker(c, e)
#define TracyTTPushZone(c, s, t, b, e)
#define TracyTTPushZoneSerial(c, s, t, b, e)

#define TracyGetTimerMul() 0
#define TracyGetBaseTime() 0
#define TracyGetCpuTime() 0
#define TracySetCpuTime( t )

namespace tracy
{
    class TTCtxScope {};
}

using TracyTTCtx = void*;

#else

#include <algorithm>
#include <atomic>
#include <cassert>
#include <limits>
#include <sstream>
#include <fstream>
#include <cmath>

#include "Tracy.hpp"
#include "../client/TracyCallstack.hpp"
#include "../client/TracyProfiler.hpp"
#include "../common/TracyAlloc.hpp"
#include "../common/TracyTTDeviceData.hpp"

#define TRACY_TT_TO_STRING_INDIRECT(T) #T
#define TRACY_TT_TO_STRING(T) TRACY_TT_TO_STRING_INDIRECT(T)
#define TRACY_TT_ASSERT(p) if(!(p)) {                                                         \
    TracyMessageL( "TRACY_TT_ASSERT failed on " TracyFile ":" TRACY_TT_TO_STRING(TracyLine) );  \
    assert(false && "TRACY_TT_ASSERT failed");                                                \
}

namespace tracy {

    enum class EventPhase : uint8_t
    {
        Begin,
        End
    };

    inline int64_t m_tcpu = 0;

    static inline double get_tracy_timer_mul()
    {
        return tracy::GetProfiler().m_timerMul;
    }

    static inline int64_t get_tracy_base_time()
    {
        return tracy::GetInitTime();
    }
    
    static inline int64_t get_cpu_time()
    {
        return tracy::Profiler::GetTime();
    }

    static inline void set_cpu_sync_time(int64_t tcpu)
    {
        m_tcpu = tcpu;
    }

    struct EventInfo
    {
        TTDeviceMarker event;
        EventPhase phase;
    };

    class TTCtx
    {
    public:
        enum { QueryCount = 64 * 1024 };

        TTCtx() : m_contextId(GetGpuCtxCounter().fetch_add(1, std::memory_order_relaxed)), m_head(0), m_tail(0) {}

        void PopulateTTContext(int64_t tcpu, double tgpu, double frequency) {
            m_frequency = frequency;
            m_tgpu = tgpu;
            if (tcpu == 0) {
                tcpu = m_tcpu;
            }

            auto item = Profiler::QueueSerial();
            MemWrite(&item->hdr.type, QueueType::GpuNewContext);
            MemWrite(&item->gpuNewContext.cpuTime, tcpu);
            MemWrite(&item->gpuNewContext.gpuTime, (int64_t)round((double)m_tgpu / m_frequency));
            memset(&item->gpuNewContext.thread, 0, sizeof(item->gpuNewContext.thread));
            MemWrite(&item->gpuNewContext.period, (float)1.0f);
            MemWrite(&item->gpuNewContext.type, GpuContextType::tt_device);
            MemWrite(&item->gpuNewContext.context, GetId());
            // No GPU drift-calibration for tt_device contexts. The Tensix wall clock is a free-running
            // ABSOLUTE counter that (after / frequency) is already in nanoseconds at the host clock's rate,
            // so an anchor-only mapping (server: gpuTime = tgpu + timeDiff) is exact. With the calibration
            // flag set, the server instead derives a drift scale (calibrationMod) from the FIRST calibration
            // delta -- but our anchor gpuTime is ~0 while device timestamps are absolute (~5e9 ns), so that
            // delta is bogus and calibrationMod comes out ~0.11, shrinking every zone duration ~9x. Omitting
            // the flag keeps durations correct (was GpuContextCalibration).
            MemWrite(&item->gpuNewContext.flags, (uint8_t)0);
            Profiler::QueueSerialFinish();

            mm_tcpu = tcpu;
        }

        // Same anchor mapping as PopulateTTContext but marks the context CALIBRATED, so the Tracy GUI does
        // NOT show the per-context manual "Drift (ns/s)/Auto" control (server shows it only when
        // !hasCalibration). We send NO GpuCalibration events, so calibrationMod stays 1.0 and the mapping is
        // identical to the uncalibrated path (gpuTime = tgpu + timeDiff either way). For consumers whose
        // device timestamps are host-rebased (perf-debug profiler): the anchor is exact, no drift wanted.
        void PopulateTTContextCalibrated(int64_t tcpu, double tgpu, double frequency) {
            m_frequency = frequency;
            m_tgpu = tgpu;
            if (tcpu == 0) {
                tcpu = m_tcpu;
            }
            auto item = Profiler::QueueSerial();
            MemWrite(&item->hdr.type, QueueType::GpuNewContext);
            MemWrite(&item->gpuNewContext.cpuTime, tcpu);
            MemWrite(&item->gpuNewContext.gpuTime, (int64_t)round((double)m_tgpu / m_frequency));
            memset(&item->gpuNewContext.thread, 0, sizeof(item->gpuNewContext.thread));
            // period = ns per timestamp unit, and it MUST be 1.0 here: PushStartMarker/PushEndMarker already
            // convert device cycles to ns themselves (they send `marker.timestamp / m_frequency`), so the
            // values on the wire are ALREADY nanoseconds. An earlier revision set this to 1/frequency on the
            // false premise that raw CYCLES were pushed -- that double-divided and shrank every device zone
            // by exactly aiclk_GHz (a 6.38 us zone displayed as 4.73 us at 1.35 GHz). If you ever switch the
            // push path to send raw cycles, change BOTH sites together.
            MemWrite(&item->gpuNewContext.period, (float)1.0f);
            MemWrite(&item->gpuNewContext.type, GpuContextType::tt_device);
            MemWrite(&item->gpuNewContext.context, GetId());
            MemWrite(&item->gpuNewContext.flags, (uint8_t)GpuContextCalibration);
            Profiler::QueueSerialFinish();
            mm_tcpu = tcpu;
        }

        // Same as PopulateTTContextCalibrated but on the per-thread lock-free queue rather than the serial one.
        void PopulateTTContextCalibratedLockfree(int64_t tcpu, double tgpu, double frequency) {
            m_frequency = frequency;
            m_tgpu = tgpu;
            if (tcpu == 0) {
                tcpu = m_tcpu;
            }
            TracyLfqPrepare(QueueType::GpuNewContext);
            MemWrite(&item->gpuNewContext.cpuTime, tcpu);
            MemWrite(&item->gpuNewContext.gpuTime, (int64_t)round((double)m_tgpu / m_frequency));
            memset(&item->gpuNewContext.thread, 0, sizeof(item->gpuNewContext.thread));
            MemWrite(&item->gpuNewContext.period, (float)1.0f);
            MemWrite(&item->gpuNewContext.type, GpuContextType::tt_device);
            MemWrite(&item->gpuNewContext.context, GetId());
            MemWrite(&item->gpuNewContext.flags, (uint8_t)GpuContextCalibration);
            TracyLfqCommit;
            mm_tcpu = tcpu;
        }

        void NameLockfree( const char* name, uint16_t len )
        {
            auto ptr = (char*)tracy_malloc( len );
            memcpy( ptr, name, len );
            TracyLfqPrepare( QueueType::GpuContextName );
            MemWrite( &item->gpuContextNameFat.context, GetId() );
            MemWrite( &item->gpuContextNameFat.ptr, (uint64_t)ptr );
            MemWrite( &item->gpuContextNameFat.size, len );
            TracyLfqCommit;
        }

        void CalibrateTTContext(int64_t tcpu, double tgpu, double frequency)
        {
            m_frequency = frequency;
            m_tgpu = tgpu;
            if (tcpu == 0) tcpu = m_tcpu;
            if (tracy::GetProfiler().IsEmitSuppressed()) {
                return;
            }

            auto item = Profiler::QueueSerial();
            MemWrite( &item->hdr.type, QueueType::GpuCalibration );
            MemWrite( &item->gpuCalibration.gpuTime, (int64_t)round((double)m_tgpu/m_frequency) );
            MemWrite( &item->gpuCalibration.cpuTime, tcpu );
            MemWrite( &item->gpuCalibration.cpuDelta, (int64_t)((tcpu - mm_tcpu) * get_tracy_timer_mul()));
            MemWrite( &item->gpuCalibration.context, GetId() );
            Profiler::QueueSerialFinish();

            mm_tcpu = tcpu;
        }

        void Name( const char* name, uint16_t len )
        {
            auto ptr = (char*)tracy_malloc( len );

            memcpy( ptr, name, len );

            auto item = Profiler::QueueSerial();
            MemWrite( &item->hdr.type, QueueType::GpuContextName );
            MemWrite( &item->gpuContextNameFat.context, GetId() );
            MemWrite( &item->gpuContextNameFat.ptr, (uint64_t)ptr );
            MemWrite( &item->gpuContextNameFat.size, len );
            Profiler::QueueSerialFinish();

            //trac_free(ptr);
        }

        tracy_force_inline uint16_t GetId() const
        {
            return m_contextId;
        }

        tracy_force_inline unsigned int NextQueryId(EventInfo eventInfo)
        {
            const auto id = m_head;
            if ((m_head + 1) % QueryCount == m_tail) m_tail = m_head;
            m_head = (m_head + 1) % QueryCount;
            TRACY_TT_ASSERT(m_head != m_tail);
            m_query[id] = eventInfo;
            return id;
        }

        tracy_force_inline EventInfo& GetQuery(unsigned int id)
        {
            TRACY_TT_ASSERT(id < QueryCount);
            return m_query[id];
        }

        std::string getRunIdString(const TTDeviceMarker& marker) {
            // TODO(MO) Until #14847 avoid attaching opID as the zone function name except for B and E FW
            // This is to avoid generating 5 to 10 times more source locations which is capped at 32K
            if (!marker.marker_name_keyword_flags[static_cast<uint16_t>(MarkerDetails::MarkerNameKeyword::BRISC_FW)] &&
                !marker.marker_name_keyword_flags[static_cast<uint16_t>(MarkerDetails::MarkerNameKeyword::ERISC_FW)]) {
                return "";
            }
            const std::string id_string = marker.risc == RiscType::TENSIX_RISC_AGG ? "TRACE ID:" : "OP ID:";
            return marker.runtime_host_id > 0 ? id_string + std::to_string(marker.runtime_host_id) : "";
        }

        tracy::Color::ColorType getMarkerColor(const TTDeviceMarker& marker) {
            if (marker.color != 0) {
                return static_cast<tracy::Color::ColorType>(marker.color);
            }
            if (marker.marker_name_keyword_flags[static_cast<uint16_t>(MarkerDetails::MarkerNameKeyword::PROFILER)]) {
                return tracy::Color::Tomato3;
            }
            switch (marker.risc) {
                case RiscType::BRISC:
                    return tracy::Color::Orange2;
                case RiscType::NCRISC:
                    return tracy::Color::SeaGreen3;
                case RiscType::TRISC_0:
                    return tracy::Color::SkyBlue3;
                case RiscType::TRISC_1:
                    return tracy::Color::Turquoise2;
                case RiscType::TRISC_2:
                    return tracy::Color::CadetBlue1;
                case RiscType::ERISC:
                    return tracy::Color::Yellow3;
                case RiscType::NONE:
                    return tracy::Color::DarkSlateGray3;
                default:
                    TRACY_TT_ASSERT(marker.risc == RiscType::TENSIX_RISC_AGG);
                    return tracy::Color::DarkSlateGray3;
            }
        }

        // One "key: value" per line; the GUI prints these verbatim in the marker tooltip. Everything
        // that varies from event to event belongs here rather than in the interned source location.
        std::string getMarkerMetaString(const TTDeviceMarker& marker) {
            std::string meta;
            const auto append = [&meta](const std::string& key, const std::string& value) {
                if (!meta.empty()) {
                    meta += '\n';
                }
                meta += key + ": " + value;
            };

            if (!marker.op_name.empty()) {
                append("Op name", marker.op_name);
            }
            if (marker.runtime_host_id != TTDeviceMarker::INVALID_NUM) {
                append("Op ID", std::to_string(marker.runtime_host_id));
            }
            if (marker.trace_id != TTDeviceMarker::INVALID_NUM) {
                append("Trace ID", std::to_string(marker.trace_id));
            }
            if (marker.data != TTDeviceMarker::INVALID_NUM) {
                append("Data", std::to_string(marker.data));
            }
            if (marker.data_high != TTDeviceMarker::INVALID_NUM) {
                append("Data high", std::to_string(marker.data_high));
            }
#ifdef TRACY_TT_HAS_FULL_DEPS
            for (const auto& entry : marker.meta_data.items()) {
                append(
                    entry.key(),
                    entry.value().is_string() ? entry.value().template get<std::string>() : entry.value().dump());
            }
#endif
            return meta;
        }

        void PushStartMarker(const TTDeviceMarker& marker) {
            if (tracy::GetProfiler().IsEmitSuppressed()) {
                return;
            }
            const auto queryId = this->NextQueryId(EventInfo{marker, EventPhase::Begin});
            const std::string run_id_string = this->getRunIdString(marker);

            const tracy::Color::ColorType color = this->getMarkerColor(marker);

            const auto srcloc = Profiler::AllocSourceLocation(
                marker.line,
                marker.file.c_str(),
                marker.file.length(),
                run_id_string.c_str(),
                run_id_string.length(),
                marker.marker_name.c_str(),
                marker.marker_name.length(),
                color);

            auto zoneBegin = Profiler::QueueSerial();
            MemWrite(&zoneBegin->hdr.type, QueueType::GpuZoneBeginAllocSrcLocSerial);
            MemWrite(&zoneBegin->gpuZoneBegin.cpuTime, Profiler::GetTime());
            MemWrite(&zoneBegin->gpuZoneBegin.srcloc, srcloc);
            MemWrite(&zoneBegin->gpuZoneBegin.thread, (uint32_t)marker.get_thread_id());
            MemWrite(&zoneBegin->gpuZoneBegin.queryId, (uint16_t)queryId);
            MemWrite(&zoneBegin->gpuZoneBegin.context, this->GetId());
            Profiler::QueueSerialFinish();

            auto zoneTime = Profiler::QueueSerial();
            MemWrite(&zoneTime->hdr.type, QueueType::GpuTime);
            MemWrite(&zoneTime->gpuTime.gpuTime, (uint64_t)round((double)marker.timestamp / m_frequency));
            MemWrite(&zoneTime->gpuTime.queryId, (uint16_t)queryId);
            MemWrite(&zoneTime->gpuTime.context, this->GetId());
            Profiler::QueueSerialFinish();
        }

        // A point-in-time device event (TS_EVENT / TS_DATA / TS_DATA_16B) rather than a zone. The
        // source location carries only the event's identity so that the server interns one per event
        // type; everything that varies per event goes in the metadata string.
        void PushMarker(const TTDeviceMarker& marker) {
            const tracy::Color::ColorType color = this->getMarkerColor(marker);

            const auto srcloc = Profiler::AllocSourceLocation(
                marker.line,
                marker.file.c_str(),
                marker.file.length(),
                "",
                0,
                marker.marker_name.c_str(),
                marker.marker_name.length(),
                color);

            const std::string meta = this->getMarkerMetaString(marker);
            if (!meta.empty()) {
                const uint16_t metaLen = (uint16_t)std::min<size_t>(meta.length(), std::numeric_limits<uint16_t>::max());
                auto ptr = (char*)tracy_malloc(metaLen);
                memcpy(ptr, meta.c_str(), metaLen);

                auto metaItem = Profiler::QueueSerial();
                MemWrite(&metaItem->hdr.type, QueueType::GpuMarkerMeta);
                MemWrite(&metaItem->gpuMarkerMetaFat.context, this->GetId());
                MemWrite(&metaItem->gpuMarkerMetaFat.ptr, (uint64_t)ptr);
                MemWrite(&metaItem->gpuMarkerMetaFat.size, metaLen);
                Profiler::QueueSerialFinish();
            }

            auto item = Profiler::QueueSerial();
            MemWrite(&item->hdr.type, QueueType::GpuMarker);
            MemWrite(&item->gpuMarker.gpuTime, (int64_t)round((double)marker.timestamp / m_frequency));
            MemWrite(&item->gpuMarker.srcloc, srcloc);
            MemWrite(&item->gpuMarker.thread, (uint32_t)marker.get_thread_id());
            MemWrite(&item->gpuMarker.context, this->GetId());
            MemWrite(&item->gpuMarker.markerType, (uint8_t)marker.marker_type);
            Profiler::QueueSerialFinish();
        }

        // Per (context, thread), calls must arrive in zone completion order (i.e., sorted by `end`); the server rebuilds nesting from that ordering.
        tracy_force_inline void PushZone(
            const SourceLocationData* srcloc, uint32_t thread, uint64_t start, uint64_t end) {
            if (tracy::GetProfiler().IsEmitSuppressed()) {
                return;
            }
            TracyLfqPrepare(QueueType::GpuZone);
            MemWrite(&item->gpuZone.gpuStart, (int64_t)round((double)start / m_frequency));
            MemWrite(&item->gpuZone.gpuEnd, (int64_t)round((double)end / m_frequency));
            MemWrite(&item->gpuZone.srcloc, (uint64_t)srcloc);
            MemWrite(&item->gpuZone.thread, thread);
            MemWrite(&item->gpuZone.context, GetId());
            TracyLfqCommit;
        }

        // SERIAL-queue twin of PushZone, same wire item. Use this when the same producer also sends
        // SERIAL items that reference this context (GpuNewContext, markers): the client drains the
        // lock-free queues BEFORE the serial one each pass, so a lock-free zone can overtake a serial
        // GpuNewContext enqueued earlier -- and the server hard-asserts on an unknown context. One
        // serial item per zone is still strictly cheaper than the legacy begin/end pair (two serial
        // items plus an alloc'd srcloc). Same ordering contract: per (context, thread), completion order.
        tracy_force_inline void PushZoneSerial(
            const SourceLocationData* srcloc, uint32_t thread, uint64_t start, uint64_t end) {
            if (tracy::GetProfiler().IsEmitSuppressed()) {
                return;
            }
            auto item = Profiler::QueueSerial();
            MemWrite(&item->hdr.type, QueueType::GpuZone);
            MemWrite(&item->gpuZone.gpuStart, (int64_t)round((double)start / m_frequency));
            MemWrite(&item->gpuZone.gpuEnd, (int64_t)round((double)end / m_frequency));
            MemWrite(&item->gpuZone.srcloc, (uint64_t)srcloc);
            MemWrite(&item->gpuZone.thread, thread);
            MemWrite(&item->gpuZone.context, GetId());
            Profiler::QueueSerialFinish();
        }

        void PushEndMarker(const TTDeviceMarker& marker) {
            if (tracy::GetProfiler().IsEmitSuppressed()) {
                return;
            }
            const auto queryId = this->NextQueryId(EventInfo{marker, EventPhase::End});

            auto zoneEnd = Profiler::QueueSerial();
            MemWrite(&zoneEnd->hdr.type, QueueType::GpuZoneEndSerial);
            MemWrite(&zoneEnd->gpuZoneEnd.cpuTime, Profiler::GetTime());
            MemWrite(&zoneEnd->gpuZoneEnd.thread, (uint32_t)marker.get_thread_id());
            MemWrite(&zoneEnd->gpuZoneEnd.queryId, (uint16_t)queryId);
            MemWrite(&zoneEnd->gpuZoneEnd.context, this->GetId());
            Profiler::QueueSerialFinish();

            auto zoneTime = Profiler::QueueSerial();
            MemWrite(&zoneTime->hdr.type, QueueType::GpuTime);
            MemWrite(&zoneTime->gpuTime.gpuTime, (uint64_t)round((double)marker.timestamp / m_frequency));
            MemWrite(&zoneTime->gpuTime.queryId, (uint16_t)queryId);
            MemWrite(&zoneTime->gpuTime.context, this->GetId());
            Profiler::QueueSerialFinish();
        }

    private:

        uint16_t m_contextId;
        double m_tgpu = 0;
        uint64_t  mm_tcpu = 0;
        double m_frequency = 0;

        EventInfo m_query[QueryCount];
        unsigned int m_head; // index at which a new event should be inserted
        unsigned int m_tail; // oldest event

    };

    static inline TTCtx* CreateTTContext()
    {
        auto ctx = (TTCtx*)tracy_malloc(sizeof(TTCtx));
        new (ctx) TTCtx();
        return ctx;
    }


    static inline void DestroyTTContext(TTCtx* ctx)
    {
        ctx->~TTCtx();
        tracy_free(ctx);
    }

}  // namespace tracy

using TracyTTCtx = tracy::TTCtx*;

#define TracyTTContext() tracy::CreateTTContext()
#define TracyTTDestroy(ctx) tracy::DestroyTTContext(ctx)
#define TracyTTContextName(ctx, name, size) ctx->Name(name, size)
#define TracyTTContextNameLockfree(ctx, name, size) ctx->NameLockfree(name, size)
#define TracyTTContextPopulate(ctx, cpuTime, timeshift, frequency) ctx->PopulateTTContext(cpuTime, timeshift, frequency)
#define TracyTTContextPopulateCalibrated(ctx, cpuTime, timeshift, frequency) \
    ctx->PopulateTTContextCalibrated(cpuTime, timeshift, frequency)
#define TracyTTContextPopulateCalibratedLockfree(ctx, cpuTime, timeshift, frequency) \
    ctx->PopulateTTContextCalibratedLockfree(cpuTime, timeshift, frequency)
#define TracyTTContextCalibrate(ctx, cpuTime, timeshift, frequency) ctx->CalibrateTTContext(cpuTime, timeshift, frequency)
#define TracyTTPushStartMarker(ctx, marker) ctx->PushStartMarker(marker)
#define TracyTTPushEndMarker(ctx, marker) ctx->PushEndMarker(marker)
#define TracyTTPushMarker(ctx, marker) ctx->PushMarker(marker)
#define TracyTTPushZone(ctx, srcloc, thread, start, end) ctx->PushZone(srcloc, thread, start, end)
#define TracyTTPushZoneSerial(ctx, srcloc, thread, start, end) ctx->PushZoneSerial(srcloc, thread, start, end)

#define TracyGetTimerMul() tracy::get_tracy_timer_mul()
#define TracyGetBaseTime() tracy::get_tracy_base_time()
#define TracyGetCpuTime() tracy::get_cpu_time()
#define TracySetCpuTime( tcpu ) tracy::set_cpu_sync_time(tcpu)

#endif
#endif
