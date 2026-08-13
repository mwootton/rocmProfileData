/**************************************************************************
 * Copyright (c) 2022 - 2024 Advanced Micro Devices, Inc.
 **************************************************************************/
#include "AmdSmiDataSource.h"

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <string>
#include <cstring>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <vector>

#include <sqlite3.h>

#include "DbResource.h"
#include "Logger.h"
#include "Utility.h"

using rpdtracer::AmdSmiDataSource;
using rpdtracer::AmdSmiDataSourcePrivate;


// ---- gpu_metrics v1.x layout (from amdgpu kernel driver) ----
//
// We only need the v1.0 base fields.  All versions share this prefix;
// later versions append fields after it.  We read structure_size bytes
// and only touch offsets within the v1.0 region.

struct gpu_metrics_header {
    uint16_t structure_size;
    uint8_t  format_revision;
    uint8_t  content_revision;
};

struct gpu_metrics_v1_base {
    gpu_metrics_header header;

    // Temperature (C)
    uint16_t temperature_edge;
    uint16_t temperature_hotspot;
    uint16_t temperature_mem;
    uint16_t temperature_vrgfx;
    uint16_t temperature_vrsoc;
    uint16_t temperature_vrmem;

    // Utilization (%)
    uint16_t average_gfx_activity;
    uint16_t average_umc_activity;
    uint16_t average_mm_activity;

    // Power (W) / Energy
    uint16_t average_socket_power;
    uint64_t energy_accumulator;

    // Driver timestamp (ns)
    uint64_t system_clock_counter;

    // Average clocks (MHz)
    uint16_t average_gfxclk_frequency;
    uint16_t average_socclk_frequency;
    uint16_t average_uclk_frequency;
    uint16_t average_vclk0_frequency;
    uint16_t average_dclk0_frequency;
    uint16_t average_vclk1_frequency;
    uint16_t average_dclk1_frequency;

    // Current clocks (MHz)
    uint16_t current_gfxclk;
    uint16_t current_socclk;
    uint16_t current_uclk;
    uint16_t current_vclk0;
    uint16_t current_dclk0;
    uint16_t current_vclk1;
    uint16_t current_dclk1;

    // Throttle
    uint32_t throttle_status;

    // Fan
    uint16_t current_fan_speed;

    // PCIe
    uint16_t pcie_link_width;
    uint16_t pcie_link_speed;
};


// ---- Sysfs GPU device ----

struct GpuDevice {
    std::string metricsPath;    // e.g. /sys/class/drm/card0/device/gpu_metrics
};


// ---- Private implementation ----

struct MetricSlot {
    int deviceIdx;
    uint32_t flag;
    std::string deviceType;
    std::string monitorType;
    int lastEmitted {INT_MIN};
    int deadband {0};
};

namespace rpdtracer {

class AmdSmiDataSourcePrivate
{
public:
    AmdSmiDataSourcePrivate(AmdSmiDataSource *cls) : p(cls) {}

    AmdSmiDataSource *p;

    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> loggingActive {false};
    DbResource *resource {nullptr};

    std::thread *worker {nullptr};
    std::atomic<bool> done {false};
    int periodUs {1000};    // 1000 Hz

    std::vector<GpuDevice*> devices;
    uint32_t enabledMetrics {AmdSmiDataSource::METRIC_DEFAULT};

    std::vector<MetricSlot> slots;

    void parseConfig();
    void buildSlots();
    void work();

    static bool collectSlot(MetricSlot &slot, gpu_metrics_v1_base &metrics, sqlite3_int64 &valueOut);
};

}  // namespace rpdtracer


// ---- Config ----

