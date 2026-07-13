// SPDX-License-Identifier: MIT
//
// @file gdma_dma.cpp
// SPDX-FileCopyrightText: 2025 Stuart Parmenter
// @brief ESP32-S3 LCD_CAM + GDMA implementation for HUB75
//
// Uses direct LCD_CAM register access and manual GDMA setup.
// Simplified ring buffer approach with software BCM state tracking.

#include <sdkconfig.h>
#include <esp_idf_version.h>

// Only compile for ESP32-S3
#ifdef CONFIG_IDF_TARGET_ESP32S3

#include "gdma_dma.h"
#include "../../color/color_convert.h"    // For RGB565 scaling utilities
#include "../../panels/scan_patterns.h"   // For scan pattern remapping
#include "../../panels/panel_layout.h"    // For panel layout remapping
#include "../../util/drawing_profiler.h"  // For drawing profiling macros
#include <cassert>                        // NOLINT(readability-simplify-boolean-expr)
#include <cstring>
#include <algorithm>
#include <esp_log.h>
#include <esp_rom_gpio.h>
#include <esp_rom_sys.h>
#include <driver/gpio.h>
// gpio_func_sel() requires private GPIO header in ESP-IDF 5.4+
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)
#include <esp_private/gpio.h>
#endif
#include <esp_private/gdma.h>
#include <soc/gpio_sig_map.h>
#include <soc/lcd_cam_struct.h>
#include <hal/gpio_hal.h>
#include <hal/gdma_ll.h>
// Header location changed in ESP-IDF 5.0
#if (ESP_IDF_VERSION_MAJOR >= 5)
#include <esp_private/periph_ctrl.h>
#else
#include <driver/periph_ctrl.h>
#endif
#include <esp_heap_caps.h>

static const char *const TAG = "GdmaDma";

namespace hub75 {

// HUB75 16-bit word layout for LCD_CAM peripheral
// Bit layout: [--|--|OE|LAT|ADDR(5-bit)|R2|G2|B2|R1|G1|B1]
enum HUB75WordBits : uint16_t {
  // RGB data bits
  R1_BIT = 0,  // Upper half red
  G1_BIT = 1,  // Upper half green
  B1_BIT = 2,  // Upper half blue
  R2_BIT = 3,  // Lower half red
  G2_BIT = 4,  // Lower half green
  B2_BIT = 5,  // Lower half blue
  // Bits 6-10: Row address (5-bit field, shifted << 6)
  LAT_BIT = 11,  // Latch signal
  OE_BIT = 12,   // Output Enable (active low)
  // Bits 13-15: Unused
};

// Address field (not individual bits)
constexpr int ADDR_SHIFT = 6;
constexpr uint16_t ADDR_MASK = 0x1F;  // 5-bit address (0-31)

// Combined RGB masks
constexpr uint16_t RGB_UPPER_MASK = (1 << R1_BIT) | (1 << G1_BIT) | (1 << B1_BIT);
constexpr uint16_t RGB_LOWER_MASK = (1 << R2_BIT) | (1 << G2_BIT) | (1 << B2_BIT);
constexpr uint16_t RGB_MASK = RGB_UPPER_MASK | RGB_LOWER_MASK;  // 0x003F

// Bit clear masks
constexpr uint16_t OE_CLEAR_MASK =
    static_cast<uint16_t>(0xFFFFu & ~(1u << OE_BIT));

GdmaDma::GdmaDma(const Hub75Config &config)
    : PlatformDma(config),
      dma_chan_(nullptr),
      bit_depth_(HUB75_BIT_DEPTH),
      lsbMsbTransitionBit_(0),
      actual_clock_hz_(resolve_actual_clock_speed(config.output_clock_speed)),
      panel_width_(config.panel_width),
      panel_height_(config.panel_height),
      layout_rows_(config.layout_rows),
      layout_cols_(config.layout_cols),
      virtual_width_(config.panel_width * config.layout_cols),
      virtual_height_(config.panel_height * config.layout_rows),
      // Use helper function to compute DMA width (doubles for four-scan panels)
      dma_width_(
          get_effective_dma_width(config.scan_wiring, config.panel_width, config.layout_rows, config.layout_cols)),
      scan_wiring_(config.scan_wiring),
      layout_(config.layout),
      needs_scan_remap_(config.scan_wiring != Hub75ScanWiring::STANDARD_TWO_SCAN),
      needs_layout_remap_(config.layout != Hub75PanelLayout::HORIZONTAL),
      rotation_(config.rotation),
      // Use helper function to compute num_rows (halves for four-scan panels)
      num_rows_(get_effective_num_rows(config.scan_wiring, config.panel_height)),
      dma_buffers_{nullptr, nullptr},
      row_buffers_{nullptr, nullptr},
      descriptors_{nullptr, nullptr},
      front_idx_(0),
      active_idx_(0),
      descriptor_count_(0),
      basis_brightness_(config.brightness),  // Use config value (default: 128)
      intensity_(1.0f) {
  // Zero-copy architecture: DMA buffers ARE the display memory
  // Note: For four-scan panels, dma_width_ is doubled and num_rows_ is halved
  // to match the physical shift register layout
}

GdmaDma::~GdmaDma() { GdmaDma::shutdown(); }

bool GdmaDma::init() {
  ESP_LOGI(TAG, "Initializing LCD_CAM peripheral with GDMA...");
  ESP_LOGI(TAG, "Pin config: R1=%d G1=%d B1=%d R2=%d G2=%d B2=%d", config_.pins.r1, config_.pins.g1, config_.pins.b1,
           config_.pins.r2, config_.pins.g2, config_.pins.b2);
  ESP_LOGI(TAG, "Pin config: A=%d B=%d C=%d D=%d E=%d LAT=%d OE=%d CLK=%d", config_.pins.a, config_.pins.b,
           config_.pins.c, config_.pins.d, config_.pins.e, config_.pins.lat, config_.pins.oe, config_.pins.clk);

  // Enable and reset LCD_CAM peripheral
  periph_module_enable(PERIPH_LCD_CAM_MODULE);
  periph_module_reset(PERIPH_LCD_CAM_MODULE);

  // Reset LCD bus
  LCD_CAM.lcd_user.lcd_reset = 1;
  esp_rom_delay_us(1000);

  // Configure LCD clock
  configure_lcd_clock();

  // Configure LCD mode (i8080 16-bit parallel)
  configure_lcd_mode();

  // Configure GPIO routing
  configure_gpio();

  // Allocate GDMA channel
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
  // ESP-IDF 6.0+: simplified config, direction via NULL parameter
  gdma_channel_alloc_config_t dma_alloc_config = {.flags = {.isr_cache_safe = 0}};
  esp_err_t err = gdma_new_ahb_channel(&dma_alloc_config, &dma_chan_, nullptr);

#elif ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)
  // ESP-IDF 5.4 - 5.x: gdma_new_ahb_channel (2-arg)
  gdma_channel_alloc_config_t dma_alloc_config = {.sibling_chan = nullptr,
                                                  .direction = GDMA_CHANNEL_DIRECTION_TX,
                                                  .flags = {.reserve_sibling = 0, .isr_cache_safe = 0}};
  esp_err_t err = gdma_new_ahb_channel(&dma_alloc_config, &dma_chan_);

#elif ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  // ESP-IDF 5.0 - 5.3: gdma_new_channel with isr_cache_safe flag
  gdma_channel_alloc_config_t dma_alloc_config = {.sibling_chan = nullptr,
                                                  .direction = GDMA_CHANNEL_DIRECTION_TX,
                                                  .flags = {.reserve_sibling = 0, .isr_cache_safe = 0}};
  esp_err_t err = gdma_new_channel(&dma_alloc_config, &dma_chan_);

#else
  // ESP-IDF < 5.0: gdma_new_channel without isr_cache_safe
  gdma_channel_alloc_config_t dma_alloc_config = {
      .sibling_chan = nullptr, .direction = GDMA_CHANNEL_DIRECTION_TX, .flags = {.reserve_sibling = 0}};
  esp_err_t err = gdma_new_channel(&dma_alloc_config, &dma_chan_);
#endif
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to allocate GDMA channel: %s", esp_err_to_name(err));
    return false;
  }

