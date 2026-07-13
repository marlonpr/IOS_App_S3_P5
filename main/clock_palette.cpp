#include "clock_palette.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "CLOCK_PALETTE";

static constexpr char kPaletteNamespace[] = "palette_cfg";
static constexpr char kPaletteKeys[][3] = {"m1", "m2", "m3"};
static constexpr uint8_t kPaletteMagic0 = 'P';
static constexpr uint8_t kPaletteMagic1 = 'L';
static constexpr size_t kPaletteHeaderSize = 6;
static constexpr size_t kPaletteEntrySize = 4;
static constexpr size_t kPaletteCrcSize = 4;
static constexpr size_t kPaletteMinimumBlobSize =
    kPaletteHeaderSize + kPaletteEntrySize + kPaletteCrcSize;
static constexpr size_t kPaletteMaximumBlobSize =
    kPaletteHeaderSize +
    (CLOCK_PALETTE_MAX_ROLES * kPaletteEntrySize) +
    kPaletteCrcSize;

static const clock_mode_palette_t kFactoryPalettes[] = {
    {
        CLOCK_PALETTE_MODE_1,
        6,
        {
            {CLOCK_PALETTE_ROLE_TIME, {255, 255, 255}},
            {CLOCK_PALETTE_ROLE_DATE, {0, 255, 0}},
            {CLOCK_PALETTE_ROLE_TEMPERATURE_COLD, {255, 255, 255}},
            {CLOCK_PALETTE_ROLE_TEMPERATURE_COOL, {0, 255, 255}},
            {CLOCK_PALETTE_ROLE_TEMPERATURE_WARM, {255, 65, 0}},
            {CLOCK_PALETTE_ROLE_TEMPERATURE_HOT, {255, 0, 0}},
        },
    },
    {
        CLOCK_PALETTE_MODE_2,
        6,
        {
            {CLOCK_PALETTE_ROLE_TIME, {255, 255, 255}},
            {CLOCK_PALETTE_ROLE_DATE, {0, 0, 255}},
            {CLOCK_PALETTE_ROLE_TEMPERATURE_COLD, {255, 255, 255}},
            {CLOCK_PALETTE_ROLE_TEMPERATURE_COOL, {0, 255, 255}},
            {CLOCK_PALETTE_ROLE_TEMPERATURE_WARM, {255, 65, 0}},
            {CLOCK_PALETTE_ROLE_TEMPERATURE_HOT, {255, 0, 0}},
        },
    },
    {
        CLOCK_PALETTE_MODE_3,
        7,
        {
            {CLOCK_PALETTE_ROLE_TIME, {255, 255, 255}},
            {CLOCK_PALETTE_ROLE_DATE, {0, 0, 255}},
            {CLOCK_PALETTE_ROLE_WEEKDAY, {0, 255, 0}},
            {CLOCK_PALETTE_ROLE_TEMPERATURE_COLD, {255, 255, 255}},
            {CLOCK_PALETTE_ROLE_TEMPERATURE_COOL, {0, 255, 255}},
            {CLOCK_PALETTE_ROLE_TEMPERATURE_WARM, {255, 65, 0}},
            {CLOCK_PALETTE_ROLE_TEMPERATURE_HOT, {255, 0, 0}},
        },
    },
};

static constexpr size_t kPaletteModeCount =
    sizeof(kFactoryPalettes) / sizeof(kFactoryPalettes[0]);

static clock_mode_palette_t s_active_palettes[kPaletteModeCount] = {};
static bool s_override_present[kPaletteModeCount] = {};
static portMUX_TYPE s_active_palette_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;
static StaticSemaphore_t s_operation_mutex_storage;
static SemaphoreHandle_t s_operation_mutex = nullptr;
static bool s_initialized = false;

typedef enum {
    BLOB_VALID = 0,
    BLOB_INVALID_LENGTH,
    BLOB_INVALID_MAGIC,
    BLOB_INVALID_VERSION,
    BLOB_INVALID_MODE,
    BLOB_INVALID_COUNT,
    BLOB_INVALID_FLAGS,
    BLOB_INVALID_CRC,
    BLOB_INVALID_DUPLICATE_ROLE,
    BLOB_INVALID_ROLE,
} palette_blob_validation_t;

