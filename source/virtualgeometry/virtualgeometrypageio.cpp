#include "virtualgeometrypageio.h"
#include <algorithm>

namespace VirtualGeometryStreaming
{
PageIo::PageIo(Reader reader) : m_Read(std::move(reader))
{
    try
    {
        for (uint32_t worker = 0; worker < 4; ++worker)
        {
            m_Workers.emplace_back([this] { Work(); });
        }
    }
    catch (...)
    {
        Shutdown();
        throw;
    }
}
PageIo::~PageIo()
{
    Shutdown();
}
bool PageIo::Submit(std::shared_ptr<const VirtualGeometryPageCache::PageManifest> manifest, LoadTicket ticket, uint32_t localPage)
{
    std::lock_guard lock(m_Mutex);
    if (m_Stopping || !manifest || localPage >= manifest->Pages.size() || m_ReservedBytes + 65536 > 64ull * 1024 * 1024)
    {
        return false;
    }
    m_Jobs.push_back({std::move(manifest), ticket, localPage});
    m_ReservedBytes += 65536;
    m_Ready.notify_one();
    return true;
}
void PageIo::Cancel(LoadTicket ticket)
{
    std::lock_guard lock(m_Mutex);
    m_Cancelled.push_back(ticket);
}
uint64_t PageIo::StagingBytes() const
{
    std::lock_guard lock(m_Mutex);
    return m_ReservedBytes;
}
std::vector<PageIoCompletion> PageIo::Poll()
{
    std::lock_guard lock(m_Mutex);
    std::vector<PageIoCompletion> result;
    result.swap(m_Completed);
    m_ReservedBytes -= result.size() * 65536ull;
    return result;
}
void PageIo::Work()
{
    while (true)
    {
        Job job;
        {
            std::unique_lock lock(m_Mutex);
            m_Ready.wait(lock, [&] { return m_Stopping || !m_Jobs.empty(); });
            if (m_Jobs.empty())
            {
                return;
            }
            job = std::move(m_Jobs.front());
            m_Jobs.pop_front();
        }
        PageIoCompletion result;
        result.Ticket = job.Ticket;
        auto begin = std::chrono::steady_clock::now();
        try
        {
            result.Payload.resize(65536);
            if (m_Read(*job.Manifest, job.LocalPage, result.Payload, result.Error))
            {
                result.ReadBytes = 65536;
            }
            else if (result.Error.empty())
            {
                result.Error = "Page read failed";
            }
        }
        catch (const std::exception& exception)
        {
            result.Error = exception.what();
        }
        result.Milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        {
            std::lock_guard lock(m_Mutex);
            auto cancelled = std::find(m_Cancelled.begin(), m_Cancelled.end(), job.Ticket);
            if (cancelled != m_Cancelled.end())
            {
                result.Error = "Page read cancelled";
                m_Cancelled.erase(cancelled);
            }
            if (!result.Error.empty())
            {
                result.Payload.clear();
            }
            m_Completed.push_back(std::move(result));
        }
    }
}
void PageIo::Shutdown()
{
    {
        std::lock_guard lock(m_Mutex);
        m_Stopping = true;
    }
    m_Ready.notify_all();
    for (auto& worker : m_Workers)
    {
        if (worker.joinable())
        {
            worker.join();
        }
    }
    m_Workers.clear();
}
}
