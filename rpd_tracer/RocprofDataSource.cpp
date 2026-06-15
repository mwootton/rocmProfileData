/*********************************************************************************
* Copyright (c) 2021 - 2023 Advanced Micro Devices, Inc. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
* THE SOFTWARE.
********************************************************************************/
#include "RocprofDataSource.h"

#include <rocprofiler-sdk/context.h>
#include <rocprofiler-sdk/fwd.h>
#include <rocprofiler-sdk/marker/api_id.h>
#include <rocprofiler-sdk/registration.h>
#include <rocprofiler-sdk/rocprofiler.h>
#include <rocprofiler-sdk/cxx/name_info.hpp>

#include <vector>
#include <array>
#include <string>

#include <sqlite3.h>
#include <fmt/format.h>

#include <nlohmann/json.hpp>

#include "Logger.h"
#include "LocalStringCache.h"
#include "UStringCache.h"
#include "Utility.h"

using rpdtracer::DataSource;
using rpdtracer::RocprofDataSource;
//using rpdtracer::RocprofApiIdList;


// Create a factory for the Logger to locate and use
extern "C" {
    DataSource *RocprofDataSourceFactory() { return new RocprofDataSource(); }
}  // extern "C"


//
// The plan:
//    Shared Class holds data common to all instances (should we ever need more than 1)
//    Anonymous namespace holds a ptr to the Shared Class.  Not member functio access needed
//    Class instances have a private object
//    Contexts have to be generated up-front
//        One context (always active) to observe code-object loading, etc
//        Class instances grab a context from an array.  For event callbacks and buffers
//

class RocprofDataSourceShared;
namespace
{
    RocprofDataSourceShared *s {nullptr};
    using kernel_symbol_data_t = rocprofiler_callback_tracing_code_object_kernel_symbol_register_data_t;
    using kernel_symbol_map_t = std::unordered_map<rocprofiler_kernel_id_t, kernel_symbol_data_t>;
    using kernel_name_map_t = std::unordered_map<rocprofiler_kernel_id_t, const char *>;
    using rocprofiler::sdk::buffer_name_info;
    using agent_info_map_t = std::unordered_map<uint64_t, rocprofiler_agent_v0_t>;

    struct PendingKernel {
        std::string stream;
        uint32_t gridX {0}, gridY {0}, gridZ {0};
        uint32_t workgroupX {0}, workgroupY {0}, workgroupZ {0};
        uint32_t sharedMemBytes {0};
    };

    struct PendingCopy {
        std::string stream;
        uint32_t size {0};
        std::string dst;
        std::string src;
        int kind {0};
    };