  // Connect GDMA to LCD peripheral
  gdma_connect(dma_chan_, GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_LCD, 0));

  // Configure GDMA strategy
  // owner_check = false: Static descriptors, no dynamic ownership handshaking needed
  // auto_update_desc = false: No descriptor writeback - prevents corruption with infinite ring
  gdma_strategy_config_t strategy_config = {.owner_check = false,
                                            .auto_update_desc = false
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
                                            ,
                                            .eof_till_data_popped = false
#endif
  };
  gdma_apply_strategy(dma_chan_, &strategy_config);

  ESP_LOGI(TAG, "GDMA strategy configured: owner_check=false, auto_update_desc=false");

  // Configure GDMA transfer for SRAM (not PSRAM)
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 0)
  gdma_transfer_config_t transfer_config = {
      .max_data_burst_size = 32,  // 32 bytes for SRAM
      .access_ext_mem = false     // Not accessing external memory
  };
  gdma_config_transfer(dma_chan_, &transfer_config);
#else
  gdma_transfer_ability_t ability = {
      .sram_trans_align = 32,
      .psram_trans_align = 64,
  };
  gdma_set_transfer_ability(dma_chan_, &ability);
#endif

  // Wait for any pending LCD operations
  while (LCD_CAM.lcd_user.lcd_start)
    ;

  // Post-init cleanup for clean state
  gdma_reset(dma_chan_);
  esp_rom_delay_us(1000);
  LCD_CAM.lcd_user.lcd_dout = 1;         // Enable data out
  LCD_CAM.lcd_user.lcd_update = 1;       // Update registers
  LCD_CAM.lcd_misc.lcd_afifo_reset = 1;  // Reset LCD TX FIFO

  // Note: No EOF callback needed with descriptor-chain approach
  // The descriptor chain encodes all timing via repetition counts

  ESP_LOGI(TAG, "GDMA EOF callback registered successfully");
  ESP_LOGI(TAG, "Panel config: %dx%d pixels, %dx%d layout, virtual: %dx%d", panel_width_, panel_height_, layout_cols_,
           layout_rows_, virtual_width_, virtual_height_);
  ESP_LOGI(TAG, "DMA config: %dx%d (width x rows), four-scan: %s", dma_width_, num_rows_,
           is_four_scan_wiring(scan_wiring_) ? "yes" : "no");

  ESP_LOGI(TAG, "LCD_CAM + GDMA initialized successfully");
  ESP_LOGI(TAG, "Clock: %.2f MHz (requested %u MHz)", actual_clock_hz_ / 1000000.0f,
           (unsigned int) (static_cast<uint32_t>(config_.output_clock_speed) / 1000000));

  // Calculate BCM timing (determines lsbMsbTransitionBit for OE control)
  calculate_bcm_timings();

  // Adjust LUT for BCM monotonicity (only needed when lsbMsbTransitionBit > 0)
  // With transition=0, BCM weights are always monotonically non-decreasing
#if HUB75_GAMMA_MODE == 1 || HUB75_GAMMA_MODE == 2
  if (lsbMsbTransitionBit_ > 0) {
    int adjusted = adjust_lut_for_bcm(lut_, bit_depth_, lsbMsbTransitionBit_);
    ESP_LOGI(TAG, "Adjusted %d LUT entries for BCM monotonicity (lsbMsbTransitionBit=%d)", adjusted,
             lsbMsbTransitionBit_);
  }
#endif

  // Validate brightness OE configuration safety margins
  if (!validate_brightness_config()) {
    return false;
  }

  // Allocate per-row bit-plane buffers
  if (!allocate_row_buffers()) {
    return false;
  }
  
  
  
  
  
  pad_words_ = (uint16_t *)heap_caps_calloc(
      num_rows_ * QUIET_PAD_WORDS,
      sizeof(uint16_t),
      MALLOC_CAP_DMA);

  if (!pad_words_) {
    ESP_LOGE(TAG,
             "Failed to allocate quiet pad buffers: rows=%d pad_words=%d",
             num_rows_,
             QUIET_PAD_WORDS);
    return false;
  }

  initialize_pad_buffers();
  
  

  // Initialize buffers with blank pixels (control bits only, RGB=0)
  initialize_blank_buffers();

  // Initialize brightness remapping coefficients (quadratic curve)
  init_brightness_coeffs(dma_width_, config_.latch_blanking);

  // Set OE bits for BCM control and brightness
  set_brightness_oe();

  // Build descriptor chain (one descriptor per bit plane)
  if (!build_descriptor_chain()) {
    return false;
  }

  ESP_LOGI(TAG, "Descriptor-chain DMA setup complete");
  return true;
}

