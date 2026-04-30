#include <latex3d/log.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>

namespace latex3d
{
    namespace
    {
        std::atomic<LogCallback> gCallback{nullptr};
        std::atomic<void *> gUser{nullptr};
    } // namespace

    void setLogCallback(LogCallback cb, void *user)
    {
        gCallback.store(cb, std::memory_order_release);
        gUser.store(user, std::memory_order_release);
    }

    void logf(LogLevel level, const char *fmt, ...)
    {
        LogCallback cb = gCallback.load(std::memory_order_acquire);
        if (!cb)
            return;
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        cb(level, buf, gUser.load(std::memory_order_acquire));
    }

} // namespace latex3d
