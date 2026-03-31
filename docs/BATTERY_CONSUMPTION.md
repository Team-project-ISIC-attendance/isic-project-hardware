# Battery Consumption Estimates

This document describes the smart dual-power reader-idle model implemented in the firmware.

All numbers below are estimates, not lab measurements. They should be treated as planning values until the hardware is measured with an inline power meter on the real ISIC reader board.

## States

| State | WiFi | PN532 | Estimated Current | Notes |
|------|------|-------|-------------------|-------|
| `Active + ActiveScan` | Connected | Reading | `70-90 mA` | Busy lesson burst, immediate uploads |
| `Active + PowerDown` | Connected | `PowerDown` with IRQ wake | `35-55 mA` | Short quiet gap, PN532 sleeps before WiFi does |
| `LightSleep + PowerDown` | Associated / power-save | `PowerDown` with IRQ wake | `18-30 mA` | Medium quiet period |
| `ModemSleep + PowerDown` | Off | `PowerDown` with IRQ wake | `2-8 mA` | Long quiet period with offline buffering |

## Default Policy

The firmware defaults to:

- `readerIdleTimeoutMs = 30000`
- `modemSleepAfterMs = 300000`
- `pn532SleepAfterMs = 10000`
- `readerReadyHoldMs = 5000`
- `burstWindowMs = 15000`
- `burstScanCount = 3`
- `burstHoldMs = 45000`
- `activityTypeMask = 0b00001`
- `pn532SleepBetweenScans = true`
- `modemSleepOnMqttDisconnect = true`

That means:

1. After a card read, both ESP and PN532 stay fully active for the short ready-hold.
2. After `10s` without another card, PN532 enters `PowerDown` first while ESP stays connected.
3. After `30s` without relevant activity, the ESP enters `LightSleep`.
4. After `5 min` without relevant activity, the ESP escalates to `ModemSleep`.
5. The first card after idle wakes PN532 and is read immediately.
6. If WiFi was in `ModemSleep`, upload happens after reconnect; attendance is buffered locally meanwhile.

## Runtime Formula

Average current can be estimated as:

```text
Iavg =
  (TactiveScan * IactiveScan +
   Tpn532Sleep * Ipn532Sleep +
   Tlight      * Ilight +
   Tmodem      * Imodem) / Ttotal
```

Battery runtime in hours:

```text
RuntimeHours = BatteryCapacitymAh / Iavg
```

Battery runtime in days:

```text
RuntimeDays = RuntimeHours / 24
```

## Example Scenarios

### Busy Lesson Block

Assume one hour with:

- `15 min` active card traffic
- `10 min` ESP active with PN532 asleep between groups
- `10 min` in `LightSleep`
- `25 min` in `ModemSleep`

Using:

- `IactiveScan = 80 mA`
- `Ipn532Sleep = 45 mA`
- `Ilight = 24 mA`
- `Imodem = 5 mA`

Then:

```text
Iavg = (15*80 + 10*45 + 10*24 + 25*5) / 60
Iavg = 33.6 mA
```

A `2000 mAh` battery would last about:

```text
2000 / 33.6 = 59.5 hours
```

### Mostly Idle School Day

Assume eight hours with:

- `40 min` active
- `40 min` ESP active with PN532 asleep
- `40 min` light sleep
- `360 min` modem sleep

Using the same current estimates:

```text
Iavg = (40*80 + 40*45 + 40*24 + 360*5) / 480
Iavg = 16.2 mA
```

A `2000 mAh` battery would last about:

```text
2000 / 16.2 = 123.5 hours
```

## What Changes Consumption Most

- WiFi reconnect frequency after long idle
- LED and buzzer feedback usage
- MQTT retry behavior when network is unavailable
- PN532 polling fallback when IRQ is not wired or disabled
- Board-level regulators, USB serial chips, and indicator LEDs

## Measurement Procedure

Use this process before publishing any final battery-life claims:

1. Power the board from a bench supply or USB power meter that can log current.
2. Disable OTA traffic and unnecessary debug logging.
3. Measure at least these states for `60s` each:
   - Fresh boot idle before `30s`
   - `LightSleep`
   - `ModemSleep`
   - Card wake from `LightSleep`
   - Card wake from `ModemSleep`
4. Repeat with:
   - WiFi available
   - WiFi unavailable
   - IRQ mode enabled
   - Polling fallback enabled
5. Record min, max, and average current.
6. Update this file with measured values and the exact board revision.

## Validation Checklist

- Confirm `PowerService` metrics show `burst_entries`, `light_sleep_entries`, and `modem_sleep_entries`.
- Confirm `Pn532Service` metrics show `early_sleep_entries`, `irq_wakeups`, and `sleep_wake_reads` increasing after the first idle card.
- Confirm no deep-sleep behavior remains in the deployed firmware path.
- Confirm the first ISIC tap after idle is captured without requiring a second presentation.