HUB75_CONST uint32_t GdmaDma::resolve_actual_clock_speed(Hub75ClockSpeed clock_speed) const {
  // ESP32-S3 LCD_CAM clock derivation:
  //   Output = PLL_F160M / lcd_clkm_div_num
  //   Constraint: lcd_clkm_div_num >= 2
  //
  // We use integer dividers only - no fractional dividers. Fractional dividers
  // cause clock jitter because the hardware alternates between two integer
  // dividers to approximate the fractional value. With pure integer division
  // from the stable 160 MHz PLL, every clock cycle is identical.
  //
  // The resulting frequencies may not be round numbers (e.g., 160/7 = 22.86 MHz),
  // but this is fine - what matters for signal integrity is that each clock
  // period is exactly the same, not that the frequency is a nice decimal.
  //
  // Available speeds: 32 MHz (div=5), 26.67 MHz (div=6), 22.86 MHz (div=7),
  //                   20 MHz (div=8), 17.78 MHz (div=9), 16 MHz (div=10), ...
  uint32_t requested_hz = static_cast<uint32_t>(clock_speed);
  uint32_t divider = (160000000 + requested_hz / 2) / requested_hz;  // Round to nearest
  return 160000000 / std::max(divider, uint32_t{2});
}

void GdmaDma::configure_lcd_clock() {
  // Configure LCD clock from PLL_F160M (160 MHz)
  // actual_clock_hz_ already resolved in constructor
  uint32_t requested_hz = static_cast<uint32_t>(config_.output_clock_speed);
  uint32_t div_num = 160000000 / actual_clock_hz_;

  if (actual_clock_hz_ != requested_hz) {
    ESP_LOGI(TAG, "Clock speed %u Hz rounded to %u Hz (160MHz / %u)", (unsigned int) requested_hz,
             (unsigned int) actual_clock_hz_, (unsigned int) div_num);
  }

  LCD_CAM.lcd_clock.lcd_clk_sel = 3;      // PLL_F160M_CLK (value 3, not 2!)
  LCD_CAM.lcd_clock.lcd_ck_out_edge = 0;  // PCLK low in 1st half cycle
  LCD_CAM.lcd_clock.lcd_ck_idle_edge = config_.clk_phase_inverted ? 1 : 0;
  LCD_CAM.lcd_clock.lcd_clkcnt_n = 1;        // Should never be zero
  LCD_CAM.lcd_clock.lcd_clk_equ_sysclk = 1;  // PCLK = CLK / 1 (simple divisor)
  LCD_CAM.lcd_clock.lcd_clkm_div_num = div_num;
  LCD_CAM.lcd_clock.lcd_clkm_div_a = 1;  // Fractional divider (0/1)
  LCD_CAM.lcd_clock.lcd_clkm_div_b = 0;

  ESP_LOGI(TAG, "LCD clock: PLL_F160M / %u = %.2f MHz", (unsigned int) div_num, actual_clock_hz_ / 1000000.0f);
}

void GdmaDma::configure_lcd_mode() {
  // Configure LCD in i8080 mode, 16-bit parallel, continuous output
  LCD_CAM.lcd_ctrl.lcd_rgb_mode_en = 0;     // i8080 mode (not RGB)
  LCD_CAM.lcd_rgb_yuv.lcd_conv_bypass = 0;  // Disable RGB/YUV converter
  LCD_CAM.lcd_misc.lcd_next_frame_en = 0;   // Do NOT auto-frame
  LCD_CAM.lcd_misc.lcd_bk_en = 1;           // Enable blanking
  LCD_CAM.lcd_misc.lcd_vfk_cyclelen = 0;
  LCD_CAM.lcd_misc.lcd_vbk_cyclelen = 0;

  LCD_CAM.lcd_data_dout_mode.val = 0;      // No data delays
  LCD_CAM.lcd_user.lcd_always_out_en = 1;  // Enable 'always out' mode for arbitrary-length transfers
  LCD_CAM.lcd_user.lcd_8bits_order = 0;    // Do not swap bytes
  LCD_CAM.lcd_user.lcd_bit_order = 0;      // Do not reverse bit order
  LCD_CAM.lcd_user.lcd_2byte_en = 1;       // 16-bit mode
  LCD_CAM.lcd_user.lcd_dout = 1;           // Enable data output

  // CRITICAL: Dummy phases required for DMA to trigger reliably
  LCD_CAM.lcd_user.lcd_dummy = 1;           // Dummy phase(s) @ LCD start
  LCD_CAM.lcd_user.lcd_dummy_cyclelen = 1;  // 1+1 dummy phase
  LCD_CAM.lcd_user.lcd_cmd = 0;             // No command at LCD start

  // Disable start signal
  LCD_CAM.lcd_user.lcd_start = 0;
}

void GdmaDma::configure_gpio() {
  // 16-bit data pins mapping
  int data_pins[16] = {
      config_.pins.r1,   // D0
      config_.pins.g1,   // D1
      config_.pins.b1,   // D2
      config_.pins.r2,   // D3
      config_.pins.g2,   // D4
      config_.pins.b2,   // D5
      config_.pins.a,    // D6
      config_.pins.b,    // D7
      config_.pins.c,    // D8
      config_.pins.d,    // D9
      config_.pins.e,    // D10
      config_.pins.lat,  // D11
      config_.pins.oe,   // D12
      -1,
      -1,
      -1  // D13-D15 unused
  };

  // Configure data pins
  for (int i = 0; i < 16; i++) {
    if (data_pins[i] >= 0) {
      esp_rom_gpio_connect_out_signal(data_pins[i], LCD_DATA_OUT0_IDX + i, false, false);
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)
      gpio_func_sel((gpio_num_t) data_pins[i], PIN_FUNC_GPIO);
#else
      gpio_hal_iomux_func_sel(GPIO_PIN_MUX_REG[data_pins[i]], PIN_FUNC_GPIO);
#endif
      gpio_set_drive_capability((gpio_num_t) data_pins[i], GPIO_DRIVE_CAP_3);  // Max drive strength
    }
  }

  // Configure WR (clock) pin
  if (config_.pins.clk >= 0) {
    esp_rom_gpio_connect_out_signal(config_.pins.clk, LCD_PCLK_IDX, config_.clk_phase_inverted, false);
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)
    gpio_func_sel((gpio_num_t) config_.pins.clk, PIN_FUNC_GPIO);
#else
    gpio_hal_iomux_func_sel(GPIO_PIN_MUX_REG[config_.pins.clk], PIN_FUNC_GPIO);
#endif
    gpio_set_drive_capability((gpio_num_t) config_.pins.clk, GPIO_DRIVE_CAP_3);  // Max drive strength
  }

  ESP_LOGD(TAG, "GPIO routing configured");
}