    PendingKernel extractKernelLaunchData(rocprofiler_tracing_operation_t op,
                                         const rocprofiler_hip_api_args_t& args)
    {
        PendingKernel pk;
        switch (op) {
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipExtModuleLaunchKernel: {
                auto& a = args.hipExtModuleLaunchKernel;
                pk.workgroupX = a.localWorkSizeX;
                pk.workgroupY = a.localWorkSizeY;
                pk.workgroupZ = a.localWorkSizeZ;
                pk.gridX = (a.localWorkSizeX > 0) ? a.globalWorkSizeX / a.localWorkSizeX : 0;
                pk.gridY = (a.localWorkSizeY > 0) ? a.globalWorkSizeY / a.localWorkSizeY : 0;
                pk.gridZ = (a.localWorkSizeZ > 0) ? a.globalWorkSizeZ / a.localWorkSizeZ : 0;
                pk.sharedMemBytes = a.sharedMemBytes;
                pk.stream = fmt::format("{}", (void*)a.stream);
                break;
            }
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipHccModuleLaunchKernel: {
                auto& a = args.hipHccModuleLaunchKernel;
                pk.workgroupX = a.localWorkSizeX;
                pk.workgroupY = a.localWorkSizeY;
                pk.workgroupZ = a.localWorkSizeZ;
                pk.gridX = (a.localWorkSizeX > 0) ? a.globalWorkSizeX / a.localWorkSizeX : 0;
                pk.gridY = (a.localWorkSizeY > 0) ? a.globalWorkSizeY / a.localWorkSizeY : 0;
                pk.gridZ = (a.localWorkSizeZ > 0) ? a.globalWorkSizeZ / a.localWorkSizeZ : 0;
                pk.sharedMemBytes = a.sharedMemBytes;
                pk.stream = fmt::format("{}", (void*)a.stream);
                break;
            }
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipModuleLaunchKernel: {
                auto& a = args.hipModuleLaunchKernel;
                pk.gridX = a.gridDimX;
                pk.gridY = a.gridDimY;
                pk.gridZ = a.gridDimZ;
                pk.workgroupX = a.blockDimX;
                pk.workgroupY = a.blockDimY;
                pk.workgroupZ = a.blockDimZ;
                pk.sharedMemBytes = a.sharedMemBytes;
                pk.stream = fmt::format("{}", (void*)a.stream);
                break;
            }
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipModuleLaunchCooperativeKernel: {
                auto& a = args.hipModuleLaunchCooperativeKernel;
                pk.gridX = a.gridDimX;
                pk.gridY = a.gridDimY;
                pk.gridZ = a.gridDimZ;
                pk.workgroupX = a.blockDimX;
                pk.workgroupY = a.blockDimY;
                pk.workgroupZ = a.blockDimZ;
                pk.sharedMemBytes = a.sharedMemBytes;
                pk.stream = fmt::format("{}", (void*)a.stream);
                break;
            }
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipLaunchKernel: {
                auto& a = args.hipLaunchKernel;
                pk.gridX = a.numBlocks.x;
                pk.gridY = a.numBlocks.y;
                pk.gridZ = a.numBlocks.z;
                pk.workgroupX = a.dimBlocks.x;
                pk.workgroupY = a.dimBlocks.y;
                pk.workgroupZ = a.dimBlocks.z;
                pk.sharedMemBytes = a.sharedMemBytes;
                pk.stream = fmt::format("{}", (void*)a.stream);
                break;
            }
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipLaunchKernel_spt: {
                auto& a = args.hipLaunchKernel_spt;
                pk.gridX = a.numBlocks.x;
                pk.gridY = a.numBlocks.y;
                pk.gridZ = a.numBlocks.z;
                pk.workgroupX = a.dimBlocks.x;
                pk.workgroupY = a.dimBlocks.y;
                pk.workgroupZ = a.dimBlocks.z;
                pk.sharedMemBytes = a.sharedMemBytes;
                pk.stream = fmt::format("{}", (void*)a.stream);
                break;
            }
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipExtLaunchKernel: {
                auto& a = args.hipExtLaunchKernel;
                pk.gridX = a.numBlocks.x;
                pk.gridY = a.numBlocks.y;
                pk.gridZ = a.numBlocks.z;
                pk.workgroupX = a.dimBlocks.x;
                pk.workgroupY = a.dimBlocks.y;
                pk.workgroupZ = a.dimBlocks.z;
                pk.sharedMemBytes = a.sharedMemBytes;
                pk.stream = fmt::format("{}", (void*)a.stream);
                break;
            }
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipLaunchCooperativeKernel: {
                auto& a = args.hipLaunchCooperativeKernel;
                pk.gridX = a.gridDim.x;
                pk.gridY = a.gridDim.y;
                pk.gridZ = a.gridDim.z;
                pk.workgroupX = a.blockDimX.x;
                pk.workgroupY = a.blockDimX.y;
                pk.workgroupZ = a.blockDimX.z;
                pk.sharedMemBytes = a.sharedMemBytes;
                pk.stream = fmt::format("{}", (void*)a.stream);
                break;
            }
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipLaunchCooperativeKernel_spt: {
                auto& a = args.hipLaunchCooperativeKernel_spt;
                pk.gridX = a.gridDim.x;
                pk.gridY = a.gridDim.y;
                pk.gridZ = a.gridDim.z;
                pk.workgroupX = a.blockDim.x;
                pk.workgroupY = a.blockDim.y;
                pk.workgroupZ = a.blockDim.z;
                pk.sharedMemBytes = a.sharedMemBytes;
                pk.stream = fmt::format("{}", (void*)a.stream);
                break;
            }
            default:
                break;
        }
        return pk;
    }

