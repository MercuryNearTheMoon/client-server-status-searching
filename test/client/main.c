#define main client_program_main
#include "../../client/main.c"
#undef main

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); exit(1); \
} } while (0)

int main(void){
    unsigned number;
    CHECK(parse_number("65535", 1, 65535, &number) == 0 && number == 65535);
    CHECK(parse_number("0", 1, 65535, &number) < 0);
    CHECK(parse_number("65536", 1, 65535, &number) < 0);
    CHECK(parse_number("-1", 1, 65535, &number) < 0);
    CHECK(parse_number("1x", 1, 65535, &number) < 0);
    CHECK(parse_number("", 1, 65535, &number) < 0);
    CHECK(parse_number(NULL, 1, 65535, &number) < 0);
    CHECK(parse_number("99999999999999999999999", 1, 65535, &number) < 0);
    CHECK(parse_command("hostname") == SYSINFO_CMD_HOSTNAME);
    CHECK(parse_command("HOLD") == SYSINFO_CMD_HOLD);
    CHECK(parse_command("INVALID") == 0);
    char *argv[] = {"client", "--host", "localhost", "--port", "1234", "--timeout", "5",
                    "--log-level", "debug", "HOSTNAME", "HOLD", "2", "UPTIME", NULL};
    struct options options = {0};
    CHECK(parse_args(13, argv, &options) == 0);
    CHECK(strcmp(options.host, "localhost") == 0 && options.port == 1234 && options.timeout == 5);
    CHECK(options.log_level == SYSINFO_LOG_DEBUG && options.count == 3);
    CHECK(options.requests[0].command == 1 && options.requests[1].command == 6 &&
          options.requests[1].seconds == 2 && options.requests[2].command == 3);
    puts("PASS client number bounds, command mapping and CLI parsing");
    return 0;
}
