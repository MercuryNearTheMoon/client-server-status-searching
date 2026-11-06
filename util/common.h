#ifndef SYSINFO_COMMON_H
#define SYSINFO_COMMON_H

#define SYSINFO_DEFAULT_PORT 8080
#define SYSINFO_MAX_PAYLOAD 4096U
#define SYSINFO_MAX_HOLD_SECONDS 30U

/* Shared return codes; functions may document additional return values. */
enum sysinfo_result {
    SYSINFO_OK = 0,
    SYSINFO_ERROR = -1
};

#endif /* SYSINFO_COMMON_H */