    PendingCopy extractCopyData(rocprofiler_tracing_operation_t op,
                                const rocprofiler_hip_api_args_t& args)
    {
        PendingCopy pc;
        switch (op) {
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipMemcpy: {
                auto& a = args.hipMemcpy;
                pc.size = a.sizeBytes;
                pc.dst = fmt::format("{}", a.dst);
                pc.src = fmt::format("{}", a.src);
                pc.kind = a.kind;
                break;
            }
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipMemcpyAsync: {
                auto& a = args.hipMemcpyAsync;
                pc.size = a.sizeBytes;
                pc.dst = fmt::format("{}", a.dst);
                pc.src = fmt::format("{}", a.src);
                pc.kind = a.kind;
                pc.stream = fmt::format("{}", (void*)a.stream);
                break;
            }
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipMemcpyWithStream: {
                auto& a = args.hipMemcpyWithStream;
                pc.size = a.sizeBytes;
                pc.dst = fmt::format("{}", a.dst);
                pc.src = fmt::format("{}", a.src);
                pc.kind = a.kind;
                pc.stream = fmt::format("{}", (void*)a.stream);
                break;
            }
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipMemcpyDtoD: {
                auto& a = args.hipMemcpyDtoD;
                pc.size = a.sizeBytes;
                pc.dst = fmt::format("{}", (void*)a.dst);
                pc.src = fmt::format("{}", (void*)a.src);
                pc.kind = hipMemcpyDeviceToDevice;
                break;
            }
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipMemcpyDtoDAsync: {
                auto& a = args.hipMemcpyDtoDAsync;
                pc.size = a.sizeBytes;
                pc.dst = fmt::format("{}", (void*)a.dst);
                pc.src = fmt::format("{}", (void*)a.src);
                pc.kind = hipMemcpyDeviceToDevice;
                pc.stream = fmt::format("{}", (void*)a.stream);
                break;
            }
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipMemcpyDtoH: {
                auto& a = args.hipMemcpyDtoH;
                pc.size = a.sizeBytes;
                pc.dst = fmt::format("{}", a.dst);
                pc.src = fmt::format("{}", (void*)a.src);
                pc.kind = hipMemcpyDeviceToHost;
                break;
            }
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipMemcpyDtoHAsync: {
                auto& a = args.hipMemcpyDtoHAsync;
                pc.size = a.sizeBytes;
                pc.dst = fmt::format("{}", a.dst);
                pc.src = fmt::format("{}", (void*)a.src);
                pc.kind = hipMemcpyDeviceToHost;
                pc.stream = fmt::format("{}", (void*)a.stream);
                break;
            }
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipMemcpyHtoD: {
                auto& a = args.hipMemcpyHtoD;
                pc.size = a.sizeBytes;
                pc.dst = fmt::format("{}", (void*)a.dst);
                pc.src = fmt::format("{}", a.src);
                pc.kind = hipMemcpyHostToDevice;
                break;
            }
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipMemcpyHtoDAsync: {
                auto& a = args.hipMemcpyHtoDAsync;
                pc.size = a.sizeBytes;
                pc.dst = fmt::format("{}", (void*)a.dst);
                pc.src = fmt::format("{}", a.src);
                pc.kind = hipMemcpyHostToDevice;
                pc.stream = fmt::format("{}", (void*)a.stream);
                break;
            }
            default:
                break;
        }
        return pc;
    }

    // Extract hip args to json
            auto extract_hip_args = [](rocprofiler_buffer_tracing_kind_t,
                  rocprofiler_tracing_operation_t,
                   uint32_t          arg_num,
                   const void* const arg_value_addr,
                   int32_t           indirection_count,
                   const char*       arg_type,
                   const char*       arg_name,
                   const char*       arg_value_str,
                   void*             cb_data) -> int {
                nlohmann::json &json = *(static_cast<nlohmann::json*>(cb_data));
                json[arg_name] = arg_value_str;
                return 0;
            };


    // copy api calls
    bool isCopyApi(uint32_t id) {
        switch (id) {
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipMemcpy:
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipMemcpy2D:
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipMemcpy2DAsync:
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipMemcpy2DFromArray:
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipMemcpy2DFromArrayAsync:
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipMemcpy2DToArray:
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipMemcpy2DToArrayAsync:
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipMemcpy3D:
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipMemcpy3DAsync:
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipMemcpyAsync:
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipMemcpyAtoH:
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipMemcpyDtoD:
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipMemcpyDtoDAsync:
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipMemcpyDtoH:
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipMemcpyDtoHAsync:
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipMemcpyFromArray:
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipMemcpyFromSymbol:
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipMemcpyFromSymbolAsync:
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipMemcpyHtoA:
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipMemcpyHtoD:
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipMemcpyHtoDAsync:
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipMemcpyParam2D:
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipMemcpyParam2DAsync:
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipMemcpyPeer:
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipMemcpyPeerAsync:
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipMemcpyToArray:
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipMemcpyToSymbol:
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipMemcpyToSymbolAsync:
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipMemcpyWithStream:
                return true;
                break;
            default:
                ;
       }
       return false;
    }

