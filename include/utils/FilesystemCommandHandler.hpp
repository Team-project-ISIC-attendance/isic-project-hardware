#ifndef ISIC_UTILS_FILESYSTEM_COMMAND_HANDLER_HPP
#define ISIC_UTILS_FILESYSTEM_COMMAND_HANDLER_HPP

#ifdef ISIC_ENABLE_FS_INSPECTOR

#include <Arduino.h>
#include <LittleFS.h>

namespace isic::utils
{

/**
 * @brief Handler for filesystem inspection commands over serial
 *
 * This class processes commands from the esp_fs_inspector.py Python tool
 * to inspect the LittleFS filesystem contents.
 *
 * SECURITY NOTE: This handler is ONLY compiled when ISIC_ENABLE_FS_INSPECTOR is defined.
 * It provides direct filesystem access over serial. Use with caution in production
 * builds as it could be a potential security vulnerability if accessible to unauthorized users.
 *
 * Commands:
 *   FS_CMD:LIST [path]      - List files in directory
 *   FS_CMD:READ <filepath>  - Read file contents
 *   FS_CMD:INFO             - Get filesystem info
 */
class FilesystemCommandHandler
{
public:
    static constexpr auto *COMMAND_PREFIX{"FS_CMD:"};
    static constexpr auto *RESPONSE_PREFIX{"FS_RESP:"};
    static constexpr auto *RESPONSE_END{"FS_END"};

    static constexpr auto *COMMAND_INFO{"INFO"};
    static constexpr auto *COMMAND_LIST{"LIST"};
    static constexpr auto *COMMAND_READ{"READ"};

    FilesystemCommandHandler() = default;
    ~FilesystemCommandHandler() = default;

    FilesystemCommandHandler(const FilesystemCommandHandler &) = delete;
    FilesystemCommandHandler &operator=(const FilesystemCommandHandler &) = delete;
    FilesystemCommandHandler(FilesystemCommandHandler &&) = delete;
    FilesystemCommandHandler &operator=(FilesystemCommandHandler &&) = delete;

    /**
     * @brief Process incoming serial data for filesystem commands
     */
    void processSerialCommands()
    {
        while (Serial.available())
        {
            auto line{Serial.readStringUntil('\n')};
            line.trim();

            if (line.startsWith(COMMAND_PREFIX))
            {
                handleCommand(line.substring(strlen(COMMAND_PREFIX)));
            }
        }
    }

private:
    void sendResponse(const String &message)
    {
        Serial.print(RESPONSE_PREFIX);
        Serial.println(message);
    }

    void endResponse()
    {
        Serial.println(RESPONSE_END);
    }

    void handleCommand(const String &cmd)
    {
        auto command{cmd};
        command.trim();

        if (command.startsWith(COMMAND_LIST))
        {
            auto path{command.substring(4)};
            path.trim();

            if (path.isEmpty())
            {
                path = "/";
            }

            handleListCommand(path);
        }
        else if (command.startsWith(COMMAND_READ))
        {
            auto filepath{command.substring(4)};
            filepath.trim();
            handleReadCommand(filepath);
        }
        else if (command.startsWith(COMMAND_INFO))
        {
            handleInfoCommand();
        }
        else
        {
            sendResponse("ERROR: Unknown command");
            endResponse();
        }
    }

