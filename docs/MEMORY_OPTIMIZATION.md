# Memory Optimization Guide

This document covers memory optimization strategies for ESP8266/ESP32 embedded development.

## Build Comparison

| Environment | RAM Used | RAM % | Flash Used | Flash % |
|-------------|----------|-------|------------|---------|
| ESP8266 | 40,168 B | 49.0% | 432 KB | 41.3% |
| ESP32 dev | 45,444 B | 13.9% | 1,008 KB | 76.9% |
| ESP32 minimal | 45,380 B | 13.8% | 920 KB | 46.8% |

### Why ESP32 uses more flash than ESP8266

Even with identical optimization flags, ESP32 uses ~2x more flash because:

1. **ESP-IDF framework** is fundamentally larger than ESP8266 Arduino core
2. **FreeRTOS kernel** - required on ESP32, not on ESP8266
3. **32-bit Xtensa LX6** produces larger instructions than ESP8266's LX106
4. **More complex HAL** - dual core support, more peripherals
5. **Larger standard library** - newlib vs uClibc

### Optimization flags that reduce flash

| Flag | Savings |
|------|---------|
| `-Os` | ~40 KB |
| `-fno-exceptions` | ~30 KB |
| `-fno-rtti` | ~10 KB |
| `-DNDEBUG` | ~8 KB |

---

## String Types: Arduino `String` vs `std::string`

### Comparison Table

| Aspect | Arduino `String` | `std::string` | Winner |
|--------|------------------|---------------|--------|
| Heap fragmentation | Very bad | Better (has SSO) | std::string |
| Memory overhead | ~16 bytes + heap | ~24 bytes (SSO inline) | std::string |
| Small strings (<15 chars) | Always heap | Stored inline (SSO) | std::string |
| Speed | Slower (more allocs) | Faster (SSO) | std::string |
| Safety | Can crash on OOM | More predictable | std::string |
| Flash size | Smaller | Larger (~2-4KB) | String |

> **SSO** = Small String Optimization - strings up to ~15 characters are stored directly in the object (on stack), avoiding heap allocation entirely.

### Best Practices for ESP8266

```cpp
// BEST - No allocation at all
const char* literal = "hello";           // Stored in flash/rodata

// GOOD - Fixed buffer on stack, no heap
char buffer[32];
snprintf(buffer, sizeof(buffer), "value=%d", x);

// OK - Pre-allocated std::string (one heap allocation)
std::string s;
s.reserve(64);  // Allocate once upfront
s += "data";    // No reallocation if under reserved capacity

// BAD - Arduino String concatenation (causes heap fragmentation!)
String s = "a" + String(x) + "b";  // Multiple allocations and deallocations!
```

### When to use each type

| Use Case | Recommended Type |
|----------|------------------|
| Static literals | `const char*` |
| Temporary formatting | `char[]` buffer with `snprintf` |
| Dynamic strings with known max size | `std::string` with `reserve()` |
| Config values (SSID, password) | `std::string` (stored once) |
| MQTT topics/payloads | `std::string` with `reserve()` |
| Logging messages | `const char*` literals |

---

## Memory Architecture

### ESP8266 Memory Layout (80KB RAM)

```
+------------------+ 0x3FFE8000
|   User Heap      | ~45KB available
|   (dynamic)      |
+------------------+
|   BSS + Data     | ~35KB (globals, static)
+------------------+
|   Stack          | ~4KB
+------------------+ 0x3FFF0000
```

### Runtime heap usage

| Component | Heap Usage |
|-----------|------------|
| WiFi stack | 10-15 KB |
| MQTT client | 3-5 KB |
| JSON parsing | 1-2 KB |
| Strings/buffers | Variable |

**Safe minimum free heap**: 15-20 KB

---

## Project-Specific Optimizations

### Already implemented

- PROGMEM for HTML content (~8-10KB saved)
- Constexpr string lookup tables (zero runtime cost)
- Vector pre-allocation with `.reserve()`
- Fixed-size event queues (4 events max)
- Smart pointers for NFC driver (RAII)
- Heap monitoring in HealthService

### Code patterns used

```cpp
// Pre-reserve strings before building
std::string result;
result.reserve(1024);

// Pre-reserve vectors
m_batch.reserve(m_config.batchMaxSize);
m_eventConnections.reserve(4);

// PROGMEM for large static content
constexpr char CONFIG_HTML[] PROGMEM = R"(...)";
```

---

## Monitoring Memory at Runtime

### Useful functions

```cpp
// ESP8266/ESP32
ESP.getFreeHeap()           // Current free heap
ESP.getHeapFragmentation()  // Fragmentation % (ESP8266)
ESP.getMaxFreeBlockSize()   // Largest allocatable block

// ESP32 only
ESP.getMinFreeHeap()        // Minimum free heap since boot
ESP.getPsramSize()          // PSRAM size (if available)
```

### Health thresholds (Config.hpp)

```cpp
kHeapCriticalThresholdBytes = 4096;   // Critical: < 4KB
kHeapWarningThresholdBytes = 8192;    // Warning: < 8KB
kFragmentationWarningThresholdPercent = 50;
```

---

## Future Optimization Opportunities

1. **Replace `JsonDocument` with `StaticJsonDocument<N>`** - Saves ~500B heap per operation
2. **Use fixed char arrays in `MqttEvent`** - Eliminates std::string overhead
3. **Add max string size constraints to config** - Predictable memory layout
4. **Reduce vector reserve sizes** - Where over-allocated
