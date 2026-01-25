# ISIC Project - Development Tools

This directory contains various development and testing tools for the ISIC project.

## Tools Overview

| Tool | Description |
|------|-------------|
| [mqtt-broker/](mqtt-broker/) | Docker-based MQTT broker for local testing |
| [esp_fs_inspector.py](esp_fs_inspector.py) | Python utility to inspect ESP filesystem over serial |

---

# ESP Filesystem Inspector

A Python utility to inspect the LittleFS filesystem on your ESP8266/ESP32 device over serial connection.

## Features

- List all files in the filesystem with sizes
- Read and display file contents (with JSON pretty-printing)
- View filesystem information (total/used/free space)
- Tail last N lines of log files
- Monitor files in real-time (like `tail -f`)
- Auto-detect available serial ports

## Firmware Requirements

**IMPORTANT**: The filesystem inspector is only available when the firmware is compiled with the `ISIC_ENABLE_FS_INSPECTOR` flag.

### Debug Builds (Inspector Enabled by Default)
```bash
# ESP8266 debug build (includes filesystem inspector)
pio run -e esp8266_debug --target upload

# ESP32 debug build (includes filesystem inspector)
pio run -e esp32dev_debug --target upload
```

### Production Builds (Inspector Disabled by Default)
By default, production builds do NOT include the filesystem inspector for security and performance reasons.

To enable it temporarily in production (use with caution):
```bash
# Add to platformio.ini [env:esp8266] section:
build_flags =
    ${env.build_flags}
    -DISIC_ENABLE_FS_INSPECTOR=1

# Then build and upload
pio run -e esp8266 --target upload
```

**Security Warning**: Only enable the filesystem inspector in production if you control physical access to the device's serial port. It provides direct read access to all files on the device.

## Installation

1. Install Python dependencies:
```bash
pip install -r requirements.txt
```

Or install pyserial directly:
```bash
pip install pyserial
```

## Usage

### List available serial ports
```bash
python esp_fs_inspector.py --list-ports
```

### List all files in filesystem
```bash
python esp_fs_inspector.py --port /dev/cu.usbserial-0001 list
```

### Read configuration file
```bash
python esp_fs_inspector.py --port /dev/cu.usbserial-0001 cat /config.json
```

This will pretty-print JSON files automatically.

### Read file raw contents
```bash
python esp_fs_inspector.py --port /dev/cu.usbserial-0001 read /config.json
```

### View last 20 lines of a log file
```bash
python esp_fs_inspector.py --port /dev/cu.usbserial-0001 tail /logs/device.log --lines 20
```

### Monitor a log file in real-time
```bash
python esp_fs_inspector.py --port /dev/cu.usbserial-0001 monitor /logs/device.log
```

Press Ctrl+C to stop monitoring.

### Get filesystem information
```bash
python esp_fs_inspector.py --port /dev/cu.usbserial-0001 info
```

Shows total size, used space, free space, and lists all files.

## Port Names by Platform

- **macOS**: `/dev/cu.usbserial-*` or `/dev/cu.SLAB_USBtoUART`
- **Linux**: `/dev/ttyUSB0` or `/dev/ttyACM0`
- **Windows**: `COM3`, `COM4`, etc.

## Custom Baud Rate

Default is 115200. To use a different baud rate:
```bash
python esp_fs_inspector.py --port /dev/cu.usbserial-0001 --baudrate 9600 list
```

## How It Works

The tool sends commands over serial to the ESP device with the prefix `FS_CMD:`. The firmware's `FilesystemCommandHandler` processes these commands and responds with data prefixed by `FS_RESP:`.

Commands sent to the device:
- `FS_CMD:LIST /` - List files in root directory
- `FS_CMD:READ /config.json` - Read file contents
- `FS_CMD:INFO` - Get filesystem info

## Troubleshooting

### "No response from device"

Make sure:
1. **Your firmware is compiled with `ISIC_ENABLE_FS_INSPECTOR=1` flag** (enabled in debug builds by default)
2. The device is connected and the correct port is selected
3. No other program (like a serial monitor) is using the serial port
4. The baud rate matches (default 115200)

If you're using a production build (esp8266 or esp32dev), the inspector is disabled by default. Upload a debug build or add the flag to your production build configuration.

### Permission denied on Linux

Add your user to the dialout group:
```bash
sudo usermod -a -G dialout $USER
```

Then log out and back in.

## Example Session

```bash
# Find your device
$ python esp_fs_inspector.py --list-ports
Available serial ports:
  /dev/cu.usbserial-0001 - USB Serial

# Check filesystem info
$ python esp_fs_inspector.py --port /dev/cu.usbserial-0001 info
Connected to /dev/cu.usbserial-0001 at 115200 baud
Getting filesystem info...

Filesystem Information:
  Total size: 1024000 bytes
  Used size: 4096 bytes
  Free size: 1019904 bytes
  Block size: 4096 bytes
  ...

# View config
$ python esp_fs_inspector.py --port /dev/cu.usbserial-0001 cat /config.json
Connected to /dev/cu.usbserial-0001 at 115200 baud
Reading '/config.json'...

============================================================
Content of /config.json:
============================================================
{
  "wifi": {
    "ssid": "MyNetwork",
    "password": "********"
  },
  "mqtt": {
    "broker": "mqtt.example.com",
    "port": 1883
  }
}
============================================================
```

## Future Log Files

When you implement logging to files, you can monitor them in real-time:

```bash
# Watch logs as they're written
python esp_fs_inspector.py --port /dev/cu.usbserial-0001 monitor /logs/system.log

# Or check recent entries
python esp_fs_inspector.py --port /dev/cu.usbserial-0001 tail /logs/error.log --lines 50
```

---

# OTA Firmware Server

A Docker-based OTA (Over-The-Air) update server for delivering firmware updates to devices over HTTP.

This setup provides:
- Firmware file hosting for devices  
- HTTP API for uploading new firmware versions  
- Shared storage between upload service and firmware server

---

## Architecture

The OTA system runs two services:

| Service | Purpose |
|---------|---------|
| **ota-server** | Serves firmware files and `manifest.json` to devices |
| **ota-upload** | Accepts firmware uploads via HTTP API |

Both services use a shared Docker volume where firmware files are stored. Once uploaded, firmware becomes immediately available to devices.

---

## Requirements

- Docker  
- Docker Compose  

Verify installation:
```bash
docker --version
docker compose version
```

---

## Running the OTA Server

Go to the directory containing `docker-compose.yml` and start the services:

```bash
docker compose up -d
```

This will start:

| Service | Port | Description |
|--------|------|-------------|
| OTA Server | **8080** | Devices download firmware and manifest |
| OTA Upload API | **8081** | Upload new firmware files |

Devices retrieve update information from:

```
GET http://<server-ip>:8080/manifest.json
```

---

## Uploading Firmware

Upload a new firmware binary using:

```bash
curl -X POST http://localhost:8081/upload \
  -F "file=@firmware.bin"
```

After upload:
- The file is stored in the shared firmware volume  
- The OTA server can immediately serve it to devices  
- No container restart is required

---

## Firmware Storage

Firmware files are stored in the Docker volume:

```
ota-firmware
```

Used by:
- **ota-upload** — writes firmware files  
- **ota-server (nginx)** — serves them over HTTP

---

## Verifying Operation

Check running containers:
```bash
docker ps
```

Check OTA server:
```bash
curl http://localhost:8080/
```

Check upload endpoint:
```bash
curl http://localhost:8081/upload
```

---

## Stopping the Server

```bash
docker compose down
```

