
#include "App.hpp"
#include "common/Logger.hpp"
#include "platform/PlatformESP.hpp"
#include "utils/FilesystemCommandHandler.hpp"

#include <Arduino.h>

namespace
{
constexpr auto TAG{"Main"};

isic::App *app = nullptr;

#ifdef ISIC_ENABLE_FS_INSPECTOR
isic::utils::FilesystemCommandHandler fsHandler;
#endif
} // namespace

void setup()
{
    // Initialize serial for debugging
    Serial.begin(115200); // TODO: Debug flag for baud rate selection and in debug no need serial.
    delay(100);

    // Print system info
    LOG_INFO(TAG, "=== ESP8266 System Information ===");
    LOG_INFO(TAG, "ESP ChipID: %08X", isic::platform::getChipId());
    LOG_INFO(TAG, "Flash size: %u KB", isic::platform::getFlashChipRealSize() / 1024);
    LOG_INFO(TAG, "CPU freq: %u MHz", ESP.getCpuFreqMHz());

    // Critical memory check BEFORE allocations
    const auto heapAtBoot = ESP.getFreeHeap();
    LOG_INFO(TAG, "Free heap at boot: %u bytes", heapAtBoot);

    // ESP8266 minimum safe heap threshold (accounting for WiFi stack, fragmentation, etc.)
    constexpr std::uint32_t MIN_SAFE_HEAP = 25000;
    if (heapAtBoot < MIN_SAFE_HEAP)
    {
        LOG_ERROR(TAG, "CRITICAL: Insufficient heap at boot! Found: %u, Required: %u",
                  heapAtBoot, MIN_SAFE_HEAP);
        LOG_ERROR(TAG, "Device may crash during initialization. Check firmware size and libraries.");
        delay(5000); // Allow user to see error before potential crash
    }

    // Create and initialize application
    LOG_INFO(TAG, "Creating application instance...");
    app = new isic::App();

    const auto heapAfterAppConstruct = ESP.getFreeHeap();
    const auto appConstructCost = static_cast<int>(heapAtBoot) - static_cast<int>(heapAfterAppConstruct);
    LOG_INFO(TAG, "App construction consumed: %d bytes, remaining: %u bytes",
             appConstructCost, heapAfterAppConstruct);

    // Memory check after app construction
    if (heapAfterAppConstruct < 20000)
    {
        LOG_WARN(TAG, "Low heap after app construction: %u bytes (warning threshold: 20000)",
                 heapAfterAppConstruct);
    }

    LOG_INFO(TAG, "Initializing application services...");
    if (const auto status = app->begin(); status.failed())
    {
        LOG_ERROR(TAG, "Application init failed: %s", status.message ? status.message : "Unknown error");
        LOG_ERROR(TAG, "Final heap: %u bytes", ESP.getFreeHeap());

        // // Blink LED to indicate error
        // pinMode(LED_BUILTIN, OUTPUT);
        // while (true) {
        //     digitalWrite(LED_BUILTIN, LOW);  // LED on (active low)
        //     delay(100);
        //     digitalWrite(LED_BUILTIN, HIGH); // LED off
        //     delay(100);
        //     yield();
        // }
    }

    const auto heapAfterInit = ESP.getFreeHeap();
    const auto initCost = static_cast<int>(heapAfterAppConstruct) - static_cast<int>(heapAfterInit);
    LOG_INFO(TAG, "Service initialization consumed: %d bytes, remaining: %u bytes",
             initCost, heapAfterInit);

    // Final memory health check
    constexpr std::uint32_t MIN_RUNTIME_HEAP = 15000;
    if (heapAfterInit < MIN_RUNTIME_HEAP)
    {
        LOG_WARN(TAG, "WARNING: Low runtime heap: %u bytes (minimum recommended: %u)",
                 heapAfterInit, MIN_RUNTIME_HEAP);
        LOG_WARN(TAG, "System may become unstable under load. Monitor for OOM crashes.");
    }
    else
    {
        LOG_INFO(TAG, "Heap health: GOOD (%u bytes free, %.1f%% available)",
                 heapAfterInit, (heapAfterInit * 100.0f) / 81920.0f);
    }

    LOG_INFO(TAG, "=== Setup complete, entering main loop ===");
}

void loop()
{
#ifdef ISIC_ENABLE_FS_INSPECTOR
    // Handle filesystem inspection commands
    fsHandler.processSerialCommands();
#endif

    if (app)
    {
        app->loop();
    }

#ifdef ISIC_DEBUG
    // Periodic heap monitoring in debug builds only
    static std::uint32_t lastHeapCheck = 0;
    static std::uint32_t lowestHeap = UINT32_MAX;
    const std::uint32_t now = millis();

    if (now - lastHeapCheck > 60000) // Every 60 seconds
    {
        const auto currentHeap = ESP.getFreeHeap();
        if (currentHeap < lowestHeap)
        {
            lowestHeap = currentHeap;
        }

        LOG_INFO(TAG, "Heap: %u bytes free, lowest: %u bytes (%.1f%% available)",
                 currentHeap, lowestHeap, (currentHeap * 100.0f) / 81920.0f);

        if (currentHeap < 10000)
        {
            LOG_WARN(TAG, "CRITICAL: Heap below 10KB! Risk of OOM crash.");
        }

        lastHeapCheck = now;
    }
#endif
}
