#pragma once
// Stands in for the MXSK05_A handset (TM1650 panel) on the MXKZH06 control box bus.
//
// The control box is the bus master (~9.6 kHz, I2C-style). Every ~3.7 ms it:
//   writes 0x01 to 0x48, then reads one key byte from 0x49   -> we answer key_code
//   writes segment bytes to 0x68 / 0x6A / 0x6C / 0x6E         -> we keep them for the height
// Protocol and codes from github.com/dong4j/desk-gateway (same MXSK05_A V1.4 board).
//
// Implemented as a bit-banged slave in an IRAM interrupt so the ESP32's hardware I2C
// stays free for the OLED.
#include <cmath>
#include <cstdio>
#include <string>
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/gpio_reg.h"

namespace desk {

static constexpr int PIN_CLK = 18;  // handset white wire
static constexpr int PIN_DAT = 19;  // handset black wire

// Key codes the control box understands
static constexpr uint8_t KEY_IDLE = 0x2E;
static constexpr uint8_t KEY_UP = 0x47;
static constexpr uint8_t KEY_DOWN = 0x4F;
static constexpr uint8_t KEY_PRESET1 = 0x17;
static constexpr uint8_t KEY_PRESET4 = 0x2F;

// ---- bus slave (runs in interrupt context) ----

enum : uint8_t { ST_IDLE, ST_ADDR, ST_ACK_ADDR, ST_WDATA, ST_ACK_DATA, ST_RDATA, ST_RACK };

static volatile uint8_t key_code = KEY_IDLE;
static volatile uint8_t digits[4] = {0, 0, 0, 0};
static volatile uint32_t polls = 0;  // key reads answered
static volatile uint32_t clk_edges = 0, dat_edges = 0, frames = 0;  // diagnostics
static esp_err_t isr_err = ESP_FAIL;

static volatile uint8_t st = ST_IDLE;
static volatile uint8_t addr = 0, shift = 0, nbits = 0, tx = 0;
static volatile uint32_t last_in = 0;

static inline void IRAM_ATTR dat_low() { REG_WRITE(GPIO_OUT_W1TC_REG, 1UL << PIN_DAT); }
static inline void IRAM_ATTR dat_release() { REG_WRITE(GPIO_OUT_W1TS_REG, 1UL << PIN_DAT); }

static inline bool IRAM_ATTR wanted(uint8_t a) {
  return a == 0x48 || a == 0x49 || a == 0x68 || a == 0x6A || a == 0x6C || a == 0x6E;
}

static void IRAM_ATTR on_edge(void *) {
  uint32_t in = REG_READ(GPIO_IN_REG);
  uint32_t changed = in ^ last_in;
  last_in = in;
  bool clk = in & (1UL << PIN_CLK);
  bool dat = in & (1UL << PIN_DAT);
  if (changed & (1UL << PIN_CLK)) clk_edges++;
  if (changed & (1UL << PIN_DAT)) dat_edges++;

  if (!(changed & (1UL << PIN_CLK))) {
    // DAT moved with CLK high: START or STOP. With CLK low it is just data.
    if (clk && (changed & (1UL << PIN_DAT))) {
      dat_release();
      if (dat) {
        st = ST_IDLE;
      } else {
        st = ST_ADDR;
        shift = 0;
        nbits = 0;
        frames++;
      }
    }
    return;
  }

  if (clk) {
    // Rising edge: the receiver samples DAT
    if ((st == ST_ADDR || st == ST_WDATA) && nbits < 8) {
      shift = (shift << 1) | (dat ? 1 : 0);
      nbits++;
    }
    return;
  }

  // Falling edge: whoever transmits sets up the next bit
  switch (st) {
    case ST_ADDR:
      if (nbits == 8) {
        addr = shift;
        if (wanted(addr)) {
          dat_low();  // ACK
          st = ST_ACK_ADDR;
        } else {
          st = ST_IDLE;  // not for us; wait for the next START
        }
      }
      break;
    case ST_WDATA:
      if (nbits == 8) {
        if (addr >= 0x68) digits[(addr - 0x68) >> 1] = shift;
        dat_low();  // ACK
        st = ST_ACK_DATA;
      }
      break;
    case ST_ACK_ADDR:
      if (addr & 1) {
        tx = key_code;
        nbits = 0;
        if (tx & 0x80) dat_release(); else dat_low();
        polls++;
        st = ST_RDATA;
      } else {
        dat_release();
        shift = 0;
        nbits = 0;
        st = ST_WDATA;
      }
      break;
    case ST_ACK_DATA:
      dat_release();
      shift = 0;
      nbits = 0;
      st = ST_WDATA;
      break;
    case ST_RDATA:
      nbits++;
      if (nbits < 8) {
        if ((tx << nbits) & 0x80) dat_release(); else dat_low();
      } else {
        dat_release();  // master ACKs/NACKs
        st = ST_RACK;
      }
      break;
    case ST_RACK:
      st = ST_IDLE;
      break;
    default:
      break;
  }
}

static void install_task(void *) {
  esp_err_t err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LEVEL3);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    err = gpio_install_isr_service(0);  // fall back to default interrupt settings
  }
  if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
    err = gpio_isr_handler_add((gpio_num_t) PIN_CLK, on_edge, nullptr);
    if (err == ESP_OK) err = gpio_isr_handler_add((gpio_num_t) PIN_DAT, on_edge, nullptr);
  }
  isr_err = err;
  vTaskDelete(nullptr);
}