static const struct { const char *name; uint32_t flag; } s_metricNames[] = {
    {"gpu_util",     AmdSmiDataSource::METRIC_GPU_UTIL},
    {"mem_util",     AmdSmiDataSource::METRIC_MEM_UTIL},
    {"mm_util",      AmdSmiDataSource::METRIC_MM_UTIL},
    {"sclk",         AmdSmiDataSource::METRIC_SCLK},
    {"mclk",         AmdSmiDataSource::METRIC_MCLK},
    {"temp_edge",    AmdSmiDataSource::METRIC_TEMP_EDGE},
    {"temp_hotspot", AmdSmiDataSource::METRIC_TEMP_HOTSPOT},
    {"temp_mem",     AmdSmiDataSource::METRIC_TEMP_MEM},
    {"power",        AmdSmiDataSource::METRIC_POWER},
    {"vram_used",    AmdSmiDataSource::METRIC_VRAM_USED},
};


void AmdSmiDataSourcePrivate::parseConfig()
{
    const char *val = getenv("RPDT_SMI_METRICS");
    if (val != nullptr) {
        if (strcmp(val, "all") == 0) {
            enabledMetrics = 0xFFFFFFFF;
            return;
        }
        if (strcmp(val, "none") == 0) {
            enabledMetrics = 0;
            return;
        }
        uint32_t flags = 0;
        std::string cfg(val);
        size_t pos = 0;
        while (pos < cfg.size()) {
            size_t end = cfg.find(',', pos);
            if (end == std::string::npos)
                end = cfg.size();
            std::string token = cfg.substr(pos, end - pos);
            while (!token.empty() && token.front() == ' ') token.erase(0, 1);
            while (!token.empty() && token.back() == ' ') token.pop_back();

            bool found = false;
            for (auto &m : s_metricNames) {
                if (token == m.name) {
                    flags |= m.flag;
                    found = true;
                    break;
                }
            }
            if (!found)
                fprintf(stderr, "rpd_tracer: unknown SMI metric '%s'\n", token.c_str());

            pos = end + 1;
        }
        enabledMetrics = flags;
    }

    const char *period = getenv("RPDT_SMI_PERIOD");
    if (period != nullptr) {
        int p = atoi(period);
        if (p > 0)
            periodUs = p;
    }
}


void AmdSmiDataSourcePrivate::buildSlots()
{
    using MF = AmdSmiDataSource::MetricFlag;
    struct MetricDef {
        uint32_t flag;
        const char *monitorType;
    };
    static const MetricDef defs[] = {
        {MF::METRIC_GPU_UTIL,     "gpu_util"},
        {MF::METRIC_MEM_UTIL,     "mem_util"},
        {MF::METRIC_MM_UTIL,      "mm_util"},
        {MF::METRIC_SCLK,         "sclk"},
        {MF::METRIC_MCLK,         "mclk"},
        {MF::METRIC_TEMP_EDGE,    "temp_edge"},
        {MF::METRIC_TEMP_HOTSPOT, "temp_hotspot"},
        {MF::METRIC_TEMP_MEM,     "temp_mem"},
        {MF::METRIC_POWER,        "power"},
        {MF::METRIC_VRAM_USED,    "vram_used"},
    };

    slots.clear();
    for (int i = 0; i < (int)devices.size(); ++i) {
        for (auto &def : defs) {
            if (enabledMetrics & def.flag) {
                if (def.flag == MF::METRIC_VRAM_USED)
                    continue;  // not available via gpu_metrics
                MetricSlot slot;
                slot.deviceIdx = i;
                slot.flag = def.flag;
                slot.deviceType = "gpu";
                slot.monitorType = def.monitorType;
                if (def.flag == MF::METRIC_POWER)
                    slot.deadband = 10;
                if (def.flag & (MF::METRIC_TEMP_EDGE | MF::METRIC_TEMP_HOTSPOT | MF::METRIC_TEMP_MEM))
                    slot.deadband = 2;
                slots.push_back(std::move(slot));
            }
        }
    }
}


