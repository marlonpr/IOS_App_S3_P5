#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "clock_ota_core.h"

namespace {

int s_tests_run = 0;

void pass(const char *name)
{
    ++s_tests_run;
    printf("PASS: %s\n", name);
}

void test_parser_accepts_valid_json()
{
    const char json[] =
        "{\"version\":\"1.0.1\",\"url\":"
        "\"https:\\/\\/github.com/marlonpr/IOS_App_S3_P5/releases/latest/download/app-template.bin\","
        "\"metadata\":{\"stable\":true}}";
    clock_ota_manifest_t manifest = {};

    assert(clock_ota_parse_version_json(json, strlen(json), &manifest));
    assert(strcmp(manifest.version, "1.0.1") == 0);
    assert(strcmp(manifest.url,
                  "https://github.com/marlonpr/IOS_App_S3_P5/releases/latest/download/app-template.bin") == 0);
    pass("version JSON parser accepts valid JSON");
}

void test_parser_rejects_missing_version()
{
    const char json[] = "{\"url\":\"https://example.com/app.bin\"}";
    clock_ota_manifest_t manifest = {};
    assert(!clock_ota_parse_version_json(json, strlen(json), &manifest));
    pass("version JSON parser rejects missing version");
}

void test_parser_rejects_missing_url()
{
    const char json[] = "{\"version\":\"1.0.1\"}";
    clock_ota_manifest_t manifest = {};
    assert(!clock_ota_parse_version_json(json, strlen(json), &manifest));
    pass("version JSON parser rejects missing URL");
}

void test_parser_rejects_non_string_fields()
{
    const char numeric_version[] =
        "{\"version\":101,\"url\":\"https://example.com/app.bin\"}";
    const char numeric_url[] =
        "{\"version\":\"1.0.1\",\"url\":101}";
    clock_ota_manifest_t manifest = {};

    assert(!clock_ota_parse_version_json(numeric_version,
                                         strlen(numeric_version),
                                         &manifest));
    assert(!clock_ota_parse_version_json(numeric_url,
                                         strlen(numeric_url),
                                         &manifest));
    pass("version JSON parser rejects non-string version and URL");
}

void test_version_compare_same()
{
    assert(clock_ota_versions_equal("1.0.1", "1.0.1"));
    pass("version compare detects same version");
}

void test_version_compare_different()
{
    assert(!clock_ota_versions_equal("1.0.0", "1.0.1"));
    pass("version compare detects different version");
}

void test_start_rejects_null_url()
{
    assert(clock_ota_try_reserve_start(nullptr) == CLOCK_OTA_START_INVALID_URL);
    pass("OTA start rejects null URL");
}

void test_start_rejects_empty_url()
{
    assert(clock_ota_try_reserve_start("") == CLOCK_OTA_START_INVALID_URL);
    pass("OTA start rejects empty URL");
}

void test_start_rejects_unsupported_scheme()
{
    assert(clock_ota_try_reserve_start("ftp://example.com/app.bin") ==
           CLOCK_OTA_START_INVALID_URL);
    pass("OTA start rejects unsupported URL scheme");
}

void test_start_accepts_http()
{
    assert(clock_ota_try_reserve_start("http://example.com/app.bin") ==
           CLOCK_OTA_START_ACCEPTED);
    clock_ota_release_start();
    pass("OTA start accepts http://");
}

void test_start_accepts_https()
{
    assert(clock_ota_try_reserve_start("https://example.com/app.bin") ==
           CLOCK_OTA_START_ACCEPTED);
    clock_ota_release_start();
    pass("OTA start accepts https://");
}

void test_start_prevents_concurrent_ota()
{
    assert(clock_ota_try_reserve_start("https://example.com/first.bin") ==
           CLOCK_OTA_START_ACCEPTED);
    assert(clock_ota_try_reserve_start("https://example.com/second.bin") ==
           CLOCK_OTA_START_ALREADY_IN_PROGRESS);
    clock_ota_release_start();
    pass("OTA start prevents concurrent OTA starts");
}

}  // namespace

int main()
{
    test_parser_accepts_valid_json();
    test_parser_rejects_missing_version();
    test_parser_rejects_missing_url();
    test_parser_rejects_non_string_fields();
    test_version_compare_same();
    test_version_compare_different();
    test_start_rejects_null_url();
    test_start_rejects_empty_url();
    test_start_rejects_unsupported_scheme();
    test_start_accepts_http();
    test_start_accepts_https();
    test_start_prevents_concurrent_ota();

    printf("All %d OTA core tests passed\n", s_tests_run);
    return 0;
}
