// Included by TracySysTrace.cpp

#include <atomic>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vector>

#include "../TracyProfiler.hpp"
#include "../TracyStringHelpers.hpp"
#include "../TracyThread.hpp"

#include "../../tracy/Tracy.hpp"

namespace tracy
{

static std::atomic<bool> s_traceActive { false };
static mach_timebase_info_data_t s_timebase;
static int s_samplingHz;
static pthread_t s_watchdogThread;

static void SysTraceEmitCallstackSample( uint32_t threadId, int64_t timestamp, const uint64_t* frames, int depth )
{
#ifdef TRACY_ON_DEMAND
    if( !GetProfiler().IsConnected() ) return;
#endif

    auto trace = (uint64_t*)tracy_malloc( ( 1 + depth ) * sizeof( uint64_t ) );
    trace[0] = (uint64_t)depth;
    memcpy( trace + 1, frames, depth * sizeof( uint64_t ) );

    TracyLfqPrepare( QueueType::CallstackSample );
    MemWrite( &item->callstackSampleFat.time, timestamp );
    MemWrite( &item->callstackSampleFat.thread, threadId );
    MemWrite( &item->callstackSampleFat.ptr, (uint64_t)trace );
    TracyLfqCommit;
}

static int SysTraceBacktrace( uint64_t* frames, int maxDepth, uint64_t pc, uint64_t fp )
{
    int depth = 0;
    frames[depth++] = pc;

    // NOTE: frame-pointer walk for now... It should be fine since the ABI
    // mandates it on ARM64 (and on x64 Apple clang preserves it by default)
    // ALSO: on ARM64 with PAC, return addresses on the stack may be signed!
    // (can strip auth bits via arm_thread_state64_get_lr() when needed)
    auto framePtr = (const uint64_t*)fp;
    while( framePtr && depth < maxDepth )
    {
        if( (uintptr_t)framePtr & (sizeof(uint64_t) - 1) ) break;  // misaligned — stop walk
        // [framePtr + 0] = saved frame pointer (previous frame)
        // [framePtr + 1] = return address (PC of caller)
        frames[depth++] = framePtr[1];
        framePtr = (const uint64_t*)framePtr[0];
    }

    return depth;
}

static void SysTraceSampleThread( mach_port_t tid )
{
    const int64_t t0 = Profiler::GetTime();
    if( thread_suspend( tid ) != KERN_SUCCESS ) return;
    const int64_t t1 = Profiler::GetTime();
    const int64_t timestamp = t0 + ( t1 - t0 ) / 2;

#if defined(__aarch64__)
    arm_thread_state64_t state;
    mach_msg_type_number_t stateCount = ARM_THREAD_STATE64_COUNT;
    const kern_return_t kr = thread_get_state( tid, ARM_THREAD_STATE64, (thread_state_t)&state, &stateCount );
#elif defined(__x86_64__)
    x86_thread_state64_t state;
    mach_msg_type_number_t stateCount = x86_THREAD_STATE64_COUNT;
    const kern_return_t kr = thread_get_state( tid, x86_THREAD_STATE64, (thread_state_t)&state, &stateCount );
#else
    #error "unsupported architecture"
#endif

    if( kr != KERN_SUCCESS )
    {
        thread_resume( tid );
        return;
    }

    constexpr int MaxDepth = 192;
    uint64_t frames [MaxDepth];

#if defined(__aarch64__)
    const int depth = SysTraceBacktrace( frames, MaxDepth, state.__pc, state.__fp );
#elif defined(__x86_64__)
    const int depth = SysTraceBacktrace( frames, MaxDepth, state.__rip, state.__rbp );
#endif

    thread_resume( tid );

    SysTraceEmitCallstackSample( (uint32_t)tid, timestamp, frames, depth );
}

static void SysTraceWait( uint64_t deadline )
{
    ZoneScopedC(tracy::Color::DimGray);
    mach_wait_until( deadline );
}

static uint64_t SysTraceRngInit()
{
    uint64_t seed = mach_absolute_time();
    seed ^= (uint64_t)(uintptr_t)&seed;
    return seed;
}

static uint32_t SysTraceRngNext( uint64_t& rng, uint32_t range )
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (uint32_t)( rng % range );
}