// ---- Enumerate AMD GPUs via sysfs ----
// Sort by card number to match HIP device enumeration order (readdir order
// is arbitrary and does not match PCI bus order).
//
// KNOWN ISSUE: deviceId mapping vs profiling backends
//   We enumerate all physical GPUs via sysfs regardless of visibility masks
//   (ROCR_VISIBLE_DEVICES, HIP_VISIBLE_DEVICES).  Profiling backends
//   (roctracer, rocprofiler-sdk, etc.) report gpuId based on the runtime's
//   filtered and renumbered device list.  When GPUs are masked out, our
//   deviceId N may not correspond to the backend's gpuId N.
//
//   Potential solutions:
//   - Match on PCI BDF: sysfs provides it in device/uevent; backends can
//     query it from HSA agent info or HIP device properties.  A BDF-keyed
//     lookup would be backend-agnostic.
//   - Store PCI BDF in the monitor table alongside deviceId so consumers
//     can join on BDF rather than relying on index equality.
//   - Filter our enumeration using the visibility env vars, but the format
//     is complex (indices, UUIDs, ranges) and fragile.
//   Workaround: use contiguous GPU indices starting from 0 (e.g. 0,1,2)
//   when setting visibility masks.  Skipping or reordering breaks alignment.

static std::vector<GpuDevice*> enumerateGpus()
{
    std::vector<GpuDevice*> gpus;

    DIR *dir = opendir("/sys/class/drm");
    if (!dir)
        return gpus;

    std::vector<std::pair<int, std::string>> candidates;
    struct dirent *ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (strncmp(ent->d_name, "card", 4) != 0)
            continue;
        const char *p = ent->d_name + 4;
        while (*p >= '0' && *p <= '9') ++p;
        if (*p != '\0')
            continue;

        int cardNum = atoi(ent->d_name + 4);
        std::string metricsPath = std::string("/sys/class/drm/") + ent->d_name + "/device/gpu_metrics";
        if (access(metricsPath.c_str(), R_OK) == 0)
            candidates.push_back({cardNum, std::move(metricsPath)});
    }
    closedir(dir);

    std::sort(candidates.begin(), candidates.end());

    for (auto &[num, path] : candidates) {
        auto *dev = new GpuDevice();
        dev->metricsPath = std::move(path);
        gpus.push_back(dev);
    }
    return gpus;
}


// ---- Factory ----

extern "C" {
    rpdtracer::DataSource *AmdSmiDataSourceFactory() { return new AmdSmiDataSource(); }
}  // extern "C"


// ---- DataSource interface ----

void AmdSmiDataSource::init()
{
    d = new AmdSmiDataSourcePrivate(this);
    d->parseConfig();

    if (d->enabledMetrics == 0)
        return;

    d->done = false;
    d->worker = new std::thread(&AmdSmiDataSourcePrivate::work, d);
}


void AmdSmiDataSource::end()
{
    if (d == nullptr)
        return;

    if (d->worker == nullptr) {
        delete d;
        d = nullptr;
        return;
    }

    d->done = true;
    d->cv.notify_one();
    d->worker->join();
    delete d->worker;

    if (d->resource) {
        d->resource->unlock();
        delete d->resource;
    }

    for (auto *gpu : d->devices)
        delete gpu;

    delete d;
    d = nullptr;
}


void AmdSmiDataSource::startTracing()
{
    std::unique_lock<std::mutex> lock(d->mutex);
    d->loggingActive = true;
    d->cv.notify_one();
}


void AmdSmiDataSource::stopTracing()
{
    std::unique_lock<std::mutex> lock(d->mutex);
    d->loggingActive = false;
    lock.unlock();

    Logger &logger = Logger::singleton();
    logger.monitorTable().endCurrentRuns(clocktime_ns());
}


void AmdSmiDataSource::flush()
{
    Logger &logger = Logger::singleton();
    logger.monitorTable().endCurrentRuns(clocktime_ns());
}


// ---- Read gpu_metrics ----

static bool readGpuMetrics(GpuDevice &gpu, gpu_metrics_v1_base &out)
{
    int fd = open(gpu.metricsPath.c_str(), O_RDONLY);
    if (fd < 0)
        return false;

    uint8_t buf[256];
    ssize_t n = read(fd, buf, sizeof(buf));
    close(fd);

    if (n < (ssize_t)sizeof(gpu_metrics_header))
        return false;

    auto *hdr = reinterpret_cast<gpu_metrics_header*>(buf);
    if (hdr->format_revision != 1)
        return false;
    if (n < (ssize_t)sizeof(gpu_metrics_v1_base))
        return false;

    memcpy(&out, buf, sizeof(gpu_metrics_v1_base));
    return true;
}


