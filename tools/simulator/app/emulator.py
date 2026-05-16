import asyncio
import json
import os
from collections import deque
from datetime import UTC, datetime
from typing import Any

from aiomqtt import Client, MqttError

from .models import (
    AttendancePublishRequest,
    ConfigPublishRequest,
    DeviceUpsertRequest,
    OtaPublishRequest,
    RawPublishRequest,
    SimulatedDevice,
    SnapshotPublishRequest,
    TrafficEvent,
    CONFIG_SECTION_KEYS,
    device_key,
    topic_for,
    utc_now_iso,
)

EVENT_LIMIT = 400
RECONNECT_DELAY_SECONDS = 3


def _json_dumps(payload: Any) -> str:
    return json.dumps(payload, indent=2, sort_keys=True)


def _decode_payload(payload: bytes | bytearray | str | object) -> str:
    if isinstance(payload, bytes):
        return payload.decode("utf-8", errors="replace")
    if isinstance(payload, bytearray):
        return bytes(payload).decode("utf-8", errors="replace")
    if isinstance(payload, str):
        return payload
    return str(payload)


class HardwareEmulator:
    def __init__(self) -> None:
        self._hostname = os.getenv("MQTT_BROKER_HOST", "localhost")
        self._port = int(os.getenv("MQTT_BROKER_PORT", "1883"))
        self._username = os.getenv("MQTT_USERNAME") or None
        self._password = os.getenv("MQTT_PASSWORD") or None
        self._client_id = os.getenv("EMULATOR_CLIENT_ID", "isic-hardware-emulator")
        self._devices: dict[str, SimulatedDevice] = {}
        self._events: deque[TrafficEvent] = deque(maxlen=EVENT_LIMIT)
        self._seen_topics: set[str] = set()
        self._lock = asyncio.Lock()
        self._running = False
        self._task: asyncio.Task[None] | None = None
        self._ensure_default_device()

    def _ensure_default_device(self) -> None:
        default_base_topic = os.getenv("EMULATOR_DEFAULT_BASE_TOPIC", "device")
        default_device_id = os.getenv(
            "EMULATOR_DEFAULT_DEVICE_ID",
            "ISIC-ESP8266-001",
        )
        device = SimulatedDevice(
            base_topic=default_base_topic,
            device_id=default_device_id,
        )
        self._devices[device.key] = device

    async def start(self) -> None:
        self._running = True
        self._task = asyncio.create_task(self._mqtt_loop())
        self._append_event(
            direction="system",
            topic="emulator/start",
            payload="MQTT listener started",
            note=f"broker={self._hostname}:{self._port}",
        )

    async def stop(self) -> None:
        self._running = False
        if self._task is not None:
            self._task.cancel()
            try:
                await self._task
            except asyncio.CancelledError:
                pass
            self._task = None

    async def state(self) -> dict[str, Any]:
        async with self._lock:
            devices = [device.as_dict() for device in self._devices.values()]
            events = [event.as_dict() for event in self._events]
            seen_topics = sorted(self._seen_topics)

        devices.sort(key=lambda item: item["key"])
        return {
            "broker_host": self._hostname,
            "broker_port": self._port,
            "devices": devices,
            "events": events,
            "seen_topics": seen_topics,
        }

    async def clear_events(self) -> None:
        async with self._lock:
            self._events.clear()
            self._seen_topics.clear()
        self._append_event(
            direction="system",
            topic="emulator/events",
            payload="Traffic history cleared",
        )

    async def upsert_device(self, request: DeviceUpsertRequest) -> dict[str, Any]:
        new_device = SimulatedDevice(
            base_topic=request.base_topic,
            device_id=request.device_id,
            auto_respond=request.auto_respond,
            firmware=request.firmware,
            location_id=request.location_id,
            next_seq=request.next_seq,
            status_payload=request.status_payload or {},
            health_payload=request.health_payload or {},
            metrics_payload=request.metrics_payload or {},
            config_payload=request.config_payload or {},
        )
        async with self._lock:
            if request.previous_key and request.previous_key != new_device.key:
                self._devices.pop(request.previous_key, None)
            self._devices[new_device.key] = new_device
        self._append_event(
            direction="system",
            topic=f"{new_device.prefix}/profile",
            payload="Virtual device saved",
        )
        return new_device.as_dict()

    async def publish_attendance(
        self,
        request: AttendancePublishRequest,
    ) -> dict[str, Any]:
        async with self._lock:
            device = self._get_or_create_device_locked(
                request.base_topic,
                request.device_id,
            )
            records = []
            for uid in request.uids:
                if request.timestamp_mode == "zero":
                    timestamp_ms = 0
                elif request.timestamp_mode == "custom":
                    timestamp_ms = request.timestamp_ms or 0
                else:
                    timestamp_ms = int(datetime.now(UTC).timestamp() * 1000)
                records.append(
                    {
                        "uid": uid,
                        "ts": timestamp_ms,
                        "seq": device.next_seq,
                    }
                )
                device.next_seq += 1
            self._increment_metric(device, "cards_processed", len(records))
            self._increment_metric(device, "batches_sent", 1)

        topic = topic_for(request.base_topic, request.device_id, "attendance")
        payload = _json_dumps(records)
        await self._publish(topic, payload)
        return {
            "topic": topic,
            "records": records,
        }

    async def publish_status(
        self,
        request: SnapshotPublishRequest,
    ) -> dict[str, Any]:
        await self._update_device_payload(
            request.base_topic,
            request.device_id,
            "status",
            request.payload,
        )
        topic = topic_for(request.base_topic, request.device_id, "status")
        await self._publish(topic, _json_dumps(request.payload), retain=True)
        return {"topic": topic}

    async def publish_health(
        self,
        request: SnapshotPublishRequest,
    ) -> dict[str, Any]:
        await self._update_device_payload(
            request.base_topic,
            request.device_id,
            "health",
            request.payload,
        )
        topic = topic_for(request.base_topic, request.device_id, "health")
        await self._publish(topic, _json_dumps(request.payload), retain=True)
        return {"topic": topic}

    async def publish_metrics(
        self,
        request: SnapshotPublishRequest,
    ) -> dict[str, Any]:
        await self._update_device_payload(
            request.base_topic,
            request.device_id,
            "metrics",
            request.payload,
        )
        topic = topic_for(request.base_topic, request.device_id, "metrics")
        await self._publish(topic, _json_dumps(request.payload), retain=True)
        return {"topic": topic}

    async def publish_config(
        self,
        request: ConfigPublishRequest,
    ) -> dict[str, Any]:
        target_section = request.section
        await self._store_config_payload(
            request.base_topic,
            request.device_id,
            request.payload,
            section=target_section,
        )
        suffix = ("config",) if target_section is None else ("config", target_section)
        topic = topic_for(request.base_topic, request.device_id, *suffix)
        await self._publish(topic, _json_dumps(request.payload))
        return {"topic": topic}

    async def publish_raw(self, request: RawPublishRequest) -> dict[str, Any]:
        await self._publish(request.topic, request.payload, retain=request.retain)
        return {"topic": request.topic}

    async def simulate_ota(self, request: OtaPublishRequest) -> dict[str, Any]:
        prefix = topic_for(request.base_topic, request.device_id)
        await self._publish(
            f"{prefix}/ota/update_available",
            request.version,
        )
        for percent in (0, 25, 50, 75, 100):
            await asyncio.sleep(0.15)
            if request.fail_at_percent is not None and percent >= request.fail_at_percent:
                await self._publish(
                    f"{prefix}/ota/error",
                    f"error: simulated failure at {percent}%",
                )
                return {"topic": f"{prefix}/ota/error"}
            await self._publish(f"{prefix}/ota/progress", str(percent))
        await self._publish(f"{prefix}/ota/completed", "success")
        return {"topic": f"{prefix}/ota/completed"}

    async def _mqtt_loop(self) -> None:
        while self._running:
            try:
                async with Client(
                    hostname=self._hostname,
                    port=self._port,
                    identifier=f"{self._client_id}-listener",
                    username=self._username,
                    password=self._password,
                ) as client:
                    await client.subscribe("#")
                    self._append_event(
                        direction="system",
                        topic="emulator/subscribe",
                        payload="Subscribed to #",
                    )
                    async for message in client.messages:
                        if not self._running:
                            break
                        topic = message.topic.value
                        payload = _decode_payload(message.payload)
                        await self._handle_incoming(topic, payload)
            except asyncio.CancelledError:
                raise
            except (MqttError, OSError, ConnectionError) as error:
                self._append_event(
                    direction="system",
                    topic="emulator/error",
                    payload=str(error),
                    note="MQTT listener reconnecting",
                )
                await asyncio.sleep(RECONNECT_DELAY_SECONDS)

    async def _handle_incoming(self, topic: str, payload: str) -> None:
        async with self._lock:
            self._seen_topics.add(topic)
            self._events.append(
                TrafficEvent(
                    timestamp=utc_now_iso(),
                    direction="in",
                    topic=topic,
                    payload=payload,
                )
            )
        await self._maybe_auto_respond(topic, payload)

    async def _maybe_auto_respond(self, topic: str, payload: str) -> None:
        control = self._parse_control_topic(topic)
        if control is None:
            return

        base_topic, current_device_id, action, section = control
        async with self._lock:
            existing_device = self._devices.get(device_key(base_topic, current_device_id))
            if existing_device is None or not existing_device.auto_respond:
                return

            if action == "status_request":
                response_payload = _json_dumps(existing_device.status_payload)
                response_topic = topic_for(base_topic, current_device_id, "status")
                retain = True
            elif action == "health_request":
                response_payload = _json_dumps(existing_device.health_payload)
                response_topic = topic_for(base_topic, current_device_id, "health")
                retain = True
            elif action == "metrics_request":
                response_payload = _json_dumps(existing_device.metrics_payload)
                response_topic = topic_for(base_topic, current_device_id, "metrics")
                retain = True
            elif action == "config_get":
                if section is None:
                    response_payload = _json_dumps(existing_device.config_payload)
                    response_topic = topic_for(base_topic, current_device_id, "config")
                else:
                    response_payload = _json_dumps(
                        existing_device.config_payload.get(section, {})
                    )
                    response_topic = topic_for(
                        base_topic,
                        current_device_id,
                        "config",
                        section,
                    )
                retain = False
            elif action == "config_set":
                try:
                    data = json.loads(payload) if payload else {}
                except json.JSONDecodeError:
                    self._events.append(
                        TrafficEvent(
                            timestamp=utc_now_iso(),
                            direction="system",
                            topic=topic,
                            payload=payload,
                            note="Ignored invalid JSON config/set payload",
                        )
                    )
                    return
                await self._apply_config_update_locked(
                    existing_device,
                    payload=data,
                    section=section,
                )
                return
            elif action == "ota_start":
                version = f"{existing_device.firmware}+ota"
            else:
                return

        if action == "ota_start":
            await self.simulate_ota(
                OtaPublishRequest(
                    base_topic=base_topic,
                    device_id=current_device_id,
                    version=version,
                )
            )
            return

        await self._publish(
            response_topic,
            response_payload,
            retain=retain,
        )

    async def _update_device_payload(
        self,
        base_topic: str,
        device_id: str,
        kind: str,
        payload: dict[str, Any],
    ) -> None:
        async with self._lock:
            device = self._get_or_create_device_locked(base_topic, device_id)
            if kind == "status":
                device.status_payload = payload
            elif kind == "health":
                device.health_payload = payload
            elif kind == "metrics":
                device.metrics_payload = payload

    async def _store_config_payload(
        self,
        base_topic: str,
        device_id: str,
        payload: dict[str, Any],
        *,
        section: str | None,
    ) -> None:
        async with self._lock:
            device = self._get_or_create_device_locked(base_topic, device_id)
            await self._apply_config_update_locked(device, payload=payload, section=section)

    async def _apply_config_update_locked(
        self,
        device: SimulatedDevice,
        *,
        payload: dict[str, Any],
        section: str | None,
    ) -> None:
        current_key = device.key
        if section is None:
            device.config_payload = payload
        elif section in CONFIG_SECTION_KEYS:
            updated = dict(device.config_payload)
            updated[section] = payload
            device.config_payload = updated
        else:
            return

        mqtt_section = device.config_payload.get("mqtt", {})
        device_section = device.config_payload.get("device", {})
        if isinstance(mqtt_section.get("baseTopic"), str):
            device.base_topic = mqtt_section["baseTopic"]
        if isinstance(device_section.get("deviceId"), str):
            device.device_id = device_section["deviceId"]
        if isinstance(device_section.get("locationId"), str):
            device.location_id = device_section["locationId"]

        if isinstance(device.status_payload, dict):
            device.status_payload["firmware"] = device.firmware
        if isinstance(device.health_payload, dict):
            device.health_payload["firmware"] = device.firmware
            device.health_payload["device_id"] = device.device_id

        new_key = device.key
        if new_key != current_key:
            self._devices.pop(current_key, None)
            self._devices[new_key] = device
        else:
            self._devices[current_key] = device

        self._events.append(
            TrafficEvent(
                timestamp=utc_now_iso(),
                direction="system",
                topic=f"{device.prefix}/config",
                payload=_json_dumps(payload),
                note="Device config updated in emulator state",
            )
        )

    def _get_or_create_device_locked(
        self,
        base_topic: str,
        device_id: str,
    ) -> SimulatedDevice:
        key = device_key(base_topic, device_id)
        existing = self._devices.get(key)
        if existing is not None:
            return existing
        created = SimulatedDevice(base_topic=base_topic, device_id=device_id)
        self._devices[key] = created
        return created

    def _increment_metric(
        self,
        device: SimulatedDevice,
        metric_name: str,
        value: int,
    ) -> None:
        attendance_metrics = device.metrics_payload.setdefault("AttendanceService", {})
        current_value = attendance_metrics.get(metric_name, 0)
        if isinstance(current_value, int):
            attendance_metrics[metric_name] = current_value + value
        else:
            attendance_metrics[metric_name] = value

    async def _publish(
        self,
        topic: str,
        payload: str,
        *,
        retain: bool = False,
    ) -> None:
        self._append_event(
            direction="out",
            topic=topic,
            payload=payload,
            note="retain=true" if retain else None,
        )
        async with Client(
            hostname=self._hostname,
            port=self._port,
            identifier=f"{self._client_id}-publisher",
            username=self._username,
            password=self._password,
        ) as client:
            await client.publish(topic, payload=payload.encode("utf-8"), retain=retain)

    def _append_event(
        self,
        *,
        direction: str,
        topic: str,
        payload: str,
        note: str | None = None,
    ) -> None:
        self._events.append(
            TrafficEvent(
                timestamp=utc_now_iso(),
                direction=direction,  # type: ignore[arg-type]
                topic=topic,
                payload=payload,
                note=note,
            )
        )

    def _parse_control_topic(
        self,
        topic: str,
    ) -> tuple[str, str, str, str | None] | None:
        parts = topic.split("/")
        if len(parts) >= 4 and parts[-1] == "request":
            kind = parts[-2]
            if kind in {"status", "health", "metrics"}:
                device_id = parts[-3]
                base_topic = "/".join(parts[:-3])
                if base_topic:
                    return base_topic, device_id, f"{kind}_request", None

        if len(parts) >= 4 and parts[-1] == "get" and parts[-2] == "config":
            device_id = parts[-3]
            base_topic = "/".join(parts[:-3])
            if base_topic:
                return base_topic, device_id, "config_get", None

        if len(parts) >= 5 and parts[-2] == "get" and parts[-3] == "config":
            device_id = parts[-4]
            base_topic = "/".join(parts[:-4])
            section = parts[-1]
            if base_topic:
                return base_topic, device_id, "config_get", section

        if len(parts) >= 4 and parts[-1] == "set" and parts[-2] == "config":
            device_id = parts[-3]
            base_topic = "/".join(parts[:-3])
            if base_topic:
                return base_topic, device_id, "config_set", None

        if len(parts) >= 5 and parts[-2] == "set" and parts[-3] == "config":
            device_id = parts[-4]
            base_topic = "/".join(parts[:-4])
            section = parts[-1]
            if base_topic:
                return base_topic, device_id, "config_set", section

        if len(parts) >= 4 and parts[-1] == "start" and parts[-2] == "ota":
            device_id = parts[-3]
            base_topic = "/".join(parts[:-3])
            if base_topic:
                return base_topic, device_id, "ota_start", None

        return None