static void SysTraceWatch( mach_port_t selfThread )
{
    const uint64_t periodNs   = 1'000'000'000ULL / s_samplingHz;
    const uint64_t periodMach = periodNs * s_timebase.denom / s_timebase.numer;

    std::vector<mach_port_t> runningThreads;
    std::vector<mach_port_t> waitingThreads;

    uint64_t rng = SysTraceRngInit();

    uint64_t deadline = mach_absolute_time();
    while( s_traceActive.load( std::memory_order_relaxed ) )
    {
        SysTraceWait(deadline);
        deadline = mach_absolute_time() + periodMach;

        ZoneScoped;

#ifdef TRACY_ON_DEMAND
        if( !GetProfiler().IsConnected() ) continue;
#endif

        thread_act_array_t threads;
        mach_msg_type_number_t threadCount;
        if( task_threads( mach_task_self(), &threads, &threadCount ) != KERN_SUCCESS ) continue;

        runningThreads.clear();
        waitingThreads.clear();

        for( mach_msg_type_number_t i = 0; i < threadCount; i++ )
        {
            const mach_port_t tid = threads[i];
            if( tid == selfThread ) continue;

            thread_basic_info_data_t info;
            mach_msg_type_number_t infoCount = THREAD_BASIC_INFO_COUNT;
            if( thread_info( tid, THREAD_BASIC_INFO, (thread_info_t)&info, &infoCount ) != KERN_SUCCESS ) continue;
            if( info.flags & TH_FLAGS_IDLE ) continue;  // kernel idle thread, not user code

            if( info.run_state == TH_STATE_RUNNING )
                runningThreads.push_back( tid );
            else
                waitingThreads.push_back( tid );
        }

        for( const mach_port_t tid : runningThreads )
            SysTraceSampleThread( tid );

        while( !waitingThreads.empty() )
        {
            if( mach_absolute_time() >= deadline ) break;
            const uint32_t idx = SysTraceRngNext( rng, (uint32_t)waitingThreads.size() );
            SysTraceSampleThread( waitingThreads[idx] );
            std::swap( waitingThreads[idx], waitingThreads.back() );
            waitingThreads.pop_back();
        }

        for( mach_msg_type_number_t i = 0; i < threadCount; i++ )
            mach_port_deallocate( mach_task_self(), threads[i] );
        vm_deallocate( mach_task_self(), (vm_address_t)threads, sizeof(thread_t) * threadCount );
    }

    ZoneScopedNC("OUT", tracy::Color::Crimson);
    std::this_thread::sleep_for( std::chrono::milliseconds(100) );
}

void SysTraceWorker( void* )
{
    ThreadExitHandler threadExitHandler;
    SetThreadName( "Tracy Mach Watchdog" );
    InitRpmalloc();
    const mach_port_t selfThread = mach_thread_self();
    SysTraceWatch( selfThread );
    mach_port_deallocate( mach_task_self(), selfThread );
}

bool SysTraceStart( int64_t& samplingPeriod )
{
    bool expected = false;
    if( !s_traceActive.compare_exchange_strong( expected, true, std::memory_order_relaxed ) )
        return false;

    mach_timebase_info( &s_timebase );
    s_samplingHz   = GetSamplingFrequency();
    samplingPeriod = SamplingFrequencyToPeriodNs( s_samplingHz );
    auto trampoline = []( void* ) -> void* { SysTraceWorker( nullptr ); return nullptr; };
    if( pthread_create( &s_watchdogThread, nullptr, trampoline, nullptr ) != 0 )
    {
        s_traceActive.store( false, std::memory_order_relaxed );
        return false;
    }
    return true;
}

void SysTraceStop()
{
    bool expected = true;
    if( !s_traceActive.compare_exchange_strong( expected, false, std::memory_order_relaxed ) )
        return;
    pthread_join( s_watchdogThread, nullptr );
}

void SysTraceGetExternalName( uint64_t thread, const char*& threadName, const char*& name )
{
    // Resolve pthread handle from the Mach port so we can query the thread name.
    const mach_port_t mach_tid = (mach_port_t)thread;
    thread_identifier_info_data_t idInfo;
    mach_msg_type_number_t idInfoCount = THREAD_IDENTIFIER_INFO_COUNT;
    if( thread_info( mach_tid, THREAD_IDENTIFIER_INFO, (thread_info_t)&idInfo, &idInfoCount ) == KERN_SUCCESS )
    {
        char buf[64] = {};
        const pthread_t pt = (pthread_t)(uintptr_t)idInfo.thread_handle;
        if( pt && pthread_getname_np( pt, buf, sizeof( buf ) ) == 0 && buf[0] != '\0' )
            threadName = CopyString( buf );
        else
            threadName = CopyString( "???", 3 );

        TracyLfqPrepare( QueueType::TidToPid );
        MemWrite( &item->tidToPid.tid, thread );
        MemWrite( &item->tidToPid.pid, (uint64_t)getpid() );
        TracyLfqCommit;
    }
    else
    {
        threadName = CopyString( "???", 3 );
    }

    name = CopyStringFast( getprogname() );
}

} // namespace tracy
