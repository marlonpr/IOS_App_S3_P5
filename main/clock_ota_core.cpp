#include "clock_ota_core.h"

#include <atomic>
#include <ctype.h>
#include <stdint.h>
#include <string.h>

namespace {

constexpr unsigned kMaxJsonDepth = 16;

std::atomic<bool> s_ota_in_progress{false};

class JsonReader {
public:
    JsonReader(const char *json, size_t length)
        : current_(json), end_(json + length)
    {
    }

    bool parse_manifest(clock_ota_manifest_t *manifest)
    {
        if (manifest == nullptr) {
            return false;
        }

        memset(manifest, 0, sizeof(*manifest));
        skip_whitespace();

        if (!consume('{')) {
            return false;
        }

        bool found_version = false;
        bool found_url = false;

        skip_whitespace();
        if (consume('}')) {
            return false;
        }

        while (current_ < end_) {
            char key[16] = {};
            bool key_overflow = false;

            if (!parse_string(key, sizeof(key), &key_overflow)) {
                return false;
            }

            skip_whitespace();
            if (!consume(':')) {
                return false;
            }
            skip_whitespace();

            bool is_version = !key_overflow && strcmp(key, "version") == 0;
            bool is_url = !key_overflow && strcmp(key, "url") == 0;

            if (is_version || is_url) {
                if ((is_version && found_version) || (is_url && found_url)) {
                    return false;
                }

                char *output = is_version ? manifest->version : manifest->url;
                size_t output_size = is_version
                                         ? sizeof(manifest->version)
                                         : sizeof(manifest->url);
                bool value_overflow = false;

                if (!parse_string(output, output_size, &value_overflow) ||
                    value_overflow || output[0] == '\0') {
                    return false;
                }

                if (is_version) {
                    found_version = true;
                } else {
                    found_url = true;
                }
            } else if (!skip_value(0)) {
                return false;
            }

            skip_whitespace();
            if (consume('}')) {
                skip_whitespace();
                return current_ == end_ && found_version && found_url;
            }

            if (!consume(',')) {
                return false;
            }
            skip_whitespace();
        }

        return false;
    }

private:
    void skip_whitespace()
    {
        while (current_ < end_ &&
               (*current_ == ' ' || *current_ == '\t' ||
                *current_ == '\r' || *current_ == '\n')) {
            ++current_;
        }
    }

    bool consume(char expected)
    {
        if (current_ >= end_ || *current_ != expected) {
            return false;
        }

        ++current_;
        return true;
    }

    static bool append_byte(char value,
                            char *output,
                            size_t output_size,
                            size_t *written,
                            bool *overflow)
    {
        if (written == nullptr || overflow == nullptr) {
            return false;
        }

        if (output != nullptr) {
            if (*written + 1 >= output_size) {
                *overflow = true;
            } else {
                output[*written] = value;
            }
        }

        ++(*written);
        return true;
    }

    static int hex_value(char c)
    {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return 10 + (c - 'a');
        }
        if (c >= 'A' && c <= 'F') {
            return 10 + (c - 'A');
        }
        return -1;
    }

    bool parse_unicode_escape(uint32_t *code_point)
    {
        if (code_point == nullptr || end_ - current_ < 4) {
            return false;
        }

        uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            int digit = hex_value(*current_++);
            if (digit < 0) {
                return false;
            }
            value = (value << 4) | static_cast<uint32_t>(digit);
        }

