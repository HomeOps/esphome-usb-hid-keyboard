#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3) || \
    defined(USE_ESP32_VARIANT_ESP32S31) || defined(USE_ESP32_VARIANT_ESP32H4)

#include "usb_hid_keyboard.h"

#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cstring>

namespace esphome::usb_hid_keyboard {

// bmRequestType for a class request directed at an interface, host->device.
static constexpr uint8_t HID_CLASS_INTF_OUT =
    usb_host::USB_TYPE_CLASS | usb_host::USB_RECIP_INTERFACE | usb_host::USB_DIR_OUT;

void USBHIDKeyboard::dump_config() {
  USBClient::dump_config();
  ESP_LOGCONFIG(TAG, "  HID interfaces found: %u", this->interface_count_);
  for (uint8_t i = 0; i != this->interface_count_; i++) {
    const auto &iface = this->interfaces_[i];
    ESP_LOGCONFIG(TAG,
                  "    Interface %u\n"
                  "      Endpoint: 0x%02X\n"
                  "      Packet size: %u\n"
                  "      Type: %s",
                  iface.interface_number, iface.in_ep != nullptr ? iface.in_ep->bEndpointAddress : 0, iface.packet_size,
                  iface.is_keyboard ? "boot keyboard" : "consumer/generic");
  }
  ESP_LOGCONFIG(TAG,
                "  on_key automations: %zu\n"
                "  on_consumer automations: %zu",
                this->key_triggers_.size(), this->consumer_triggers_.size());
}

void USBHIDKeyboard::on_connected() {
  this->interface_count_ = 0;
  this->find_hid_interfaces_();

  if (this->interface_count_ == 0) {
    ESP_LOGE(TAG, "No HID interface with an interrupt IN endpoint found");
    this->status_set_error(LOG_STR("No HID interface found"));
    this->disconnect();
    return;
  }

  // Claim every interface we intend to read from. A device that refuses the
  // claim cannot be read, so drop that interface rather than reading garbage.
  uint8_t claimed = 0;
  for (uint8_t i = 0; i != this->interface_count_; i++) {
    auto &iface = this->interfaces_[i];
    auto err = usb_host_interface_claim(this->handle_, this->device_handle_, iface.interface_number, 0);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "Could not claim interface %u: %s", iface.interface_number, esp_err_to_name(err));
      continue;
    }
    iface.claimed = true;
    claimed++;
    ESP_LOGD(TAG, "Claimed HID interface %u (%s), ep=0x%02X mps=%u", iface.interface_number,
             iface.is_keyboard ? "boot keyboard" : "consumer/generic", iface.in_ep->bEndpointAddress,
             iface.packet_size);
  }

  if (claimed == 0) {
    ESP_LOGE(TAG, "Failed to claim any HID interface");
    this->status_set_error(LOG_STR("Interface claim failed"));
    this->disconnect();
    return;
  }

  this->status_clear_error();

  // Kick off the async SET_PROTOCOL / SET_IDLE sequence. run_setup_() drives it
  // from the main loop; each control transfer's callback wakes us back up.
  this->cfg_active_ = true;
  this->cfg_in_flight_ = false;
  this->cfg_ok_ = true;
  this->cfg_iface_ = 0;
  this->cfg_step_ = 0;
  this->cfg_done_.store(false);
  this->enable_loop_soon_any_context();
}

