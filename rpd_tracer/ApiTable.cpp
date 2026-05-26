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
#include "Table.h"
#include "BufferPool.h"

#include <thread>
#include <mutex>

#include "Utility.h"

using rpdtracer::ApiTable;

namespace rpdtracer {

const char *SCHEMA_API = R"|(
CREATE TEMPORARY TABLE "temp_rocpd_api" ("id" integer NOT NULL PRIMARY KEY AUTOINCREMENT, "pid" integer NOT NULL, "tid" integer NOT NULL, "start" integer NOT NULL, "end" integer NOT NULL, "apiName_id" bigint NOT NULL REFERENCES "rocpd_string" ("id") DEFERRABLE INITIALLY DEFERRED, "category_id" bigint NOT NULL REFERENCES "rocpd_string" ("id") DEFERRABLE INITIALLY DEFERRED, "domain_id" bigint NOT NULL REFERENCES "rocpd_string" ("id") DEFERRABLE INITIALLY DEFERRED, "args_id" bigint NOT NULL REFERENCES "rocpd_ustring" ("id") DEFERRABLE INITIALLY DEFERRED)
)|";

class ApiTablePrivate
{
public:
    ApiTablePrivate(ApiTable *cls) : p(cls) {
        rows = p->m_slot->rows<ApiTable::row>();
    }
    static const int BUFFERSIZE = 4096 * 16;
    static const int BATCHSIZE = 4096;           // rows per transaction
    ApiTable::row *rows;

    sqlite3_stmt *apiInsert;
    sqlite3_stmt *apiInsertNoId;
    bool directWrite;

    ApiTable *p;
};


ApiTable::ApiTable(const char *basefile, bool directWrite, BufferPool &pool)
: BufferedTable(basefile, pool.allocate<ApiTable::row>(ApiTablePrivate::BUFFERSIZE, "ApiTable"), ApiTablePrivate::BATCHSIZE)
, d(new ApiTablePrivate(this))
{
    int ret;
    d->directWrite = directWrite;

    if (!directWrite) {
        ret = sqlite3_exec(m_connection, SCHEMA_API, NULL, NULL, NULL);
        ret = sqlite3_prepare_v2(m_connection, "insert into temp_rocpd_api(id, pid, tid, start, end, domain_id, category_id, apiName_id, args_id) values (?,?,?,?,?,?,?,?,?)", -1, &d->apiInsert, NULL);
        ret = sqlite3_prepare_v2(m_connection, "insert into temp_rocpd_api(pid, tid, start, end, domain_id, category_id, apiName_id, args_id) values (?,?,?,?,?,?)", -1, &d->apiInsertNoId, NULL);
    } else {
        ret = sqlite3_prepare_v2(m_connection, "insert into rocpd_api(id, pid, tid, start, end, domain_id, category_id, apiName_id, args_id) values (?,?,?,?,?,?,?,?,?)", -1, &d->apiInsert, NULL);
        ret = sqlite3_prepare_v2(m_connection, "insert into rocpd_api(pid, tid, start, end, domain_id, category_id, apiName_id, args_id) values (?,?,?,?,?,?)", -1, &d->apiInsertNoId, NULL);
    }

}


ApiTable::~ApiTable()
{
    delete d;
}


void ApiTable::insert(const ApiTable::row &row)
{
    std::unique_lock<std::mutex> lock(m_mutex);

    if (m_slot->head() - m_slot->tail() >= ApiTablePrivate::BUFFERSIZE) {
        //FIXME
        const timestamp_t start = clocktime_ns();
        m_wait.notify_one();
        m_wait.wait(lock);
        //FIXME
        const timestamp_t end = clocktime_ns();
        lock.unlock();
        createOverheadRecord(start, end, "BLOCKING", "rpd_tracer::ApiTable::insert");
        lock.lock();
    }

    d->rows[(++m_slot->head()) % ApiTablePrivate::BUFFERSIZE] = row;

    if (workerRunning() == false && (m_slot->head() - m_slot->tail()) >= ApiTablePrivate::BATCHSIZE) {
        lock.unlock();
        m_wait.notify_one();
    }
}



void ApiTable::flushRows()
{
    if (d->directWrite)
        return;

    int ret = 0;
    ret = sqlite3_exec(m_connection, "begin transaction", NULL, NULL, NULL);
    ret = sqlite3_exec(m_connection, "insert into rocpd_api select * from temp_rocpd_api", NULL, NULL, NULL);
    rpdLog("rocpd_api: %d\n", ret);
    ret = sqlite3_exec(m_connection, "delete from temp_rocpd_api", NULL, NULL, NULL);
    ret = sqlite3_exec(m_connection, "commit", NULL, NULL, NULL);
}


void ApiTable::writeRows()
{
    std::unique_lock<std::mutex> wlock(m_writeMutex);
    std::unique_lock<std::mutex> lock(m_mutex);

    if (m_slot->head() == m_slot->tail())
        return;

    //FIXME
    const timestamp_t cb_begin_time = clocktime_ns();

    int start = m_slot->tail() + 1;
    int end = m_slot->tail() + BATCHSIZE;
    end = (end > m_slot->head()) ? m_slot->head() : end;
    lock.unlock();

    sqlite3_exec(m_connection, "BEGIN DEFERRED TRANSACTION", NULL, NULL, NULL);

    for (int i = start; i <= end; ++i) {
        // insert rocpd_api
        int index = 1;
        ApiTable::row &r = d->rows[i % m_slot->capacity()];
        sqlite3_bind_int64(d->apiInsert, index++, r.api_id + m_idOffset);
        sqlite3_bind_int(d->apiInsert, index++, r.pid);
        sqlite3_bind_int(d->apiInsert, index++, r.tid);
        sqlite3_bind_int64(d->apiInsert, index++, r.start);
        sqlite3_bind_int64(d->apiInsert, index++, r.end);
        sqlite3_bind_int64(d->apiInsert, index++, r.domain_id + m_idOffset);
        sqlite3_bind_int64(d->apiInsert, index++, r.category_id + m_idOffset);
        sqlite3_bind_int64(d->apiInsert, index++, r.apiName_id + m_idOffset);
        sqlite3_bind_int64(d->apiInsert, index++, r.args_id + m_idOffset);
        int ret = sqlite3_step(d->apiInsert);
        sqlite3_reset(d->apiInsert);
    }
    lock.lock();
    m_slot->tail() = end;
    lock.unlock();

    sqlite3_exec(m_connection, "END TRANSACTION", NULL, NULL, NULL);
    //FIXME
    const timestamp_t cb_end_time = clocktime_ns();
    char buff[4096];
    std::snprintf(buff, 4096, "count=%d | remaining=%d", end - start + 1, m_slot->head() - m_slot->tail());
    createOverheadRecord(cb_begin_time, cb_end_time, "ApiTable::writeRows", buff);
}

}  // namespace rpdtracer