static void setup() {
  gpio_config_t io{};
  io.pin_bit_mask = 1ULL << PIN_CLK;
  io.mode = GPIO_MODE_INPUT;
  io.pull_up_en = GPIO_PULLUP_ENABLE;  // stands in for the handset's 2k pull-ups
  io.intr_type = GPIO_INTR_ANYEDGE;
  gpio_config(&io);
  io.pin_bit_mask = 1ULL << PIN_DAT;
  io.mode = GPIO_MODE_INPUT_OUTPUT_OD;
  gpio_config(&io);
  dat_release();
  last_in = REG_READ(GPIO_IN_REG);
  // Keep the interrupt on core 1, away from Wi-Fi on core 0
  xTaskCreatePinnedToCore(install_task, "desk_isr", 2048, nullptr, 5, nullptr, 1);
}

// ---- height from the display bytes ----

// This board's segment wiring (not the standard 7-segment table). Bit 5 is unused
// by every digit, so it is taken to be the decimal point.
static int seg_to_digit(uint8_t s) {
  switch (s & ~0x20) {
    case 0x5F: return 0;
    case 0x44: return 1;
    case 0x9E: return 2;
    case 0xD6: return 3;
    case 0xC5: return 4;
    case 0xD3: return 5;
    case 0xDB: return 6;
    case 0x46: return 7;
    case 0xDF: return 8;
    case 0xD7: return 9;
    default: return -1;
  }
}

// Anything outside this isn't a height. Waking from sleep, the box briefly shows
// "07.0", which would otherwise look like the desk is already below the sit height.
static constexpr float MIN_HEIGHT = 18.0f;
static constexpr float MAX_HEIGHT = 60.0f;

// The number on the (virtual) display, e.g. 27.0. NAN while blank, showing text,
// or showing a number that can't be a height.
static float height() {
  float value = 0;
  int decimals = -1;
  bool any = false;
  for (int i = 0; i < 3; i++) {  // DIG4 mirrors DIG3
    uint8_t s = digits[i];
    if (s == 0) continue;
    int d = seg_to_digit(s);
    if (d < 0) return NAN;
    value = value * 10 + d;
    any = true;
    if (decimals >= 0) decimals++;
    if (s & 0x20) decimals = 0;
  }
  if (!any) return NAN;
  for (int i = 0; i < decimals; i++) value /= 10;
  if (value < MIN_HEIGHT || value > MAX_HEIGHT) return NAN;
  return value;
}

