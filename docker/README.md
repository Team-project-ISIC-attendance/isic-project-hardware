# ISIC Project - Docker Test Environment

This folder contains Docker configuration for running a local MQTT broker for testing the ISIC hardware firmware.

## Prerequisites

- [Docker](https://docs.docker.com/get-docker/)
- [Docker Compose](https://docs.docker.com/compose/install/)

## Quick Start

### Start MQTT Broker

```bash
cd docker
docker-compose up -d
```

### Start with Debug Tools (includes MQTT Explorer web UI)

```bash
cd docker
docker-compose --profile debug up -d
```

### Stop Services

```bash
docker-compose down
```

### View Logs

```bash
# Follow mosquitto logs
docker-compose logs -f mosquitto

# Or read the log file directly
cat mosquitto/log/mosquitto.log
```

## Services

| Service | Port | Description |
|---------|------|-------------|
| Mosquitto MQTT | 1883 | Standard MQTT protocol |
| Mosquitto WebSocket | 9001 | MQTT over WebSockets |
| MQTT Explorer (debug) | 4000 | Web UI for debugging |

## Testing with CLI

### Subscribe to all device messages

```bash
# Using mosquitto_sub (install: brew install mosquitto on macOS)
mosquitto_sub -h localhost -p 1883 -t "device/#" -v

# Or from inside the container
docker exec -it isic-mqtt-broker mosquitto_sub -t "device/#" -v
```

### Publish test messages

```bash
# Publish a test attendance record
mosquitto_pub -h localhost -p 1883 -t "device/isic-esp32-001/attendance" \
  -m '{"cardId":"04:A1:B2:C3","timestamp":1699900000}'

# Trigger OTA update command
mosquitto_pub -h localhost -p 1883 -t "device/isic-esp32-001/ota/set" \
  -m '{"action":"check"}'

# Send config update
mosquitto_pub -h localhost -p 1883 -t "device/isic-esp32-001/config/set" \
  -m '{"attendance":{"debounceMs":3000}}'
```

### Subscribe to specific topics

```bash
# Health status
mosquitto_sub -h localhost -p 1883 -t "device/+/health" -v

# OTA status
mosquitto_sub -h localhost -p 1883 -t "device/+/ota/status" -v

# All status updates
mosquitto_sub -h localhost -p 1883 -t "device/+/status" -v
```

## Firmware Configuration

Update your firmware's `AppConfig` to point to the Docker MQTT broker:

```cpp
// In AppConfig.hpp or your config initialization
cfg.mqtt.broker = "YOUR_HOST_IP";  // e.g., "192.168.1.100"
cfg.mqtt.port = 1883;
```

> **Note**: Use your host machine's actual IP address (not `localhost`) when running
> firmware on ESP32, as the device needs to reach the broker over the network.

Find your IP:
```bash
# macOS
ipconfig getifaddr en0

# Linux
hostname -I | awk '{print $1}'
```

## Topic Structure

The ISIC firmware uses the following MQTT topic structure:

```
device/<device_id>/
├── attendance          # Attendance records (single)
├── attendance/batch    # Batched attendance records
├── status              # Device online/offline status
├── health              # Health check reports
├── config/
│   ├── set             # Receive config commands
│   └── status          # Config acknowledgments
├── ota/
│   ├── set             # Receive OTA commands
│   ├── status          # OTA state machine status
│   ├── progress        # Download progress
│   └── error           # Error messages
├── pn532/status        # PN532 reader status
├── metrics             # Performance metrics
└── modules             # Module status
```

## Troubleshooting

### Cannot connect to broker

1. Check if container is running:
   ```bash
   docker ps | grep mosquitto
   ```

2. Check broker logs:
   ```bash
   docker-compose logs mosquitto
   ```

3. Verify port is available:
   ```bash
   lsof -i :1883
   ```

### ESP32 cannot connect

1. Ensure ESP32 and host are on the same network
2. Check firewall settings allow port 1883
3. Use host IP address, not `localhost`
4. Verify WiFi credentials in firmware config

### Permission issues with volumes

```bash
# Fix permissions on macOS/Linux
chmod -R 755 mosquitto/
```

## Security Warning

⚠️ This configuration allows **anonymous connections** and is intended for **local development only**.

For production deployments, configure:
- Authentication (username/password)
- TLS/SSL encryption
- Access Control Lists (ACL)
