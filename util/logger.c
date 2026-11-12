#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "logger.h"

static enum sysinfo_log_level current_level = SYSINFO_LOG_INFO;

void sysinfo_log_set_level(enum sysinfo_log_level level){
    if (level < SYSINFO_LOG_ERROR || level > SYSINFO_LOG_DEBUG) return;
    current_level = level;
}

void sysinfo_log(enum sysinfo_log_level level, const char *format, ...){
    int saved_errno = errno;
    if (format == NULL || level < SYSINFO_LOG_ERROR || level > SYSINFO_LOG_DEBUG) return;
    if ((int)level > SYSINFO_COMPILE_LOG_LEVEL || level > current_level) return;
#if !defined(ENABLE_DEBUG) || !ENABLE_DEBUG
    if (level == SYSINFO_LOG_DEBUG) return;
#endif

    static const char *const names[] = {"ERROR", "WARN", "INFO", "DEBUG"};
    char timestamp[32] = "time-unavailable";
    time_t now = time(NULL);
    struct tm utc;
    if (now != (time_t)-1 && gmtime_r(&now, &utc) != NULL) {
        if (strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc) == 0)
            strcpy(timestamp, "time-unavailable");
    }

    /* At most 512 bytes including newline: one bounded write per record. */
    char line[512];
    int prefix = snprintf(line, sizeof(line), "[%s] %s pid=%ld ",
                          names[level], timestamp, (long)getpid());
    if (prefix < 0 || (size_t)prefix >= sizeof(line) - 1) {
        errno = saved_errno;
        return;
    }

    va_list args;
    va_start(args, format);
    int formatted = vsnprintf(line + prefix, sizeof(line) - (size_t)prefix, format, args);
    va_end(args);
    if (formatted < 0) {
        errno = saved_errno;
        return;
    }

    size_t length = (size_t)prefix;
    if ((size_t)formatted >= sizeof(line) - length) {
        length = sizeof(line) - 1;
        memcpy(line + length - 3, "...", 3);
    }
    else {
        length += (size_t)formatted;
    }
    for (size_t i = (size_t)prefix; i < length; ++i) {
        if (line[i] == '\n' || line[i] == '\r' || line[i] == '\0') line[i] = ' ';
    }
    line[length++] = '\n';

    ssize_t written;
    do {
        written = write(STDERR_FILENO, line, length);
    } while (written < 0 && errno == EINTR);
    /* Do not recursively log failures or split partial records into writes. */
    errno = saved_errno;
}
