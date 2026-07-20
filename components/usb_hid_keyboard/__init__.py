import esphome.codegen as cg
from esphome import automation
from esphome.components import esp32
from esphome.components.usb_host import register_usb_client
import esphome.config_validation as cv
from esphome.const import CONF_TRIGGER_ID
from esphome.cpp_types import Component

# Resolved by name rather than imported directly: the set of USB-capable
# variants grows between ESPHome releases (S31 and H4 exist on dev but not in
# 2026.4.0), and a hard import of a not-yet-released constant breaks the
# component on every older version.
_USB_CAPABLE_VARIANTS = (
    "VARIANT_ESP32S2",
    "VARIANT_ESP32S3",
    "VARIANT_ESP32S31",
    "VARIANT_ESP32P4",
    "VARIANT_ESP32H4",
)
SUPPORTED_VARIANTS = [
    getattr(esp32, name) for name in _USB_CAPABLE_VARIANTS if hasattr(esp32, name)
]

# usb_host must be DEPENDENCIES, not AUTO_LOAD. Auto-loading registers the
# module but never runs its to_code(), so USBHost is never instantiated and the
# USB_HOST_MAX_REQUESTS / USB_HOST_MAX_PACKET_SIZE defines that usb_host.h
# relies on are never emitted. The config would validate and then fail to build
# (or build and do nothing). An explicit `usb_host:` block is required.
DEPENDENCIES = ["esp32", "usb_host"]
CODEOWNERS = ["@ocalvo"]

CONF_VID = "vid"
CONF_PID = "pid"
CONF_ON_KEY = "on_key"
CONF_ON_CONSUMER = "on_consumer"

usb_hid_keyboard_ns = cg.esphome_ns.namespace("usb_hid_keyboard")
USBHIDKeyboard = usb_hid_keyboard_ns.class_("USBHIDKeyboard", Component)

KeyTrigger = usb_hid_keyboard_ns.class_(
    "KeyTrigger", automation.Trigger.template(cg.uint8, cg.uint8)
)
ConsumerTrigger = usb_hid_keyboard_ns.class_(
    "ConsumerTrigger", automation.Trigger.template(cg.uint16)
)

# NOTE: usb_host.usb_device_schema() cannot be used here. It makes vid/pid
# *required* unless a truthy default is passed -- and `0` (our "match any
# device" sentinel) is falsy, so `usb_device_schema(vid=0, pid=0)` still yields
# a required key. The schema is therefore spelled out, but the config it
# produces is still consumed by usb_host.register_usb_client().
CONFIG_SCHEMA = cv.All(
    cv.COMPONENT_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(USBHIDKeyboard),
            cv.Optional(CONF_VID, default=0): cv.hex_uint16_t,
            cv.Optional(CONF_PID, default=0): cv.hex_uint16_t,
            cv.Optional(CONF_ON_KEY): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(KeyTrigger)}
            ),
            cv.Optional(CONF_ON_CONSUMER): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ConsumerTrigger)}
            ),
        }
    ),
    esp32.only_on_variant(supported=SUPPORTED_VARIANTS),
)


async def to_code(config):
    # register_usb_client() reads CONF_ID / CONF_VID / CONF_PID and does the
    # cg.new_Pvariable + register_component dance for us.
    var = await register_usb_client(config)

    for conf in config.get(CONF_ON_KEY, ()):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(
            trigger, [(cg.uint8, "keycode"), (cg.uint8, "modifier")], conf
        )

    for conf in config.get(CONF_ON_CONSUMER, ()):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(cg.uint16, "usage")], conf)
