/**************************************************************************
 * Copyright (c) 2022 - 2024 Advanced Micro Devices, Inc.
 **************************************************************************/
#include "AmdSmiDataSource.h"

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <cstring>

#include "Logger.h"
#include "Utility.h"

using rpdtracer::AmdSmiDataSource;


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


// ---- Factory ----

extern "C" {
    rpdtracer::DataSource *AmdSmiDataSourceFactory() { return new AmdSmiDataSource(); }
}  // extern "C"


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


void AmdSmiDataSource::parseConfig()
{
    const char *val = getenv("RPDT_SMI_METRICS");
    if (val != nullptr) {
        if (strcmp(val, "all") == 0) {
            m_enabledMetrics = 0xFFFFFFFF;
            return;
        }
        if (strcmp(val, "none") == 0) {
            m_enabledMetrics = 0;
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
        m_enabledMetrics = flags;
    }

    const char *period = getenv("RPDT_SMI_PERIOD");
    if (period != nullptr) {
        int p = atoi(period);
        if (p > 0)
            m_periodUs = p;
    }
}


void AmdSmiDataSource::buildSlots()
{
    struct MetricDef {
        uint32_t flag;
        const char *monitorType;
    };
    static const MetricDef defs[] = {
        {METRIC_GPU_UTIL,     "gpu_util"},
        {METRIC_MEM_UTIL,     "mem_util"},
        {METRIC_MM_UTIL,      "mm_util"},
        {METRIC_SCLK,         "sclk"},
        {METRIC_MCLK,         "mclk"},
        {METRIC_TEMP_EDGE,    "temp_edge"},
        {METRIC_TEMP_HOTSPOT, "temp_hotspot"},
        {METRIC_TEMP_MEM,     "temp_mem"},
        {METRIC_POWER,        "power"},
        {METRIC_VRAM_USED,    "vram_used"},
    };

    m_slots.clear();
    for (int d = 0; d < (int)m_devices.size(); ++d) {
        for (auto &def : defs) {
            if (m_enabledMetrics & def.flag) {
                if (def.flag == METRIC_VRAM_USED)
                    continue;  // not available via gpu_metrics
                MetricSlot slot;
                slot.deviceIdx = d;
                slot.flag = def.flag;
                slot.deviceType = "gpu";
                slot.monitorType = def.monitorType;
                if (def.flag == METRIC_POWER)
                    slot.deadband = 10;
                if (def.flag & (METRIC_TEMP_EDGE | METRIC_TEMP_HOTSPOT | METRIC_TEMP_MEM))
                    slot.deadband = 2;
                m_slots.push_back(std::move(slot));
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

static std::vector<GpuDevice> enumerateGpus()
{
    std::vector<GpuDevice> gpus;

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
        GpuDevice dev;
        dev.metricsPath = std::move(path);
        gpus.push_back(std::move(dev));
    }
    return gpus;
}


// ---- DataSource interface ----

void AmdSmiDataSource::init()
{
    parseConfig();

    if (m_enabledMetrics == 0)
        return;

    m_done = false;
    m_worker = new std::thread(&AmdSmiDataSource::work, this);
}


void AmdSmiDataSource::end()
{
    if (m_worker == nullptr)
        return;

    m_done = true;
    m_cv.notify_one();
    m_worker->join();
    delete m_worker;
    m_worker = nullptr;

    if (m_resource) {
        m_resource->unlock();
        delete m_resource;
        m_resource = nullptr;
    }

    for (auto dev : m_devices)
        delete static_cast<GpuDevice*>(dev);
    m_devices.clear();
}


void AmdSmiDataSource::startTracing()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_loggingActive = true;
    m_cv.notify_one();
}


void AmdSmiDataSource::stopTracing()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_loggingActive = false;
    lock.unlock();

    Logger &logger = Logger::singleton();
    logger.monitorTable().endCurrentRuns(clocktime_ns());
}


void AmdSmiDataSource::flush()
{
    Logger &logger = Logger::singleton();
    logger.monitorTable().endCurrentRuns(clocktime_ns());
}


// ---- Read gpu_metrics and extract values for all enabled slots ----

static bool readGpuMetrics(GpuDevice &gpu, gpu_metrics_v1_base &out)
{
    // Re-open each read; sysfs files don't support lseek reliably
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


bool AmdSmiDataSource::collectSlot(MetricSlot &slot,
                                   void *devPtr,
                                   std::string &valueOut)
{
    auto *m = static_cast<gpu_metrics_v1_base*>(devPtr);
    int raw;

    switch (slot.flag) {
    case METRIC_GPU_UTIL:
        if (m->average_gfx_activity == UINT16_MAX) return false;
        raw = m->average_gfx_activity; break;
    case METRIC_MEM_UTIL:
        if (m->average_umc_activity == UINT16_MAX) return false;
        raw = m->average_umc_activity; break;
    case METRIC_MM_UTIL:
        if (m->average_mm_activity == UINT16_MAX) return false;
        raw = m->average_mm_activity; break;
    case METRIC_SCLK:
        if (m->current_gfxclk == UINT16_MAX) return false;
        raw = m->current_gfxclk; break;
    case METRIC_MCLK:
        if (m->current_uclk == UINT16_MAX) return false;
        raw = m->current_uclk; break;
    case METRIC_TEMP_EDGE:
        if (m->temperature_edge == UINT16_MAX) return false;
        raw = m->temperature_edge; break;
    case METRIC_TEMP_HOTSPOT:
        if (m->temperature_hotspot == UINT16_MAX) return false;
        raw = m->temperature_hotspot; break;
    case METRIC_TEMP_MEM:
        if (m->temperature_mem == UINT16_MAX) return false;
        raw = m->temperature_mem; break;
    case METRIC_POWER:
        if (m->average_socket_power == UINT16_MAX) return false;
        raw = m->average_socket_power; break;
    default:
        return false;
    }

    if (slot.deadband > 0 && slot.lastEmitted != INT_MIN) {
        if (abs(raw - slot.lastEmitted) < slot.deadband)
            return false;
    }
    slot.lastEmitted = raw;

    valueOut = fmt::format("{}", raw);
    return true;
}


// ---- Worker thread ----

void AmdSmiDataSource::work()
{
    auto gpus = enumerateGpus();
    if (gpus.empty())
        return;

    for (auto &gpu : gpus)
        m_devices.push_back(new GpuDevice(std::move(gpu)));

    buildSlots();

    // Block until startTracing() fires.  Logger construction is complete
    // by that point, so Logger::singleton() is safe to call.
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [&]{ return m_loggingActive || m_done.load(); });
    }
    if (m_done) return;

    Logger &logger = Logger::singleton();

    const char *dbfile = getenv("RPDT_FILENAME");
    if (dbfile == nullptr)
        dbfile = "./trace.rpd";
    m_resource = new DbResource(std::string(dbfile), std::string("amdsmi_logger_active"));
    bool haveResource = m_resource->tryLock();

    std::vector<gpu_metrics_v1_base> metricsCache(m_devices.size());

    while (!m_done) {
        if (haveResource && m_loggingActive) {
            for (size_t i = 0; i < m_devices.size(); ++i)
                readGpuMetrics(*static_cast<GpuDevice*>(m_devices[i]), metricsCache[i]);

            std::string value;
            for (auto &slot : m_slots) {
                auto &mc = metricsCache[slot.deviceIdx];
                if (collectSlot(slot, &mc, value)) {
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

        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_done) break;
        auto waitTime = std::chrono::microseconds(
            haveResource ? m_periodUs : m_periodUs * 10);
        m_cv.wait_for(lock, waitTime);

        if (!haveResource)
            haveResource = m_resource->tryLock();
    }
}