// ---- Metric collection ----

bool AmdSmiDataSourcePrivate::collectSlot(MetricSlot &slot,
                                          gpu_metrics_v1_base &m,
                                          sqlite3_int64 &valueOut)
{
    using MF = AmdSmiDataSource::MetricFlag;
    int raw;

    switch (slot.flag) {
    case MF::METRIC_GPU_UTIL:
        if (m.average_gfx_activity == UINT16_MAX) return false;
        raw = m.average_gfx_activity; break;
    case MF::METRIC_MEM_UTIL:
        if (m.average_umc_activity == UINT16_MAX) return false;
        raw = m.average_umc_activity; break;
    case MF::METRIC_MM_UTIL:
        if (m.average_mm_activity == UINT16_MAX) return false;
        raw = m.average_mm_activity; break;
    case MF::METRIC_SCLK:
        if (m.current_gfxclk == UINT16_MAX) return false;
        raw = m.current_gfxclk; break;
    case MF::METRIC_MCLK:
        if (m.current_uclk == UINT16_MAX) return false;
        raw = m.current_uclk; break;
    case MF::METRIC_TEMP_EDGE:
        if (m.temperature_edge == UINT16_MAX) return false;
        raw = m.temperature_edge; break;
    case MF::METRIC_TEMP_HOTSPOT:
        if (m.temperature_hotspot == UINT16_MAX) return false;
        raw = m.temperature_hotspot; break;
    case MF::METRIC_TEMP_MEM:
        if (m.temperature_mem == UINT16_MAX) return false;
        raw = m.temperature_mem; break;
    case MF::METRIC_POWER:
        if (m.average_socket_power == UINT16_MAX) return false;
        raw = m.average_socket_power; break;
    default:
        return false;
    }

    if (slot.deadband > 0 && slot.lastEmitted != INT_MIN) {
        if (abs(raw - slot.lastEmitted) < slot.deadband)
            return false;
    }
    slot.lastEmitted = raw;

    valueOut = raw;
    return true;
}


// ---- Worker thread ----

void AmdSmiDataSourcePrivate::work()
{
    devices = enumerateGpus();
    if (devices.empty())
        return;

    buildSlots();

    // Block until startTracing() fires.  Logger construction is complete
    // by that point, so Logger::singleton() is safe to call.
    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&]{ return loggingActive || done.load(); });
    }
    if (done) return;

    Logger &logger = Logger::singleton();

    const char *dbfile = getenv("RPDT_FILENAME");
    if (dbfile == nullptr)
        dbfile = "./trace.rpd";
    resource = new DbResource(std::string(dbfile), std::string("amdsmi_logger_active"));
    bool haveResource = resource->tryLock();

    std::vector<gpu_metrics_v1_base> metricsCache(devices.size());

    while (!done) {
        if (haveResource && loggingActive) {
            for (size_t i = 0; i < devices.size(); ++i)
                readGpuMetrics(*devices[i], metricsCache[i]);

            sqlite3_int64 value;
            for (auto &slot : slots) {
                auto &mc = metricsCache[slot.deviceIdx];
                if (collectSlot(slot, mc, value)) {
                    MonitorTable::row mrow;
                    mrow.deviceId = slot.deviceIdx;
                    mrow.deviceType = slot.deviceType;
                    mrow.monitorType = slot.monitorType;
                    mrow.start = mc.system_clock_counter;
                    mrow.end = 0;
                    mrow.value = value;
                    logger.monitorTable().insert(mrow);
                }
            }
        }

        std::unique_lock<std::mutex> lock(mutex);
        if (done) break;
        auto waitTime = std::chrono::microseconds(
            haveResource ? periodUs : 1000000);
        cv.wait_for(lock, waitTime);

        if (!haveResource)
            haveResource = resource->tryLock();
    }
}
