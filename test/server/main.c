#define main server_program_main
#include "../../server/main.c"
#undef main

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); exit(1); \
} } while (0)

int main(void){
    unsigned value;
    CHECK(parse_number("65535", 0, 65535, &value) == 0 && value == 65535);
    CHECK(parse_number("65536", 0, 65535, &value) < 0);
    CHECK(parse_number("-1", 0, 65535, &value) < 0);
    CHECK(parse_number("1x", 0, 65535, &value) < 0);
    CHECK(parse_number("", 0, 65535, &value) < 0);
    CHECK(parse_number("9999999999999999999999999999", 0, 65535, &value) < 0);
    struct shared_cache *cache = init_cache();
    CHECK(cache != NULL);
    char output[INFO_SIZE];
    CHECK(get_cached_sysinfo(cache, 3600, 1, output) == 0 && output[0]);
    CHECK(pthread_mutex_lock(&cache->mutex) == 0);
    strcpy(cache->values[0], "cached-marker");
    CHECK(pthread_mutex_unlock(&cache->mutex) == 0);
    CHECK(get_cached_sysinfo(cache, 3600, 1, output) == 0);
    CHECK(strcmp(output, "cached-marker") == 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        int ok = get_cached_sysinfo(cache, 3600, 1, output) == 0 &&
                 strcmp(output, "cached-marker") == 0;
        _exit(ok ? 0 : 1);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(get_cached_sysinfo(cache, 0, 1, output) == 0);
    CHECK(strcmp(output, "cached-marker") != 0);
    CHECK(pthread_mutex_lock(&cache->mutex) == 0);
    strcpy(cache->values[0], "expired-marker");
    cache->updated.tv_sec -= 5;
    CHECK(pthread_mutex_unlock(&cache->mutex) == 0);
    CHECK(get_cached_sysinfo(cache, 1, 1, output) == 0);
    CHECK(strcmp(output, "expired-marker") != 0);
    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        if (pthread_mutex_lock(&cache->mutex) != 0) _exit(1);
        strcpy(cache->values[0], "half-written");
        _exit(0); /* Intentionally die while holding the robust mutex. */
    }
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(get_cached_sysinfo(cache, 3600, 1, output) == 0);
    CHECK(strcmp(output, "half-written") != 0);
    for (unsigned command = 1; command <= 5; ++command)
        CHECK(get_cached_sysinfo(cache, 1, command, output) == 0 && output[0]);
    CHECK(pthread_mutex_destroy(&cache->mutex) == 0);
    CHECK(munmap(cache, sizeof(*cache)) == 0);
    puts("PASS CLI bounds, shared cache hit/expiry/disable, owner-death recovery and all queries");
    return 0;
}