void USBHIDKeyboard::find_hid_interfaces_() {
  const usb_config_desc_t *config_desc;
  if (usb_host_get_active_config_descriptor(this->device_handle_, &config_desc) != ESP_OK) {
    ESP_LOGE(TAG, "usb_host_get_active_config_descriptor failed");
    return;
  }

  for (uint8_t intf_idx = 0; this->interface_count_ != MAX_HID_INTERFACES; intf_idx++) {
    int conf_offset = 0;
    const auto *intf_desc = usb_parse_interface_descriptor(config_desc, intf_idx, 0, &conf_offset);
    if (intf_desc == nullptr)
      break;  // ran off the end of the configuration

    ESP_LOGV(TAG, "intf %u: class=%02X subclass=%02X protocol=%02X endpoints=%u", intf_desc->bInterfaceNumber,
             intf_desc->bInterfaceClass, intf_desc->bInterfaceSubClass, intf_desc->bInterfaceProtocol,
             intf_desc->bNumEndpoints);

    if (intf_desc->bInterfaceClass != USB_CLASS_HID)
      continue;

    // Take the first interrupt IN endpoint on this interface.
    const usb_ep_desc_t *in_ep = nullptr;
    for (uint8_t ep_idx = 0; ep_idx != intf_desc->bNumEndpoints; ep_idx++) {
      int ep_offset = conf_offset;
      const auto *ep = usb_parse_endpoint_descriptor_by_index(intf_desc, ep_idx, config_desc->wTotalLength, &ep_offset);
      if (ep == nullptr)
        break;
      if ((ep->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) == USB_BM_ATTRIBUTES_XFER_INT &&
          (ep->bEndpointAddress & usb_host::USB_DIR_IN) != 0) {
        in_ep = ep;
        break;
      }
    }

    if (in_ep == nullptr) {
      ESP_LOGV(TAG, "HID interface %u has no interrupt IN endpoint; skipping", intf_desc->bInterfaceNumber);
      continue;
    }

    auto &iface = this->interfaces_[this->interface_count_++];
    iface.in_ep = in_ep;
    iface.interface_number = intf_desc->bInterfaceNumber;
    // Clamp so a device misreporting its MPS cannot make us submit an
    // oversized transfer.
    iface.packet_size = std::min<uint16_t>(in_ep->wMaxPacketSize, MAX_TRANSFER_SIZE);
    iface.is_keyboard = intf_desc->bInterfaceProtocol == USB_HID_PROTOCOL_KEYBOARD;
    iface.claimed = false;
    iface.read_started.store(false);
    memset(iface.prev_keys, 0, sizeof(iface.prev_keys));
  }
}

void USBHIDKeyboard::setup_transfer_(uint8_t request, uint16_t value, uint16_t index) {
  this->cfg_done_.store(false);
  bool submitted = this->control_transfer(  //
      HID_CLASS_INTF_OUT, request, value, index, [this](const usb_host::TransferStatus &status) {
        this->cfg_ok_ = status.success;
        // Release: publishes cfg_ok_ before the loop observes cfg_done_.
        this->cfg_done_.store(true, std::memory_order_release);
        this->enable_loop_soon_any_context();
        App.wake_loop_threadsafe();
      });
  if (!submitted) {
    // No callback will fire, so synthesize a failed completion; otherwise the
    // state machine waits forever.
    ESP_LOGW(TAG, "HID setup control transfer submit failed (request 0x%02X)", request);
    this->cfg_ok_ = false;
    this->cfg_done_.store(true, std::memory_order_release);
  }
}

