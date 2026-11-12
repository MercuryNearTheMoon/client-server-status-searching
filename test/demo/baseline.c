/* Isolated teaching baseline, not a historical release or production server.
 * Localhost only, at most 8 connections, 3-second receive timeout, cleanup on stop.
 * Intentionally omits structured parse errors, early idle timeout and live reaping.
 */
#define main production_server_main
#include "../../server/main.c"
#undef main

int main(void){
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) return 1;
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = 0,
                                  .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) < 0 || listen(listener, 8) < 0)
        return 1;
    socklen_t size = sizeof(address);
    if (getsockname(listener, (struct sockaddr *)&address, &size) < 0) return 1;
    struct sigaction action = {0};
    action.sa_handler = stop_server;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    printf("READY port=%u pid=%ld\n", ntohs(address.sin_port), (long)getpid());
    fflush(stdout);
    pid_t children[8] = {0};
    unsigned count = 0;
    while (!stopping && count < 8) {
        struct pollfd socket = {.fd = listener, .events = POLLIN};
        if (poll(&socket, 1, 100) <= 0) continue;
        int fd = accept(listener, NULL, NULL);
        if (fd < 0) continue;
        struct timeval timeout = {.tv_sec = 3};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        pid_t child = fork();
        if (child == 0) {
            signal(SIGTERM, SIG_DFL);
            signal(SIGINT, SIG_DFL);
            alarm(4);
            close(listener);
            uint8_t wire[SYSINFO_HEADER_SIZE], payload[SYSINFO_MAX_PAYLOAD];
            struct sysinfo_header header;
            if (sysinfo_recv_all(fd, wire, sizeof(wire)) == SYSINFO_IO_OK &&
                sysinfo_header_decode(&header, wire) == SYSINFO_OK &&
                header.type == SYSINFO_REQUEST && header.payload_length == 1 &&
                sysinfo_recv_all(fd, payload, 1) == SYSINFO_IO_OK && payload[0] == SYSINFO_CMD_HOSTNAME) {
                char hostname[256] = {0};
                if (gethostname(hostname, sizeof(hostname) - 1) == 0)
                    send_frame(fd, SYSINFO_RESPONSE, hostname, strlen(hostname));
            }
            close(fd);
            _exit(0);
        }
        close(fd);
        if (child > 0) children[count++] = child;
        /* Deliberately no waitpid here: one bounded zombie demonstration. */
    }
    close(listener);
    for (unsigned i = 0; i < count; ++i) kill(children[i], SIGTERM);
    while (waitpid(-1, NULL, 0) > 0 || errno == EINTR) {}
    return 0;
}