        *code_point = value;
        return true;
    }

    static bool append_code_point(uint32_t code_point,
                                  char *output,
                                  size_t output_size,
                                  size_t *written,
                                  bool *overflow)
    {
        if (code_point == 0 ||
            (code_point >= 0xD800 && code_point <= 0xDFFF)) {
            if (output != nullptr) {
                *overflow = true;
            }
            return true;
        }

        if (code_point <= 0x7F) {
            return append_byte(static_cast<char>(code_point),
                               output,
                               output_size,
                               written,
                               overflow);
        }

        if (code_point <= 0x7FF) {
            return append_byte(static_cast<char>(0xC0 | (code_point >> 6)),
                               output,
                               output_size,
                               written,
                               overflow) &&
                   append_byte(static_cast<char>(0x80 | (code_point & 0x3F)),
                               output,
                               output_size,
                               written,
                               overflow);
        }

        return append_byte(static_cast<char>(0xE0 | (code_point >> 12)),
                           output,
                           output_size,
                           written,
                           overflow) &&
               append_byte(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)),
                           output,
                           output_size,
                           written,
                           overflow) &&
               append_byte(static_cast<char>(0x80 | (code_point & 0x3F)),
                           output,
                           output_size,
                           written,
                           overflow);
    }

    bool parse_string(char *output, size_t output_size, bool *overflow)
    {
        if (overflow == nullptr || !consume('"')) {
            return false;
        }

        *overflow = false;
        size_t written = 0;

        while (current_ < end_) {
            unsigned char value = static_cast<unsigned char>(*current_++);

            if (value == '"') {
                if (output != nullptr && output_size > 0) {
                    size_t terminator = written < output_size ? written : output_size - 1;
                    output[terminator] = '\0';
                }
                return true;
            }

            if (value < 0x20) {
                return false;
            }

            if (value != '\\') {
                if (!append_byte(static_cast<char>(value),
                                 output,
                                 output_size,
                                 &written,
                                 overflow)) {
                    return false;
                }
                continue;
            }

            if (current_ >= end_) {
                return false;
            }

            char escape = *current_++;
            char decoded = '\0';

            switch (escape) {
                case '"': decoded = '"'; break;
                case '\\': decoded = '\\'; break;
                case '/': decoded = '/'; break;
                case 'b': decoded = '\b'; break;
                case 'f': decoded = '\f'; break;
                case 'n': decoded = '\n'; break;
                case 'r': decoded = '\r'; break;
                case 't': decoded = '\t'; break;
                case 'u': {
                    uint32_t code_point = 0;
                    if (!parse_unicode_escape(&code_point) ||
                        !append_code_point(code_point,
                                           output,
                                           output_size,
                                           &written,
                                           overflow)) {
                        return false;
                    }
                    continue;
                }
                default:
                    return false;
            }

            if (!append_byte(decoded,
                             output,
                             output_size,
                             &written,
                             overflow)) {
                return false;
            }
        }

        return false;
    }

    bool skip_literal(const char *literal)
    {
        size_t length = strlen(literal);
        if (static_cast<size_t>(end_ - current_) < length ||
            memcmp(current_, literal, length) != 0) {
            return false;
        }

        current_ += length;
        return true;
    }

    bool skip_number()
    {
        const char *start = current_;

        if (current_ < end_ && *current_ == '-') {
            ++current_;
        }

        if (current_ >= end_) {
            return false;
        }

        if (*current_ == '0') {
            ++current_;
        } else if (*current_ >= '1' && *current_ <= '9') {
            do {
                ++current_;
            } while (current_ < end_ && isdigit(static_cast<unsigned char>(*current_)));
        } else {
            return false;
        }

        if (current_ < end_ && *current_ == '.') {
            ++current_;
            const char *fraction_start = current_;
            while (current_ < end_ && isdigit(static_cast<unsigned char>(*current_))) {
                ++current_;
            }
            if (current_ == fraction_start) {
                return false;
            }
        }

        if (current_ < end_ && (*current_ == 'e' || *current_ == 'E')) {
            ++current_;
            if (current_ < end_ && (*current_ == '+' || *current_ == '-')) {
                ++current_;
            }
            const char *exponent_start = current_;
            while (current_ < end_ && isdigit(static_cast<unsigned char>(*current_))) {
                ++current_;
            }
            if (current_ == exponent_start) {
                return false;
            }
        }

        return current_ > start;
    }

    bool skip_array(unsigned depth)
    {
        if (!consume('[')) {
            return false;
        }

        skip_whitespace();
        if (consume(']')) {
            return true;
        }

        while (current_ < end_) {
            if (!skip_value(depth + 1)) {
                return false;
            }
            skip_whitespace();
            if (consume(']')) {
                return true;
            }
            if (!consume(',')) {
                return false;
            }
            skip_whitespace();
        }

        return false;
    }

    bool skip_object(unsigned depth)
    {
        if (!consume('{')) {
            return false;
        }

        skip_whitespace();
        if (consume('}')) {
            return true;
        }

        while (current_ < end_) {
            bool ignored_overflow = false;
            if (!parse_string(nullptr, 0, &ignored_overflow)) {
                return false;
            }
            skip_whitespace();
            if (!consume(':')) {
                return false;
            }
            skip_whitespace();
            if (!skip_value(depth + 1)) {
                return false;
            }
            skip_whitespace();
            if (consume('}')) {
                return true;
            }
            if (!consume(',')) {
                return false;
            }
            skip_whitespace();
        }

        return false;
    }

    bool skip_value(unsigned depth)
    {
        if (depth > kMaxJsonDepth || current_ >= end_) {
            return false;
        }

        switch (*current_) {
            case '"': {
                bool ignored_overflow = false;
                return parse_string(nullptr, 0, &ignored_overflow);
            }
            case '{':
                return skip_object(depth);
            case '[':
                return skip_array(depth);
            case 't':
                return skip_literal("true");
            case 'f':
                return skip_literal("false");
            case 'n':
                return skip_literal("null");
            default:
                return skip_number();
        }
    }

    const char *current_;
    const char *end_;
};

}  // namespace

bool clock_ota_parse_version_json(const char *json,
                                  size_t json_length,
                                  clock_ota_manifest_t *manifest)
{
    if (json == nullptr || json_length == 0 || manifest == nullptr) {
        return false;
    }

    JsonReader reader(json, json_length);
    return reader.parse_manifest(manifest);
}

bool clock_ota_versions_equal(const char *current_version,
                              const char *available_version)
{
    return current_version != nullptr &&
           available_version != nullptr &&
           strcmp(current_version, available_version) == 0;
}

bool clock_ota_url_is_supported(const char *url)
{
    if (url == nullptr) {
        return false;
    }

    size_t url_length = strnlen(url, CLOCK_OTA_URL_MAX_LENGTH + 1);
    if (url_length == 0 || url_length > CLOCK_OTA_URL_MAX_LENGTH) {
        return false;
    }

    constexpr char kHttpPrefix[] = "http://";
    constexpr char kHttpsPrefix[] = "https://";

    if (strncmp(url, kHttpPrefix, sizeof(kHttpPrefix) - 1) == 0) {
        return url[sizeof(kHttpPrefix) - 1] != '\0';
    }

    if (strncmp(url, kHttpsPrefix, sizeof(kHttpsPrefix) - 1) == 0) {
        return url[sizeof(kHttpsPrefix) - 1] != '\0';
    }

    return false;
}

clock_ota_start_result_t clock_ota_try_reserve_start(const char *url)
{
    if (!clock_ota_url_is_supported(url)) {
        return CLOCK_OTA_START_INVALID_URL;
    }

    bool expected = false;
    if (!s_ota_in_progress.compare_exchange_strong(expected, true)) {
        return CLOCK_OTA_START_ALREADY_IN_PROGRESS;
    }

    return CLOCK_OTA_START_ACCEPTED;
}

void clock_ota_release_start(void)
{
    s_ota_in_progress.store(false);
}