    // kernel api calls
    bool isKernelApi(uint32_t id) {
        switch (id) {
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipExtLaunchKernel:
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipExtLaunchMultiKernelMultiDevice:
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipLaunchCooperativeKernel:
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipLaunchCooperativeKernelMultiDevice:
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipLaunchKernel:
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipModuleLaunchCooperativeKernel:
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipModuleLaunchCooperativeKernelMultiDevice:
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipModuleLaunchKernel:
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipExtModuleLaunchKernel:
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipHccModuleLaunchKernel:
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipLaunchCooperativeKernel_spt:
            case ROCPROFILER_HIP_RUNTIME_API_ID_hipLaunchKernel_spt:
                return true;
                break;
            default:
                ;
       }
       return false;
    }

    class RocprofApiIdList : public rpdtracer::ApiIdList
    {
    public:
        RocprofApiIdList(buffer_name_info &names);
        uint32_t mapName(const std::string &apiName) override;
        std::vector<rocprofiler_tracing_operation_t> allEnabled();
    private:
        std::unordered_map<std::string, size_t> m_nameMap;
    };

} // namespace

class RocprofDataSourceShared
{
public:
    static RocprofDataSourceShared& singleton();

    rocprofiler_client_id_t *clientId {nullptr};
    rocprofiler_client_finalize_t finalizer {nullptr};
    rocprofiler_tool_configure_result_t cfg = rocprofiler_tool_configure_result_t{
                                            sizeof(rocprofiler_tool_configure_result_t),
                                            &RocprofDataSource::toolInit,
                                            &RocprofDataSource::toolFinialize,
                                            nullptr};

    // Contexts
    rocprofiler_context_id_t utilityContext = {0};
    std::array<rocprofiler_context_id_t,1> contexts = {0};
    std::array<RocprofDataSource*,1> instances = {nullptr};
    size_t nextContext = 0;	// first available context in contexts array

    // Buffers
    std::array<rocprofiler_buffer_id_t,1> client_buffers = {0};

    // Manage kernel names - #betterThanRoctracer

    kernel_symbol_map_t kernel_info = {};
    kernel_name_map_t kernel_names = {};

    // Manage buffer name - #betterThanRoctracer
    buffer_name_info name_info = {};

    // Agent info
    // <rocprofiler_profile_config_id_t.handle, rocprofiler_agent_v0_t>
    agent_info_map_t agents = {};

private:
    RocprofDataSourceShared() { s = this; }
    ~RocprofDataSourceShared() { s = nullptr; }
};

RocprofDataSourceShared &RocprofDataSourceShared::singleton()
{
    static RocprofDataSourceShared *instance = new RocprofDataSourceShared();	// Leak this
    return *instance;
}



namespace rpdtracer {

class RocprofDataSourcePrivate
{
public:
    size_t id;

    std::unordered_map<uint64_t, PendingKernel> pendingKernels;
    std::unordered_map<uint64_t, PendingCopy> pendingCopies;

    bool logArgs { true };

