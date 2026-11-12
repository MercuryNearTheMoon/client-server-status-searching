#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "common.h"
#include "io.h"
#include "logger.h"
#include "protocol.h"

#define MAX_CHILDREN 256
#define INFO_SIZE 1024

enum server_error {
    ERROR_FRAME = 1, ERROR_COMMAND = 2, ERROR_ARGUMENT = 3,
    ERROR_TIMEOUT = 4, ERROR_INTERNAL = 5, ERROR_BUSY = 6
};

struct options {
    unsigned port, ttl, timeout, max_clients;
    enum sysinfo_log_level log_level;
};

struct shared_cache {
    pthread_mutex_t mutex;
    int valid;
    struct timespec updated;
    char values[5][INFO_SIZE];
};

static volatile sig_atomic_t stopping;

static void stop_server(int signal_number){
    (void)signal_number;
    stopping = 1;
}

static int parse_number(const char *text, unsigned min, unsigned max, unsigned *out){
    if (text == NULL || *text == '\0') return -1;
    for (const char *p = text; *p; ++p) {
        if (*p < '0' || *p > '9') return -1;
    }
    errno = 0;
    char *end;
    unsigned long value = strtoul(text, &end, 10);
    if (errno || *end || value < min || value > max) return -1;
    *out = (unsigned)value;
    return 0;
}

static int parse_args(int argc, char **argv, struct options *options){
    static const struct option args[] = {
        {"port", required_argument, NULL, 'p'},
        {"cache-ttl", required_argument, NULL, 'c'},
        {"timeout", required_argument, NULL, 't'},
        {"max-clients", required_argument, NULL, 'm'},
        {"log-level", required_argument, NULL, 'l'},
        {"help", no_argument, NULL, 'h'}, {NULL, 0, NULL, 0}
    };
    int arg;
    while ((arg = getopt_long(argc, argv, "p:c:t:m:l:h", args, NULL)) != -1) {
        switch (arg) {
        case 'p': if (parse_number(optarg, 0, 65535, &options->port)) return -1; break;
        case 'c': if (parse_number(optarg, 0, 3600, &options->ttl)) return -1; break;
        case 't': if (parse_number(optarg, 1, 300, &options->timeout)) return -1; break;
        case 'm': if (parse_number(optarg, 1, MAX_CHILDREN, &options->max_clients)) return -1; break;
        case 'l': {
            const char *levels[] = {"error", "warn", "info", "debug"};
            int found = 0;
            for (int i = 0; i < 4; ++i) {
                if (strcmp(optarg, levels[i]) == 0) {
                    options->log_level = (enum sysinfo_log_level)i;
                    found = 1;
                }
            }
            if (!found) return -1;
            break;
        }
        case 'h': return 1;
        default: return -1;
        }
    }
    return optind == argc ? 0 : -1;
}