bool GdmaDma::allocate_row_buffers() {
  size_t pixels_per_bitplane = dma_width_;  // DMA buffer width (all panels chained horizontally)
  size_t buffer_size_per_row = pixels_per_bitplane * bit_depth_ * 2;  // uint16_t = 2 bytes
  size_t total_buffer_size = num_rows_ * buffer_size_per_row;

  // Always allocate first buffer (buffer A, index 0)
  ESP_LOGI(TAG, "Allocating buffer A: %zu bytes for %d rows", total_buffer_size, num_rows_);
  dma_buffers_[0] = (uint8_t *) heap_caps_calloc(1, total_buffer_size, MALLOC_CAP_DMA);
  if (!dma_buffers_[0]) {
    ESP_LOGE(TAG, "Failed to allocate %zu bytes for buffer A", total_buffer_size);
    return false;
  }

  // Allocate metadata array for buffer A
  row_buffers_[0] = new RowBitPlaneBuffer[num_rows_];

  // Point each row's metadata into the single allocation
  uint8_t *current_ptr = dma_buffers_[0];
  for (int row = 0; row < num_rows_; row++) {
    row_buffers_[0][row].buffer_size = buffer_size_per_row;
    row_buffers_[0][row].data = current_ptr;
    current_ptr += buffer_size_per_row;
  }

  // Set indices for single-buffer mode (both point to buffer 0)
  front_idx_ = 0;
  active_idx_ = 0;

  ESP_LOGI(TAG, "Buffer A allocated: %d rows × %zu bytes/row = %zu total", num_rows_, buffer_size_per_row,
           total_buffer_size);

  // Conditionally allocate second buffer (buffer B, index 1)
  if (config_.double_buffer) {
    ESP_LOGI(TAG, "Allocating buffer B: %zu bytes (double buffering enabled)", total_buffer_size);
    dma_buffers_[1] = (uint8_t *) heap_caps_calloc(1, total_buffer_size, MALLOC_CAP_DMA);
    if (!dma_buffers_[1]) {
      ESP_LOGE(TAG, "Failed to allocate %zu bytes for buffer B", total_buffer_size);
      // Continue in single-buffer mode
      ESP_LOGW(TAG, "Continuing in single-buffer mode");
      return true;
    }

    // Allocate metadata array for buffer B
    row_buffers_[1] = new RowBitPlaneBuffer[num_rows_];

    // Point each row's metadata into the single allocation
    current_ptr = dma_buffers_[1];
    for (int row = 0; row < num_rows_; row++) {
      row_buffers_[1][row].buffer_size = buffer_size_per_row;
      row_buffers_[1][row].data = current_ptr;
      current_ptr += buffer_size_per_row;
    }

    // Set indices for double-buffer mode (front=0, active=1)
    active_idx_ = 1;

    ESP_LOGI(TAG, "Buffer B allocated: %d rows × %zu bytes/row = %zu total (double buffer mode)", num_rows_,
             buffer_size_per_row, total_buffer_size);
  }

  return true;
}

void GdmaDma::start_transfer() {
  if (!dma_chan_ || !descriptors_[front_idx_]) {
    ESP_LOGE(TAG, "DMA channel or descriptors not initialized");
    return;
  }

  ESP_LOGI(TAG, "Starting descriptor-chain DMA:");
  ESP_LOGI(TAG, "  Descriptor count: %zu", descriptor_count_);
  ESP_LOGI(TAG, "  Rows: %d, Bits: %d", num_rows_, bit_depth_);

  // Prime LCD registers
  LCD_CAM.lcd_user.lcd_update = 1;
  esp_rom_delay_us(10);

  // Start GDMA transfer from first descriptor in chain (front buffer)
  gdma_start(dma_chan_, (intptr_t) &descriptors_[front_idx_][0]);

  // Delay before starting LCD
  esp_rom_delay_us(100);

  // Start LCD engine (will run continuously via descriptor loop)
  LCD_CAM.lcd_user.lcd_start = 1;

  ESP_LOGI(TAG, "Descriptor-chain DMA transfer started - running continuously");
}

void GdmaDma::stop_transfer() {
  if (!dma_chan_) {
    return;
  }

  // Disable LCD output
  LCD_CAM.lcd_user.lcd_start = 0;
  LCD_CAM.lcd_user.lcd_update = 1;  // Apply the stop command

  gdma_stop(dma_chan_);

  ESP_LOGI(TAG, "DMA transfer stopped");
}

// No EOF callback needed - descriptor chain handles all timing

void GdmaDma::shutdown() {
  GdmaDma::stop_transfer();

  if (dma_chan_) {
    gdma_disconnect(dma_chan_);
    gdma_del_channel(dma_chan_);
    dma_chan_ = nullptr;
  }

  // Free all allocated resources (using array structure)
  for (int i = 0; i < 2; i++) {
    // Free descriptor chains
    if (descriptors_[i]) {
      heap_caps_free(descriptors_[i]);
      descriptors_[i] = nullptr;
    }

    // Free raw DMA buffers (single allocation per buffer)
    if (dma_buffers_[i]) {
      heap_caps_free(dma_buffers_[i]);
      dma_buffers_[i] = nullptr;
    }

    // Free metadata arrays
    if (row_buffers_[i]) {
      delete[] row_buffers_[i];
      row_buffers_[i] = nullptr;
    }
  }

  descriptor_count_ = 0;
  
  if (pad_words_) {
    heap_caps_free(pad_words_);
    pad_words_ = nullptr;
  }
  
  
  

  periph_module_disable(PERIPH_LCD_CAM_MODULE);

  ESP_LOGI(TAG, "Shutdown complete");
}




uint16_t *GdmaDma::pad_row_ptr(int row) {
  return &pad_words_[row * QUIET_PAD_WORDS];
}


void GdmaDma::initialize_pad_buffers() {
  if (!pad_words_) {
    return;
  }

  for (int row = 0; row < num_rows_; row++) {
    const uint16_t row_addr = row & ADDR_MASK;
    uint16_t *pad = pad_row_ptr(row);

    for (int x = 0; x < QUIET_PAD_WORDS; x++) {
      pad[x] = static_cast<uint16_t>(
          (row_addr << ADDR_SHIFT) | (1u << OE_BIT));
    }
  }
}


