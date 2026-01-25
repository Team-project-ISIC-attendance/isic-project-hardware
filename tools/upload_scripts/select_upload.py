"""
PlatformIO pre-build script to select upload method.

Use in platformio.ini:
  flash_method = serial | ota

- serial: Normal USB/serial upload
- ota: Disables upload (build only), deploy_ota.py will POST firmware

Override via env var:
  FLASH_METHOD=ota pio run
"""

Import("env")
import os


def normalize(value: str) -> str:
    return str(value).strip().lower()


method = normalize(os.getenv("FLASH_METHOD", env.GetProjectOption("flash_method", "serial") or "serial"))

if method == "ota":
    # Disable upload - we use custom HTTP OTA, not espota
    env.Replace(UPLOAD_PROTOCOL="custom")
    env.Replace(UPLOADCMD="echo 'OTA mode: upload disabled, use deploy_ota.py'")
    print("[upload] method=ota (upload disabled, deploy_ota.py will handle)")
elif method in ("serial", "uart", "usb"):
    print("[upload] method=serial")
else:
    print(f"[upload] unknown flash_method='{method}', using default")
