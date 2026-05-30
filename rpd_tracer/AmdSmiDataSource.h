/**************************************************************************
 * Copyright (c) 2022 - 2024 Advanced Micro Devices, Inc.
 **************************************************************************/
#pragma once

#include "DataSource.h"
#include "DbResource.h"

#include <sqlite3.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <vector>
#include <string>

namespace rpdtracer {

class AmdSmiDataSource : public DataSource
{
public:
    void init() override;
    void end() override;
    void startTracing() override;
    void stopTracing() override;
    void flush() override;

    enum MetricFlag : uint32_t {
        METRIC_GPU_UTIL     = 1 << 0,
        METRIC_MEM_UTIL     = 1 << 1,
        METRIC_SCLK         = 1 << 2,
        METRIC_MCLK         = 1 << 3,
        METRIC_TEMP_EDGE    = 1 << 4,
        METRIC_TEMP_HOTSPOT = 1 << 5,
        METRIC_TEMP_MEM     = 1 << 6,
        METRIC_POWER        = 1 << 7,
        METRIC_VRAM_USED    = 1 << 8,
        METRIC_MM_UTIL      = 1 << 9,
    };

    static constexpr uint32_t METRIC_DEFAULT =
        METRIC_GPU_UTIL | METRIC_SCLK | METRIC_TEMP_HOTSPOT | METRIC_POWER;

private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_loggingActive {false};
    DbResource *m_resource {nullptr};

    void work();
    std::thread *m_worker {nullptr};
    std::atomic<bool> m_done {false};
    int m_periodUs {8333};    // ~120 Hz

    // Cached device handles (void* == amdsmi_processor_handle)
    std::vector<void*> m_devices;

    uint32_t m_enabledMetrics {METRIC_DEFAULT};

    void parseConfig();
    void buildSlots();

    struct MetricSlot {
        int deviceIdx;
        uint32_t flag;
        std::string deviceType;
        std::string monitorType;
    };
    std::vector<MetricSlot> m_slots;

    static bool collectSlot(const MetricSlot &slot, void *dev, std::string &valueOut);
};

}    // namespace rpdtracer