static struct shared_cache *init_cache(void){
    struct shared_cache *cache = mmap(NULL, sizeof(*cache), PROT_READ | PROT_WRITE,
                                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (cache == MAP_FAILED) return NULL;
    memset(cache, 0, sizeof(*cache));
    pthread_mutexattr_t attr;
    int result = pthread_mutexattr_init(&attr);
    if (result == 0) {
        result = pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
        if (result == 0) result = pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
        if (result == 0) result = pthread_mutex_init(&cache->mutex, &attr);
        pthread_mutexattr_destroy(&attr);
    }
    if (result != 0) {
        munmap(cache, sizeof(*cache));
        errno = result;
        return NULL;
    }
    return cache;
}

static int get_sysinfo(char values[5][INFO_SIZE]){
    struct utsname uts;
    struct sysinfo info;
    double load[3];
    if (gethostname(values[0], INFO_SIZE) < 0 || uname(&uts) < 0 ||
        sysinfo(&info) < 0 || getloadavg(load, 3) != 3) return -1;
    values[0][INFO_SIZE - 1] = '\0';
    snprintf(values[1], INFO_SIZE, "%s %s %s %s %s", uts.sysname, uts.nodename,
             uts.release, uts.version, uts.machine);
    snprintf(values[2], INFO_SIZE, "uptime_seconds=%ld", info.uptime);
    snprintf(values[3], INFO_SIZE, "total_bytes=%llu free_bytes=%llu",
             (unsigned long long)info.totalram * info.mem_unit,
             (unsigned long long)info.freeram * info.mem_unit);
    snprintf(values[4], INFO_SIZE, "load1=%.2f load5=%.2f load15=%.2f", load[0], load[1], load[2]);
    return 0;
}

static int get_cached_sysinfo(struct shared_cache *cache, unsigned ttl,
                             unsigned command, char output[INFO_SIZE]){
    int result = pthread_mutex_lock(&cache->mutex);
    if (result == EOWNERDEAD) {
        cache->valid = 0;
        result = pthread_mutex_consistent(&cache->mutex);
        if (result != 0) {
            pthread_mutex_unlock(&cache->mutex);
            return -1;
        }
    }
    else if (result != 0) return -1;

    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
        pthread_mutex_unlock(&cache->mutex);
        return -1;
    }
    double age = (double)(now.tv_sec - cache->updated.tv_sec) +
                 (double)(now.tv_nsec - cache->updated.tv_nsec) / 1000000000.0;
    if (!cache->valid || age >= ttl) {
        char values[5][INFO_SIZE] = {{0}};
        if (get_sysinfo(values) < 0 || clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
            pthread_mutex_unlock(&cache->mutex);
            return -1;
        }
        memcpy(cache->values, values, sizeof(values));
        cache->updated = now;
        cache->valid = 1;
    }
    memcpy(output, cache->values[command - 1], INFO_SIZE);
    pthread_mutex_unlock(&cache->mutex);
    return 0;
}

static int send_frame(int fd, unsigned type, const void *payload, size_t length){
    struct sysinfo_header header = {
        SYSINFO_PROTOCOL_MAGIC, SYSINFO_PROTOCOL_VERSION, (uint8_t)type, 0, (uint32_t)length
    };
    uint8_t wire[SYSINFO_HEADER_SIZE];
    if (sysinfo_header_encode(wire, &header) != SYSINFO_OK) return -1;
    if (sysinfo_send_all(fd, wire, sizeof(wire)) != SYSINFO_IO_OK) return -1;
    return sysinfo_send_all(fd, payload, length) == SYSINFO_IO_OK ? 0 : -1;
}

static void send_error(int fd, unsigned code, const char *message){
    uint8_t payload[256];
    size_t length = strlen(message);
    if (length > sizeof(payload) - 2) length = sizeof(payload) - 2;
    uint16_t wire_code = htons((uint16_t)code);
    memcpy(payload, &wire_code, 2);
    memcpy(payload + 2, message, length);
    send_frame(fd, SYSINFO_ERROR_RESPONSE, payload, length + 2);
}