static bool rgb_equal(clock_rgb_t left, clock_rgb_t right)
{
    return left.r == right.r && left.g == right.g && left.b == right.b;
}

static const clock_mode_palette_t *factory_palette_for_mode(uint8_t mode)
{
    for (size_t i = 0; i < kPaletteModeCount; ++i) {
        if (kFactoryPalettes[i].mode == mode) {
            return &kFactoryPalettes[i];
        }
    }

    return nullptr;
}

static size_t palette_index_for_mode(uint8_t mode)
{
    return (size_t)(mode - CLOCK_PALETTE_MODE_1);
}

static const char *palette_key_for_mode(uint8_t mode)
{
    if (!clock_palette_is_supported_mode(mode)) {
        return nullptr;
    }

    return kPaletteKeys[palette_index_for_mode(mode)];
}

static bool ensure_initialized(void)
{
    if (s_initialized) {
        return true;
    }

    portENTER_CRITICAL(&s_init_mux);

    if (!s_initialized) {
        s_operation_mutex =
            xSemaphoreCreateMutexStatic(&s_operation_mutex_storage);

        if (s_operation_mutex != nullptr) {
            memcpy(s_active_palettes,
                   kFactoryPalettes,
                   sizeof(s_active_palettes));
            memset(s_override_present, 0, sizeof(s_override_present));
            s_initialized = true;
        }
    }

    portEXIT_CRITICAL(&s_init_mux);

    if (!s_initialized) {
        ESP_LOGE(TAG, "Failed to initialize palette mutex");
    }

    return s_initialized;
}

static bool operation_lock(void)
{
    return ensure_initialized() &&
           xSemaphoreTake(s_operation_mutex, portMAX_DELAY) == pdTRUE;
}

static void operation_unlock(void)
{
    if (s_operation_mutex != nullptr) {
        xSemaphoreGive(s_operation_mutex);
    }
}

static void replace_active_palette(const clock_mode_palette_t *palette,
                                   bool override_present)
{
    if (palette == nullptr ||
        !clock_palette_is_supported_mode(palette->mode)) {
        return;
    }

    size_t index = palette_index_for_mode(palette->mode);

    portENTER_CRITICAL(&s_active_palette_mux);
    s_active_palettes[index] = *palette;
    s_override_present[index] = override_present;
    portEXIT_CRITICAL(&s_active_palette_mux);
}

static void replace_all_active_with_defaults(void)
{
    portENTER_CRITICAL(&s_active_palette_mux);
    memcpy(s_active_palettes,
           kFactoryPalettes,
           sizeof(s_active_palettes));
    memset(s_override_present, 0, sizeof(s_override_present));
    portEXIT_CRITICAL(&s_active_palette_mux);
}

static bool get_active_palette_state(uint8_t mode,
                                     clock_mode_palette_t *out_palette,
                                     bool *out_override_present)
{
    if (out_palette == nullptr || !clock_palette_is_supported_mode(mode) ||
        !ensure_initialized()) {
        return false;
    }

    size_t index = palette_index_for_mode(mode);

    portENTER_CRITICAL(&s_active_palette_mux);
    *out_palette = s_active_palettes[index];

    if (out_override_present != nullptr) {
        *out_override_present = s_override_present[index];
    }

    portEXIT_CRITICAL(&s_active_palette_mux);
    return true;
}

static uint32_t palette_crc32(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xFFFFFFFFu;

    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];

        for (uint8_t bit = 0; bit < 8; ++bit) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }

    return crc ^ 0xFFFFFFFFu;
}

static uint32_t read_le_u32(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static void write_le_u32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
    data[2] = (uint8_t)(value >> 16);
    data[3] = (uint8_t)(value >> 24);
}

static const char *blob_validation_reason(palette_blob_validation_t result)
{
    switch (result) {
        case BLOB_INVALID_LENGTH:
            return "length";
        case BLOB_INVALID_MAGIC:
            return "magic";
        case BLOB_INVALID_VERSION:
            return "version";
        case BLOB_INVALID_MODE:
            return "mode";
        case BLOB_INVALID_COUNT:
            return "count";
        case BLOB_INVALID_FLAGS:
            return "flags";
        case BLOB_INVALID_CRC:
            return "crc";
        case BLOB_INVALID_DUPLICATE_ROLE:
            return "duplicate-role";
        case BLOB_INVALID_ROLE:
            return "role";
        case BLOB_VALID:
        default:
            return "valid";
    }
}

