/**************************************************************************
 * Copyright (c) 2022 - 2024 Advanced Micro Devices, Inc.
 **************************************************************************/
#pragma once

#include "DataSource.h"

#include <cstdint>

namespace rpdtracer {

class AmdSmiDataSourcePrivate;
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
    AmdSmiDataSourcePrivate *d;
    friend class AmdSmiDataSourcePrivate;
};

}    // namespace rpdtracer
