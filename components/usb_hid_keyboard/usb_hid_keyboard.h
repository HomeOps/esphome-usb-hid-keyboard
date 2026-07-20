#pragma once

// usb_host is only buildable on the ESP32 variants with a USB OTG peripheral.
// The guard mirrors esphome/components/usb_host/usb_host.h so clang-tidy and
// non-USB builds skip the whole translation unit.
#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3) || \
    defined(USE_ESP32_VARIANT_ESP32S31) || defined(USE_ESP32_VARIANT_ESP32H4)

#include "esphome/components/usb_host/usb_host.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/event_pool.h"
#include "esphome/core/lock_free_queue.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace esphome::usb_hid_keyboard {

static const char *const TAG = "usb_hid_keyboard";

// A composite remote typically exposes two HID interfaces: a boot keyboard and
// a consumer-control interface. Four covers every device we've seen with room
// to spare, and keeps the per-interface state in a fixed array.
static constexpr uint8_t MAX_HID_INTERFACES = 4;

// Largest interrupt IN report we will accept. Boot keyboard reports are 8
// bytes, consumer reports 2-3. Anything longer is truncated rather than
// overrunning the buffer.
static constexpr uint8_t MAX_REPORT_SIZE = 16;

// Depth of the USB-task -> main-loop report queue. Keypresses are a human-rate
// event; 16 is generous.
static constexpr uint8_t REPORT_QUEUE_SIZE = 16;

// Upper bound on the IN transfer length we will submit, used to clamp a device
// that misreports its wMaxPacketSize. 64 is the full-speed maximum for an
// interrupt endpoint, which covers every HID keyboard/remote.
//
// Deliberately a local constant rather than usb_host::USB_MAX_PACKET_SIZE:
// that symbol only exists on ESPHome dev (it arrived with the usb_host
// max_packet_size option) and referencing it breaks the build on 2026.4.0.
static constexpr uint16_t MAX_TRANSFER_SIZE = 64;

// Boot keyboard reports carry up to 6 simultaneous keycodes (HID 1.11 B.1).
static constexpr uint8_t BOOT_KEY_ROLLOVER = 6;

// ── HID class requests (HID 1.11 §7.2) ──────────────────────────────────────
static constexpr uint8_t HID_SET_IDLE = 0x0A;
static constexpr uint8_t HID_SET_PROTOCOL = 0x0B;
static constexpr uint8_t HID_PROTOCOL_BOOT = 0x00;

// ── USB interface protocol codes ────────────────────────────────────────────
// USB_CLASS_HID is deliberately not defined here: ESP-IDF already provides it
// as a macro in usb/usb_types_ch9.h, and a macro ignores namespaces, so a
// same-named constexpr expands to `uint8_t 0x03 = 0x03` and fails to compile.
static constexpr uint8_t USB_HID_PROTOCOL_KEYBOARD = 0x01;

class USBHIDKeyboard;

// Bodies are defined below USBHIDKeyboard, which is incomplete at this point.
class KeyTrigger : public Trigger<uint8_t, uint8_t> {
 public:
  explicit KeyTrigger(USBHIDKeyboard *parent);
};

class ConsumerTrigger : public Trigger<uint16_t> {
 public:
  explicit ConsumerTrigger(USBHIDKeyboard *parent);
};

// One report handed from the USB task to the main loop. Triggers must not run
// in the USB task context, so reports are copied into a pool-allocated struct
// and drained in loop().
struct HidReport {
  uint8_t data[MAX_REPORT_SIZE];
  uint8_t length;
  uint8_t interface_index;

  // Required by EventPool; nothing to free for a POD payload.
  void release() {}
};

// State for a single claimed HID interface.
struct HidInterface {
  const usb_ep_desc_t *in_ep{nullptr};
  uint8_t interface_number{0xFF};
  uint16_t packet_size{8};
  bool is_keyboard{false};
  bool claimed{false};
  // Guards against two in-flight IN transfers on the same endpoint. Touched
  // from both the USB task (re-arm) and the main loop (initial start).
  std::atomic<bool> read_started{false};
  // Previous boot-keyboard report, for press-edge detection. Main loop only.
  uint8_t prev_keys[BOOT_KEY_ROLLOVER]{};
};

class USBHIDKeyboard : public usb_host::USBClient {
 public:
  USBHIDKeyboard(uint16_t vid, uint16_t pid) : usb_host::USBClient(vid, pid) {}

  void loop() override;
  void dump_config() override;

  void add_on_key_trigger(KeyTrigger *trigger) { this->key_triggers_.push_back(trigger); }
  void add_on_consumer_trigger(ConsumerTrigger *trigger) { this->consumer_triggers_.push_back(trigger); }

 protected:
  void on_connected() override;
  void on_disconnected() override;

  // Descriptor walk: find every HID interface with an interrupt IN endpoint.
  void find_hid_interfaces_();

  // Async SET_PROTOCOL/SET_IDLE sequence, driven from loop(). Returns true if
  // it did work this iteration (so loop() knows not to disable itself).
  bool run_setup_();
  void setup_transfer_(uint8_t request, uint16_t value, uint16_t index);

  // Arm an interrupt IN transfer; the completion callback re-arms it.
  void start_read_(uint8_t iface_index);

  // Main-loop report handling.
  void handle_report_(const HidReport &report);
  void parse_keyboard_(HidInterface &iface, const uint8_t *data, uint8_t len);
  void parse_consumer_(const uint8_t *data, uint8_t len);

  HidInterface interfaces_[MAX_HID_INTERFACES];
  uint8_t interface_count_{0};

  // Setup state machine. cfg_done_ is set from the USB task and read from the
  // main loop, hence atomic with release/acquire ordering.
  std::atomic<bool> cfg_done_{false};
  bool cfg_active_{false};
  bool cfg_in_flight_{false};
  bool cfg_ok_{true};
  uint8_t cfg_iface_{0};
  uint8_t cfg_step_{0};

  // USB task -> main loop report handoff.
  LockFreeQueue<HidReport, REPORT_QUEUE_SIZE> report_queue_;
  EventPool<HidReport, REPORT_QUEUE_SIZE - 1> report_pool_;

  std::vector<KeyTrigger *> key_triggers_;
  std::vector<ConsumerTrigger *> consumer_triggers_;
};

inline KeyTrigger::KeyTrigger(USBHIDKeyboard *parent) { parent->add_on_key_trigger(this); }
inline ConsumerTrigger::ConsumerTrigger(USBHIDKeyboard *parent) { parent->add_on_consumer_trigger(this); }

}  // namespace esphome::usb_hid_keyboard

#endif  // USB-capable ESP32 variant
