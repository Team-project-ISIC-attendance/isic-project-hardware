"""
PlatformIO Post-Build Script for OTA Deployment

Usage:
  pio run              -> Build only
  pio run -t upload    -> Build + upload to OTA server

Settings in platformio.ini:
  flash_method = serial | ota
  ota_server_url = http://<server-ip>:8081/upload
"""

Import("env")
import shutil
import json
import hashlib
import os
import sys
from pathlib import Path
from datetime import datetime

try:
    import requests
    HAS_REQUESTS = True
except ImportError:
    HAS_REQUESTS = False


def get_firmware_version(env):
    version = env.GetProjectOption("custom_firmware_version", None)
    if version:
        return version
    for flag in env.get("BUILD_FLAGS", []):
        if "FIRMWARE_VERSION" in str(flag):
            import re
            match = re.search(r'FIRMWARE_VERSION[=\\"]+"?([0-9.]+)', str(flag))
            if match:
                return match.group(1)
    return "0.0.0"


def post_firmware_to_server(firmware_path: Path, manifest: dict, server_url: str):
    """POST firmware to remote OTA server"""
    if not HAS_REQUESTS:
        print("[OTA] ERROR: requests library not installed")
        print("[OTA] Run: pip install requests")
        return False

    print(f"\n[OTA] Uploading to {server_url}...")
    print(f"[OTA] Python: {sys.executable}")

    # Test connection first with socket
    import socket
    try:
        from urllib.parse import urlparse
        parsed = urlparse(server_url)
        host = parsed.hostname
        port = parsed.port or 80
        print(f"[OTA] Testing connection to {host}:{port}...")
        sock = socket.create_connection((host, port), timeout=5)
        sock.close()
        print(f"[OTA] Socket connection OK")
    except Exception as e:
        print(f"[OTA] Socket connection failed: {e}")

    # Test HTTP connection
    try:
        health_url = server_url.rsplit('/', 1)[0] + '/health'
        test = requests.get(health_url, timeout=10)
        print(f"[OTA] Server health: {test.status_code}")
    except Exception as e:
        print(f"[OTA] Server health check failed: {e}")

    # Retry up to 3 times
    max_retries = 3
    timeout = 120  # 2 minutes for large files

    for attempt in range(1, max_retries + 1):
        try:
            print(f"[OTA] Attempt {attempt}/{max_retries}...")
            # Read file into memory
            firmware_data = firmware_path.read_bytes()
            files = {'firmware': ('firmware.bin', firmware_data, 'application/octet-stream')}
            data = {
                'version': manifest['version'],
                'md5': manifest['md5'],
                'size': str(manifest['size']),
                'board': manifest['board']
            }
            response = requests.post(server_url, files=files, data=data, timeout=timeout)

            if response.status_code in (200, 201):
                print(f"[OTA] Upload OK! v{manifest['version']} ({manifest['size']} bytes)")
                return True
            else:
                print(f"[OTA] Upload FAILED! Status: {response.status_code}")
                print(f"[OTA] Response: {response.text[:200]}")
                if attempt < max_retries:
                    print(f"[OTA] Retrying...")
                    continue
                return False

        except requests.exceptions.Timeout:
            print(f"[OTA] Timeout on attempt {attempt}")
            if attempt < max_retries:
                print(f"[OTA] Retrying...")
                continue
        except requests.exceptions.ConnectionError as e:
            print(f"[OTA] Connection error on attempt {attempt}: {e}")
            if attempt < max_retries:
                import time
                time.sleep(2)  # Wait before retry
                print(f"[OTA] Retrying...")
                continue
        except Exception as e:
            print(f"[OTA] ERROR: {e}")
            return False

    print(f"[OTA] Failed after {max_retries} attempts")
    print(f"[OTA] Make sure Docker is running on server PC")
    return False


def deploy_firmware(source, target, env):
    """Deploy firmware - runs before upload"""

    firmware_path = Path(env.subst("$BUILD_DIR")) / "firmware.bin"
    if not firmware_path.exists():
        print(f"[OTA] Firmware not found: {firmware_path}")
        return

    project_dir = Path(env.get("PROJECT_DIR", "."))
    deploy_dir = project_dir / "ota" / "firmware"
    deploy_dir.mkdir(parents=True, exist_ok=True)

    version = get_firmware_version(env)
    board = env.get("BOARD", "unknown")
    flash_method = os.getenv("FLASH_METHOD", env.GetProjectOption("flash_method", "serial") or "serial").strip().lower()

    print(f"\n{'='*60}")
    print(f"OTA Deployment - {flash_method.upper()} mode")
    print(f"{'='*60}")
    print(f"Version: {version}")
    print(f"Board: {board}")

    # Copy firmware locally
    dest_firmware = deploy_dir / "firmware.bin"
    shutil.copy(firmware_path, dest_firmware)

    # Calculate MD5
    md5_hash = hashlib.md5(firmware_path.read_bytes()).hexdigest()
    file_size = firmware_path.stat().st_size

    print(f"Size: {file_size} bytes")
    print(f"MD5: {md5_hash}")

    manifest = {
        "version": version,
        "file": "firmware.bin",
        "md5": md5_hash,
        "size": file_size,
        "board": board,
        "timestamp": datetime.utcnow().isoformat() + "Z"
    }

    manifest_path = deploy_dir / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2))

    # If OTA mode, POST to server
    if flash_method == "ota":
        server_url = env.GetProjectOption("ota_server_url", "") or os.getenv("OTA_SERVER_URL", "")
        if server_url:
            post_firmware_to_server(firmware_path, manifest, server_url)
        else:
            print("[OTA] No ota_server_url set in platformio.ini")

    print(f"{'='*60}\n")


env.AddPreAction("upload", deploy_firmware)
