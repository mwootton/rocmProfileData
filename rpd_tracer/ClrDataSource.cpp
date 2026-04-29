/**************************************************************************
 * Copyright (c) 2022 Advanced Micro Devices, Inc.
 **************************************************************************/
#include "ClrDataSource.h"

#include <hip/hip_runtime.h>  // FIXME removeme
#include <hip/amd_detail/hip_profiler_ext.h>

#include <stdio.h>
#include <fmt/format.h>

#include "Logger.h"
#include "Utility.h"

namespace rpdtracer {

// Create a factory for the Logger to locate and use
extern "C" {
    DataSource *ClrDataSourceFactory() { return new ClrDataSource(); }
}  // extern "C"

// FIXME: can we avoid shutdown corruption?
// Other rocm libraries crashing on unload
// libsqlite unloading before we are done using it

static std::once_flag register_once;


void ClrDataSource::init()
{
    m_apiList.setInvertMode(true);  // Omit the specified api
    m_apiList.add("hipGetDevice");
    m_apiList.add("hipSetDevice");
    m_apiList.add("hipGetLastError");
    m_apiList.add("__hipPushCallConfiguration");
    m_apiList.add("__hipPopCallConfiguration");
    m_apiList.add("hipCtxSetCurrent");
    m_apiList.add("hipGetDevicePropertiesR0600");
    m_apiList.add("hipGetDeviceCount");
    m_apiList.add("hipDeviceGetAttribute");
    m_apiList.add("hipRuntimeGetVersion");
    m_apiList.add("hipPeekAtLastError");
    m_apiList.add("hipModuleGetFunction");
    m_apiList.loadUserPrefs();
}

void ClrDataSource::end()
{
    flush();
}

void ClrDataSource::startTracing()
{
    // Work around teething bug
    hipDeviceProp_t devProp;
    (void)hipGetDeviceProperties(&devProp, 0);

    uint64_t startId;
    (void)hipProfilerEnableExt(&startId, 0);
    m_ranges.push_back({startId, 0});
    std::call_once(register_once, atexit, Logger::rpdFinalize);
}

void ClrDataSource::stopTracing()
{
    uint64_t endId;
    (void)hipProfilerDisableExt(&endId);
    m_ranges.back().end = endId;
}

void ClrDataSource::flush()
{
    const timestamp_t cb_begin_time = clocktime_ns();

    const HipApiRecordExt* const* chunks;
    size_t chunk_count, chunk_size, total_count;
    (void)hipProfilerGetRecordsExt(&chunks, &chunk_count, &chunk_size, &total_count);

    Logger &logger = Logger::singleton();
    static sqlite3_int64 domain_id = logger.stringTable().getOrCreate("hip");
    static uint64_t correlation_id = 0;

    size_t start_chunk = m_processedCount / chunk_size;
    size_t start_index = m_processedCount % chunk_size;

    size_t range_idx = 0;
    for (size_t c = start_chunk; c < chunk_count; ++c) {
        size_t n = (total_count - c * chunk_size < chunk_size)
                   ? total_count - c * chunk_size : chunk_size;
        for (size_t i = (c == start_chunk ? start_index : 0); i < n; ++i) {
            const HipApiRecordExt* r = &chunks[c][i];
            uint64_t record_id = c * chunk_size + i;
            while (range_idx < m_ranges.size() && m_ranges[range_idx].end > 0 && record_id >= m_ranges[range_idx].end)
                ++range_idx;
            if (range_idx >= m_ranges.size() || record_id < m_ranges[range_idx].start)
                continue;
            if (!m_apiList.contains(r->api_name))
                continue;
            ApiTable::row row;
            row.pid = GetPid();
            row.tid = static_cast<int>(static_cast<sqlite3_int64>(r->thread_id)); // FIXME need OS tid
            row.start = static_cast<sqlite3_int64>(r->start_ns);
            row.end = static_cast<sqlite3_int64>(r->end_ns);
            row.domain_id = domain_id;
            row.category_id = EMPTY_STRING_ID;
            row.apiName_id = logger.stringTable().getOrCreate(r->api_name);
            row.args_id = EMPTY_STRING_ID;
            row.api_id = correlation_id;
            logger.apiTable().insert(row);


            ++correlation_id;
            if (r->has_gpu_activity) {
                static sqlite3_int64 dispatch_type_id = logger.stringTable().getOrCreate("KernelExecution");
                static sqlite3_int64 copy_type_id = logger.stringTable().getOrCreate("Memcpy");
                static sqlite3_int64 barrier_type_id = logger.stringTable().getOrCreate("Barrier");

                const HipGpuActivityExt* gpu = &r->gpu;
                while (gpu != nullptr) {
                    OpTable::row oprow;
                    oprow.gpuId = static_cast<int>(gpu->device_id);
                    oprow.queueId = static_cast<int>(gpu->queue_id);
                    oprow.sequenceId = 0;
                    oprow.start = static_cast<sqlite3_int64>(gpu->begin_ns);
                    oprow.end = static_cast<sqlite3_int64>(gpu->end_ns);
                    oprow.api_id = row.api_id;

                    if (gpu->op == HIP_OP_DISPATCH_EXT) {
                        oprow.opType_id = dispatch_type_id;
                        oprow.description_id = (gpu->kernel_name && !gpu->is_graph)
                            ? logger.stringTable().getOrCreate(gpu->kernel_name)
                            : EMPTY_STRING_ID;

                        KernelApiTable::row krow;
                        krow.api_id = row.api_id;
                        krow.stream = fmt::format("{}", (void*)r->stream);
                        krow.kernelName_id = oprow.description_id;  // EMPTY_STRING_ID for graph launches
                        krow.gridX = static_cast<int>(gpu->grid_x);
                        krow.gridY = static_cast<int>(gpu->grid_y);
                        krow.gridZ = static_cast<int>(gpu->grid_z);
                        krow.workgroupX = static_cast<int>(gpu->block_x);
                        krow.workgroupY = static_cast<int>(gpu->block_y);
                        krow.workgroupZ = static_cast<int>(gpu->block_z);
                        logger.kernelApiTable().insert(krow);
                    } else if (gpu->op == HIP_OP_COPY_EXT) {
                        oprow.opType_id = copy_type_id;
                        static sqlite3_int64 sdma_id = logger.stringTable().getOrCreate("SDMA");
                        oprow.description_id = hipCopyKindIsSDMAExt((HipCopyKindExt)gpu->copy_kind)
                            ? sdma_id
                            : EMPTY_STRING_ID;

                        CopyApiTable::row crow;
                        crow.api_id = row.api_id;
                        crow.stream = fmt::format("{}", (void*)r->stream);
                        crow.size = static_cast<int>(gpu->bytes);
                        crow.dst = fmt::format("{}", gpu->dst);
                        crow.src = fmt::format("{}", gpu->src);
                        crow.kind = static_cast<int>(gpu->copy_kind);
                        logger.copyApiTable().insert(crow);
                    } else {
                        oprow.opType_id = barrier_type_id;
                        oprow.description_id = EMPTY_STRING_ID;
                    }

                    logger.opTable().insert(oprow);
                    gpu = gpu->next;
                }
            }
        }
    }
    m_ranges.erase(m_ranges.begin(), m_ranges.begin() + range_idx);

    size_t incremental_count = total_count - m_processedCount;
    m_processedCount = total_count;

    const timestamp_t cb_end_time = clocktime_ns();
    char buff[4096];
    std::snprintf(buff, 4096, "count=%lu | total=%lu", incremental_count, total_count);
    logger.createOverheadRecord(cb_begin_time, cb_end_time, "ClrDataSource::flush", buff);
}

}    // namespace rpdtracer
