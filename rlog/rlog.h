
/*********************************************************************************
* Copyright (c) 2021 - 2024 Advanced Micro Devices, Inc. All rights reserved.
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

#pragma once

#include <dlfcn.h>
#include <stdio.h>


#ifdef __cplusplus
extern "C"
{
#endif

    // C API functions ------------------------------------------------------------
    static inline void rlog_markFull(const char *domain, const char *category, const char *apiname, const char *args);
    static inline void rlog_markCategory(const char *category, const char *apiname, const char *args);
    static inline void rlog_markArgs(const char *apiname, const char *args);

    static inline void rlog_rangePushFull(const char *domain, const char *category, const char *apiname, const char *args);
    static inline void rlog_rangePushCategory(const char *category, const char *apiname, const char *args);
    static inline void rlog_rangePushArgs(const char *apiname, const char *args);

    static inline void rlog_rangePop();

    static inline int rlog_registerActiveCallback(void (*cb)());

    static inline void rlog_setDefaultDomain(const char *ddomain);
    static inline void rlog_setDefaultCategory(const char *dcat);
    static inline const char *rlog_getProperty(const char *domain, const char *property, const char *defaultValue);
    // END C API functions --------------------------------------------------------

#ifdef __cplusplus
}
#endif


#ifdef __cplusplus

namespace rlog
{

    // C++ API functions -------------------------------------------------------------
    static inline void init();

    static inline void mark(const char *domain, const char *category, const char *apiname, const char *args);
    static inline void mark(const char *category, const char *apiname, const char *args);
    static inline void mark(const char *apiname, const char *args);

    static inline void rangePush(const char *domain, const char *category, const char *apiname, const char *args);
    static inline void rangePush(const char *category, const char *apiname, const char *args);
    static inline void rangePush(const char *apiname, const char *args);

    static inline void rangePop();

    static inline int registerActiveCallback(void (*cb)());

    static inline void setDefaultDomain(const char *);
    static inline void setDefaultCategory(const char *);
    static inline const char *getProperty(const char *domain, const char *property, const char *defaultValue);

    // END C++ API functions ---------------------------------------------------------

    // Static data and function pointers to maintain state within C++ code.
    namespace
    {
        const char *domain = "";
        const char *category = "";

        static void (*log_mark_)(const char *, const char *, const char *) = nullptr;
        static void (*log_rangePush_)(const char *, const char *, const char *) = nullptr;
        static void (*log_rangePop_)() = nullptr;
        static void (*registerActiveCallback_)(void (*cb)()) = nullptr;

        static void (*roctx_mark_)(const char *message) = nullptr;
        static void (*roctx_rangePush_)(const char *message) = nullptr;
        static void (*roctx_rangePop_)() = nullptr;
    }

    static inline void init()
    {
#if 0
        void *dl = dlopen("librlog.so", RTLD_LAZY);
        if (dl)
        {
            log_mark_ = (void (*)(const char *, const char *, const char *))dlsym(dl, "log_mark");
            log_rangePush_ = (void (*)(const char *, const char *, const char *))dlsym(dl, "log_rangePush");
            log_rangePop_ = (void (*)())dlsym(dl, "log_rangePop");
            //TODO: debug prints, remove before merge
            fprintf(stderr, "Info : librlog found\n");
        }
        else
#endif
        {

            void *dltx = dlopen("libroctx64.so", RTLD_LAZY);
            if (dltx)
            {
                roctx_mark_ = (void (*)(const char *))dlsym(dltx, "roctxMarkA");
                if (!roctx_mark_)
                    fprintf(stderr, "Error : roctx_mark_ not set \n");
                roctx_rangePush_ = (void (*)(const char *))dlsym(dltx, "roctxRangePushA");
                if (!roctx_rangePush_)
                    fprintf(stderr, "Error : roctx_rangePush_ not set \n");
                roctx_rangePop_ = (void (*)())dlsym(dltx, "roctxRangePop");
                if (!roctx_rangePop_)
                    fprintf(stderr, "Error : roctx_rangePop_ not set \n");
                if (!roctx_mark_)
                    fprintf(stderr, "Error : roctx_mark_ not set \n");

                //TODO: debug prints, remove before merge
                fprintf(stderr, "Info : libroctx found\n");
            }
            else
                //TODO: debug prints, remove before merge
                fprintf(stderr, "Warning : librlog & libroctx NOT found\n");
        }
    }

    // Inline function implementations
    static inline void mark(const char *domain, const char *category, const char *apiname, const char *args)
    {
        if (log_mark_)
            log_mark_(domain, apiname, args);
        if (roctx_mark_)
        {
            char buff[4096];
            snprintf(buff, 4096, "%s : %s : api = %s | %s", domain, category, apiname, args);
            roctx_mark_(buff);
        }
    }

    static inline void mark(const char *category, const char *apiname, const char *args)
    {
        mark(domain, category, apiname, args);
    }

    static inline void mark(const char *apiname, const char *args)
    {
        mark(domain, category, apiname, args);
    }

    static inline void rangePush(const char *domain, const char *category, const char *apiname, const char *args)
    {
        fprintf(stderr, "RK Test rangePush \n");

        if (log_rangePush_)
            log_rangePush_(domain, apiname, args);

        if (roctx_rangePush_)
        {
            fprintf(stderr, "RK Test roctx_rangePush_ \n");
            char buff[4096];
            snprintf(buff, 4096, "%s : %s : api = %s | %s", domain, category, apiname, args);
            roctx_rangePush_(buff);
        }
        else
            fprintf(stderr, "Error roctx_rangePush_ not set \n");
    }

    static inline void rangePush(const char *category, const char *apiname, const char *args)
    {
        rangePush(domain, category, apiname, args);
    }

    static inline void rangePush(const char *apiname, const char *args)
    {
        rangePush(domain, category, apiname, args);
    }

    static inline void rangePop()
    {
        if (log_rangePop_)
            log_rangePop_();
        if (roctx_rangePop_)
            roctx_rangePop_();
    }

    static inline int registerActiveCallback(void (*cb)())
    {
        if (registerActiveCallback_)
        {
            registerActiveCallback_(cb);
            return 0;
        }
        else
            return -1;
    }

    static inline void setDefaultDomain(const char *ddomain)
    {
        domain = ddomain;
    }

    static inline void setDefaultCategory(const char *dcat)
    {
        category = dcat;
    }

    // FIXME: lifetime
    static inline const char *getProperty(const char *domain, const char *property, const char *defaultValue)
    {
        return defaultValue;
    }

} // namespace rlog

#endif // __cplusplus