bool USBHIDKeyboard::run_setup_() {
  if (!this->cfg_active_)
    return false;

  if (this->cfg_in_flight_) {
    // Acquire: pairs with the release store in setup_transfer_'s callback.
    if (!this->cfg_done_.load(std::memory_order_acquire))
      return false;  // still waiting — the callback re-wakes the loop, no spin
    this->cfg_in_flight_ = false;
    this->cfg_done_.store(false);
    this->cfg_step_++;
  }

  // Skip interfaces we could not claim.
  while (this->cfg_iface_ < this->interface_count_ && !this->interfaces_[this->cfg_iface_].claimed) {
    this->cfg_iface_++;
    this->cfg_step_ = 0;
  }

  if (this->cfg_iface_ >= this->interface_count_) {
    this->cfg_active_ = false;
    ESP_LOGI(TAG, "HID setup complete on %u interface(s)", this->interface_count_);
    return true;
  }

  auto &iface = this->interfaces_[this->cfg_iface_];

  switch (this->cfg_step_) {
    case 0:
      // Boot protocol gives a fixed 8-byte report layout, so we do not need to
      // parse the report descriptor. Only meaningful on a keyboard interface.
      if (iface.is_keyboard) {
        this->setup_transfer_(HID_SET_PROTOCOL, HID_PROTOCOL_BOOT, iface.interface_number);
        this->cfg_in_flight_ = true;
        return true;
      }
      this->cfg_step_++;
      [[fallthrough]];

    case 1:
      // SET_IDLE(0) = report only on change, so a held key does not spam the
      // bus at the endpoint's polling rate.
      this->setup_transfer_(HID_SET_IDLE, 0, iface.interface_number);
      this->cfg_in_flight_ = true;
      return true;

    default:
      // Some devices NAK SET_PROTOCOL or SET_IDLE but still stream reports, so
      // a failure here is a warning, not a reason to give up on the interface.
      if (!this->cfg_ok_)
        ESP_LOGW(TAG, "HID setup incomplete on interface %u; reading anyway", iface.interface_number);
      this->start_read_(this->cfg_iface_);
      this->cfg_iface_++;
      this->cfg_step_ = 0;
      this->cfg_ok_ = true;
      return true;
  }
}

