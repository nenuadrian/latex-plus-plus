#pragma once

namespace latex3d
{

    enum class LogLevel
    {
        Info,
        Warn,
        Error,
    };

    // Library log dispatch. By default the callback is null and all messages
    // are silently dropped. Embedders that want diagnostics (failed font load,
    // missing MATH table, malformed LaTeX input) install a callback at
    // startup; the library calls it from any thread.
    using LogCallback = void (*)(LogLevel, const char *msg, void *user);

    void setLogCallback(LogCallback cb, void *user);

    // Internal: dispatch a formatted log message. The vsnprintf happens on a
    // stack buffer; messages longer than ~512 bytes are truncated. Callers
    // should use the LATEX3D_LOG_* macros below rather than calling directly.
    void logf(LogLevel level, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
        __attribute__((format(printf, 2, 3)))
#endif
            ;

#define LATEX3D_LOG_INFO(...)  ::latex3d::logf(::latex3d::LogLevel::Info,  __VA_ARGS__)
#define LATEX3D_LOG_WARN(...)  ::latex3d::logf(::latex3d::LogLevel::Warn,  __VA_ARGS__)
#define LATEX3D_LOG_ERROR(...) ::latex3d::logf(::latex3d::LogLevel::Error, __VA_ARGS__)

} // namespace latex3d
