"""
PlatformIO post-build script.

After every build, copies the firmware binary to a predictable output location:
  dist/<env>/firmware.bin
  dist/<env>/manifest.json

To push an OTA update: upload firmware.bin via the admin UI, then flash from there.
"""

Import("env")  # noqa: F821  (PlatformIO global)

import hashlib
import json
import shutil
import zipfile
from datetime import datetime, timezone
from pathlib import Path


def _version(env) -> str:
    return env.GetProjectOption("custom_firmware_version", "0.0.0") or "0.0.0"


def _board_name(env_name: str) -> str:
    if "esp8266" in env_name:
        return "esp8266"
    if "esp32" in env_name:
        return "esp32"
    return "unknown"


def post_build(source, target, env):
    fw_src = Path(env.subst("$BUILD_DIR")) / "firmware.bin"
    if not fw_src.exists():
        return

    version = _version(env)
    env_name = env["PIOENV"]
    board = _board_name(env_name)

    out_dir = Path(env.subst("$PROJECT_DIR")) / "dist" / env_name
    out_dir.mkdir(parents=True, exist_ok=True)

    out_bin = out_dir / "firmware.bin"
    shutil.copy(fw_src, out_bin)

    data = fw_src.read_bytes()
    md5 = hashlib.md5(data).hexdigest()
    size = len(data)

    manifest = {
        "version": version,
        "board": board,
        "file": "firmware.bin",
        "md5": md5,
        "size": size,
        "timestamp": datetime.now(timezone.utc).isoformat(),
    }
    manifest_path = out_dir / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2))

    zip_name = f"{version}-{board}.zip"
    zip_path = out_dir / zip_name
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.write(out_bin, "firmware.bin")
        zf.write(manifest_path, "manifest.json")

    print(f"\n[BUILD] {env_name}  v{version}  {size // 1024} KB  md5:{md5[:8]}...")
    print(f"[BUILD] -> {out_bin}")
    print(f"[BUILD]    {zip_path.name}  (firmware.bin + manifest.json)")
    print(f"[BUILD]    Upload {zip_name} via the admin UI to push an OTA update.")


env.AddPostAction("$BUILD_DIR/firmware.bin", post_build)