void USBHIDKeyboard::start_read_(uint8_t iface_index) {
  auto &iface = this->interfaces_[iface_index];
  if (!iface.claimed)
    return;

  // Claim the in-flight flag so the USB task and main loop cannot both submit.
  bool expected = false;
  if (!iface.read_started.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
    return;

  // CALLBACK CONTEXT: runs in the USB task. Copy and queue only; triggers run
  // on the main loop.
  auto callback = [this, iface_index](const usb_host::TransferStatus &status) {
    auto &cb_iface = this->interfaces_[iface_index];

    if (!status.success) {
      ESP_LOGW(TAG, "IN transfer failed on interface %u: %s", cb_iface.interface_number,
               esp_err_to_name(status.error_code));
      // Do not re-arm: a disconnect or halted endpoint would spin forever.
      // on_disconnected() resets the flag when the device comes back.
      cb_iface.read_started.store(false, std::memory_order_release);
      return;
    }

    if (status.data_len > 0) {
      HidReport *report = this->report_pool_.allocate();
      if (report == nullptr) {
        // Main loop is behind. Drop this report rather than blocking the USB
        // task; the next keypress will still be delivered.
        this->report_queue_.increment_dropped_count();
      } else {
        report->length = static_cast<uint8_t>(std::min<size_t>(status.data_len, MAX_REPORT_SIZE));
        report->interface_index = iface_index;
        memcpy(report->data, status.data, report->length);
        this->report_queue_.push(report);
        this->enable_loop_soon_any_context();
        App.wake_loop_threadsafe();
      }
    }

    // Re-arm immediately from the USB task so no keypress is missed between
    // main-loop iterations.
    cb_iface.read_started.store(false, std::memory_order_release);
    this->start_read_(iface_index);
  };

  if (!this->transfer_in(iface.in_ep->bEndpointAddress, callback, iface.packet_size)) {
    ESP_LOGW(TAG, "IN transfer submit failed for ep 0x%02X", iface.in_ep->bEndpointAddress);
    iface.read_started.store(false, std::memory_order_release);
  }
}

void USBHIDKeyboard::loop() {
  bool had_work = this->process_usb_events_();
  had_work |= this->run_setup_();

  HidReport *report;
  while ((report = this->report_queue_.pop()) != nullptr) {
    had_work = true;
    this->handle_report_(*report);
    this->report_pool_.release(report);
  }

  uint16_t dropped = this->report_queue_.get_and_reset_dropped_count();
  if (dropped > 0)
    ESP_LOGW(TAG, "Dropped %u HID report(s) — main loop is running behind", dropped);

  // Callbacks re-enable the loop via enable_loop_soon_any_context().
  if (!had_work)
    this->disable_loop();
}

void USBHIDKeyboard::handle_report_(const HidReport &report) {
  auto &iface = this->interfaces_[report.interface_index];

  // ESP_LOGV compiles away entirely below verbose, so the string is only built
  // when it will actually be printed.
  ESP_LOGV(TAG, "intf %u report: %s", iface.interface_number, format_hex_pretty(report.data, report.length).c_str());

  // A boot keyboard report is 8 bytes: modifier, reserved, then 6 keycodes.
  // Anything shorter on a keyboard interface is a consumer-style report that
  // some composite remotes multiplex onto the same endpoint.
  if (iface.is_keyboard && report.length >= 3) {
    this->parse_keyboard_(iface, report.data, report.length);
  } else {
    this->parse_consumer_(report.data, report.length);
  }
}

void USBHIDKeyboard::parse_keyboard_(HidInterface &iface, const uint8_t *data, uint8_t len) {
  const uint8_t modifier = data[0];
  const uint8_t *keys = data + 2;
  const uint8_t key_count = std::min<uint8_t>(len - 2, BOOT_KEY_ROLLOVER);

  // Fire only on the press edge: a keycode present now but absent from the
  // previous report. Holding a key re-sends the same report, and without this
  // every automation would run repeatedly.
  for (uint8_t i = 0; i != key_count; i++) {
    const uint8_t keycode = keys[i];
    if (keycode == 0)
      continue;

    bool was_pressed = false;
    for (uint8_t j = 0; j != BOOT_KEY_ROLLOVER; j++) {
      if (iface.prev_keys[j] == keycode) {
        was_pressed = true;
        break;
      }
    }
    if (was_pressed)
      continue;

    ESP_LOGD(TAG, "key 0x%02X modifier 0x%02X", keycode, modifier);
    for (auto *trigger : this->key_triggers_)
      trigger->trigger(keycode, modifier);
  }

  memset(iface.prev_keys, 0, sizeof(iface.prev_keys));
  memcpy(iface.prev_keys, keys, key_count);
}

void USBHIDKeyboard::parse_consumer_(const uint8_t *data, uint8_t len) {
  if (len < 2)
    return;

  // Consumer reports are a little-endian 16-bit usage. A 3-byte report is a
  // report ID followed by the usage; 2 bytes is the bare usage. Longer reports
  // also carry a leading report ID.
  const uint8_t offset = (len == 2) ? 0 : 1;
  if (static_cast<uint8_t>(len - offset) < 2)
    return;

  const uint16_t usage = static_cast<uint16_t>(data[offset]) | (static_cast<uint16_t>(data[offset + 1]) << 8);

  // Zero is the release report — every key-up sends it, and it carries no
  // information about which key was released.
  if (usage == 0)
    return;

  ESP_LOGD(TAG, "consumer 0x%04X", usage);
  for (auto *trigger : this->consumer_triggers_)
    trigger->trigger(usage);
}

void USBHIDKeyboard::on_disconnected() {
  for (uint8_t i = 0; i != this->interface_count_; i++) {
    auto &iface = this->interfaces_[i];
    if (!iface.claimed)
      continue;
    if (iface.in_ep != nullptr) {
      usb_host_endpoint_halt(this->device_handle_, iface.in_ep->bEndpointAddress);
      usb_host_endpoint_flush(this->device_handle_, iface.in_ep->bEndpointAddress);
    }
    usb_host_interface_release(this->handle_, this->device_handle_, iface.interface_number);
    iface.claimed = false;
    iface.read_started.store(false);
    memset(iface.prev_keys, 0, sizeof(iface.prev_keys));
  }
  this->interface_count_ = 0;
  this->cfg_active_ = false;
  this->cfg_in_flight_ = false;

  // Drain anything the USB task queued before the device went away.
  HidReport *report;
  while ((report = this->report_queue_.pop()) != nullptr)
    this->report_pool_.release(report);

  USBClient::on_disconnected();
}

}  // namespace esphome::usb_hid_keyboard

#endif  // USB-capable ESP32 variant
