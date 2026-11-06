#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/protocol.h"

static void print_bytes(const uint8_t bytes[SYSINFO_HEADER_SIZE])
{
    for (size_t i = 0; i < SYSINFO_HEADER_SIZE; ++i) {
        printf("%02X%s", (unsigned int)bytes[i],
               i + 1 == SYSINFO_HEADER_SIZE ? "\n" : " ");
    }
}

static int test_encode(const char *name, const struct sysinfo_header *header,
                       int expected_result, const uint8_t *expected)
{
    uint8_t out[SYSINFO_HEADER_SIZE];
    /* Detect bytes the encoder forgot to write. */
    memset(out, 0xA5, sizeof(out));
    uint8_t original[SYSINFO_HEADER_SIZE];
    memcpy(original, out, sizeof(out));
    int result = sysinfo_header_encode(out, header);
    int passed = result == expected_result;
    if (expected != NULL) {
        passed = passed && memcmp(out, expected, sizeof(out)) == 0;
    }
    if (expected_result == SYSINFO_ERROR) {
        passed = passed && memcmp(out, original, sizeof(out)) == 0;
    }
    printf("[%s] %s (return: %d, expected: %d)\n",
           passed ? "PASS" : "FAIL", name, result, expected_result);
    if (expected != NULL && !passed) {
        printf("  actual:   ");
        print_bytes(out);
        printf("  expected: ");
        print_bytes(expected);
    }
    return passed ? 0 : 1;
}

/* Compare fields, not struct padding. */
static int same_header(const struct sysinfo_header *a,
                       const struct sysinfo_header *b)
{
    return a->magic == b->magic && a->version == b->version &&
           a->type == b->type && a->flags == b->flags &&
           a->payload_length == b->payload_length;
}

static int test_decode(const char *name, const uint8_t *input,
                       int expected_result, const struct sysinfo_header *expected)
{
    struct sysinfo_header out;
    memset(&out, 0xA5, sizeof(out));
    unsigned char original[sizeof(out)];
    memcpy(original, &out, sizeof(out));
    uint8_t input_copy[SYSINFO_HEADER_SIZE];
    if (input != NULL)
        memcpy(input_copy, input, sizeof(input_copy));

    int result = sysinfo_header_decode(&out, input);
    int passed = result == expected_result;
    if (expected != NULL)
        passed = passed && same_header(&out, expected);
    if (expected_result == SYSINFO_ERROR)
        passed = passed && memcmp(&out, original, sizeof(out)) == 0;
    if (input != NULL)
        passed = passed && memcmp(input, input_copy, sizeof(input_copy)) == 0;

    printf("[%s] decode: %s (return: %d, expected: %d)\n",
           passed ? "PASS" : "FAIL", name, result, expected_result);
    if (!passed && expected != NULL) {
        printf("  actual: magic=%08lX version=%u type=%u flags=%u payload=%lu\n",
               (unsigned long)out.magic, (unsigned)out.version,
               (unsigned)out.type, (unsigned)out.flags,
               (unsigned long)out.payload_length);
        printf("  expected: magic=%08lX version=%u type=%u flags=%u payload=%lu\n",
               (unsigned long)expected->magic, (unsigned)expected->version,
               (unsigned)expected->type, (unsigned)expected->flags,
               (unsigned long)expected->payload_length);
    }
    return passed ? 0 : 1;
}