static int64_t monotonic_ms(void){
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/* One absolute deadline covers both header and payload, including drip feeds. */
static int recv_until(int fd, void *buffer, size_t length, int64_t deadline){
    size_t done = 0;
    while (done < length) {
        int64_t now = monotonic_ms();
        if (now < 0) return SYSINFO_IO_ERROR;
        int64_t left = deadline - now;
        if (left <= 0) { errno = ETIMEDOUT; return SYSINFO_IO_ERROR; }
        struct pollfd socket = {.fd = fd, .events = POLLIN};
        int ready = poll(&socket, 1, (int)left);
        if (ready < 0 && errno == EINTR) continue;
        if (ready == 0) { errno = ETIMEDOUT; return SYSINFO_IO_ERROR; }
        if (ready < 0) return SYSINFO_IO_ERROR;
        ssize_t count = recv(fd, (char *)buffer + done, length - done, MSG_DONTWAIT);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (count < 0) return SYSINFO_IO_ERROR;
        if (count == 0) return done == 0 ? SYSINFO_IO_EOF : SYSINFO_IO_TRUNCATED;
        done += (size_t)count;
    }
    return SYSINFO_IO_OK;
}

static void serve_client(int fd, struct shared_cache *cache, const struct options *options){
    for (;;) {
        uint8_t wire[SYSINFO_HEADER_SIZE], payload[SYSINFO_MAX_PAYLOAD];
        struct sysinfo_header header;
        int64_t start = monotonic_ms();
        if (start < 0) return;
        int64_t deadline = start + options->timeout * 1000;
        int result = recv_until(fd, wire, sizeof(wire), deadline);
        if (result == SYSINFO_IO_EOF) return;
        if (result != SYSINFO_IO_OK) {
            int timeout = result == SYSINFO_IO_ERROR && errno == ETIMEDOUT;
            send_error(fd, timeout ? ERROR_TIMEOUT : ERROR_FRAME,
                       timeout ? "request timeout" : "incomplete header");
            return;
        }
        if (sysinfo_header_decode(&header, wire) != SYSINFO_OK || header.type != SYSINFO_REQUEST) {
            send_error(fd, ERROR_FRAME, "invalid request header");
            return;
        }
        if (header.payload_length == 0) {
            send_error(fd, ERROR_ARGUMENT, "missing command");
            continue;
        }
        result = recv_until(fd, payload, header.payload_length, deadline);
        if (result != SYSINFO_IO_OK) {
            int timeout = result == SYSINFO_IO_ERROR && errno == ETIMEDOUT;
            send_error(fd, timeout ? ERROR_TIMEOUT : ERROR_FRAME,
                       timeout ? "request timeout" : "incomplete payload");
            return;
        }
        unsigned command = payload[0];
        if (command < SYSINFO_CMD_HOSTNAME || command > SYSINFO_CMD_HOLD) {
            send_error(fd, ERROR_COMMAND, "unknown command");
            continue;
        }
        char output[INFO_SIZE];
        if (command == SYSINFO_CMD_HOLD) {
            uint32_t seconds;
            if (header.payload_length != 5) {
                send_error(fd, ERROR_ARGUMENT, "HOLD requires uint32 seconds");
                continue;
            }
            memcpy(&seconds, payload + 1, sizeof(seconds));
            seconds = ntohl(seconds);
            if (seconds == 0 || seconds > SYSINFO_MAX_HOLD_SECONDS) {
                send_error(fd, ERROR_ARGUMENT, "HOLD seconds must be 1..30");
                continue;
            }
            struct timespec delay = {.tv_sec = seconds};
            while (nanosleep(&delay, &delay) < 0 && errno == EINTR) {}
            snprintf(output, sizeof(output), "held_seconds=%u", seconds);
        }
        else {
            if (header.payload_length != 1) {
                send_error(fd, ERROR_ARGUMENT, "query takes no arguments");
                continue;
            }
            if (get_cached_sysinfo(cache, options->ttl, command, output) < 0) {
                send_error(fd, ERROR_INTERNAL, "system information unavailable");
                continue;
            }
        }
        result = send_frame(fd, SYSINFO_RESPONSE, output, strlen(output));
        SYSINFO_INFO_LOG("connection_pid=%ld command=%u status=%s elapsed_ms=%lld",
                         (long)getpid(), command, result == 0 ? "ok" : "send_error",
                         (long long)(monotonic_ms() - start));
        if (result < 0) return;
    }
}

static int create_listener(unsigned *port){
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int enabled = 1;
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons((uint16_t)*port),
                                  .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) < 0 ||
        bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 || listen(fd, 128) < 0) {
        close(fd);
        return -1;
    }
    socklen_t size = sizeof(address);
    if (getsockname(fd, (struct sockaddr *)&address, &size) < 0) { close(fd); return -1; }
    *port = ntohs(address.sin_port);
    return fd;
}

static void reap_children(pid_t children[MAX_CHILDREN], unsigned *active){
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (unsigned i = 0; i < MAX_CHILDREN; ++i) {
            if (children[i] == pid) { children[i] = 0; --*active; break; }
        }
    }
}

