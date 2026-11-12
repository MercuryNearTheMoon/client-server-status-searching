#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "util/logger.h"

#define CHECK(expr) do { \
    if (!(expr)) { \
        printf("FAIL line %d: %s\n", __LINE__, #expr); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

static int enabled(int level, int runtime){
    if (level > SYSINFO_COMPILE_LOG_LEVEL || level > runtime) return 0;
#if !defined(ENABLE_DEBUG) || !ENABLE_DEBUG
    if (level == SYSINFO_LOG_DEBUG) return 0;
#endif
    return 1;
}

static FILE *capture(int *saved_fd){
    FILE *file = tmpfile();
    CHECK(file != NULL);
    *saved_fd = dup(STDERR_FILENO);
    CHECK(*saved_fd >= 0);
    CHECK(dup2(fileno(file), STDERR_FILENO) >= 0);
    return file;
}

static size_t finish(FILE *file, int saved_fd, char *output, size_t capacity){
    CHECK(dup2(saved_fd, STDERR_FILENO) >= 0);
    close(saved_fd);
    CHECK(fseek(file, 0, SEEK_SET) == 0);
    size_t length = fread(output, 1, capacity - 1, file);
    CHECK(!ferror(file));
    CHECK(length < capacity - 1);
    output[length] = '\0';
    fclose(file);
    return length;
}

int main(void){
    int saved_fd;
    char output[8192];
    const char *const names[] = {"ERROR", "WARN", "INFO", "DEBUG"};

    /* Default runtime threshold is INFO, including direct function calls. */
    FILE *file = capture(&saved_fd);
    for (int level = 0; level < 4; ++level)
        sysinfo_log((enum sysinfo_log_level)level, "default-%s", names[level]);
    finish(file, saved_fd, output, sizeof(output));
    for (int level = 0; level < 4; ++level)
        CHECK((strstr(output, names[level]) != NULL) == enabled(level, SYSINFO_LOG_INFO));

    for (int runtime = 0; runtime < 4; ++runtime) {
        sysinfo_log_set_level((enum sysinfo_log_level)runtime);
        file = capture(&saved_fd);
        for (int level = 0; level < 4; ++level) {
            errno = EBUSY;
            sysinfo_log((enum sysinfo_log_level)level, "runtime-%s", names[level]);
            CHECK(errno == EBUSY);
        }
        finish(file, saved_fd, output, sizeof(output));
        for (int level = 0; level < 4; ++level)
            CHECK((strstr(output, names[level]) != NULL) == enabled(level, runtime));
    }
    puts("PASS default/runtime/compile-time levels and errno preservation");

    sysinfo_log_set_level(SYSINFO_LOG_DEBUG);
    file = capture(&saved_fd);
    int evaluated[4] = {0};
    SYSINFO_ERROR_LOG("error %d", ++evaluated[0]);
    SYSINFO_WARN_LOG("warn %d", ++evaluated[1]);
    SYSINFO_INFO_LOG("info %d", ++evaluated[2]);
    SYSINFO_DEBUG("debug %d", ++evaluated[3]);
    finish(file, saved_fd, output, sizeof(output));
    for (int level = 0; level < 4; ++level) {
        CHECK(evaluated[level] == enabled(level, SYSINFO_LOG_DEBUG));
        CHECK((strstr(output, names[level]) != NULL) == enabled(level, SYSINFO_LOG_DEBUG));
    }
    puts("PASS macros and disabled argument evaluation");

    file = capture(&saved_fd);
    sysinfo_log((enum sysinfo_log_level)-1, "invalid");
    sysinfo_log((enum sysinfo_log_level)4, "invalid");
    sysinfo_log(SYSINFO_LOG_ERROR, NULL);
    CHECK(finish(file, saved_fd, output, sizeof(output)) == 0);
    sysinfo_log_set_level(SYSINFO_LOG_ERROR);
    sysinfo_log_set_level((enum sysinfo_log_level)-1);
    sysinfo_log_set_level((enum sysinfo_log_level)4);
    file = capture(&saved_fd);
    sysinfo_log(SYSINFO_LOG_INFO, "must be filtered");
    CHECK(finish(file, saved_fd, output, sizeof(output)) == 0);
    puts("PASS invalid arguments and unchanged level");

    file = capture(&saved_fd);
    sysinfo_log(SYSINFO_LOG_ERROR, "value=%d text=%s\nnext\rline", 42, "hello");
    size_t length = finish(file, saved_fd, output, sizeof(output));
    if (enabled(SYSINFO_LOG_ERROR, SYSINFO_LOG_ERROR)) {
        char pid[64];
        snprintf(pid, sizeof(pid), "pid=%ld ", (long)getpid());
        CHECK(strstr(output, pid) != NULL);
        CHECK(strstr(output, "value=42 text=hello next line\n") != NULL);
        CHECK(output[length - 1] == '\n');
        CHECK(strchr(output, '\n') == output + length - 1);
        int year, month, day, hour, minute, second;
        CHECK(sscanf(output, "[ERROR] %d-%d-%dT%d:%d:%dZ", &year, &month,
                     &day, &hour, &minute, &second) == 6);
        CHECK(year >= 2020 && month >= 1 && month <= 12 && day >= 1 && day <= 31);
        CHECK(hour >= 0 && hour < 24 && minute >= 0 && minute < 60 && second >= 0 && second <= 60);
    }
    else CHECK(length == 0);
    puts("PASS formatting, UTC timestamp, PID and newline sanitization");

    char long_message[2048];
    memset(long_message, 'x', sizeof(long_message) - 1);
    long_message[sizeof(long_message) - 1] = '\0';
    file = capture(&saved_fd);
    sysinfo_log(SYSINFO_LOG_ERROR, "%s", long_message);
    length = finish(file, saved_fd, output, sizeof(output));
    if (enabled(0, 0)) {
        CHECK(length == 512);
        CHECK(strcmp(output + length - 4, "...\n") == 0);
    }
    else CHECK(length == 0);
    puts("PASS bounded truncation");

    file = capture(&saved_fd);
    pid_t children[8];
    for (int i = 0; i < 8; ++i) {
        children[i] = fork();
        CHECK(children[i] >= 0);
        if (children[i] == 0) {
            sysinfo_log(SYSINFO_LOG_ERROR, "child=%d", i);
            _exit(0);
        }
    }
    for (int i = 0; i < 8; ++i) {
        int status;
        pid_t result;
        do { result = waitpid(children[i], &status, 0); } while (result < 0 && errno == EINTR);
        CHECK(result == children[i] && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    finish(file, saved_fd, output, sizeof(output));
    int records = 0;
    for (char *line = strtok(output, "\n"); line != NULL; line = strtok(NULL, "\n")) {
        long pid;
        int index;
        CHECK(strncmp(line, "[ERROR] ", 8) == 0);
        char *fields = strstr(line, "pid=");
        CHECK(fields != NULL && sscanf(fields, "pid=%ld child=%d", &pid, &index) == 2);
        CHECK(index >= 0 && index < 8 && pid == (long)children[index]);
        children[index] = -1; /* Reject duplicate records. */
        ++records;
    }
    CHECK(records == (enabled(0, 0) ? 8 : 0));
    puts("PASS forked records and child PIDs");

    saved_fd = dup(STDERR_FILENO);
    CHECK(saved_fd >= 0);
    close(STDERR_FILENO);
    errno = EBUSY;
    sysinfo_log(SYSINFO_LOG_ERROR, "write failure");
    int preserved = errno;
    CHECK(dup2(saved_fd, STDERR_FILENO) >= 0);
    close(saved_fd);
    CHECK(preserved == EBUSY);
    puts("PASS failed write preserves errno");
    return EXIT_SUCCESS;
}