static palette_blob_validation_t decode_override_blob(
    const uint8_t *blob,
    size_t length,
    uint8_t expected_mode,
    clock_mode_palette_t *out_override)
{
    if (blob == nullptr || out_override == nullptr ||
        length < kPaletteMinimumBlobSize ||
        length > kPaletteMaximumBlobSize) {
        return BLOB_INVALID_LENGTH;
    }

    if (blob[0] != kPaletteMagic0 || blob[1] != kPaletteMagic1) {
        return BLOB_INVALID_MAGIC;
    }

    if (blob[2] != CLOCK_PALETTE_SCHEMA_VERSION) {
        return BLOB_INVALID_VERSION;
    }

    if (blob[3] != expected_mode ||
        !clock_palette_is_supported_mode(blob[3])) {
        return BLOB_INVALID_MODE;
    }

    const clock_mode_palette_t *factory =
        factory_palette_for_mode(expected_mode);
    uint8_t count = blob[4];

    if (factory == nullptr || count == 0 || count > factory->count) {
        return BLOB_INVALID_COUNT;
    }

    if (blob[5] != 0) {
        return BLOB_INVALID_FLAGS;
    }

    size_t expected_length =
        kPaletteHeaderSize +
        ((size_t)count * kPaletteEntrySize) +
        kPaletteCrcSize;

    if (length != expected_length) {
        return BLOB_INVALID_LENGTH;
    }

    size_t crc_offset = length - kPaletteCrcSize;
    uint32_t expected_crc = read_le_u32(&blob[crc_offset]);
    uint32_t actual_crc = palette_crc32(blob, crc_offset);

    if (actual_crc != expected_crc) {
        return BLOB_INVALID_CRC;
    }

    clock_mode_palette_t decoded = {};
    decoded.mode = expected_mode;
    decoded.count = count;

    for (uint8_t i = 0; i < count; ++i) {
        size_t offset = kPaletteHeaderSize + ((size_t)i * kPaletteEntrySize);
        uint8_t role = blob[offset];

        if (!clock_palette_is_supported_role(expected_mode, role)) {
            return BLOB_INVALID_ROLE;
        }

        for (uint8_t previous = 0; previous < i; ++previous) {
            if (decoded.entries[previous].role == role) {
                return BLOB_INVALID_DUPLICATE_ROLE;
            }
        }

        decoded.entries[i] = {
            role,
            {blob[offset + 1], blob[offset + 2], blob[offset + 3]},
        };
    }

    *out_override = decoded;
    return BLOB_VALID;
}

static size_t encode_override_blob(const clock_mode_palette_t *override_palette,
                                   uint8_t *blob,
                                   size_t capacity)
{
    if (override_palette == nullptr || blob == nullptr ||
        override_palette->count == 0 ||
        override_palette->count > CLOCK_PALETTE_MAX_ROLES) {
        return 0;
    }

    size_t length =
        kPaletteHeaderSize +
        ((size_t)override_palette->count * kPaletteEntrySize) +
        kPaletteCrcSize;

    if (capacity < length) {
        return 0;
    }

    blob[0] = kPaletteMagic0;
    blob[1] = kPaletteMagic1;
    blob[2] = CLOCK_PALETTE_SCHEMA_VERSION;
    blob[3] = override_palette->mode;
    blob[4] = override_palette->count;
    blob[5] = 0;

    for (uint8_t i = 0; i < override_palette->count; ++i) {
        size_t offset = kPaletteHeaderSize + ((size_t)i * kPaletteEntrySize);
        const clock_palette_entry_t *entry = &override_palette->entries[i];

        blob[offset] = entry->role;
        blob[offset + 1] = entry->color.r;
        blob[offset + 2] = entry->color.g;
        blob[offset + 3] = entry->color.b;
    }

    size_t crc_offset = length - kPaletteCrcSize;
    write_le_u32(&blob[crc_offset], palette_crc32(blob, crc_offset));
    return length;
}

