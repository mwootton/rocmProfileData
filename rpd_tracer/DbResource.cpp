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
#include "DbResource.h"

#include <mutex>
#include <fmt/format.h>
#include "Utility.h"

using rpdtracer::DbResource;

namespace rpdtracer {

class DbResourcePrivate
{
public:
    DbResourcePrivate(DbResource *cls) : p(cls) {}

    std::string resourceName;
    bool locked {false};

    DbResource *p;

    static sqlite3 *s_connection;
    static std::mutex s_mutex;
    static int s_refCount;

    static void open(const char *basefile);
    static void close();
    static int resourceCallback(void *data, int argc, char **argv, char **colName);
};

sqlite3 *DbResourcePrivate::s_connection = nullptr;
std::mutex DbResourcePrivate::s_mutex;
int DbResourcePrivate::s_refCount = 0;

void DbResourcePrivate::open(const char *basefile)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_refCount++ == 0)
        rpdSqliteOpen(basefile, &s_connection);
}

void DbResourcePrivate::close()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    if (--s_refCount == 0) {
        sqlite3_close(s_connection);
        s_connection = nullptr;
    }
}


DbResource::DbResource(const std::string &basefile, const std::string &resourceName)
: d(new DbResourcePrivate(this))
{
    DbResourcePrivate::open(basefile.c_str());
    d->resourceName = resourceName;
}

DbResource::~DbResource()
{
    unlock();
    DbResourcePrivate::close();
}

int DbResourcePrivate::resourceCallback(void *data, int argc, char **argv, char **colName)
{
    sqlite3_int64 &resourceId = *(sqlite3_int64*)data;
    resourceId = atoll(argv[0]);
    return 0;
}

bool DbResource::tryLock()
{
   std::lock_guard<std::mutex> lock(DbResourcePrivate::s_mutex);
   if (d->locked == false) {
       sqlite3_int64 resourceValue = -1;
       sqlite3_exec(DbResourcePrivate::s_connection, fmt::format("SELECT value FROM rocpd_metadata WHERE tag = 'resourceLock::{}'", d->resourceName).c_str(), &DbResourcePrivate::resourceCallback, &resourceValue, NULL);
       if (resourceValue <= 0) {
           sqlite3_exec(DbResourcePrivate::s_connection, "BEGIN EXCLUSIVE TRANSACTION", NULL, NULL, NULL);
           resourceValue = -1;
           sqlite3_exec(DbResourcePrivate::s_connection, fmt::format("SELECT value FROM rocpd_metadata WHERE tag = 'resourceLock::{}'", d->resourceName).c_str(), &DbResourcePrivate::resourceCallback, &resourceValue, NULL);
           if (resourceValue <= 0) {
               int ret = sqlite3_exec(DbResourcePrivate::s_connection, fmt::format("INSERT OR REPLACE INTO rocpd_metadata(tag, value) VALUES ('resourceLock::{}', 1)", d->resourceName).c_str(), NULL, NULL, NULL);
               if (ret == SQLITE_OK)
                   d->locked = true;
           }
           sqlite3_exec(DbResourcePrivate::s_connection, "END TRANSACTION", NULL, NULL, NULL);
       }
   }
   return d->locked;
}

void DbResource::unlock()
{
    std::lock_guard<std::mutex> lock(DbResourcePrivate::s_mutex);
    if (d->locked) {
        sqlite3_exec(DbResourcePrivate::s_connection, "BEGIN EXCLUSIVE TRANSACTION", NULL, NULL, NULL);
        sqlite3_exec(DbResourcePrivate::s_connection, fmt::format("UPDATE rocpd_metadata SET value = '0' WHERE tag = 'resourceLock::{}'", d->resourceName).c_str(), NULL, NULL, NULL);
        sqlite3_exec(DbResourcePrivate::s_connection, "END TRANSACTION", NULL, NULL, NULL);
        d->locked = false;
    }
}

bool DbResource::isLocked()
{
    return d->locked;
}

}  // namespace rpdtracer