int main(void)
{
    const struct sysinfo_header valid = {
        .magic = SYSINFO_PROTOCOL_MAGIC,
        .version = SYSINFO_PROTOCOL_VERSION,
        .type = SYSINFO_REQUEST,
        .flags = 0,
        .payload_length = 32
    };
    const uint8_t request32[SYSINFO_HEADER_SIZE] = {
        0x53, 0x49, 0x4E, 0x46, 0x01, 0x01,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x20
    };
    const uint8_t response0[SYSINFO_HEADER_SIZE] = {
        0x53, 0x49, 0x4E, 0x46, 0x01, 0x02,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    const uint8_t error4096[SYSINFO_HEADER_SIZE] = {
        0x53, 0x49, 0x4E, 0x46, 0x01, 0x03,
        0x00, 0x00, 0x00, 0x00, 0x10, 0x00
    };
    int failures = 0;
    int total = 0;
    struct sysinfo_header h = valid;

    failures += test_encode("request, payload=32", &h, SYSINFO_OK, request32);
    ++total;
    h.type = SYSINFO_RESPONSE;
    h.payload_length = 0;
    failures += test_encode("response, payload=0", &h, SYSINFO_OK, response0);
    ++total;
    h.type = SYSINFO_ERROR_RESPONSE;
    h.payload_length = 4096;
    failures += test_encode("error response, payload=4096", &h,
                            SYSINFO_OK, error4096);
    ++total;

    /* Change only one field per invalid-header test. */
    h = valid;
    h.magic = 0;
    failures += test_encode("reject invalid magic", &h, SYSINFO_ERROR, NULL);
    ++total;
    h = valid;
    h.version = 2;
    failures += test_encode("reject unsupported version", &h, SYSINFO_ERROR, NULL);
    ++total;
    h = valid;
    h.type = 0;
    failures += test_encode("reject type=0", &h, SYSINFO_ERROR, NULL);
    ++total;
    h.type = 4;
    failures += test_encode("reject type=4", &h, SYSINFO_ERROR, NULL);
    ++total;
    h = valid;
    h.flags = 1;
    failures += test_encode("reject nonzero flags", &h, SYSINFO_ERROR, NULL);
    ++total;
    h = valid;
    h.payload_length = 4097;
    failures += test_encode("reject payload=4097", &h, SYSINFO_ERROR, NULL);
    ++total;
    h.payload_length = UINT32_MAX;
    failures += test_encode("reject payload=UINT32_MAX", &h, SYSINFO_ERROR, NULL);
    ++total;

    failures += test_encode("reject NULL header", NULL, SYSINFO_ERROR, NULL);
    ++total;
    int null_output_ok = sysinfo_header_encode(NULL, &valid) == SYSINFO_ERROR;
    printf("[%s] reject NULL output\n", null_output_ok ? "PASS" : "FAIL");
    failures += !null_output_ok;
    ++total;

    /* Independent wire fixtures prevent matching encode/decode bugs passing. */
    h = valid;
    failures += test_decode("request, payload=32", request32, SYSINFO_OK, &h);
    ++total;
    h.type = SYSINFO_RESPONSE;
    h.payload_length = 0;
    failures += test_decode("response, payload=0", response0, SYSINFO_OK, &h);
    ++total;
    h.type = SYSINFO_ERROR_RESPONSE;
    h.payload_length = 4096;
    failures += test_decode("error response, payload=4096", error4096, SYSINFO_OK, &h);
    ++total;

    const struct {
        const char *name;
        size_t offset;
        uint8_t value;
    } invalid_cases[] = {
        {"invalid magic", 0, 0},
        {"unsupported version", 4, 2},
        {"type=0", 5, 0},
        {"type=4", 5, 4},
        {"flags low byte nonzero", 7, 1},
        {"flags high byte nonzero", 6, 1},
        {"payload=4097", 11, 1},
        {"payload=65536", 9, 1}
    };
    uint8_t wire[SYSINFO_HEADER_SIZE];
    for (size_t i = 0; i < sizeof(invalid_cases) / sizeof(invalid_cases[0]); ++i) {
        /* The payload=4097 case starts from 4096; others start from zero. */
        memcpy(wire, invalid_cases[i].offset == 11 ? error4096 : response0,
               sizeof(wire));
        wire[invalid_cases[i].offset] = invalid_cases[i].value;
        failures += test_decode(invalid_cases[i].name, wire, SYSINFO_ERROR, NULL);
        ++total;
    }
    memcpy(wire, request32, sizeof(wire));
    memset(wire + 8, 0xFF, 4);
    failures += test_decode("payload=UINT32_MAX", wire, SYSINFO_ERROR, NULL);
    ++total;
    failures += test_decode("NULL input", NULL, SYSINFO_ERROR, NULL);
    ++total;
    int null_header_ok = sysinfo_header_decode(NULL, request32) == SYSINFO_ERROR;
    printf("[%s] decode: NULL output\n", null_header_ok ? "PASS" : "FAIL");
    failures += !null_header_ok;
    ++total;

    /* Byte buffers have no integer-alignment guarantee. */
    _Alignas(uint32_t) uint8_t unaligned[SYSINFO_HEADER_SIZE + 1];
    memcpy(unaligned + 1, request32, SYSINFO_HEADER_SIZE);
    failures += test_decode("unaligned input", unaligned + 1, SYSINFO_OK, &valid);
    ++total;

    /* Supplemental round trip; fixed-wire tests above remain authoritative. */
    h = valid;
    h.payload_length = 0x0102;
    struct sysinfo_header decoded = {0};
    int roundtrip_ok = sysinfo_header_encode(wire, &h) == SYSINFO_OK &&
                       sysinfo_header_decode(&decoded, wire) == SYSINFO_OK &&
                       same_header(&decoded, &h);
    printf("[%s] encode/decode round trip\n", roundtrip_ok ? "PASS" : "FAIL");
    failures += !roundtrip_ok;
    ++total;

    printf("\n%d/%d passed; %d failed\n", total - failures, total, failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