static bool palettes_equal(const clock_mode_palette_t *left,
                           const clock_mode_palette_t *right)
{
    if (left == nullptr || right == nullptr ||
        left->mode != right->mode || left->count != right->count) {
        return false;
    }

    for (uint8_t i = 0; i < left->count; ++i) {
        if (left->entries[i].role != right->entries[i].role ||
            !rgb_equal(left->entries[i].color, right->entries[i].color)) {
            return false;
        }
    }

    return true;
}

static bool canonicalize_complete_palette(
    uint8_t mode,
    const clock_palette_entry_t *entries,
    size_t count,
    clock_mode_palette_t *out_palette)
{
    const clock_mode_palette_t *factory = factory_palette_for_mode(mode);

    if (factory == nullptr || entries == nullptr || out_palette == nullptr ||
        count != factory->count) {
        return false;
    }

    clock_mode_palette_t canonical = *factory;
    bool found[CLOCK_PALETTE_MAX_ROLES] = {};

    for (size_t input_index = 0; input_index < count; ++input_index) {
        uint8_t role = entries[input_index].role;
        bool matched = false;

        for (uint8_t role_index = 0; role_index < factory->count; ++role_index) {
            if (factory->entries[role_index].role != role) {
                continue;
            }

            if (found[role_index]) {
                return false;
            }

            canonical.entries[role_index].color = entries[input_index].color;
            found[role_index] = true;
            matched = true;
            break;
        }

        if (!matched) {
            return false;
        }
    }

    for (uint8_t i = 0; i < factory->count; ++i) {
        if (!found[i]) {
            return false;
        }
    }

    *out_palette = canonical;
    return true;
}

static void apply_override(const clock_mode_palette_t *override_palette,
                           clock_mode_palette_t *effective_palette)
{
    if (override_palette == nullptr || effective_palette == nullptr) {
        return;
    }

    for (uint8_t i = 0; i < override_palette->count; ++i) {
        const clock_palette_entry_t *override_entry =
            &override_palette->entries[i];

        for (uint8_t j = 0; j < effective_palette->count; ++j) {
            if (effective_palette->entries[j].role == override_entry->role) {
                effective_palette->entries[j].color = override_entry->color;
                break;
            }
        }
    }
}

static clock_mode_palette_t make_sparse_override(
    const clock_mode_palette_t *effective,
    const clock_mode_palette_t *factory)
{
    clock_mode_palette_t sparse = {};
    sparse.mode = effective->mode;

    for (uint8_t i = 0; i < effective->count; ++i) {
        if (!rgb_equal(effective->entries[i].color,
                       factory->entries[i].color)) {
            sparse.entries[sparse.count++] = effective->entries[i];
        }
    }

    return sparse;
}

static esp_err_t erase_mode_key(uint8_t mode)
{
    nvs_handle_t handle;
    esp_err_t result =
        nvs_open(kPaletteNamespace, NVS_READWRITE, &handle);

    if (result != ESP_OK) {
        return result;
    }

    result = nvs_erase_key(handle, palette_key_for_mode(mode));

    if (result == ESP_ERR_NVS_NOT_FOUND) {
        result = ESP_OK;
    } else if (result == ESP_OK) {
        result = nvs_commit(handle);
    }

    nvs_close(handle);
    return result;
}

bool clock_palette_is_supported_mode(uint8_t mode)
{
    return mode >= CLOCK_PALETTE_MODE_1 &&
           mode <= CLOCK_PALETTE_MODE_3;
}

bool clock_palette_is_supported_role(uint8_t mode, uint8_t role)
{
    const clock_mode_palette_t *factory = factory_palette_for_mode(mode);

    if (factory == nullptr) {
        return false;
    }

    for (uint8_t i = 0; i < factory->count; ++i) {
        if (factory->entries[i].role == role) {
            return true;
        }
    }

    return false;
}

bool clock_palette_snapshot_get_color(const clock_mode_palette_t *palette,
                                      uint8_t role,
                                      clock_rgb_t *out_color)
{
    if (palette == nullptr || out_color == nullptr ||
        palette->count > CLOCK_PALETTE_MAX_ROLES) {
        return false;
    }

    for (uint8_t i = 0; i < palette->count; ++i) {
        if (palette->entries[i].role == role) {
            *out_color = palette->entries[i].color;
            return true;
        }
    }

    return false;
}