void GdmaDma::update_quiet_pad_oe(uint8_t brightness) {
  if (!pad_words_) {
    return;
  }

  const int usable = QUIET_PAD_WORDS - PAD_GUARD_HEAD - PAD_GUARD_TAIL;

  if (usable <= 0) {
    ESP_LOGE(TAG,
             "Invalid quiet pad: pad_words=%d head=%d tail=%d",
             QUIET_PAD_WORDS,
             PAD_GUARD_HEAD,
             PAD_GUARD_TAIL);
    return;
  }

  // Use the same brightness remap as the original driver.
  const int effective_brightness = remap_brightness(brightness);

  int display = 0;

  if (brightness > 0 && effective_brightness > 0) {
    display = (usable * effective_brightness) >> 8;

    if (display == 0) {
      display = 1;
    }
  }

  if (display > usable) {
    display = usable;
  }

  const int x_min = PAD_GUARD_HEAD + ((usable - display) / 2);
  const int x_max = x_min + display;

  for (int row = 0; row < num_rows_; row++) {
    uint16_t *pad = pad_row_ptr(row);

    for (int x = 0; x < QUIET_PAD_WORDS; x++) {
      if (x >= x_min && x < x_max) {
        pad[x] &= OE_CLEAR_MASK;  // OE low = visible
      } else {
        pad[x] |= static_cast<uint16_t>(1u << OE_BIT);  // OE high = blank
      }
    }
  }

  ESP_LOGI(TAG,
           "Quiet-pad OE: brightness=%u effective=%d display=%d/%d window=%d..%d pad_words=%d",
           (unsigned)brightness,
           effective_brightness,
           display,
           usable,
           x_min,
           x_max,
           QUIET_PAD_WORDS);
}

// ============================================================================
// Brightness Control (Override Base Class)
// ============================================================================

void GdmaDma::set_basis_brightness(uint8_t brightness) {
  basis_brightness_ = brightness;

  if (brightness == 0) {
    ESP_LOGI(TAG, "Brightness set to 0 (display off)");
  } else {
    ESP_LOGD(TAG, "Basis brightness set to %u", (unsigned) brightness);
  }

  // Apply brightness change immediately by updating OE bits in DMA buffers
  set_brightness_oe();
}

void GdmaDma::set_intensity(float intensity) {
  // Clamp to valid range (0.0-1.0)
  if (intensity < 0.0f) {
    intensity = 0.0f;
  } else if (intensity > 1.0f) {
    intensity = 1.0f;
  }

  intensity_ = intensity;

  ESP_LOGD(TAG, "Intensity set to %.2f", intensity);

  // Apply intensity change immediately by updating OE bits in DMA buffers
  set_brightness_oe();
}

void GdmaDma::set_rotation(Hub75Rotation rotation) { rotation_ = rotation; }

// ============================================================================
// Pixel API (Direct DMA Buffer Writes)
// ============================================================================

HUB75_IRAM void GdmaDma::draw_pixels(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint8_t *buffer,
                                     Hub75PixelFormat format, Hub75ColorOrder color_order, bool big_endian) {
  // Always write to active buffer (CPU drawing buffer)
  RowBitPlaneBuffer *target_buffers = row_buffers_[active_idx_];

  if (!target_buffers || !buffer) [[unlikely]] {
    return;
  }

  // Calculate rotated dimensions (user-facing coordinates)
  const uint16_t rotated_width = RotationTransform::get_rotated_width(virtual_width_, virtual_height_, rotation_);
  const uint16_t rotated_height = RotationTransform::get_rotated_height(virtual_width_, virtual_height_, rotation_);

  // Bounds check against rotated (user-facing) display size
  if (x >= rotated_width || y >= rotated_height) [[unlikely]] {
    return;
  }

  // Clip to display bounds
  if (x + w > rotated_width) [[unlikely]] {
    w = rotated_width - x;
  }
  if (y + h > rotated_height) [[unlikely]] {
    h = rotated_height - y;
  }

  // Pre-compute pixel stride for pointer arithmetic (avoids multiply per pixel)
  const size_t pixel_stride = (format == Hub75PixelFormat::RGB888)   ? 3
                              : (format == Hub75PixelFormat::RGB565) ? 2
                                                                     : /* RGB888_32 */ 4;

  // Check if we can use identity fast path (no coordinate transforms needed)
  const bool identity_transform = (rotation_ == Hub75Rotation::ROTATE_0) && !needs_layout_remap_ && !needs_scan_remap_;

  // Pre-compute bit plane stride (bytes between bit planes)
  const size_t bit_plane_stride = dma_width_ * 2;

  // Process each pixel
  const uint8_t *pixel_ptr = buffer;
  for (uint16_t dy = 0; dy < h; dy++) {
    for (uint16_t dx = 0; dx < w; dx++) {
      uint16_t px = x + dx;
      uint16_t py = y + dy;
      uint16_t row;
      bool is_lower;

      HUB75_PROFILE_BEGIN();

      // Fast path: identity transform (no rotation, standard layout, standard scan)
      if (identity_transform) {
        // Simple row/half calculation without modulo (subtraction is cheaper)
        if (py < num_rows_) {
          row = py;
          is_lower = false;
        } else {
          row = py - num_rows_;
          is_lower = true;
        }
      } else {
        // Full coordinate transformation pipeline
        auto transformed = transform_coordinate(px, py, rotation_, needs_layout_remap_, needs_scan_remap_, layout_,
                                                scan_wiring_, panel_width_, panel_height_, layout_rows_, layout_cols_,
                                                virtual_width_, virtual_height_, dma_width_, num_rows_);
        px = transformed.x;
        row = transformed.row;
        is_lower = transformed.is_lower;
      }

      HUB75_PROFILE_STAGE(PROFILE_TRANSFORM);

      // Extract RGB888 from pixel format (always_inline will inline the switch)
      uint8_t r8 = 0, g8 = 0, b8 = 0;
      extract_rgb888_from_format(pixel_ptr, 0, format, color_order, big_endian, r8, g8, b8);
      pixel_ptr += pixel_stride;

      HUB75_PROFILE_STAGE(PROFILE_EXTRACT);

      // Apply LUT correction
      const uint16_t r_corrected = lut_[r8];
      const uint16_t g_corrected = lut_[g8];
      const uint16_t b_corrected = lut_[b8];

      HUB75_PROFILE_STAGE(PROFILE_LUT);

      // Branchless bit-plane update using shift+and (avoids ternary branches on Xtensa)
      uint8_t *base_ptr = target_buffers[row].data;
      for (int bit = 0; bit < bit_depth_; bit++) {
        uint16_t *buf = (uint16_t *) (base_ptr + (bit * bit_plane_stride));

        // Extract single bits (0 or 1) without branches using shift+and
        const uint16_t r_bit = (r_corrected >> bit) & 1;
        const uint16_t g_bit = (g_corrected >> bit) & 1;
        const uint16_t b_bit = (b_corrected >> bit) & 1;

        uint16_t word = buf[px];
        if (is_lower) {
          word = (word & ~RGB_LOWER_MASK) | (r_bit << R2_BIT) | (g_bit << G2_BIT) | (b_bit << B2_BIT);
        } else {
          word = (word & ~RGB_UPPER_MASK) | (r_bit << R1_BIT) | (g_bit << G1_BIT) | (b_bit << B1_BIT);
        }
        buf[px] = word;
      }

      HUB75_PROFILE_STAGE(PROFILE_BITPLANE);
      HUB75_PROFILE_PIXEL();
    }
  }
}

