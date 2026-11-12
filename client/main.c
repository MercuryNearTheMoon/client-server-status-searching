#define _POSIX_C_SOURCE 200809L
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <netdb.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "common.h"
#include "io.h"
#include "logger.h"
#include "protocol.h"

#define MAX_REQUESTS 64

struct request {
    unsigned command, seconds;
};

struct options {
    const char *host;
    unsigned port, timeout, count;
    enum sysinfo_log_level log_level;
    struct request requests[MAX_REQUESTS];
};

static int parse_number(const char *text, unsigned min, unsigned max, unsigned *out){
    if (text == NULL || *text == '\0') return -1;
    for (const char *p = text; *p; ++p) if (*p < '0' || *p > '9') return -1;
    errno = 0;
    char *end;
    unsigned long value = strtoul(text, &end, 10);
    if (errno || *end || value < min || value > max) return -1;
    *out = (unsigned)value;
    return 0;
}

static unsigned parse_command(const char *text){
    const char *commands[] = {"HOSTNAME", "UNAME", "UPTIME", "MEMORY", "LOADAVG", "HOLD"};
    for (unsigned i = 0; i < 6; ++i) if (strcasecmp(text, commands[i]) == 0) return i + 1;
    return 0;
}

static int parse_args(int argc, char **argv, struct options *options){
    static const struct option args[] = {
        {"host", required_argument, NULL, 'H'}, {"port", required_argument, NULL, 'p'},
        {"timeout", required_argument, NULL, 't'}, {"log-level", required_argument, NULL, 'l'},
        {"help", no_argument, NULL, 'h'}, {NULL, 0, NULL, 0}
    };
    int arg;
    while ((arg = getopt_long(argc, argv, "+H:p:t:l:h", args, NULL)) != -1) {
        switch (arg) {
        case 'H': if (*optarg == '\0') return -1; options->host = optarg; break;
        case 'p': if (parse_number(optarg, 1, 65535, &options->port)) return -1; break;
        case 't': if (parse_number(optarg, 1, 300, &options->timeout)) return -1; break;
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
    while (optind < argc) {
        if (options->count == MAX_REQUESTS) return -1;
        struct request *request = &options->requests[options->count];
        request->command = parse_command(argv[optind++]);
        if (!request->command) return -1;
        if (request->command == SYSINFO_CMD_HOLD) {
            if (optind == argc || parse_number(argv[optind++], 1, SYSINFO_MAX_HOLD_SECONDS,
                                               &request->seconds)) return -1;
        }
        ++options->count;
    }
    if (options->count == 0) {
        options->requests[0].command = SYSINFO_CMD_HOSTNAME;
        options->count = 1;
    }
    return 0;
}

static int64_t monotonic_ms(void){
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int wait_socket(int fd, short events, int64_t deadline){
    for (;;) {
        int64_t now = monotonic_ms();
        if (now < 0) return -1;
        int64_t left = deadline - now;
        if (left <= 0) { errno = ETIMEDOUT; return -1; }
        struct pollfd socket = {.fd = fd, .events = events};
        int result = poll(&socket, 1, (int)left);
        if (result < 0 && errno == EINTR) continue;
        if (result == 0) errno = ETIMEDOUT;
        return result > 0 ? 0 : -1;
    }
}

static int connect_server(const struct options *options){
    char port[6];
    snprintf(port, sizeof(port), "%u", options->port);
    struct addrinfo hints = {0}, *addresses;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int result = getaddrinfo(options->host, port, &hints, &addresses);
    if (result != 0) {
        fprintf(stderr, "resolve %s: %s\n", options->host, gai_strerror(result));
        return -1;
    }
    int64_t start = monotonic_ms();
    if (start < 0) { freeaddrinfo(addresses); return -1; }
    int64_t deadline = start + options->timeout * 1000;
    int fd = -1, saved_errno = ECONNREFUSED;
    for (struct addrinfo *address = addresses; address != NULL; address = address->ai_next) {
        fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd < 0) { saved_errno = errno; continue; }
        int flags = fcntl(fd, F_GETFL);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) goto next_address;
        result = connect(fd, address->ai_addr, address->ai_addrlen);
        if (result < 0) {
            if (errno != EINPROGRESS && errno != EINTR) goto next_address;
            if (wait_socket(fd, POLLOUT, deadline) < 0) goto next_address;
            int socket_error;
            socklen_t size = sizeof(socket_error);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &size) < 0) goto next_address;
            if (socket_error) { errno = socket_error; goto next_address; }
        }
        if (fcntl(fd, F_SETFL, flags) < 0) goto next_address;
        struct timeval timeout = {.tv_sec = options->timeout};
        if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) goto next_address;
        break;
