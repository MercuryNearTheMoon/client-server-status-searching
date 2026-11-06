#ifndef SYSINFO_LOGGER_H
#define SYSINFO_LOGGER_H

enum sysinfo_log_level {
    SYSINFO_LOG_ERROR = 0,
    SYSINFO_LOG_WARN = 1,
    SYSINFO_LOG_INFO = 2,
    SYSINFO_LOG_DEBUG = 3
};

/* Per-process runtime threshold; intended default is SYSINFO_LOG_INFO. */
void sysinfo_log_set_level(enum sysinfo_log_level level);

/* printf-style output to stderr. Pass untrusted text using "%s".
 * Implementation must also discard DEBUG when ENABLE_DEBUG is disabled.
 */
void sysinfo_log(enum sysinfo_log_level level, const char *format, ...);

/* Compile-time switch: -DENABLE_DEBUG=1. Disabled calls do not evaluate args. */
#if defined(ENABLE_DEBUG) && ENABLE_DEBUG
#define SYSINFO_DEBUG(...) sysinfo_log(SYSINFO_LOG_DEBUG, __VA_ARGS__)
#else
#define SYSINFO_DEBUG(...) ((void)0)
#endif

#endif /* SYSINFO_LOGGER_H */