void GdmaDma::clear() {
  // Always write to active buffer (CPU drawing buffer)
  RowBitPlaneBuffer *target_buffers = row_buffers_[active_idx_];

  if (!target_buffers) {
    return;
  }

  // Clear RGB bits in all buffers (keep control bits)
  for (int row = 0; row < num_rows_; row++) {
    for (int bit = 0; bit < bit_depth_; bit++) {
      uint16_t *buf = (uint16_t *) (target_buffers[row].data + (bit * dma_width_ * 2));

      for (uint16_t x = 0; x < dma_width_; x++) {
        // Clear RGB bits but preserve row address, LAT, OE
        buf[x] &= ~RGB_MASK;
      }
    }
  }
}

HUB75_IRAM void GdmaDma::fill(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t r, uint8_t g, uint8_t b) {
  // Always write to active buffer (CPU drawing buffer)
  RowBitPlaneBuffer *target_buffers = row_buffers_[active_idx_];

  if (!target_buffers) [[unlikely]] {
    return;
  }

  // Calculate rotated dimensions (user-facing coordinates)
  const uint16_t rotated_width = RotationTransform::get_rotated_width(virtual_width_, virtual_height_, rotation_);
  const uint16_t rotated_height = RotationTransform::get_rotated_height(virtual_width_, virtual_height_, rotation_);

  // Bounds check against rotated (user-facing) display size
  if (x >= rotated_width || y >= rotated_height) [[unlikely]] {
    return;
  }

  // Clip to display bounds
  if (x + w > rotated_width) [[unlikely]] {
    w = rotated_width - x;
  }
  if (y + h > rotated_height) [[unlikely]] {
    h = rotated_height - y;
  }

  // Pre-compute LUT-corrected color values (ONCE for entire fill)
  const uint16_t r_corrected = lut_[r];
  const uint16_t g_corrected = lut_[g];
  const uint16_t b_corrected = lut_[b];

  // Pre-compute bit patterns for all bit planes (ONCE for entire fill)
  // This eliminates per-pixel bit extraction and conditional logic
  uint16_t upper_patterns[HUB75_BIT_DEPTH];
  uint16_t lower_patterns[HUB75_BIT_DEPTH];
  for (int bit = 0; bit < bit_depth_; bit++) {
    const uint16_t mask = (1 << bit);
    upper_patterns[bit] = ((r_corrected & mask) ? (1 << R1_BIT) : 0) | ((g_corrected & mask) ? (1 << G1_BIT) : 0) |
                          ((b_corrected & mask) ? (1 << B1_BIT) : 0);
    lower_patterns[bit] = ((r_corrected & mask) ? (1 << R2_BIT) : 0) | ((g_corrected & mask) ? (1 << G2_BIT) : 0) |
                          ((b_corrected & mask) ? (1 << B2_BIT) : 0);
  }

  // Pre-compute values for inner loop
  const size_t bit_plane_stride = dma_width_ * 2;
  const bool identity_transform = (rotation_ == Hub75Rotation::ROTATE_0) && !needs_layout_remap_ && !needs_scan_remap_;

  // Fill loop
  for (uint16_t dy = 0; dy < h; dy++) {
    for (uint16_t dx = 0; dx < w; dx++) {
      uint16_t px = x + dx;
      uint16_t py = y + dy;
      uint16_t row;
      bool is_lower;

      // Fast path: identity transform (no rotation, standard layout, standard scan)
      if (identity_transform) {
        if (py < num_rows_) {
          row = py;
          is_lower = false;
        } else {
          row = py - num_rows_;
          is_lower = true;
        }
      } else {
        // Full coordinate transformation pipeline
        auto transformed = transform_coordinate(px, py, rotation_, needs_layout_remap_, needs_scan_remap_, layout_,
                                                scan_wiring_, panel_width_, panel_height_, layout_rows_, layout_cols_,
                                                virtual_width_, virtual_height_, dma_width_, num_rows_);
        px = transformed.x;
        row = transformed.row;
        is_lower = transformed.is_lower;
      }

      // Update all bit planes using pre-computed patterns
      uint8_t *base_ptr = target_buffers[row].data;
      for (int bit = 0; bit < bit_depth_; bit++) {
        uint16_t *buf = (uint16_t *) (base_ptr + (bit * bit_plane_stride));
        uint16_t word = buf[px];  // Read existing word (preserves control bits)

        if (is_lower) {
          word = (word & ~RGB_LOWER_MASK) | lower_patterns[bit];
        } else {
          word = (word & ~RGB_UPPER_MASK) | upper_patterns[bit];
        }

        buf[px] = word;
      }
    }
  }
}

void GdmaDma::flip_buffer() {
  // Single buffer mode: no-op (both indices point to buffer 0)
  if (!row_buffers_[1] || !descriptors_[1]) {
    return;
  }

  // Seamless descriptor chain redirection (no stop/start!)
  //
  // DMA is continuously traversing a circular descriptor chain (front buffer).
  // To switch buffers without stopping DMA:
  //   1. Redirect old front's last descriptor to new buffer's first descriptor
  //   2. Restore new buffer's circularity (so it loops forever when active)
  //   3. DMA seamlessly transitions at next frame boundary
  //
  // Example: Switching from buffer A to buffer B:
  //   Before: A_last → A_first (circular)
  //   After:  A_last → B_first, B_last → B_first (A→B splice, B circular)
  //   DMA finishes A, jumps to B, continues B forever
  //
  // No stop, no start, no visual glitch!

  // Step 1: Redirect current front's last descriptor to new buffer's first descriptor
  descriptors_[front_idx_][descriptor_count_ - 1].next = &descriptors_[active_idx_][0];

  // Step 2: Restore new buffer's circularity (for when it becomes old front later)
  descriptors_[active_idx_][descriptor_count_ - 1].next = &descriptors_[active_idx_][0];

  // Step 3: Swap indices (after descriptor manipulation)
  std::swap(front_idx_, active_idx_);

  // DMA seamlessly transitions at next frame boundary - no interruption!
}

// ============================================================================
// Buffer Initialization
// ============================================================================