    bool idsCached {false};
    sqlite3_int64 kernelExecId {0};
    sqlite3_int64 memcpyId {0};
    sqlite3_int64 domainId {0};
    void cacheIds();
};


RocprofDataSource::RocprofDataSource()
: d(new RocprofDataSourcePrivate)
{
    RocprofDataSourceShared::singleton();	// CRITICAL: static init

    if (s->utilityContext.handle == 0) {
        // s->contexts have not been created.  Force registration
        auto ret = rocprofiler_force_configure(nullptr);
    }

    // assign ourselves then next available id and context
    assert(s->nextContext < s->contexts.size());
    d->id = s->nextContext++;
    s->instances[d->id] = this;

    // Suppress args logging
    d->logArgs = (atoi(getConfig("RPDT_ROCPROF_NOARGS", "rocprof_noargs", "0")) == 0);
}

RocprofDataSource::~RocprofDataSource()
{
    // FIXME: stop context?
    s->instances[d->id] = NULL;
    delete d;
}

void RocprofDataSource::init()
{
    stopTracing();
}

void RocprofDataSource::end()
{
    flush();

    if (s != nullptr && s->finalizer != nullptr && s->clientId != nullptr) {
        s->finalizer(*s->clientId);
        s->finalizer = nullptr;
        s->clientId = nullptr;
    }
}

void RocprofDataSource::startTracing()
{
    if (s->contexts[d->id].handle == 0)
        return;
    rocprofiler_start_context(s->contexts[d->id]);
}

void RocprofDataSource::stopTracing()
{
    if (s->contexts[d->id].handle == 0)
        return;
    rocprofiler_stop_context(s->contexts[d->id]);
}

void RocprofDataSource::flush()
{
    rocprofiler_flush_buffer(s->client_buffers[d->id]);
}

void RocprofDataSourcePrivate::cacheIds()
{
    if (idsCached)
        return;
    Logger &logger = Logger::singleton();
    kernelExecId = logger.stringTable().getOrCreate("KernelExecution");
    memcpyId = logger.stringTable().getOrCreate("Memcpy");
    domainId = logger.stringTable().getOrCreate("hip");
    idsCached = true;
}

void RocprofDataSource::reset()
{
    d->idsCached = false;
}


// roctx handling moved to RoctxDataSource


void RocprofDataSource::buffer_callback(rocprofiler_context_id_t context, rocprofiler_buffer_id_t buffer_id, rocprofiler_record_header_t** headers, size_t num_headers, void* user_data, uint64_t drop_count)
{
    assert(drop_count == 0 && "drop count should be zero for lossless policy");
    static thread_local rpdtracer::LocalStringCache t_stringCache;
    RocprofDataSource &instance = **(reinterpret_cast<RocprofDataSource**>(user_data));
    instance.d->cacheIds();

    Logger &logger = Logger::singleton();

    int64_t last_correlation = -1;
    const timestamp_t cb_begin_time = clocktime_ns();

    for (size_t i = 0; i < num_headers; ++i) {
        auto* header = headers[i];

        if (header->category == ROCPROFILER_BUFFER_CATEGORY_TRACING) {
            if (header->kind == ROCPROFILER_BUFFER_TRACING_KERNEL_DISPATCH) {

                auto* record = static_cast<rocprofiler_buffer_tracing_kernel_dispatch_record_t*>(header->payload);
                auto& dispatch = record->dispatch_info;
                sqlite3_int64 desc_id = t_stringCache.lookup(s->kernel_names.at(record->dispatch_info.kernel_id), logger.stringTable(), logger.storageGeneration());

                OpTable::row row;
                row.gpuId = s->agents.at(dispatch.agent_id.handle).logical_node_type_id;
                row.queueId = dispatch.queue_id.handle;
                row.sequenceId = 0;
                row.start = adjust_external_ts(record->start_timestamp);
                row.end = adjust_external_ts(record->end_timestamp);
                row.description_id = desc_id;
                row.opType_id = instance.d->kernelExecId;
                row.api_id = record->correlation_id.internal;

                logger.opTable().insert(row);

                auto it = instance.d->pendingKernels.find(record->correlation_id.internal);
                if (it != instance.d->pendingKernels.end()) {
                    auto& pk = it->second;
                    KernelApiTable::row krow;
                    krow.api_id = record->correlation_id.internal;
                    krow.stream = pk.stream;
                    krow.gridX = pk.gridX;
                    krow.gridY = pk.gridY;
                    krow.gridZ = pk.gridZ;
                    krow.workgroupX = pk.workgroupX;
                    krow.workgroupY = pk.workgroupY;
                    krow.workgroupZ = pk.workgroupZ;
                    krow.groupSegmentSize = pk.sharedMemBytes;
                    krow.privateSegmentSize = dispatch.private_segment_size;
                    krow.kernelName_id = desc_id;
                    logger.kernelApiTable().insert(krow);
                }
                last_correlation = record->correlation_id.internal;
            }
            else if (header->kind == ROCPROFILER_BUFFER_TRACING_MEMORY_COPY) {

                auto &copy = *(static_cast<rocprofiler_buffer_tracing_memory_copy_record_t*>(header->payload));
                sqlite3_int64 name_id = t_stringCache.lookup(std::string(s->name_info[copy.kind][copy.operation]).c_str(), logger.stringTable(), logger.storageGeneration());
                sqlite3_int64 desc_id = t_stringCache.lookup("", logger.stringTable(), logger.storageGeneration());

                OpTable::row row;
                row.gpuId = 0;
                row.queueId = 0;
                row.sequenceId = 0;
                row.start = adjust_external_ts(copy.start_timestamp);
                row.end = adjust_external_ts(copy.end_timestamp);
                row.description_id = desc_id;
                row.opType_id = name_id;
                row.api_id = copy.correlation_id.internal;

                logger.opTable().insert(row);

                auto it = instance.d->pendingCopies.find(copy.correlation_id.internal);
                if (it != instance.d->pendingCopies.end()) {
                    auto& pc = it->second;
                    CopyApiTable::row crow;
                    crow.api_id = copy.correlation_id.internal;
                    crow.stream = pc.stream;
                    crow.size = pc.size;
                    crow.dst = pc.dst;
                    crow.src = pc.src;
                    crow.dstDevice = s->agents.at(copy.dst_agent_id.handle).logical_node_id;
                    crow.srcDevice = s->agents.at(copy.src_agent_id.handle).logical_node_id;
                    crow.kind = pc.kind;
                    crow.sync = true;
                    logger.copyApiTable().insert(crow);
                }
                last_correlation = copy.correlation_id.internal;
            }
            else if (header->kind == ROCPROFILER_BUFFER_TRACING_HIP_RUNTIME_API_EXT) {
                auto &hipapi = *(static_cast<rocprofiler_buffer_tracing_hip_api_ext_record_t*>(header->payload));

                // extract args as json
                nlohmann::json json;
                if (instance.d->logArgs) {
                    rocprofiler_iterate_buffer_tracing_record_args(
                        *header, extract_hip_args,
                        &json);
                }

                // Add an api table entry
                sqlite3_int64 name_id = t_stringCache.lookup(std::string(s->name_info[hipapi.kind][hipapi.operation]).c_str(), logger.stringTable(), logger.storageGeneration());

                ApiTable::row row;
                row.pid = GetPid();
                row.tid = hipapi.thread_id;
                row.start = adjust_external_ts(hipapi.start_timestamp);
                row.end = adjust_external_ts(hipapi.end_timestamp);
                row.domain_id = instance.d->domainId;
                row.category_id = EMPTY_STRING_ID;
                row.apiName_id = name_id;
                if (instance.d->logArgs) {
                    static thread_local rpdtracer::UStringCache t_ustringCache;
                    row.args_id = t_ustringCache.lookup(json.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace), logger.ustringTable(), logger.storageGeneration());
                }
                else
                    row.args_id = EMPTY_STRING_ID;
                row.api_id = hipapi.correlation_id.internal;

                logger.apiTable().insert(row);

                if (isKernelApi(hipapi.operation)) {
                    instance.d->pendingKernels[hipapi.correlation_id.internal] =
                        extractKernelLaunchData(hipapi.operation, hipapi.args);
                }
                else if (isCopyApi(hipapi.operation)) {
                    instance.d->pendingCopies[hipapi.correlation_id.internal] =
                        extractCopyData(hipapi.operation, hipapi.args);
                }

                last_correlation = hipapi.correlation_id.internal;
            }
            else if (header->kind == ROCPROFILER_BUFFER_TRACING_CORRELATION_ID_RETIREMENT) {
                auto* record = static_cast<rocprofiler_buffer_tracing_correlation_id_retirement_record_t*>(header->payload);
                instance.d->pendingKernels.erase(record->internal_correlation_id);
                instance.d->pendingCopies.erase(record->internal_correlation_id);
            }
        }
    }
    const timestamp_t cb_end_time = clocktime_ns();
    char buff[4096];
    std::snprintf(buff, 4096, "count=%ld last=%ld", num_headers, last_correlation);
    logger.createOverheadRecord(cb_begin_time, cb_end_time, "RocprofDataSource::buffer_callback", buff);
}

