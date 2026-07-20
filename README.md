# ESPHome USB HID Keyboard (host)

Read a **USB HID keyboard or remote** plugged into an ESP32-S3 and fire ESPHome
automations for every keypress. This is the HID layer on top of ESPHome's
built-in `usb_host` component, which provides the bus but no keyboard support.

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/HomeOps/esphome-usb-hid-keyboard
      ref: v1.0.0          # pin a release
    components: [usb_hid_keyboard]

usb_host:                  # required — provides the bus

usb_hid_keyboard:
  vid: 0x0000              # 0/0 = match any device
  pid: 0x0000
  on_key:
    - lambda: |-
        switch (keycode) {
          case 0x52: id(tv_up).press();   break;
          case 0x51: id(tv_down).press(); break;
        }
  on_consumer:
    - lambda: |-
        switch (usage) {
          case 0xE9: id(tv_volume_up).press();   break;
          case 0xEA: id(tv_volume_down).press(); break;
        }
```

Supported targets: **ESP32-S3, S2, S31, P4, H4** (ESP-IDF framework — USB host is
not available on the classic ESP32 or on the Arduino framework).

---

## Why this exists

Short version: **the capability exists elsewhere, but not in a form you can
depend on.** Here is the full survey, so nobody has to redo it.

### Upstream ESPHome doesn't have it

The `dev` branch ships `usb_host`, `usb_uart`, `usb_cdc_acm`, and `tinyusb` —
there is no HID host component. `usb_host` gives you enumeration, transfer
plumbing, and a `USBClient` base class, and stops there. Plug a keyboard into it
and nothing happens; see [esphome#10649](https://github.com/esphome/esphome/issues/10649),
where exactly that is reported against 2025.8.3 and closed without a component
landing. ([esphome#3917](https://github.com/esphome/esphome/pull/3917) is
sometimes mistaken for this — it's USB *device* HID, making the ESP32 pretend to
*be* a keyboard. Opposite direction.)

### The one real prior art: NonaSuomy's `usb_hidx`

[NonaSuomy/esphome-usb-host-hidx-keyboard-touchpad-mouse-gamepad](https://github.com/NonaSuomy/esphome-usb-host-hidx-keyboard-touchpad-mouse-gamepad)
is the only genuine ESPHome USB-HID-host implementation we found, and it is
substantial work — keyboard, mouse, gamepad, and 17 device drivers. Credit where
it's due: it independently established that composite remotes put media keys on
a **second HID interface**, which is the single most useful fact in this problem
space and saved us a debugging cycle.

We still didn't build on it, for four reasons.

**1. It ships as a fork of ESPHome, not as an external component.** You consume it
with `source: github://NonaSuomy/esphome@hidx-testing-001` — that pulls the
entire ESPHome fork. The repo you find by searching contains only markdown and a
test YAML; the code lives on a branch of the fork. Pinning it means pinning
stale ESPHome internals along with the component.

**2. The branches are drifting badly.** `hidx-testing-001` is 1634 commits behind
`esphome/dev`; `hidx-testing-002` is 753 behind. The documented branch is `001`
— the *older* one. There are no tags and no releases, so there is nothing stable
to pin `ref:` to.

**3. No triggers — and this is the disqualifier for our use case.** `usb_hidx`
exposes input exclusively through sensors: `text_sensor` for the keyboard,
`binary_sensor` per key, `sensor` for axes. There is no `on_key` or
`on_consumer`. Driving a 20-button remote would mean 20 `binary_sensor`
declarations and 20 separate `on_press:` blocks, instead of one lambda with a
`switch`. That is the wrong shape for a Harmony-style bridge.

**4. Architecture: it bypasses `usb_host::USBClient`.** It `AUTO_LOAD`s
`usb_host` and then registers its *own* raw `usb_host_client_register`, so two
clients coexist. From that follow several problems we didn't want to inherit:

- `usb_hidx.cpp` allocates `uint8_t temp_data[65]` and `memcpy`s
  `actual_num_bytes` into it with no bound check — a report larger than 64 bytes
  overruns the stack.
- Media reports are tagged by prepending a `0xFF` marker byte and shifting the
  payload, which re-indexes every downstream parser and collides with a
  legitimate report ID of `0xFF`.
- The media interface is hardcoded to interface index 1.
- `handle_new_device()` calls `vTaskDelay(100ms)` and is reached from `loop()`,
  blocking the main loop.
- When idle it power-cycles the USB root port every 5 seconds via
  `usb_host_lib_set_root_port_power()` — a global action that disrupts any other
  `usb_host` client in the same firmware.

None of these are unfixable, but fixing them means rewriting the client layer,
which is the majority of the component.

### What this repo does differently

- **Subclasses `usb_host::USBClient`** instead of re-registering a raw ESP-IDF
  client, following the pattern ESPHome's own `usb_uart` uses. One client, no
  root-port games, no duplicated enumeration.
- **Trigger-first API** — `on_key` and `on_consumer` with `keycode`, `modifier`,
  and `usage` lambda variables. Sensors are not required.
- **Installs as a normal external component.** No ESPHome fork; your ESPHome
  version stays yours.