bool clock_palette_snapshot_get_temperature_color(
    const clock_mode_palette_t *palette,
    float temp_c,
    clock_rgb_t *out_color)
{
    uint8_t role;

    if (temp_c < 10.0f) {
        role = CLOCK_PALETTE_ROLE_TEMPERATURE_COLD;
    } else if (temp_c < 20.0f) {
        role = CLOCK_PALETTE_ROLE_TEMPERATURE_COOL;
    } else if (temp_c < 30.0f) {
        role = CLOCK_PALETTE_ROLE_TEMPERATURE_WARM;
    } else {
        role = CLOCK_PALETTE_ROLE_TEMPERATURE_HOT;
    }

    return clock_palette_snapshot_get_color(palette, role, out_color);
}

bool clock_palette_get_mode_snapshot(uint8_t mode,
                                     clock_mode_palette_t *out_palette)
{
    return get_active_palette_state(mode, out_palette, nullptr);
}

bool clock_palette_get_factory_snapshot(uint8_t mode,
                                        clock_mode_palette_t *out_palette)
{
    const clock_mode_palette_t *factory = factory_palette_for_mode(mode);

    if (factory == nullptr || out_palette == nullptr) {
        return false;
    }

    *out_palette = *factory;
    return true;
}

bool clock_palette_get_color(uint8_t mode,
                             uint8_t role,
                             clock_rgb_t *out_color)
{
    clock_mode_palette_t snapshot = {};

    return clock_palette_get_mode_snapshot(mode, &snapshot) &&
           clock_palette_snapshot_get_color(&snapshot, role, out_color);
}

esp_err_t clock_palette_load_from_nvs(void)
{
    if (!operation_lock()) {
        return ESP_ERR_INVALID_STATE;
    }

    replace_all_active_with_defaults();
    ESP_LOGI(TAG, "Palette defaults initialized");

    nvs_handle_t handle;
    esp_err_t result =
        nvs_open(kPaletteNamespace, NVS_READONLY, &handle);

    if (result == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "Palette NVS namespace not found, using defaults");
        operation_unlock();
        return ESP_OK;
    }

    if (result != ESP_OK) {
        ESP_LOGW(TAG,
                 "Palette NVS open failed, using defaults: %s",
                 esp_err_to_name(result));
        operation_unlock();
        return result;
    }

    for (size_t i = 0; i < kPaletteModeCount; ++i) {
        uint8_t mode = kFactoryPalettes[i].mode;
        const char *key = palette_key_for_mode(mode);
        size_t blob_length = 0;
        esp_err_t read_result =
            nvs_get_blob(handle, key, nullptr, &blob_length);

        if (read_result == ESP_ERR_NVS_NOT_FOUND) {
            continue;
        }

        if (read_result != ESP_OK) {
            ESP_LOGW(TAG,
                     "Palette override ignored: mode=%u reason=nvs-read",
                     mode);
            continue;
        }

        if (blob_length == 0 || blob_length > kPaletteMaximumBlobSize) {
            ESP_LOGW(TAG,
                     "Palette override ignored: mode=%u reason=length",
                     mode);
            continue;
        }

        uint8_t blob[kPaletteMaximumBlobSize] = {};
        size_t read_length = blob_length;
        read_result = nvs_get_blob(handle, key, blob, &read_length);

        if (read_result != ESP_OK) {
            ESP_LOGW(TAG,
                     "Palette override ignored: mode=%u reason=nvs-read",
                     mode);
            continue;
        }

        clock_mode_palette_t override_palette = {};
        palette_blob_validation_t validation =
            decode_override_blob(blob,
                                 read_length,
                                 mode,
                                 &override_palette);

        if (validation != BLOB_VALID) {
            ESP_LOGW(TAG,
                     "Palette override ignored: mode=%u reason=%s",
                     mode,
                     blob_validation_reason(validation));
            continue;
        }

        clock_mode_palette_t effective = kFactoryPalettes[i];
        apply_override(&override_palette, &effective);
        replace_active_palette(&effective, true);

        ESP_LOGI(TAG,
                 "Palette override loaded: mode=%u count=%u",
                 mode,
                 override_palette.count);
    }

    nvs_close(handle);
    operation_unlock();
    return ESP_OK;
}