bool GdmaDma::validate_brightness_config() {
  if (QUIET_PAD_WORDS <= (PAD_GUARD_HEAD + PAD_GUARD_TAIL)) {
    ESP_LOGE(TAG,
             "Invalid quiet pad: QUIET_PAD_WORDS=%d guard_head=%d guard_tail=%d",
             QUIET_PAD_WORDS,
             PAD_GUARD_HEAD,
             PAD_GUARD_TAIL);
    return false;
  }

  const size_t pad_bytes = QUIET_PAD_WORDS * sizeof(uint16_t);

  if (pad_bytes > 4092) {
    ESP_LOGE(TAG,
             "Invalid quiet pad: %zu bytes exceeds safe GDMA descriptor size",
             pad_bytes);
    return false;
  }

  ESP_LOGI(TAG,
           "Quiet-pad config validated: pad_words=%d pad_bytes=%zu guards=%d/%d",
           QUIET_PAD_WORDS,
           pad_bytes,
           PAD_GUARD_HEAD,
           PAD_GUARD_TAIL);

  return true;
}

void GdmaDma::initialize_buffer_internal(RowBitPlaneBuffer *buffers) {
  if (!buffers) {
    return;
  }

  for (int row = 0; row < num_rows_; row++) {
    const uint16_t row_addr = row & ADDR_MASK;

    for (int bit = 0; bit < bit_depth_; bit++) {
      uint16_t *buf =
          (uint16_t *)(buffers[row].data +
                       (bit * dma_width_ * sizeof(uint16_t)));

      for (uint16_t x = 0; x < dma_width_; x++) {
        buf[x] = static_cast<uint16_t>(
            (row_addr << ADDR_SHIFT) | (1u << OE_BIT));
      }

      buf[dma_width_ - 1] |= static_cast<uint16_t>(1u << LAT_BIT);

      if (dma_width_ >= 2) {
        buf[dma_width_ - 2] |= static_cast<uint16_t>(1u << LAT_BIT);
      }
    }
  }
}

void GdmaDma::initialize_blank_buffers() {
  if (!row_buffers_[0]) {
    ESP_LOGE(TAG, "Row buffers not allocated");
    return;
  }

  ESP_LOGI(TAG, "Initializing blank DMA buffers with control bits...");
  for (auto &row_buffer : row_buffers_) {
    if (row_buffer) {
      initialize_buffer_internal(row_buffer);
    }
  }
  ESP_LOGI(TAG, "Blank buffers initialized");
}



void GdmaDma::set_brightness_oe() {
  if (!row_buffers_[0]) {
    ESP_LOGE(TAG, "Row buffers not allocated");
    return;
  }

  if (!pad_words_) {
    ESP_LOGE(TAG, "Quiet pad buffers not allocated");
    return;
  }

  const uint8_t brightness =
      (uint8_t)((float)basis_brightness_ * intensity_);

  update_quiet_pad_oe(brightness);
}

bool GdmaDma::build_descriptor_chain_internal(RowBitPlaneBuffer *buffers,
                                              dma_descriptor_t *descriptors) {
  if (!buffers || !descriptors || !pad_words_) {
    return false;
  }

  const size_t bytes_per_data_bitplane = dma_width_ * sizeof(uint16_t);
  const size_t bytes_per_pad = QUIET_PAD_WORDS * sizeof(uint16_t);

  size_t desc_idx = 0;

  auto add_desc = [&](uint8_t *buffer, size_t length_bytes) -> bool {
    if (desc_idx >= descriptor_count_) {
      ESP_LOGE(TAG,
               "Descriptor overflow: desc_idx=%zu descriptor_count=%zu",
               desc_idx,
               descriptor_count_);
      return false;
    }

    dma_descriptor_t *const desc = &descriptors[desc_idx];

    desc->dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
    desc->dw0.suc_eof = 0;
    desc->dw0.size = length_bytes;
    desc->dw0.length = length_bytes;
    desc->buffer = buffer;
    desc->next = nullptr;

    if (desc_idx > 0) {
      descriptors[desc_idx - 1].next = desc;
    }

    desc_idx++;
    return true;
  };

  ESP_LOGI(TAG, "Building slot-scanned quiet-pad BCM descriptor chain");

  for (int bit = 0; bit < bit_depth_; bit++) {
    const int repetitions =
        (bit <= lsbMsbTransitionBit_)
            ? 1
            : (1 << (bit - lsbMsbTransitionBit_ - 1));

    for (int rep = 0; rep < repetitions; rep++) {
      for (int row = 0; row < num_rows_; row++) {
        uint8_t *const data_buffer =
            buffers[row].data + (bit * bytes_per_data_bitplane);

        uint8_t *const pad_buffer =
            reinterpret_cast<uint8_t *>(pad_row_ptr(row));

        if (!add_desc(data_buffer, bytes_per_data_bitplane)) {
          return false;
        }

        if (!add_desc(pad_buffer, bytes_per_pad)) {
          return false;
        }
      }
    }
  }

  if (desc_idx != descriptor_count_) {
    ESP_LOGE(TAG,
             "Descriptor count mismatch: built=%zu expected=%zu",
             desc_idx,
             descriptor_count_);
    return false;
  }

  descriptors[descriptor_count_ - 1].next = &descriptors[0];
  descriptors[descriptor_count_ - 1].dw0.suc_eof = 1;

  return true;
}

bool GdmaDma::build_descriptor_chain() {
  if (num_rows_ == 0) {
    ESP_LOGE(TAG, "Invalid configuration: num_rows_ is 0");
    return false;
  }

  if (!pad_words_) {
    ESP_LOGE(TAG, "Quiet pad buffers not allocated");
    return false;
  }

  const size_t bcm_repetitions =
      GdmaDma::calculate_bcm_transmissions(bit_depth_, lsbMsbTransitionBit_);

  // Slot-scanned:
  // each repetition = all rows
  // each row = data descriptor + pad descriptor
  descriptor_count_ = bcm_repetitions * num_rows_ * 2;

  const size_t total_descriptor_bytes =
      sizeof(dma_descriptor_t) * descriptor_count_;

	  ESP_LOGI(TAG, "Slot-scanned quiet-pad BCM descriptor chain built: %zu descriptors",
	           descriptor_count_);

  ESP_LOGI(TAG,
           "bcm_repetitions=%zu rows=%d row slot=%u data + %d pad",
           bcm_repetitions,
           num_rows_,
           dma_width_,
           QUIET_PAD_WORDS);

  for (auto &descriptor : descriptors_) {
    if (descriptor) {
      heap_caps_free(descriptor);
      descriptor = nullptr;
    }
  }

  descriptors_[0] = (dma_descriptor_t *)heap_caps_calloc(
      1,
      total_descriptor_bytes,
      MALLOC_CAP_DMA);

  if (!descriptors_[0]) {
    ESP_LOGE(TAG,
             "Failed to allocate %zu descriptors [0] (%zu bytes)",
             descriptor_count_,
             total_descriptor_bytes);
    return false;
  }

  if (!build_descriptor_chain_internal(row_buffers_[0], descriptors_[0])) {
    ESP_LOGE(TAG, "Failed to build slot-scanned quiet-pad descriptor chain [0]");
    return false;
  }

		   
   if (config_.double_buffer) {
     descriptors_[1] = (dma_descriptor_t *)heap_caps_calloc(
         1,
         total_descriptor_bytes,
         MALLOC_CAP_DMA);

     if (!descriptors_[1]) {
       ESP_LOGE(TAG,
                "Failed to allocate %zu descriptors [1] (%zu bytes)",
                descriptor_count_,
                total_descriptor_bytes);
       return false;
     }

     if (!build_descriptor_chain_internal(row_buffers_[1], descriptors_[1])) {
       ESP_LOGE(TAG, "Failed to build slot-scanned quiet-pad descriptor chain [1]");
       return false;
     }
   }

  return true;
}