- **CI compile gate on every PR** against current ESPHome, so an upstream release
  that breaks the component is caught here rather than in your bedroom at 11pm.
- **release-please tags** you can pin with `ref:`.
- **Handles multiple HID interfaces** rather than assuming interface 1, so
  composite keyboard-plus-consumer remotes work without per-device patches.

### Licensing

ESPHome is dual-licensed: C++/runtime files (`.c`, `.cpp`, `.h`, `.hpp`, `.tcc`,
`.ino`) under **GPLv3**, Python and everything else under **MIT**. Any ESPHome
component's C++ includes ESPHome headers and is therefore a GPLv3 derivative, so
this repo carries ESPHome's `LICENSE` verbatim and inherits the same split.

NonaSuomy's `usb_hidx` lives in a fork of ESPHome with that `LICENSE` file intact
and byte-identical to upstream, so it carries the same terms — GPLv3 for its
C++, MIT for its Python. It is therefore reusable with attribution, which is
noted here and at any point in the source where we drew on it. Our decision not
to build on it was architectural, not legal.

> Note: the separate `esphome-usb-host-hidx-*` documentation repo has no LICENSE
> file of its own. It contains only markdown and a test YAML, but its `backup/`
> folder may hold code copies that would be unlicensed as distributed there.

---

## Configuration

### `usb_hid_keyboard`

| Option | Type | Default | Description |
|---|---|---|---|
| `id` | ID | generated | Component ID. |
| `vid` | hex uint16 | `0x0000` | Vendor ID to match. `0` with `pid: 0` matches **any** device — useful for cheap 2.4 GHz remotes whose VID varies by batch. |
| `pid` | hex uint16 | `0x0000` | Product ID to match. |
| `on_key` | automation | — | Fires on each key **press edge** from the HID Keyboard page. Variables: `keycode` (`uint8_t`), `modifier` (`uint8_t`). |
| `on_consumer` | automation | — | Fires on each non-zero HID Consumer page usage. Variable: `usage` (`uint16_t`). |

Both triggers are optional, and multiple automations may be attached to each.

### `usb_host` is required

An explicit `usb_host:` block must be present — `usb_hid_keyboard` declares it as
a dependency rather than auto-loading it. (Auto-loading registers the module but
never runs its `to_code()`, so `USBHost` is never instantiated and the
`USB_HOST_MAX_REQUESTS` define that `usb_host.h` needs is never emitted. The
config validates and then fails to build.)

An empty block is fine; the defaults are sensible:

```yaml
usb_host:
  enable_hubs: false        # default
  max_transfer_requests: 16 # default
```

Note that `usb_host`'s own options vary by ESPHome version — `max_packet_size`
exists on `dev` but not in 2026.4.0. Check `esphome config` against your
installed version before relying on one.

### Finding your remote's keycodes

Unmapped keys are logged at `WARN`, so the discovery loop is: plug it in, mash
every button, read the log.

```yaml
usb_hid_keyboard:
  on_key:
    - lambda: ESP_LOGW("hid", "key 0x%02X mod 0x%02X", keycode, modifier);
  on_consumer:
    - lambda: ESP_LOGW("hid", "consumer 0x%04X", usage);
```

Keyboard-page codes are USB HID Usage Tables §10; consumer-page usages are §15.

---

## Hardware notes

USB host on the ESP32-S3 uses fixed pins (GPIO19 `D-`, GPIO20 `D+`) — there is no
pin configuration, which is why `usb_host` has no `dp_pin`/`dm_pin` options.

Bus-powered receivers draw from the 5V rail. A 2.4 GHz dongle is fine on an Atom
S3U's USB-A port, but multiple devices or anything hungry wants external power.

---

## Acknowledgements

**[@NonaSuomy](https://github.com/NonaSuomy)** — for
[`usb_hidx`](https://github.com/NonaSuomy/esphome-usb-host-hidx-keyboard-touchpad-mouse-gamepad),
the first working USB HID host implementation for ESPHome, covering keyboards,
mice, gamepads, and 17 device drivers. It proved the thing was possible on
`usb_host` at all, and it established that composite remotes expose media keys on
a **separate HID interface** — the single most useful fact in this problem space,
and one we would otherwise have burned a debugging cycle rediscovering. This
component takes a different architectural path (see
[Why this exists](#why-this-exists)), but it starts from ground NonaSuomy mapped
first.

**[@clydebarrow](https://github.com/clydebarrow)** — for ESPHome's `usb_host` and
`usb_uart` components. This component subclasses `USBClient` and follows
`usb_uart`'s descriptor-walk and async control-transfer state machine closely
enough that reading `usb_uart.cpp` is the best way to understand this code.

**[@markusg1234](https://github.com/markusg1234)** — for
`ESPHome-espidf_ble_keyboard`, forked as
[HomeOps/esphome-blekeyboard](https://github.com/HomeOps/esphome-blekeyboard),
which is the BLE half of the bridge this component was written for and the
template for this repo's CI and release layout.

## License

See [LICENSE](LICENSE) — ESPHome's dual license. C++/runtime files are GPLv3;
Python and everything else is MIT.