// Last height the display showed. Only this ESP32 moves the desk, so it stays right
// while the box sleeps with its display off. NAN until seen once after boot.
static float last_height = NAN;
static float known_height() {
  float h = height();
  if (!std::isnan(h)) last_height = h;
  return last_height;
}

// ---- what we press (main loop context) ----

static bool up_held = false, down_held = false;
static int64_t nudge_until = 0;
static uint8_t nudge_key = KEY_IDLE;
static int64_t up_since = 0, down_since = 0;
static constexpr int64_t MAX_HOLD_US = 15 * 1000000LL;  // same cap the box uses

static int64_t now_us() { return esp_timer_get_time(); }

// Go to a height by holding Up or Down and watching the display, instead of using
// the box's presets.
static constexpr int64_t GOTO_MAX_US = 40 * 1000000LL;     // give up after this long
static constexpr int64_t GOTO_STALL_US = 3 * 1000000LL;    // or if the height stops changing
// A sleeping box only wakes on the first press, and after a long idle it can take
// several seconds before it will move, so if the desk hasn't started moving after a
// press, let go for a moment and press again
static constexpr int64_t GOTO_START_US = 4000 * 1000LL;
static constexpr int64_t GOTO_REPRESS_GAP_US = 1000 * 1000LL;
static constexpr int GOTO_MAX_PRESSES = 4;
static constexpr int64_t GOTO_WAKE_US = 2000 * 1000LL;     // short Up press to read the height
static constexpr int64_t REPRESS_US = 10 * 1000000LL;      // re-press before the hold cap
static constexpr int64_t REPRESS_GAP_US = 150 * 1000LL;
static constexpr float GOTO_CLOSE_ENOUGH = 0.2f;           // inches; already there

static bool goto_active = false, goto_waking = false, goto_up = false, goto_moved = false;
static float goto_target = 0, goto_coast = 0.3f;
static float goto_last_h = NAN;
static int64_t goto_started = 0, goto_last_change = 0, goto_press_at = 0;
static int goto_presses = 0;

static void cancel_goto() { goto_active = goto_waking = false; }

static void hold_up(bool on) { cancel_goto(); up_held = on; up_since = now_us(); }
static void hold_down(bool on) { cancel_goto(); down_held = on; down_since = now_us(); }
static void nudge(uint8_t key, int ms) { cancel_goto(); nudge_key = key; nudge_until = now_us() + ms * 1000LL; }

// Stop everything. The desk stops as soon as no key is held.
static void stop() {
  cancel_goto();
  up_held = down_held = false;
  nudge_until = 0;
}

// target: height to end at. coast: how far before it to let go, since the desk
// keeps moving briefly after release.
static void go_to(float target, float coast) {
  stop();
  int64_t t = now_us();
  goto_target = target;
  goto_coast = coast;
  goto_active = true;
  goto_started = goto_last_change = goto_press_at = t;
  goto_moved = false;
  goto_presses = 1;
  float h = known_height();
  goto_last_h = h;
  if (std::isnan(h)) {
    goto_waking = true;  // height unknown since boot: press Up briefly to wake the display
    return;
  }
  if (std::fabs(h - target) <= GOTO_CLOSE_ENOUGH) {
    goto_active = false;
    return;
  }
  goto_up = target > h;
  ESP_LOGI("desk", "go_to %.1f from %.1f, display %s", target, h, std::isnan(height()) ? "off" : "on");
}

