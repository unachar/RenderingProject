#pragma once
#include "virtualgeometrypagecache.h"
#include "virtualgeometryresidency.h"
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace VirtualGeometryStreaming
{
    struct PageIoCompletion
    {
        LoadTicket Ticket;
        std::vector<uint8_t> Payload;
        std::string Error;
        uint64_t ReadBytes = 0;
        double Milliseconds = 0;
    };

    class PageIo
    {
      public:
        using Reader = std::function<bool(const VirtualGeometryPageCache::PageManifest&, uint32_t, std::span<uint8_t>,
                                          std::string&)>;
        explicit PageIo(Reader reader = VirtualGeometryPageCache::ReadPage);
        ~PageIo();
        bool Submit(std::shared_ptr<const VirtualGeometryPageCache::PageManifest> manifest, LoadTicket ticket,
                    uint32_t localPage);
        std::vector<PageIoCompletion> Poll();
        void Cancel(LoadTicket ticket);
        void Shutdown();
        uint64_t StagingBytes() const;

      private:
        struct Job
        {
            std::shared_ptr<const VirtualGeometryPageCache::PageManifest> Manifest;
            LoadTicket Ticket;
            uint32_t LocalPage = 0;
        };

        void Work();
        Reader m_Read;
        mutable std::mutex m_Mutex;
        std::condition_variable m_Ready;
        std::deque<Job> m_Jobs;
        std::vector<PageIoCompletion> m_Completed;
        std::vector<LoadTicket> m_Cancelled;
        std::vector<LoadTicket> m_Active;
        std::vector<std::thread> m_Workers;
        uint64_t m_ReservedBytes = 0;
        bool m_Stopping = false;
    };
} // 名前空間 VirtualGeometryStreaming の終わり。