// ============================================================================
// BCM Timing Calculation (Platform-Specific)
// ============================================================================

// Calculate number of transmissions per row for BCM timing
// Must match the actual descriptor allocation in build_descriptor_chain():
//   bits <= transition: 1 descriptor each
//   bits > transition: 2^(bit - transition - 1) descriptors each
HUB75_CONST constexpr int GdmaDma::calculate_bcm_transmissions(int bit_depth, int lsb_msb_transition) {
  int transmissions = lsb_msb_transition + 1;  // Bits 0 to transition: 1 each

  // Add BCM repetitions for bits above transition
  for (int i = lsb_msb_transition + 1; i < bit_depth; ++i) {
    transmissions += (1 << (i - lsb_msb_transition - 1));
  }

  return transmissions;
}

void GdmaDma::calculate_bcm_timings() {
  const uint32_t target_hz = config_.min_refresh_rate;

  const size_t row_slot_words =
      static_cast<size_t>(dma_width_) + static_cast<size_t>(QUIET_PAD_WORDS);

  const float row_slot_time_us =
      (row_slot_words * 1000000.0f) / actual_clock_hz_;

  ESP_LOGI(TAG,
           "Quiet-pad row slot: %zu words = %.2f us (%u data + %d pad @ %lu Hz)",
           row_slot_words,
           row_slot_time_us,
           dma_width_,
           QUIET_PAD_WORDS,
           (unsigned long)actual_clock_hz_);

  lsbMsbTransitionBit_ = 0;
  int actual_hz = 0;

  while (true) {
    const int bcm_repetitions =
        GdmaDma::calculate_bcm_transmissions(bit_depth_, lsbMsbTransitionBit_);

    const float frame_time_us =
        row_slot_time_us * num_rows_ * bcm_repetitions;

    actual_hz = static_cast<int>(1000000.0f / frame_time_us);

    ESP_LOGD(TAG,
             "Testing lsbMsbTransitionBit=%d: bcm_repetitions=%d, refresh=%d Hz",
             lsbMsbTransitionBit_,
             bcm_repetitions,
             actual_hz);

    if (actual_hz >= static_cast<int>(target_hz)) {
      break;
    }

    if (lsbMsbTransitionBit_ < bit_depth_ - 1) {
      lsbMsbTransitionBit_++;
    } else {
      ESP_LOGW(TAG,
               "Cannot achieve target %lu Hz, max is %d Hz",
               (unsigned long)target_hz,
               actual_hz);
      break;
    }
  }

  ESP_LOGI(TAG,
           "Quiet-pad BCM: bit_depth=%d, lsbMsbTransitionBit=%d, refresh=%d Hz target=%lu",
           bit_depth_,
           lsbMsbTransitionBit_,
           actual_hz,
           (unsigned long)target_hz);
}

// ============================================================================
// Compile-Time Validation (ESP-IDF 5.x only - requires consteval/GCC 9+)
// ============================================================================

#if ESP_IDF_VERSION_MAJOR >= 5
namespace {

// Validate BCM calculations match actual descriptor allocation
// Formula: (transition + 1) base + sum of 2^(i - transition - 1) for i > transition
consteval bool test_bcm_12bit_transition0() {
  // 12-bit depth, transition=0: 1 + (1+2+4+8+16+32+64+128+256+512+1024) = 1 + 2047 = 2048
  constexpr int transmissions = GdmaDma::calculate_bcm_transmissions(12, 0);
  return transmissions == 2048;
}

consteval bool test_bcm_10bit_transition0() {
  // 10-bit depth, transition=0: 1 + (1+2+4+8+16+32+64+128+256) = 1 + 511 = 512
  constexpr int transmissions = GdmaDma::calculate_bcm_transmissions(10, 0);
  return transmissions == 512;
}

consteval bool test_bcm_8bit_transition0() {
  // 8-bit depth, transition=0: 1 + (1+2+4+8+16+32+64) = 1 + 127 = 128
  constexpr int transmissions = GdmaDma::calculate_bcm_transmissions(8, 0);
  return transmissions == 128;
}

consteval bool test_bcm_8bit_transition1() {
  // 8-bit, transition=1: 2 + (1+2+4+8+16+32) = 2 + 63 = 65
  constexpr int transmissions = GdmaDma::calculate_bcm_transmissions(8, 1);
  return transmissions == 65;
}

consteval bool test_bcm_8bit_transition2() {
  // 8-bit, transition=2: 3 + (1+2+4+8+16) = 3 + 31 = 34
  constexpr int transmissions = GdmaDma::calculate_bcm_transmissions(8, 2);
  return transmissions == 34;
}

// Static assertions
static_assert(test_bcm_12bit_transition0(), "BCM: 12-bit/transition=0 should produce 2048 transmissions");
static_assert(test_bcm_10bit_transition0(), "BCM: 10-bit/transition=0 should produce 512 transmissions");
static_assert(test_bcm_8bit_transition0(), "BCM: 8-bit/transition=0 should produce 128 transmissions");
static_assert(test_bcm_8bit_transition1(), "BCM: 8-bit/transition=1 should produce 65 transmissions");
static_assert(test_bcm_8bit_transition2(), "BCM: 8-bit/transition=2 should produce 34 transmissions");

}  // namespace
#endif  // ESP_IDF_VERSION_MAJOR >= 5

}  // namespace hub75

#endif  // CONFIG_IDF_TARGET_ESP32S3