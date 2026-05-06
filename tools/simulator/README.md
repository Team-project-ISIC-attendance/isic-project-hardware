# ISIC Hardware Emulator

Browser-based Python UI for emulating the MQTT hardware device without real ESP8266 hardware.

This tool is separate from the production root `docker-compose.yml`. Run it on
its own when you want one or more fake hardware devices for testing.

## Features

- Live MQTT traffic monitor subscribed to `#`
- Virtual devices with configurable `baseTopic` and `deviceId`
- Attendance scan publishing with batched records and sequence numbers
- Manual publish for `status`, `health`, `metrics`, `config`, raw topics, and OTA topics
- Automatic replies to backend control topics:
  - `status/request`
  - `health/request`
  - `metrics/request`
  - `config/get`
  - `config/get/<section>`
  - `config/set`
  - `config/set/<section>`
  - `ota/start`

## Local run

```bash
cd hardware/tools/simulator
pip install .
uvicorn app.main:app --reload --port 8040
```

## Docker run

```bash
cd hardware/tools/simulator
docker compose up --build
```

Open `http://localhost:8040`.

To point it at the main project broker running from the repository root:

```bash
cd hardware/tools/simulator
MQTT_BROKER_HOST=host.docker.internal docker compose up --build
```

To run multiple simulators at once, change at least the HTTP port and usually
the default device ID too:

```bash
cd hardware/tools/simulator
EMULATOR_HTTP_PORT=8041 EMULATOR_DEFAULT_DEVICE_ID=ISIC-ESP8266-002 docker compose up --build
```

Environment variables:

- `EMULATOR_HTTP_PORT` default `8040`
- `MQTT_BROKER_HOST` default `localhost`
- `MQTT_BROKER_PORT` default `1883`
- `EMULATOR_DEFAULT_BASE_TOPIC` default `device`
- `EMULATOR_DEFAULT_DEVICE_ID` default `ISIC-ESP8266-001`
