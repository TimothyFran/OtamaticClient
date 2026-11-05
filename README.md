# OtamaticClient

OTA client for ESP32 — works with both the **Arduino framework** and plain **ESP-IDF** projects.

Managed OTA updates through the Otamatic platform: the device periodically
checks for a new firmware version and, when available, downloads and
installs it automatically.

- HTTPS connectivity through the ESP-IDF HTTP client (Wi-Fi, Ethernet, GSM — anything with a TCP/IP stack)
- Automatic periodic checks with integrity verification (SHA-256)
- Optional firmware signature verification (ECDSA P-256)
- Rollback protection with post-reboot verification
- Event-based API to observe the update lifecycle

## The Otamatic platform

Devices can be easily managed through the **[Otamatic](https://www.otamatic.eu)** platform at
[www.otamatic.eu](https://www.otamatic.eu), which lets you set up and manage your
devices fleet in **less than 5 minutes**.

It offers advanced features, including:

- Multiple codebases
- Release channels
- Scheduled releases
- Staged rollout releases
- Release monitoring

## Requirements

- ESP32 board with the Arduino framework (**arduino-esp32 core 3.x or later**)
  or a plain ESP-IDF project (ESP-IDF >= 5.1)
- An OTA-compatible partition scheme (two app slots, e.g. `ota_0`/`ota_1`)
- Network connectivity (Wi-Fi, Ethernet, or GSM)

## Installation

OtamaticClient is available both in the **Arduino Library Manager** and in the
**PlatformIO Library Registry**.

### Arduino IDE

Open **Sketch → Include Library → Manage Libraries...**, search for
**OtamaticClient** by *Timothy Franceschi (TimothyFran)* and click
**Install** (with the latest version selected).

### PlatformIO

Add it to the `lib_deps` section of your `platformio.ini`:

```ini
[env:esp32dev]
platform = espressif32
framework = arduino
lib_deps = timothyfran/OtamaticClient @ ^1.0.0
```

or install it from the PlatformIO Home → **Libraries** panel by searching for
**OtamaticClient**.

## Usage

The client embeds the ESP-IDF HTTP client (available both in the Arduino core
and in plain ESP-IDF) and talks to the Otamatic API over HTTPS, with certificate
validation through the mbedTLS root store. Wi-Fi, Ethernet or GSM are all fine:
whatever brings a TCP/IP stack up before `begin()` works.

```cpp
#include <Arduino.h>
#include <OtamaticClient.h>
#include <WiFi.h>

OtamaticClient ota;

void setup() {
    WiFi.begin("your-ssid", "your-password");
    while (WiFi.status() != WL_CONNECTED) delay(500);

    // Sync the system clock: TLS certificate validation checks validity
    // dates, otherwise every HTTPS connection fails.
    configTime(0, 0, "pool.ntp.org");

    OtamaticConfig config;
    config.apiKey = "your-otamatic-service-key";
    config.firmwareVersion = 1;              // monotonically increasing integer
    config.versionName = "1.0.0";            // human-readable version

    ota.begin(config);
}

void loop() {
    // Nothing to do: the client runs its own auto-check task.
}
```

That's it: the device now checks for updates every 5 minutes and applies
them automatically (rebooting when done). See
[`src/otamatic/otamatic_config.hpp`](src/otamatic/otamatic_config.hpp) for all
the available options (check interval, signed updates, rollback verification,
failed-version guard, manual mode, ...).

## Events

The client reports every step of the update lifecycle through a callback, so
your sketch can react (log, drive LEDs, notify a server, ...). Each event
carries typed data: an error code for failures, the progress percentage
during the download, and the firmware version/size involved.

```cpp
void onOtaEvent(const OtamaticClientEventData& event) {
    switch (event.event) {
        case OtamaticClientEvent::UpdateProgress:
            Serial.printf("Downloading: %u%%\n", event.progress);
            break;
        case OtamaticClientEvent::OperationFailed:
            Serial.printf("OTA failed: %s\n", OtamaticClient::getErrorName(event.error));
            break;
        default:
            Serial.printf("OTA event: %s (version %lu)\n",
                          OtamaticClient::getEventName(event.event),
                          (unsigned long)event.version);
            break;
    }
}

void setup() {
    // ...
    ota.onEvent(onOtaEvent);
    ota.begin(config);
}
```

If you need to forward a context (e.g. a class instance), register the
callback with a user pointer:

```cpp
void onOtaEventEx(const OtamaticClientEventData& event, void* userData) {
    MyClass* self = static_cast<MyClass*>(userData);
    self->handleOtaEvent(event);
}

// ...
ota.onEvent(onOtaEventEx, &myInstance);
```

The callback runs synchronously in the context of the task that generated the
event (the auto-check task or the caller of `checkForUpdate()`/`applyUpdate()`):
keep it short and do not call back into the client from inside it.

Event and error codes are plain enums (`OtamaticClientEvent`,
`OtamaticClientError`); `OtamaticClient::getEventName()` and
`OtamaticClient::getErrorName()` turn them into human-readable strings.

## Next steps

- Create your account on the [Otamatic platform](https://www.otamatic.eu) — in under 5 minutes.
- Browse the [examples](examples/) to go further:
  - [basic_example (Arduino)](examples/arduino/basic_example/basic_example.ino) — basic setup with event logging
  - [basic_example (ESP-IDF)](examples/esp-idf/basic_example/) — the same example as a plain ESP-IDF project
- See the `src/OtamaticClient.h` header and the [extras](extras/) folder for the full API documentation.
- Planning a self-hosted server? See [docs/API.md](docs/API.md) for the HTTP API specification.

## License

This project is licensed under the **Business Source License 1.1** (SPDX: `BUSL-1.1`) — see the [LICENSE](LICENSE) file.

### The short version

**You CAN**

- ✓ Use OtamaticClient in production for your own applications
- ✓ Self-host it for your team, company, or clients
- ✓ Modify the source code for your needs
- ✓ Build commercial products powered by OtamaticClient
- ✓ Use it in internal tools and firmware
- ✓ Read, study, and learn from the source code

**You CANNOT**

- ✗ Offer OtamaticClient as a hosted OTA update service to third parties
- ✗ Embed OtamaticClient inside your product and sell access to it as a managed OTA platform
- ✗ Resell OtamaticClient as a managed OTA platform

These restrictions only apply if you are competing directly with the Licensor's own managed OTA offering. If in doubt, contact us.

For commercial licensing, contact the Licensor: **Timothy Franceschi <timothy@franceschi.es>**.