void RocprofDataSource::code_object_callback(rocprofiler_callback_tracing_record_t record, rocprofiler_user_data_t* user_data, void* callback_data)
{
    //fprintf(stderr, "code_object_callback\n");
    if(record.kind == ROCPROFILER_CALLBACK_TRACING_CODE_OBJECT &&
       record.operation == ROCPROFILER_CODE_OBJECT_LOAD)
    {
        if(record.phase == ROCPROFILER_CALLBACK_PHASE_UNLOAD)
        {
            // flush the buffer to ensure that any lookups for the client kernel names for the code
            // object are completed
// FIXME
            //auto flush_status = rocprofiler_flush_buffer(client_buffer);
            //if(flush_status != ROCPROFILER_STATUS_ERROR_BUFFER_BUSY)
            //    ;
        }
    }
    else if(record.kind == ROCPROFILER_CALLBACK_TRACING_CODE_OBJECT &&
            record.operation == ROCPROFILER_CODE_OBJECT_DEVICE_KERNEL_SYMBOL_REGISTER)
    {
        auto* data = static_cast<kernel_symbol_data_t*>(record.payload);
        if (record.phase == ROCPROFILER_CALLBACK_PHASE_LOAD)
        {
            s->kernel_info.emplace(data->kernel_id, *data);
            s->kernel_names.emplace(data->kernel_id, cxx_demangle(data->kernel_name));
        }
        else if (record.phase == ROCPROFILER_CALLBACK_PHASE_UNLOAD)
        {
            // FIXME: clear these?  At minimum need kernel names at shutdown, async completion
            //s->kernel_info.erase(data->kernel_id);
            //s->kernel_names.erase(data->kernel_id);
        }
    }
}


