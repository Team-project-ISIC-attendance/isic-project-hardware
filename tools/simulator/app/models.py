from collections.abc import Iterable
from dataclasses import dataclass, field
from datetime import UTC, datetime
from typing import Any, Literal

from pydantic import BaseModel, Field, field_validator

CONFIG_SECTION_KEYS = (
    "wifi",
    "mqtt",
    "device",
    "pn532",
    "attendance",
    "feedback",
    "health",
    "ota",
    "power",
)


def utc_now_iso() -> str:
    return datetime.now(UTC).isoformat()


def device_key(base_topic: str, device_id: str) -> str:
    return f"{base_topic}/{device_id}"


def topic_for(base_topic: str, device_id: str, *suffix: str) -> str:
    return "/".join((base_topic, device_id, *suffix))


def make_default_status(firmware: str) -> dict[str, Any]:
    return {
        "firmware": firmware,
        "state": "online",
    }


def make_default_health(
    device_id: str,
    firmware: str,
) -> dict[str, Any]:
    return {
        "device_id": device_id,
        "firmware": firmware,
        "state": "healthy",
        "uptime_s": 1234,
        "cpu_freq": 80,
        "free_heap": 32768,
        "heap_state": "healthy",
        "heap_fragm": 8,
        "fragm_state": "healthy",
        "wifi_rssi": -58,
        "wifi_rssi_state": "healthy",
    }


def make_default_metrics() -> dict[str, Any]:
    return {
        "ConfigService": {"state": "running"},
        "WiFiService": {"state": "running", "disconnect_count": 0},
        "MqttService": {
            "state": "running",
            "published": 0,
            "failed": 0,
            "received": 0,
            "reconnects": 0,
        },
        "AttendanceService": {
            "state": "running",
            "cards_processed": 0,
            "cards_debounced": 0,
            "batches_sent": 0,
            "errors": 0,
        },
    }


def make_default_config(
    base_topic: str,
    device_id: str,
    location_id: str,
) -> dict[str, Any]:
    return {
        "magic": 1230190915,
        "version": 1,
        "wifi": {
            "stationSsid": "test-network",
            "stationPassword": "change-me",
            "stationConnectRetryDelayMs": 5000,
            "stationConnectionTimeoutMs": 15000,
            "stationFastReconnectIntervalMs": 2000,
            "stationSlowReconnectIntervalMs": 15000,
            "stationMaxFastConnectionAttempts": 5,
            "stationPowerSaveEnabled": False,
            "stationHasEverConnected": True,
            "accessPointSsidPrefix": "ISIC-EMU",
            "accessPointPassword": "change-me",
            "accessPointModeTimeoutMs": 300000,
        },
        "mqtt": {
            "brokerAddress": "mqtt",
            "port": 1883,
            "username": "",
            "password": "",
            "baseTopic": base_topic,
            "keepAliveIntervalSec": 30,
            "reconnectMinIntervalMs": 1000,
            "reconnectMaxIntervalMs": 15000,
        },
        "device": {
            "deviceId": device_id,
            "locationId": location_id,
        },
        "pn532": {
            "spiSckPin": 14,
            "spiMisoPin": 12,
            "spiMosiPin": 13,
            "spiCsPin": 15,
            "irqPin": 4,
            "pollIntervalMs": 250,
            "readTimeoutMs": 1000,
            "maxConsecutiveErrors": 5,
            "recoveryDelayMs": 2000,
        },
        "attendance": {
            "debounceIntervalMs": 3000,
            "batchMaxSize": 10,
            "batchFlushIntervalMs": 1500,
            "offlineBufferSize": 100,
            "offlineBufferFlushIntervalMs": 5000,
            "batchingEnabled": True,
            "offlineQueuePolicy": "drop_oldest",
        },
        "feedback": {
            "enabled": True,
            "ledEnabled": True,
            "ledPin": 2,
            "ledRedPin": 2,
            "ledGreenPin": 0,
            "ledBluePin": 15,
            "buzzerEnabled": False,
            "buzzerPin": 5,
            "ledActiveHigh": False,
            "beepFrequencyHz": 3000,
            "successBlinkDurationMs": 150,
            "errorBlinkDurationMs": 500,
        },
        "health": {
            "healthCheckIntervalMs": 30000,
            "statusUpdateIntervalMs": 10000,
            "metricsPublishIntervalMs": 60000,
            "publishToMqtt": True,
        },
        "ota": {
            "enabled": True,
            "serverUrl": "http://ota.local",
            "username": "",
            "password": "",
            "timeoutMs": 60000,
            "checkOnConnect": False,
        },
        "power": {
            "readerIdleTimeoutMs": 10000,
            "modemSleepAfterMs": 30000,
            "pn532SleepAfterMs": 10000,
            "readerReadyHoldMs": 1000,
            "burstWindowMs": 3000,
            "burstScanCount": 5,
            "burstHoldMs": 1000,
            "enableNfcWakeup": False,
            "nfcWakeupPin": 16,
            "autoSleepEnabled": False,
            "disableWiFiDuringSleep": False,
            "pn532SleepBetweenScans": False,
            "modemSleepOnMqttDisconnect": False,
            "activityTypeMask": 255,
        },
    }