esp_err_t clock_palette_init(void)
{
    if (!ensure_initialized()) {
        return ESP_ERR_INVALID_STATE;
    }

    return clock_palette_load_from_nvs();
}

esp_err_t clock_palette_save_mode_override(
    uint8_t mode,
    const clock_palette_entry_t *entries,
    size_t count)
{
    clock_mode_palette_t submitted = {};

    if (!canonicalize_complete_palette(mode,
                                       entries,
                                       count,
                                       &submitted)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!operation_lock()) {
        return ESP_ERR_INVALID_STATE;
    }

    clock_mode_palette_t current = {};
    bool override_present = false;
    bool snapshot_ok =
        get_active_palette_state(mode, &current, &override_present);

    if (!snapshot_ok) {
        operation_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    const clock_mode_palette_t *factory = factory_palette_for_mode(mode);
    bool submitted_is_factory = palettes_equal(&submitted, factory);

    if (palettes_equal(&submitted, &current) &&
        !(submitted_is_factory && override_present)) {
        ESP_LOGI(TAG,
                 "Palette unchanged: mode=%u, NVS save skipped",
                 mode);
        operation_unlock();
        return ESP_OK;
    }

    clock_mode_palette_t sparse = make_sparse_override(&submitted, factory);
    esp_err_t result = ESP_OK;

    if (sparse.count == 0) {
        result = erase_mode_key(mode);
    } else {
        uint8_t blob[kPaletteMaximumBlobSize] = {};
        size_t blob_length =
            encode_override_blob(&sparse, blob, sizeof(blob));

        if (blob_length == 0) {
            result = ESP_ERR_INVALID_SIZE;
        } else {
            nvs_handle_t handle;
            result = nvs_open(kPaletteNamespace, NVS_READWRITE, &handle);

            if (result == ESP_OK) {
                result = nvs_set_blob(handle,
                                      palette_key_for_mode(mode),
                                      blob,
                                      blob_length);

                if (result == ESP_OK) {
                    result = nvs_commit(handle);
                }

                nvs_close(handle);
            }
        }
    }

    if (result == ESP_OK) {
        replace_active_palette(&submitted, sparse.count != 0);

        if (sparse.count == 0) {
            ESP_LOGI(TAG,
                     "Palette override erased, using defaults: mode=%u",
                     mode);
        } else {
            ESP_LOGI(TAG,
                     "Palette override saved: mode=%u count=%u",
                     mode,
                     sparse.count);
        }
    } else {
        ESP_LOGE(TAG,
                 "Palette save failed: mode=%u error=%s",
                 mode,
                 esp_err_to_name(result));
    }

    operation_unlock();
    return result;
}

esp_err_t clock_palette_restore_mode_defaults(uint8_t mode)
{
    const clock_mode_palette_t *factory = factory_palette_for_mode(mode);

    if (factory == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!operation_lock()) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = erase_mode_key(mode);

    if (result == ESP_OK) {
        replace_active_palette(factory, false);
        ESP_LOGI(TAG,
                 "Palette override erased, using defaults: mode=%u",
                 mode);
    } else {
        ESP_LOGE(TAG,
                 "Palette default restore failed: mode=%u error=%s",
                 mode,
                 esp_err_to_name(result));
    }

    operation_unlock();
    return result;
}

esp_err_t clock_palette_restore_all_defaults(void)
{
    if (!operation_lock()) {
        return ESP_ERR_INVALID_STATE;
    }

    nvs_handle_t handle;
    esp_err_t result =
        nvs_open(kPaletteNamespace, NVS_READWRITE, &handle);

    if (result == ESP_OK) {
        result = nvs_erase_all(handle);

        if (result == ESP_OK) {
            result = nvs_commit(handle);
        }

        nvs_close(handle);
    }

    if (result == ESP_OK) {
        replace_all_active_with_defaults();
        ESP_LOGI(TAG, "Palette overrides cleared");
    } else {
        ESP_LOGE(TAG,
                 "Palette override clear failed: %s",
                 esp_err_to_name(result));
    }

    operation_unlock();
    return result;
}