std::vector<rocprofiler_agent_v0_t>
get_gpu_device_agents()
{
    std::vector<rocprofiler_agent_v0_t> agents;

    // Callback used by rocprofiler_query_available_agents to return
    // agents on the device. This can include CPU agents as well. We
    // select GPU agents only (i.e. type == ROCPROFILER_AGENT_TYPE_GPU)
    rocprofiler_query_available_agents_cb_t iterate_cb = [](rocprofiler_agent_version_t agents_ver,
                                                            const void**                agents_arr,
                                                            size_t                      num_agents,
                                                            void*                       udata) {
        if(agents_ver != ROCPROFILER_AGENT_INFO_VERSION_0)
            throw std::runtime_error{"unexpected rocprofiler agent version"};
        auto* agents_v = static_cast<std::vector<rocprofiler_agent_v0_t>*>(udata);
        for(size_t i = 0; i < num_agents; ++i)
        {
            const auto* agent = static_cast<const rocprofiler_agent_v0_t*>(agents_arr[i]);
            //if(agent->type == ROCPROFILER_AGENT_TYPE_GPU) agents_v->emplace_back(*agent);
            agents_v->emplace_back(*agent);
        }
        return ROCPROFILER_STATUS_SUCCESS;
    };

    // Query the agents, only a single callback is made that contains a vector
    // of all agents.
    rocprofiler_query_available_agents(ROCPROFILER_AGENT_INFO_VERSION_0,
                                           iterate_cb,
                                           sizeof(rocprofiler_agent_t),
                                           const_cast<void*>(static_cast<const void*>(&agents)));
    return agents;
}


//
//
// Static setup
//
//


extern "C" rocprofiler_tool_configure_result_t*
rocprofiler_configure(uint32_t                 version,
                      const char*              runtime_version,
                      uint32_t                 priority,
                      rocprofiler_client_id_t* id)
{
    // If a RocprofilerDataSource instance hasn't been create yet, just pass
    if (s == nullptr)
        return nullptr;

    //RocprofDataSourceShared::singleton();	// CRITICAL: static init

    id->name = "rpd_tracer";
    s->clientId = id;

    // return pointer to configure data
    return &s->cfg;
}


