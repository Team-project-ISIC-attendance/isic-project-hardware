# Battery Consumption Estimates

This document describes the smart dual-power reader-idle model implemented in the firmware.

All numbers below are estimates, not lab measurements. They should be treated as planning values until the hardware is measured with an inline power meter on the real ISIC reader board.

## States

| State | WiFi | PN532 | Estimated Current | Notes |
|------|------|-------|-------------------|-------|
| `Active + ActiveScan` | Connected | Fast polling | `70-90 mA` | Busy lesson burst, immediate uploads |
| `Active + LowRateScan` | Connected | Awake, reduced polling cadence | `45-65 mA` | Short quiet gap after the ready-hold |
| `LightSleep + LowRateScan` | Associated / power-save | Awake, medium polling cadence | `25-40 mA` | Medium quiet period |
| `ModemSleep + LowRateScan` | Off | Awake, slow polling cadence | `12-25 mA` | Long quiet period with offline buffering |

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
2. After `10s` without another card, the reader reduces PN532 polling cadence while ESP stays connected.
3. After `30s` without relevant activity, the ESP enters `LightSleep`.
4. After `5 min` without relevant activity, the ESP escalates to `ModemSleep`.
5. The first card after idle is still detected by polling, with a longer but bounded delay.
6. If WiFi was in `ModemSleep`, upload happens after reconnect; attendance is buffered locally meanwhile.

## Runtime Formula

Average current can be estimated as:

```text
Iavg =
  (TactiveScan * IactiveScan +
   Tpn532Idle  * Ipn532Idle +
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
- `10 min` ESP active with reduced PN532 polling between groups
- `10 min` in `LightSleep`
- `25 min` in `ModemSleep`

Using:

- `IactiveScan = 80 mA`
- `Ipn532Idle = 55 mA`
- `Ilight = 24 mA`
- `Imodem = 5 mA`

Then:

```text
Iavg = (15*80 + 10*55 + 10*30 + 25*18) / 60
Iavg = 41.7 mA
```

A `2000 mAh` battery would last about:

```text
2000 / 41.7 = 48.0 hours
```

### Mostly Idle School Day

Assume eight hours with:

- `40 min` active
- `40 min` ESP active with reduced PN532 polling
- `40 min` light sleep
- `360 min` modem sleep

Using the same current estimates:

```text
Iavg = (40*80 + 40*55 + 40*30 + 360*18) / 480
Iavg = 27.0 mA
```

A `2000 mAh` battery would last about:

```text
2000 / 27.0 = 74.1 hours
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
   - IRQ wakeup enabled, if that hardware path is still supported
   - Polling-only low-power mode enabled
5. Record min, max, and average current.
6. Update this file with measured values and the exact board revision.

## Validation Checklist

- Confirm `PowerService` metrics show `burst_entries`, `light_sleep_entries`, and `modem_sleep_entries`.
- Confirm `Pn532Service` metrics show `reads_successful` increasing normally after idle and no repeated `Failed to send PowerDown command` loop.
- Confirm no deep-sleep behavior remains in the deployed firmware path.
- Confirm the first ISIC tap after idle is captured without requiring a second presentation.
