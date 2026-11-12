#ifndef SYSINFO_LOGGER_H
#define SYSINFO_LOGGER_H

enum sysinfo_log_level {
    SYSINFO_LOG_ERROR = 0,
    SYSINFO_LOG_WARN = 1,
    SYSINFO_LOG_INFO = 2,
    SYSINFO_LOG_DEBUG = 3
};

/* Per-process runtime threshold, default INFO. Invalid levels are ignored. */
void sysinfo_log_set_level(enum sysinfo_log_level level);

/* printf-style output to stderr. Pass untrusted text using "%s".
 * Preserves errno; invalid level / NULL format is ignored.
 * Records include UTC timestamp and PID, are bounded to 512 bytes, and end
 * with a newline. Long messages are truncated; embedded CR/LF become spaces.
 * Best-effort output: write failures are not returned. Not signal-handler safe.
 * Implementation also discards DEBUG when ENABLE_DEBUG is disabled.
 */
void sysinfo_log(enum sysinfo_log_level level, const char *format, ...);

/* Compile-time ceiling: -1=off, 0=error, 1=warn, 2=info, 3=debug.
 * Runtime level further filters output. Disabled macros do not evaluate args.
 */
#ifndef SYSINFO_COMPILE_LOG_LEVEL
#define SYSINFO_COMPILE_LOG_LEVEL 2
#endif
#if SYSINFO_COMPILE_LOG_LEVEL < -1 || SYSINFO_COMPILE_LOG_LEVEL > 3
#error "SYSINFO_COMPILE_LOG_LEVEL must be -1..3"
#endif

#define SYSINFO_LOG_WRITE(level, label, ...) sysinfo_log(level, __VA_ARGS__)

#if SYSINFO_COMPILE_LOG_LEVEL >= 0
#define SYSINFO_ERROR_LOG(...) SYSINFO_LOG_WRITE(SYSINFO_LOG_ERROR, "ERROR", __VA_ARGS__)
#else
#define SYSINFO_ERROR_LOG(...) ((void)0)
#endif
#if SYSINFO_COMPILE_LOG_LEVEL >= 1
#define SYSINFO_WARN_LOG(...) SYSINFO_LOG_WRITE(SYSINFO_LOG_WARN, "WARN", __VA_ARGS__)
#else
#define SYSINFO_WARN_LOG(...) ((void)0)
#endif
#if SYSINFO_COMPILE_LOG_LEVEL >= 2
#define SYSINFO_INFO_LOG(...) SYSINFO_LOG_WRITE(SYSINFO_LOG_INFO, "INFO", __VA_ARGS__)
#else
#define SYSINFO_INFO_LOG(...) ((void)0)
#endif
#if defined(ENABLE_DEBUG) && ENABLE_DEBUG && SYSINFO_COMPILE_LOG_LEVEL >= 3
#define SYSINFO_DEBUG(...) SYSINFO_LOG_WRITE(SYSINFO_LOG_DEBUG, "DEBUG", __VA_ARGS__)
#else
#define SYSINFO_DEBUG(...) ((void)0)
#endif

#endif /* SYSINFO_LOGGER_H */
