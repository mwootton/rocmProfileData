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
#include "Logger.h"

#include <algorithm>
#include <list>
#include <vector>
#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <fmt/format.h>

#include "Utility.h"
#include "Schema.h"

using rpdtracer::Logger;

namespace rpdtracer { void rlogClientInit(); }

// GFH - This mirrors the function in the pre-refactor code.  Allows both code paths to compile.
//   See table classes for users.  Todo: build a proper threaded record writer
void rpdtracer::createOverheadRecord(uint64_t start, uint64_t end, const std::string &name, const std::string &args)
{
    Logger::singleton().createOverheadRecord(start, end, name, args);
}

namespace {
    bool loggerInitialized { false };
}

Logger& Logger::singleton()
{
    static Logger logger;
    return logger;
}

void Logger::rpdInit() {
    bool doInit = true;
    char *val = getenv("RPDT_DELAYINIT");
    if (val != NULL) {
        int delayinit = atoi(val);
        if (delayinit != 0)
            doInit = false;
    }
    if (doInit)
        Logger::singleton();

    // Indicate the tracer loaded.  Used for snooping without loading
    setenv("RPDT_LOADED", "1", 1);
}

void Logger::rpdFinalize() {
    if (loggerInitialized)
        Logger::singleton().finalize();
}


void Logger::rpdstart()
{
    std::unique_lock<std::mutex> lock(m_activeMutex);
    if (m_activeCount == 0) {
        rlog::mark("rpd_tracer", "", "rpdstart", "");
        for (auto it = m_sources.begin(); it != m_sources.end(); ++it)
            (*it)->startTracing();
    }
    ++m_activeCount;
}

void Logger::rpdstop()
{
    std::unique_lock<std::mutex> lock(m_activeMutex);
    if (m_activeCount == 1) {
        rlog::mark("rpd_tracer", "", "rpdstop", "");
        for (auto it = m_sources.begin(); it != m_sources.end(); ++it)
            (*it)->stopTracing();
    }
    --m_activeCount;
}

void Logger::rpdflush()
{
    //fprintf(stderr, "rpd_tracer: FLUSH\n");
    const timestamp_t cb_begin_time = clocktime_ns();

    // Have the data sources flush out whatever they have available
    for (auto it = m_sources.begin(); it != m_sources.end(); ++it)
            (*it)->flush();

    m_stringTable->flush();
    m_ustringTable->flush();
    m_kernelApiTable->flush();
    m_copyApiTable->flush();
    m_opTable->flush();
    m_apiTable->flush();
    m_monitorTable->flush();
    m_stackFrameTable->flush();

    const timestamp_t cb_end_time = clocktime_ns();
    createOverheadRecord(cb_begin_time, cb_end_time, "rpdflush", "");
}

void Logger::rpd_rangePush(const char *domain, const char *apiName, const char* args)
{
    {
        std::unique_lock<std::mutex> lock(m_activeMutex);
        if (m_activeCount == 0)
            return;
    }
    ApiTable::row row;
    row.pid = GetPid();
    row.tid = GetTid();
    row.start = clocktime_ns();
    row.end = row.start;
    row.domain_id = m_stringTable->getOrCreate(domain);
    row.category_id = EMPTY_STRING_ID;
    row.apiName_id = m_stringTable->getOrCreate(apiName);
    row.args_id = m_ustringTable->create(args);
    row.api_id = 0;
    m_apiTable->pushRoctx(row);
}

void Logger::rpd_rangePop()
{
    {
        std::unique_lock<std::mutex> lock(m_activeMutex);
        if (m_activeCount == 0)
            return;
    }
    ApiTable::row row;
    row.pid = GetPid();
    row.tid = GetTid();
    row.start = clocktime_ns();
    row.end = row.start;
    row.apiName_id = EMPTY_STRING_ID;
    row.args_id = EMPTY_STRING_ID;
    row.api_id = 0;
    m_apiTable->popRoctx(row);
}