@dataclass
class SimulatedDevice:
    base_topic: str
    device_id: str
    auto_respond: bool = True
    firmware: str = "simulator-1.0.0"
    location_id: str = "LAB-01"
    next_seq: int = 1
    status_payload: dict[str, Any] = field(default_factory=dict)
    health_payload: dict[str, Any] = field(default_factory=dict)
    metrics_payload: dict[str, Any] = field(default_factory=dict)
    config_payload: dict[str, Any] = field(default_factory=dict)

    def __post_init__(self) -> None:
        if not self.status_payload:
            self.status_payload = make_default_status(self.firmware)
        if not self.health_payload:
            self.health_payload = make_default_health(
                device_id=self.device_id,
                firmware=self.firmware,
            )
        if not self.metrics_payload:
            self.metrics_payload = make_default_metrics()
        if not self.config_payload:
            self.config_payload = make_default_config(
                base_topic=self.base_topic,
                device_id=self.device_id,
                location_id=self.location_id,
            )

    @property
    def key(self) -> str:
        return device_key(self.base_topic, self.device_id)

    @property
    def prefix(self) -> str:
        return topic_for(self.base_topic, self.device_id)

    def as_dict(self) -> dict[str, Any]:
        return {
            "key": self.key,
            "prefix": self.prefix,
            "base_topic": self.base_topic,
            "device_id": self.device_id,
            "auto_respond": self.auto_respond,
            "firmware": self.firmware,
            "location_id": self.location_id,
            "next_seq": self.next_seq,
            "status_payload": self.status_payload,
            "health_payload": self.health_payload,
            "metrics_payload": self.metrics_payload,
            "config_payload": self.config_payload,
        }


@dataclass
class TrafficEvent:
    timestamp: str
    direction: Literal["in", "out", "system"]
    topic: str
    payload: str
    note: str | None = None

    def as_dict(self) -> dict[str, Any]:
        return {
            "timestamp": self.timestamp,
            "direction": self.direction,
            "topic": self.topic,
            "payload": self.payload,
            "note": self.note,
        }


class DeviceUpsertRequest(BaseModel):
    previous_key: str | None = None
    base_topic: str = Field(default="device", min_length=1)
    device_id: str = Field(default="ISIC-ESP8266-001", min_length=1)
    auto_respond: bool = True
    firmware: str = Field(default="simulator-1.0.0", min_length=1)
    location_id: str = Field(default="LAB-01", min_length=1)
    next_seq: int = Field(default=1, ge=1)
    status_payload: dict[str, Any] | None = None
    health_payload: dict[str, Any] | None = None
    metrics_payload: dict[str, Any] | None = None
    config_payload: dict[str, Any] | None = None


class AttendancePublishRequest(BaseModel):
    base_topic: str = Field(default="device", min_length=1)
    device_id: str = Field(default="ISIC-ESP8266-001", min_length=1)
    uids: list[str] = Field(default_factory=list)
    timestamp_mode: Literal["now", "zero", "custom"] = "now"
    timestamp_ms: int | None = Field(default=None, ge=0)

    @field_validator("uids")
    @classmethod
    def validate_uids(cls, value: Iterable[str]) -> list[str]:
        cleaned = [item.strip().upper() for item in value if item.strip()]
        if not cleaned:
            raise ValueError("At least one UID is required")
        return cleaned


class SnapshotPublishRequest(BaseModel):
    base_topic: str = Field(default="device", min_length=1)
    device_id: str = Field(default="ISIC-ESP8266-001", min_length=1)
    payload: dict[str, Any]


class ConfigPublishRequest(BaseModel):
    base_topic: str = Field(default="device", min_length=1)
    device_id: str = Field(default="ISIC-ESP8266-001", min_length=1)
    payload: dict[str, Any]
    section: str | None = None

    @field_validator("section")
    @classmethod
    def validate_section(cls, value: str | None) -> str | None:
        if value is None or value == "":
            return None
        if value not in CONFIG_SECTION_KEYS:
            raise ValueError("Unsupported config section")
        return value


class RawPublishRequest(BaseModel):
    topic: str = Field(min_length=1)
    payload: str = ""
    retain: bool = False


class OtaPublishRequest(BaseModel):
    base_topic: str = Field(default="device", min_length=1)
    device_id: str = Field(default="ISIC-ESP8266-001", min_length=1)
    version: str = Field(default="simulator-1.0.1", min_length=1)
    fail_at_percent: int | None = Field(default=None, ge=0, le=100)
