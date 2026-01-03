
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
    LOG_INFO(TAG, "ESP ChipID: %08X", isic::platform::getChipId());
    LOG_INFO(TAG, "Flash size: %u KB", isic::platform::getFlashChipRealSize() / 1024);
    LOG_INFO(TAG, "Free heap: %u bytes", ESP.getFreeHeap());
    LOG_INFO(TAG, "CPU freq: %u MHz", ESP.getCpuFreqMHz());

    // Create and initialize application
    app = new isic::App();

    if (const auto status = app->begin(); status.failed())
    {
        LOG_ERROR(TAG, "Application init failed: %s", status.message ? status.message : "Unknown error");

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

    LOG_INFO(TAG, "Setup complete, entering main loop");
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
}