    void handleListCommand(const String &path)
    {
        if (!LittleFS.begin())
        {
            sendResponse("ERROR: Failed to mount filesystem");
            endResponse();
            return;
        }

        auto foundAny{false};

#ifdef ISIC_PLATFORM_ESP8266
        auto dir{LittleFS.openDir(path)};
        while (dir.next())
        {
            auto filename{dir.fileName()};
            const auto filesize{dir.fileSize()};

            auto entry{filename + " (" + String(filesize) + " bytes)"}; // Format: filename (size bytes)
            sendResponse(entry);
            foundAny = true;
        }
#elif defined(ISIC_PLATFORM_ESP32)
        auto root{LittleFS.open(path)};
        if (!root || !root.isDirectory())
        {
            sendResponse("ERROR: Failed to open directory or path is not a directory");
            endResponse();
            return;
        }

        auto file{root.openNextFile()};
        while (file)
        {
            auto filename{String(file.path())};
            const auto filesize{file.size()};

            // Format: filename (size bytes)
            auto entry{filename + " (" + String(filesize) + " bytes)"}; // Format: filename (size bytes)
            sendResponse(entry);
            foundAny = true;

            file = root.openNextFile();
        }
#endif

        if (!foundAny)
        {
            sendResponse("(empty or directory not found)");
        }

        endResponse();
    }

    void handleReadCommand(const String &filepath)
    {
        if (!LittleFS.begin())
        {
            sendResponse("ERROR: Failed to mount filesystem");
            endResponse();
            return;
        }

        if (!LittleFS.exists(filepath))
        {
            sendResponse("ERROR: File not found: " + filepath);
            endResponse();
            return;
        }

        auto file{LittleFS.open(filepath, "r")};
        if (!file)
        {
            sendResponse("ERROR: Failed to open file: " + filepath);
            endResponse();
            return;
        }

        // Read and send file contents line by line
        while (file.available())
        {
            String line = file.readStringUntil('\n');
            sendResponse(line);
        }

        file.close();
        endResponse();
    }

    void handleInfoCommand()
    {
        if (!LittleFS.begin())
        {
            sendResponse("ERROR: Failed to mount filesystem");
            endResponse();
            return;
        }

#ifdef ISIC_PLATFORM_ESP8266
        FSInfo fsInfo;
        LittleFS.info(fsInfo);

        sendResponse("Filesystem Information:");
        sendResponse("  Total size: " + String(fsInfo.totalBytes) + " bytes");
        sendResponse("  Used size: " + String(fsInfo.usedBytes) + " bytes");
        sendResponse("  Free size: " + String(fsInfo.totalBytes - fsInfo.usedBytes) + " bytes");
        sendResponse("  Block size: " + String(fsInfo.blockSize) + " bytes");
        sendResponse("  Page size: " + String(fsInfo.pageSize) + " bytes");
        sendResponse("  Max open files: " + String(fsInfo.maxOpenFiles));
        sendResponse("  Max path length: " + String(fsInfo.maxPathLength));
#elif defined(ISIC_PLATFORM_ESP32)
        sendResponse("Filesystem Information:");
        sendResponse("  Total size: " + String(LittleFS.totalBytes()) + " bytes");
        sendResponse("  Used size: " + String(LittleFS.usedBytes()) + " bytes");
        sendResponse("  Free size: " + String(LittleFS.totalBytes() - LittleFS.usedBytes()) + " bytes");
#endif

        // List all files
        sendResponse("");
        sendResponse("All files:");
        std::size_t totalFiles{0};

#ifdef ISIC_PLATFORM_ESP8266
        auto dir{LittleFS.openDir("/")};
        while (dir.next())
        {
            String filename = dir.fileName();
            size_t filesize = dir.fileSize();
            sendResponse("  " + filename + " (" + String(filesize) + " bytes)");
            totalFiles++;
        }
#elif defined(ISIC_PLATFORM_ESP32)
        if (auto root{LittleFS.open("/")}; root && root.isDirectory())
        {
            auto file{root.openNextFile()};
            while (file)
            {
                auto filename{String(file.path())};
                const auto filesize{file.size()};

                sendResponse("  " + filename + " (" + String(filesize) + " bytes)");
                totalFiles++;
                file = root.openNextFile();
            }
        }
#endif

        sendResponse("");
        sendResponse("Total files: " + String(totalFiles));

        endResponse();
    }
};
} // namespace isic::utils

#endif // ISIC_ENABLE_FS_INSPECTOR
#endif // ISIC_UTILS_FILESYSTEM_COMMAND_HANDLER_HPP