void Logger::init()
{
    fprintf(stderr, "rpd_tracer, because\n");

    rlogClientInit();

    rlog::getProperty("rpd_tracer", "filename", "./trace.rpd");
    const char *filename = getConfig("RPDT_FILENAME", "filename", "./trace.rpd");
    m_filename = filename;

    // Ensure schema exists

    ensureSchema(filename);

    // Create table recorders

    bool directWrite = false;

    const char *dwrite = getenv("RPDT_DIRECTWRITE");
    if (dwrite != nullptr) {
        int val = atoi(dwrite);
        directWrite = (val != 0);
    }

    m_metadataTable = new MetadataTable(filename);
    m_stringTable = new StringTable(filename, directWrite);
    m_ustringTable = new UStringTable(filename, directWrite);
    m_kernelApiTable = new KernelApiTable(filename, directWrite);
    m_copyApiTable = new CopyApiTable(filename, directWrite);
    m_opTable = new OpTable(filename, directWrite);
    m_apiTable = new ApiTable(filename, directWrite);
    m_monitorTable = new MonitorTable(filename, directWrite);
    m_stackFrameTable = new StackFrameTable(filename, directWrite);

    // Log our session and pid
    m_metadataTable->insert("session", fmt::format("id={} pid={}", m_metadataTable->sessionId(), GetPid()));

    // Offset primary keys so they do not collide between sessions
    sqlite3_int64 offset = m_metadataTable->sessionId() * (sqlite3_int64(1) << 32);
    m_metadataTable->setIdOffset(offset);
    m_stringTable->setIdOffset(offset);
    m_ustringTable->setIdOffset(offset);
    m_kernelApiTable->setIdOffset(offset);
    m_copyApiTable->setIdOffset(offset);
    m_opTable->setIdOffset(offset);
    m_apiTable->setIdOffset(offset);
    m_stackFrameTable->setIdOffset(offset);

    // Create one instance of each available datasource
    std::list<std::string> factories = {
        "ClrDataSourceFactory",
        "RoctxDataSourceFactory",
        "NvtxDataSourceFactory",
        "RocprofDataSourceFactory",
        "RoctracerDataSourceFactory",
        "CuptiDataSourceFactory",
        "RlogDataSourceFactory",
        "RocmSmiDataSourceFactory"
        };

    // RPDT_DATASOURCES: comma-separated list of DataSource names to prioritize.
    // Each is moved to the front of the factory list (or added if not present),
    // preserving the order given so the first entry ends up first.
    const char *dsenv = getConfig("RPDT_DATASOURCES", "datasources", "");
    if (dsenv[0] != '\0') {
        std::vector<std::string> extra;
        std::string dslist(dsenv);
        size_t pos = 0, end;
        do {
            end = dslist.find(',', pos);
            std::string name = dslist.substr(pos, end == std::string::npos ? end : end - pos);
            if (!name.empty())
                extra.push_back(name + "Factory");
            pos = end + 1;
        } while (end != std::string::npos);
        for (auto it = extra.rbegin(); it != extra.rend(); ++it) {
            factories.remove(*it);
            factories.push_front(*it);
        }
    }

    std::list<std::string> rocmFactories = {
        "RocprofDataSourceFactory",
        "ClrDataSourceFactory",
        "RoctracerDataSourceFactory"
        };

    bool rocmSourceAdded = false;
    for (auto it = factories.begin(); it != factories.end(); ++it) {
        bool isRocmFactory = std::find(rocmFactories.begin(), rocmFactories.end(), *it) != rocmFactories.end();
        if (isRocmFactory && rocmSourceAdded)
            continue;
        DataSource* (*func) (void) = (DataSource* (*)()) dlsym(RTLD_DEFAULT, (*it).c_str());
        if (func) {
            m_sources.push_back(func());
            if (isRocmFactory)
                rocmSourceAdded = true;
            std::string sourceName = it->substr(0, it->size() - 7);  // strip "Factory"
            m_metadataTable->insert("process_datasource", fmt::format("pid={} source={}", GetPid(), sourceName));
        }
    }

    // Initialize data sources
    for (auto it = m_sources.begin(); it != m_sources.end(); ++it)
            (*it)->init();

    // Allow starting with recording disabled via ENV
    bool startTracing = true;
    if (atoi(getConfig("RPDT_AUTOSTART", "autostart", "1")) == 0)
        startTracing = false;
    if (startTracing == true) {
        for (auto it = m_sources.begin(); it != m_sources.end(); ++it)
            (*it)->startTracing();
        std::unique_lock<std::mutex> lock(m_activeMutex);
        ++m_activeCount;
    }
    // Start autoflush hack
    {
        int frequency = atoi(getConfig("RPDT_AUTOFLUSH", "autoflush", "0"));
        if (frequency > 0) {
            m_period = 1000000 / frequency;  // usecs
            m_done = false;
            m_worker = new std::thread(&Logger::autoflushWorker, this);
        }
    }

    // Enable stack frame recording
    m_writeStackFrames = (atoi(getConfig("RPDT_STACKFRAMES", "stackframes", "0")) != 0);

    loggerInitialized = true;  // detect lazy init
}

static bool doFinalize = true;
std::mutex finalizeMutex;

void Logger::finalize()
{
    std::lock_guard<std::mutex> guard(finalizeMutex);
    if (doFinalize == true) {
        doFinalize = false;

        m_done = true;
        if (m_worker != nullptr)
            m_worker->join();

        {
            std::unique_lock<std::mutex> lock(m_activeMutex);
            if (m_activeCount > 0) {
                for (auto it = m_sources.begin(); it != m_sources.end(); ++it)
                    (*it)->stopTracing();
            }
        }

        for (auto it = m_sources.begin(); it != m_sources.end(); ++it)
            (*it)->end();

        // Flush recorders
        const timestamp_t begin_time = clocktime_ns();
        m_opTable->finalize();		// OpTable before subclassOpTables
        m_kernelApiTable->finalize();
        m_copyApiTable->finalize();
        m_monitorTable->finalize();
        m_stackFrameTable->finalize();
        m_writeOverheadRecords = false;	// Don't make any new overhead records (api calls)
        m_apiTable->finalize();
        m_ustringTable->finalize();
        m_stringTable->finalize();	// String table last

        const timestamp_t end_time = clocktime_ns();
        fprintf(stderr, "rpd_tracer: finalized in %f ms\n", 1.0 * (end_time - begin_time) / 1000000);
    }
}

void Logger::autoflushWorker()
{
    while (m_done == false) {
        rpdflush();
        usleep(m_period);
    }
}

void Logger::createOverheadRecord(uint64_t start, uint64_t end, const std::string &name, const std::string &args)
{
    if (m_writeOverheadRecords == false)
        return;
    static sqlite3_int64 domain_id = m_stringTable->getOrCreate("rpd_tracer");
    static sqlite3_int64 category_id = m_stringTable->getOrCreate("overhead");
    ApiTable::row row;
    row.pid = GetPid();
    row.tid = GetTid();
    row.start = start;
    row.end = end;
    row.domain_id = domain_id;
    row.category_id = category_id;
    row.apiName_id = m_stringTable->getOrCreate(name);
    row.args_id = m_ustringTable->create(args);
    row.api_id = 0;

    //fprintf(stderr, "overhead: %s (%s) - %f usec\n", name.c_str(), args.c_str(), (end-start) / 1000.0);

    m_apiTable->insertRoctx(row);
}