// Runs every update while going to a height; returns the key to hold
static uint8_t goto_key(int64_t t) {
  float h = known_height();
  if (goto_waking) {
    if (std::isnan(h) && t - goto_started < GOTO_WAKE_US) return KEY_UP;
    goto_waking = false;
    if (std::isnan(h)) { goto_active = false; return KEY_IDLE; }  // display never came on
    go_to(goto_target, goto_coast);
    if (!goto_active) return KEY_IDLE;
  }
  bool arrived = goto_up ? h >= goto_target - goto_coast : h <= goto_target + goto_coast;
  if (std::fabs(h - goto_last_h) >= 0.1f) {
    if (!goto_moved)
      ESP_LOGI("desk", "go_to %.1f: moving after %.1fs, press %d", goto_target,
               (t - goto_started) / 1e6, goto_presses);
    goto_last_h = h;
    goto_last_change = t;
    goto_moved = true;
  }
  bool stalled = goto_moved && t - goto_last_change > GOTO_STALL_US;  // at a limit, or blocked
  if (arrived || stalled || t - goto_started > GOTO_MAX_US) {
    ESP_LOGI("desk", "go_to %.1f done at %.1f after %.1fs (%s)", goto_target, h,
             (t - goto_started) / 1e6, arrived ? "arrived" : stalled ? "stalled" : "timeout");
    goto_active = false;
    return KEY_IDLE;
  }
  if (t < goto_press_at) return KEY_IDLE;  // let go between presses
  if (!goto_moved && t - goto_press_at > GOTO_START_US) {
    if (goto_presses >= GOTO_MAX_PRESSES) {
      ESP_LOGW("desk", "go_to %.1f gave up: no movement after %d presses", goto_target, goto_presses);
      goto_active = false;
      return KEY_IDLE;
    }
    ESP_LOGI("desk", "go_to %.1f: no movement after press %d, pressing again", goto_target, goto_presses);
    goto_presses++;
    goto_press_at = t + GOTO_REPRESS_GAP_US;
    return KEY_IDLE;
  }
  int64_t held = t - goto_press_at;
  if (held >= REPRESS_US + REPRESS_GAP_US) {
    goto_press_at = t;
    held = 0;
  }
  if (held >= REPRESS_US) return KEY_IDLE;
  return goto_up ? KEY_UP : KEY_DOWN;
}

// Call every few ms: works out which key the panel is "holding"
static void update() {
  int64_t t = now_us();
  known_height();
  bool up = up_held && t - up_since < MAX_HOLD_US;
  bool down = down_held && t - down_since < MAX_HOLD_US;
  uint8_t k = KEY_IDLE;
  if (goto_active) k = goto_key(t);
  else if (t < nudge_until) k = nudge_key;
  else if (up != down) k = up ? KEY_UP : KEY_DOWN;  // both together would be a factory reset
  key_code = k;
}

// True while the control box is polling us
static bool connected() {
  static uint32_t last_polls = 0;
  static int64_t last_change = 0;
  uint32_t p = polls;
  if (p != last_polls) { last_polls = p; last_change = now_us(); }
  return last_change != 0 && now_us() - last_change < 1000000;
}

// ---- sitting / standing ----

// Above the threshold counts as standing (the YAML passes halfway between the
// sit and stand heights)
static bool standing(float threshold) {
  float h = known_height();
  return !std::isnan(h) && h >= threshold;
}

// Minutes since the desk last went to sitting height (0 while standing)
static int64_t sit_since = 0;
static int sitting_minutes(float threshold) {
  if (standing(threshold)) {
    sit_since = 0;
    return 0;
  }
  if (sit_since == 0) sit_since = now_us();
  return (int) ((now_us() - sit_since) / 60000000LL);
}

// Digits, key, then diagnostics: interrupt status, CLK/DAT levels now, edges, frames, polls
static std::string raw() {
  uint32_t in = REG_READ(GPIO_IN_REG);
  char buf[128];
  snprintf(buf, sizeof(buf), "%02X %02X %02X %02X key %02X | isr %d lvl %d%d edges %lu/%lu frames %lu polls %lu",
           digits[0], digits[1], digits[2], digits[3], key_code, (int) isr_err,
           (int) ((in >> PIN_CLK) & 1), (int) ((in >> PIN_DAT) & 1),
           (unsigned long) clk_edges, (unsigned long) dat_edges,
           (unsigned long) frames, (unsigned long) polls);
  return buf;
}

}  // namespace desk
