# User Manual

## Purpose

This firmware turns an ESP8266/ESP32 + PN532 device into an attendance reader for passive ISIC cards. The reader is designed for classroom use, where many cards may be presented in a short burst and the device must still save battery when the room becomes quiet.

## Typical User Flow

1. Power the device.
2. Ensure the reader is configured to reach the target Wi-Fi network and MQTT backend.
3. Present an ISIC card to the PN532 reader area.
4. The reader captures the card, records attendance, and uploads or buffers the event.

## What The Device Does

- Reads passive ISIC cards through the PN532 reader
- Buffers attendance locally when Wi-Fi or MQTT is unavailable
- Reconnects and flushes buffered attendance when connectivity returns
- Uses smart power behavior to stay responsive during busy classes and save battery when idle

## Hardware Checklist

- ESP8266 or ESP32 board flashed with this firmware
- PN532 wired in SPI mode
- PN532 IRQ connected to the configured ESP GPIO
- Stable power source or battery pack
- Wi-Fi network available for normal online operation

## Power Behavior

The device does not use ESP deep sleep in the main runtime flow.

Instead it works in stages:

- After a card read, both ESP and PN532 stay fully active for a short hold period.
- During repeated scans, the device enters burst behavior and stays fully responsive.
- After a short quiet period, PN532 sleeps first.
- After a longer quiet period, ESP Wi-Fi drops into lighter power modes.
- During long quiet periods, the device can buffer attendance offline and reconnect later.

This design reduces battery usage without making the first student wait for a second card tap.

## Configuration Expectations

The reader needs working values for:

- Wi-Fi station credentials
- MQTT broker address and topic base
- device and location identifiers
- PN532 pin configuration

If the station network is not available or the device is not configured, the firmware can operate in access-point based setup mode depending on configuration state.

## Operational Notes

- Card reading remains the most important event in the system.
- If connectivity is down, attendance is not lost immediately; it is buffered first.
- The PN532 IRQ line is important for first-tap wake behavior after idle.
- If a class is actively scanning cards, the reader will try to stay awake instead of repeatedly sleeping and waking.

## Troubleshooting

### Reader powers on but no cards are detected

Check:

- PN532 wiring
- SPI pins
- IRQ pin wiring
- whether PN532 is in SPI mode

### Cards scan but are not uploaded

Check:

- Wi-Fi association
- MQTT broker reachability
- device configuration values

Buffered attendance may still be stored locally until connectivity returns.

### Device seems slow after long idle

Check:

- Wi-Fi reconnect quality
- battery voltage stability
- PN532 IRQ wiring

Without a correct IRQ wake path, polling fallback may increase latency.

## For Advanced Setup And Maintenance

Use:

- [Developer Manual](DEVELOPER_MANUAL.md)
- [Architecture And Layers](ARCHITECTURE.md)