int main(int argc, char **argv){
    struct options options = {8080, 1, 10, 64, SYSINFO_LOG_INFO};
    int parsed = parse_args(argc, argv, &options);
    if (parsed != 0) {
        fprintf(parsed > 0 ? stdout : stderr,
                "Usage: %s [--port 0..65535] [--cache-ttl 0..3600] [--timeout 1..300] "
                "[--max-clients 1..256] [--log-level error|warn|info|debug]\n", argv[0]);
        return parsed > 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    sysinfo_log_set_level(options.log_level);
    struct shared_cache *cache = init_cache();
    if (cache == NULL) { perror("init_cache"); return EXIT_FAILURE; }
    int listener = create_listener(&options.port);
    if (listener < 0) {
        perror("listen");
        pthread_mutex_destroy(&cache->mutex);
        munmap(cache, sizeof(*cache));
        return EXIT_FAILURE;
    }
    struct sigaction action = {0};
    action.sa_handler = stop_server;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGTERM, &action, NULL) < 0 || sigaction(SIGINT, &action, NULL) < 0) {
        perror("sigaction");
        close(listener);
        pthread_mutex_destroy(&cache->mutex);
        munmap(cache, sizeof(*cache));
        return EXIT_FAILURE;
    }
    printf("READY port=%u pid=%ld\n", options.port, (long)getpid());
    fflush(stdout);
    pid_t children[MAX_CHILDREN] = {0};
    unsigned active = 0;
    int failed = 0;
    while (!stopping) {
        reap_children(children, &active);
        struct pollfd socket = {.fd = listener, .events = POLLIN};
        int ready = poll(&socket, 1, 100);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) { failed = 1; break; }
        if (ready == 0 || !(socket.revents & POLLIN)) continue;
        struct sockaddr_in peer;
        socklen_t peer_size = sizeof(peer);
        int fd = accept(listener, (struct sockaddr *)&peer, &peer_size);
        if (fd < 0) { if (errno == EINTR) continue; failed = 1; break; }
        struct timeval timeout = {.tv_sec = 1};
        if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
            close(fd); continue;
        }
        if (active >= options.max_clients) {
            send_error(fd, ERROR_BUSY, "server busy");
            close(fd); continue;
        }
        /* Block termination across fork so a child cannot inherit a swallowed stop. */
        sigset_t blocked, previous;
        sigemptyset(&blocked); sigaddset(&blocked, SIGTERM); sigaddset(&blocked, SIGINT);
        if (sigprocmask(SIG_BLOCK, &blocked, &previous) < 0) { close(fd); failed = 1; break; }
        pid_t pid = fork();
        if (pid == 0) {
            action.sa_handler = SIG_DFL;
            sigaction(SIGTERM, &action, NULL);
            sigaction(SIGINT, &action, NULL);
            sigprocmask(SIG_SETMASK, &previous, NULL);
            close(listener);
            char address[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, &peer.sin_addr, address, sizeof(address)))
                SYSINFO_INFO_LOG("connection_pid=%ld peer=%s:%u", (long)getpid(), address, ntohs(peer.sin_port));
            serve_client(fd, cache, &options);
            close(fd);
            munmap(cache, sizeof(*cache));
            _exit(0);
        }
        if (pid > 0) {
            for (unsigned i = 0; i < MAX_CHILDREN; ++i) {
                if (children[i] == 0) { children[i] = pid; ++active; break; }
            }
        }
        else send_error(fd, ERROR_INTERNAL, "fork failed");
        close(fd);
        sigprocmask(SIG_SETMASK, &previous, NULL);
    }
    close(listener);
    for (unsigned i = 0; i < MAX_CHILDREN; ++i) if (children[i]) kill(children[i], SIGTERM);
    /* SIGKILL bounds cleanup even if a child is stopped or holding the cache lock. */
    struct timespec grace = {.tv_nsec = 100000000};
    nanosleep(&grace, NULL);
    reap_children(children, &active);
    for (unsigned i = 0; i < MAX_CHILDREN; ++i) if (children[i]) kill(children[i], SIGKILL);
    while (waitpid(-1, NULL, 0) > 0 || errno == EINTR) {}
    pthread_mutex_destroy(&cache->mutex);
    munmap(cache, sizeof(*cache));
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
