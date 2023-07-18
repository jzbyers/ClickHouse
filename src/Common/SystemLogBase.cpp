#include <Interpreters/AsynchronousMetricLog.h>
#include <Interpreters/CrashLog.h>
#include <Interpreters/MetricLog.h>
#include <Interpreters/OpenTelemetrySpanLog.h>
#include <Interpreters/PartLog.h>
#include <Interpreters/QueryLog.h>
#include <Interpreters/QueryThreadLog.h>
#include <Interpreters/QueryViewsLog.h>
#include <Interpreters/SessionLog.h>
#include <Interpreters/TextLog.h>
#include <Interpreters/TraceLog.h>
#include <Interpreters/FilesystemCacheLog.h>
#include <Interpreters/FilesystemReadPrefetchesLog.h>
#include <Interpreters/ProcessorsProfileLog.h>
#include <Interpreters/ZooKeeperLog.h>
#include <Interpreters/TransactionsInfoLog.h>
#include <Interpreters/AsynchronousInsertLog.h>

#include <Common/MemoryTrackerBlockerInThread.h>
#include <Common/SystemLogBase.h>
#include <Common/ThreadPool.h>

#include <Common/logger_useful.h>
#include <base/scope_guard.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int TIMEOUT_EXCEEDED;
}

namespace
{
    constexpr size_t DBMS_SYSTEM_LOG_QUEUE_SIZE = 1048576;
}

ISystemLog::~ISystemLog() = default;


template <typename LogElement>
SystemLogQueue<LogElement>::SystemLogQueue(
    const String & name_,
    size_t flush_interval_milliseconds_)
    : log(&Poco::Logger::get(name_))
    , flush_interval_milliseconds(flush_interval_milliseconds_)
{}

static thread_local bool recursive_add_call = false;

template <typename LogElement>
void SystemLogQueue<LogElement>::add(const LogElement & element)
{
    /// It is possible that the method will be called recursively.
    /// Better to drop these events to avoid complications.
    if (recursive_add_call)
        return;
    recursive_add_call = true;
    SCOPE_EXIT({ recursive_add_call = false; });

    /// Memory can be allocated while resizing on queue.push_back.
    /// The size of allocation can be in order of a few megabytes.
    /// But this should not be accounted for query memory usage.
    /// Otherwise the tests like 01017_uniqCombined_memory_usage.sql will be flacky.
    MemoryTrackerBlockerInThread temporarily_disable_memory_tracker;

    /// Should not log messages under mutex.
    bool queue_is_half_full = false;

    {
        std::unique_lock lock(mutex);

        if (is_shutdown)
            return;

        if (queue.size() == DBMS_SYSTEM_LOG_QUEUE_SIZE / 2)
        {
            queue_is_half_full = true;

            // The queue more than half full, time to flush.
            // We only check for strict equality, because messages are added one
            // by one, under exclusive lock, so we will see each message count.
            // It is enough to only wake the flushing thread once, after the message
            // count increases past half available size.
            const uint64_t queue_end = queue_front_index + queue.size();
            if (requested_flush_up_to < queue_end)
                requested_flush_up_to = queue_end;

            flush_event.notify_all();
        }

        if (queue.size() >= DBMS_SYSTEM_LOG_QUEUE_SIZE)
        {
            // Ignore all further entries until the queue is flushed.
            // Log a message about that. Don't spam it -- this might be especially
            // problematic in case of trace log. Remember what the front index of the
            // queue was when we last logged the message. If it changed, it means the
            // queue was flushed, and we can log again.
            if (queue_front_index != logged_queue_full_at_index)
            {
                logged_queue_full_at_index = queue_front_index;

                // TextLog sets its logger level to 0, so this log is a noop and
                // there is no recursive logging.
                lock.unlock();
                LOG_ERROR(log, "Queue is full for system log '{}' at {}", demangle(typeid(*this).name()), queue_front_index);
            }

            return;
        }

        queue.push_back(element);
    }

    if (queue_is_half_full)
        LOG_INFO(log, "Queue is half full for system log '{}'.", demangle(typeid(*this).name()));
}

template <typename LogElement>
void SystemLogQueue<LogElement>::shutdown()
{ 
    is_shutdown = true;         
    /// Tell thread to shutdown.
    flush_event.notify_all();
}

template <typename LogElement>
void SystemLogQueue<LogElement>::waitFlush(uint64_t this_thread_requested_offset_)
{
    // Use an arbitrary timeout to avoid endless waiting. 60s proved to be
    // too fast for our parallel functional tests, probably because they
    // heavily load the disk.
    const int timeout_seconds = 180;
    std::unique_lock lock(mutex);
    bool result = flush_event.wait_for(lock, std::chrono::seconds(timeout_seconds), [&]
    {
        return flushed_up_to >= this_thread_requested_offset_ && !is_force_prepare_tables;
    });

    if (!result)
    {
        throw Exception(ErrorCodes::TIMEOUT_EXCEEDED, "Timeout exceeded ({} s) while flushing system log '{}'.",
            toString(timeout_seconds), demangle(typeid(*this).name()));
    }
}

template <typename LogElement>
void SystemLogQueue<LogElement>::confirm(uint64_t to_flush_end)
{
    std::lock_guard lock(mutex);
    flushed_up_to = to_flush_end;
    is_force_prepare_tables = false;
    flush_event.notify_all();
}

template <typename LogElement>
void SystemLogQueue<LogElement>::pop(std::vector<LogElement>& output, uint64_t& to_flush_end, bool& should_prepare_tables_anyway, bool& exit_this_thread)
{
    std::unique_lock lock(mutex);
    flush_event.wait_for(lock,
        std::chrono::milliseconds(flush_interval_milliseconds),
        [&] ()
        {
            return requested_flush_up_to > flushed_up_to || is_shutdown || is_force_prepare_tables;
        }
    );

    queue_front_index += queue.size();
    to_flush_end = queue_front_index;
    // Swap with existing array from previous flush, to save memory
    // allocations.
    output.resize(0);
    queue.swap(output);

    should_prepare_tables_anyway = is_force_prepare_tables;

    exit_this_thread = is_shutdown;
}

template <typename LogElement>
uint64_t SystemLogQueue<LogElement>::notifyFlush(bool force)
{
    uint64_t this_thread_requested_offset;

    {
        std::lock_guard lock(mutex);
        if (is_shutdown)
            return uint64_t(-1);

        this_thread_requested_offset = queue_front_index + queue.size();

        // Publish our flush request, taking care not to overwrite the requests
        // made by other threads.
        is_force_prepare_tables |= force;
        requested_flush_up_to = std::max(requested_flush_up_to, this_thread_requested_offset);

        flush_event.notify_all();
    }

    LOG_DEBUG(log, "Requested flush up to offset {}", this_thread_requested_offset);
    return this_thread_requested_offset;
}

#define INSTANTIATE_SYSTEM_LOG_BASE(ELEMENT) template class SystemLogQueue<ELEMENT>;
SYSTEM_LOG_ELEMENTS(INSTANTIATE_SYSTEM_LOG_BASE)

}