int RocprofDataSource::toolInit(rocprofiler_client_finalize_t finialize_func, void* tool_data)
{
    s->finalizer = finialize_func;

    //s->name_info = common::get_buffer_tracing_names();
    s->name_info = rocprofiler::sdk::get_buffer_tracing_names();  // FIXME: decide
    //s->name_info = rocprofiler::sdk::get_callback_tracing_names();

    auto agent_info = get_gpu_device_agents();

    for (auto agent : agent_info) {
        s->agents[agent.id.handle] = agent;
    }

    // Common context
    //-------------------------------------------------------
    rocprofiler_create_context(&s->utilityContext);

    // Code Objects
    auto code_object_ops = std::vector<rocprofiler_tracing_operation_t>{
        ROCPROFILER_CODE_OBJECT_DEVICE_KERNEL_SYMBOL_REGISTER};

    rocprofiler_configure_callback_tracing_service(s->utilityContext,
                                                   ROCPROFILER_CALLBACK_TRACING_CODE_OBJECT,
                                                   code_object_ops.data(),
                                                   code_object_ops.size(),
                                                   RocprofDataSource::code_object_callback,
                                                   nullptr);

    {
        int isValid = 0;
        rocprofiler_context_is_valid(s->utilityContext, &isValid);
        if (isValid == 0) {
            s->utilityContext.handle = 0;   // Can't destroy it, so leak it
            return -1;
        }
    }

    rocprofiler_start_context(s->utilityContext);

    // select some api calls to omit, in the most inconvenient way possible
    // #betterThanRoctracer

    RocprofApiIdList apiList(s->name_info);
    apiList.setInvertMode(true);  // Omit the specified api
    apiList.add("hipGetDevice");
    apiList.add("hipSetDevice");
    apiList.add("hipGetLastError");
    apiList.add("__hipPushCallConfiguration");
    apiList.add("__hipPopCallConfiguration");
    apiList.add("hipCtxSetCurrent");
    apiList.add("hipGetDevicePropertiesR0600");
    apiList.add("hipGetDeviceCount");
    apiList.add("hipDeviceGetAttribute");
    apiList.add("hipRuntimeGetVersion");
    apiList.add("hipPeekAtLastError");
    apiList.add("hipModuleGetFunction");

    // Get a vector of the enabled api calls
    auto apis = apiList.allEnabled();

    // Instance s->contexts
    //-------------------------------------------------------

    //for (auto &context : s->contexts) {
    for (int i = 0; i < s->contexts.size(); ++i) {
        auto &context = s->contexts[i];
        auto &buffer = s->client_buffers[i];
        auto instance = &s->instances[i];

        rocprofiler_create_context(&context);

        // Buffers
        constexpr auto buffer_size_bytes      = 0x40000;
        constexpr auto buffer_watermark_bytes = buffer_size_bytes / 8;

        rocprofiler_create_buffer(context,
                                  buffer_size_bytes,
                                  buffer_watermark_bytes,
                                  ROCPROFILER_BUFFER_POLICY_LOSSLESS,
                                  RocprofDataSource::buffer_callback,
                                  instance,
                                  &buffer);

        rocprofiler_configure_buffer_tracing_service(context,
                                                     ROCPROFILER_BUFFER_TRACING_KERNEL_DISPATCH,
                                                     nullptr,
                                                     0,
                                                     buffer);

        rocprofiler_configure_buffer_tracing_service(context,
                                                     ROCPROFILER_BUFFER_TRACING_MEMORY_COPY,
                                                     nullptr,
                                                     0,
                                                     buffer);

        rocprofiler_configure_buffer_tracing_service(context,
                                                     ROCPROFILER_BUFFER_TRACING_HIP_RUNTIME_API_EXT,
                                                     apis.data(),
                                                     apis.size(),
                                                     buffer);

        rocprofiler_configure_buffer_tracing_service(context,
                                                     ROCPROFILER_BUFFER_TRACING_CORRELATION_ID_RETIREMENT,
                                                     nullptr,
                                                     0,
                                                     buffer);

        auto client_thread = rocprofiler_callback_thread_t{};
        rocprofiler_create_callback_thread(&client_thread);
        rocprofiler_assign_callback_thread(buffer, client_thread);

        int isValid = 0;
        rocprofiler_context_is_valid(context, &isValid);
        if (isValid == 0) {
            context.handle = 0;   // Can't destroy it, so leak it
            return -1;
        }
        //rocprofiler_start_context(context);
        rocprofiler_stop_context(context);
    }

    return 0;
}

void RocprofDataSource::toolFinialize(void* tool_data)
{
    if (s == nullptr)
        return;

    if (s->utilityContext.handle != 0) {
        rocprofiler_stop_context(s->utilityContext);
        s->utilityContext.handle = 0;
    }
    for (auto &context : s->contexts) {
        if (context.handle != 0) {
            rocprofiler_stop_context(context);
            context.handle = 0;
        }
    }
}

} // namespace rpdtracer

namespace {

RocprofApiIdList::RocprofApiIdList(buffer_name_info &names)
: m_nameMap()
{
    auto &hipapis = names[ROCPROFILER_CALLBACK_TRACING_HIP_RUNTIME_API].operations;

    for (size_t i = 0; i < hipapis.size(); ++i) {
        m_nameMap.emplace(hipapis[i], i);
    }
}

uint32_t RocprofApiIdList::mapName(const std::string &apiName)
{
    auto it = m_nameMap.find(apiName);
    if (it != m_nameMap.end()) {
        return it->second;
    }
    return 0;
}

std::vector<rocprofiler_tracing_operation_t> RocprofApiIdList::allEnabled()
{
    std::vector<rocprofiler_tracing_operation_t> oplist;
    for (auto &it : m_nameMap) {
        if (contains(it.second))
            oplist.push_back(it.second);
    }
    return oplist;
}

} // anonymous namespace