next_address:
        saved_errno = errno;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(addresses);
    if (fd < 0) fprintf(stderr, "connect %s:%u: %s\n", options->host, options->port, strerror(saved_errno));
    return fd;
}

static int recv_until(int fd, void *buffer, size_t length, int64_t deadline){
    size_t total_read = 0;
    while (total_read < length) {
        if (wait_socket(fd, POLLIN, deadline) < 0) return -1;
        ssize_t bytes_read = recv(fd, (char *)buffer + total_read, length - total_read, MSG_DONTWAIT);
        if (bytes_read < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (bytes_read < 0) return -1;
        if (bytes_read == 0) { errno = ECONNRESET; return -1; }
        total_read += (size_t)bytes_read;
    }
    return 0;
}

static int execute_request(int fd, const struct request *request, unsigned timeout){
    uint8_t payload[SYSINFO_MAX_PAYLOAD], wire[SYSINFO_HEADER_SIZE];
    payload[0] = (uint8_t)request->command;
    size_t length = 1;
    if (request->command == SYSINFO_CMD_HOLD) {
        uint32_t seconds = htonl(request->seconds);
        memcpy(payload + 1, &seconds, sizeof(seconds));
        length = 5;
    }
    struct sysinfo_header header = {
        SYSINFO_PROTOCOL_MAGIC, SYSINFO_PROTOCOL_VERSION, SYSINFO_REQUEST, 0, (uint32_t)length
    };
    if (sysinfo_header_encode(wire, &header) != SYSINFO_OK ||
        sysinfo_send_all(fd, wire, sizeof(wire)) != SYSINFO_IO_OK ||
        sysinfo_send_all(fd, payload, length) != SYSINFO_IO_OK) {
        fprintf(stderr, "send request failed: %s\n", strerror(errno));
        return -1;
    }
    int64_t start = monotonic_ms();
    if (start < 0) return -1;
    int64_t deadline = start + timeout * 1000;
    if (recv_until(fd, wire, sizeof(wire), deadline) < 0) {
        fprintf(stderr, "receive header: %s\n", strerror(errno));
        return -1;
    }
    if (sysinfo_header_decode(&header, wire) != SYSINFO_OK ||
        (header.type != SYSINFO_RESPONSE && header.type != SYSINFO_ERROR_RESPONSE) ||
        header.payload_length == 0) {
        fprintf(stderr, "invalid response header\n");
        return -1;
    }
    if (recv_until(fd, payload, header.payload_length, deadline) < 0) {
        fprintf(stderr, "receive payload: %s\n", strerror(errno));
        return -1;
    }
    if (header.type == SYSINFO_ERROR_RESPONSE) {
        if (header.payload_length < 3) {
            fprintf(stderr, "invalid error response\n");
            return -1;
        }
        uint16_t code;
        memcpy(&code, payload, sizeof(code));
        fprintf(stderr, "server error %u: ", (unsigned)ntohs(code));
        fwrite(payload + 2, 1, header.payload_length - 2, stderr);
        fputc('\n', stderr);
        return -1;
    }
    if (fwrite(payload, 1, header.payload_length, stdout) != header.payload_length ||
        fputc('\n', stdout) == EOF || fflush(stdout) == EOF) {
        fprintf(stderr, "write output failed\n");
        return -1;
    }
    SYSINFO_DEBUG("command=%u received=%u bytes", request->command, header.payload_length);
    return 0;
}

int main(int argc, char **argv){
    struct options options = {.host = "127.0.0.1", .port = SYSINFO_DEFAULT_PORT,
                               .timeout = 35, .log_level = SYSINFO_LOG_WARN};
    int parsed = parse_args(argc, argv, &options);
    if (parsed != 0) {
        fprintf(parsed > 0 ? stdout : stderr,
                "Usage: %s [--host HOST] [--port 1..65535] [--timeout 1..300] "
                "[--log-level error|warn|info|debug] [HOSTNAME|UNAME|UPTIME|MEMORY|LOADAVG|HOLD SECONDS ...]\n",
                argv[0]);
        return parsed > 0 ? EXIT_SUCCESS : 2;
    }
    sysinfo_log_set_level(options.log_level);
    int fd = connect_server(&options);
    if (fd < 0) return EXIT_FAILURE;
    int result = EXIT_SUCCESS;
    for (unsigned i = 0; i < options.count; ++i) {
        if (execute_request(fd, &options.requests[i], options.timeout) < 0) {
            result = EXIT_FAILURE;
            break;
        }
    }
    close(fd);
    return result;
}
