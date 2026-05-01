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

#include <thread>

#include "Utility.h"

using rpdtracer::MetadataTable;

namespace rpdtracer {

class MetadataTablePrivate
{
public:
    MetadataTablePrivate(MetadataTable *cls) : p(cls) {} 

    sqlite3_stmt *sessionInsert;
    sqlite3_stmt *metaInsert;

    sqlite3_int64 sessionId;
    void createSession();

    MetadataTable *p;
};

int sessionCallback(void *data, int argc, char **argv, char **colName)
{
    sqlite3_int64 &sessionId = *(sqlite3_int64*)data;
    sessionId = atoll(argv[0]);
    return 0;
}

MetadataTable::MetadataTable(const char *basefile)
: Table(basefile)
, d(new MetadataTablePrivate(this))
{
    sqlite3_prepare_v2(m_connection, "INSERT INTO rocpd_metadata(tag, value) VALUES (?,?)", -1, &d->metaInsert, NULL);
    d->createSession();
}

void MetadataTable::flush()
{
}

void MetadataTable::finalize()
{
}

void MetadataTable::insert(const std::string &tag, const std::string &value)
{
    sqlite3_exec(m_connection, "BEGIN", NULL, NULL, NULL);
    sqlite3_bind_text(d->metaInsert, 1, tag.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(d->metaInsert, 2, value.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(d->metaInsert);
    sqlite3_reset(d->metaInsert);
    sqlite3_exec(m_connection, "END", NULL, NULL, NULL);
}

sqlite3_int64 MetadataTable::sessionId()
{
	return d->sessionId;
}


void MetadataTablePrivate::createSession()
{
    int ret;
    sqlite3_exec(p->m_connection, "BEGIN EXCLUSIVE TRANSACTION", NULL, NULL, NULL);
    // get or create session count property

    sqlite3_int64 sessionId = -1;
    char *error_msg;
    ret = sqlite3_exec(p->m_connection, "SELECT value FROM rocpd_metadata WHERE tag = 'session_count'", &sessionCallback, &sessionId, &error_msg);
    if (sessionId == -1) {
        sessionId = 0;
        ret = sqlite3_exec(p->m_connection, "INSERT into rocpd_metadata(tag, value) VALUES ('session_count', 1)", NULL, NULL, &error_msg);
    }
    else {
        char buff[4096];
        std::snprintf(buff, 4096, "UPDATE rocpd_metadata SET value = '%lld' WHERE tag = 'session_count'", sessionId + 1);
        ret = sqlite3_exec(p->m_connection, buff, NULL, NULL, &error_msg);
    }

    sqlite3_exec(p->m_connection, "END TRANSACTION", NULL, NULL, NULL);

    //printf("Opening session: %lld\n", sessionId);
    fflush(stdout);

    this->sessionId = sessionId;
}

}  // namespace rpdtracer
