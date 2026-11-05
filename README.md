# OtamaticClient

OTA client for ESP32 with the Arduino framework.

Managed OTA updates through the Otamatic platform: the device periodically
checks for a new firmware version and, when available, downloads and
installs it automatically.

- Works over any Arduino `Client` (Wi-Fi, Ethernet, GSM — HTTP or HTTPS)
- Automatic periodic checks with integrity verification (SHA-256)
- Optional firmware signature verification (ECDSA P-256)
- Rollback protection with post-reboot verification
- Local update portal for manual uploads from a browser
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

- ESP32 board with the Arduino framework
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
lib_deps = timothyfran/OtamaticClient @ ^0.4.0
```

or install it from the PlatformIO Home → **Libraries** panel by searching for
**OtamaticClient**.

## Usage

OtamaticClient is transport agnostic: it implements plain HTTP/1.1 on top of any `Client` implementation, so it works over Wi-Fi, Ethernet, GSM, in both HTTP and HTTPS (depending on the client class you pass in).
The transport is attached with `setClient()` and can be re-bound at runtime (e.g. after a network change) without losing any configuration.

```cpp
#include <Arduino.h>
#include <OtamaticClient.h>
#include <WiFi.h>

#define FIRMWARE_VERSION 1

WiFiClient httpClient;               // HTTP over Wi-Fi
// WiFiClientSecure httpClient;      // HTTPS over Wi-Fi (setCABundle/setInsecure)
// EthernetClient httpClient;        // HTTP over Ethernet (Arduino Ethernet)
// TinyGsmClient httpClient(modem);  // HTTP over GSM/LTE (TinyGSM)

OtamaticClient ota;

void setup() {
    // ... connect your network first ...
    ota.setClient(&httpClient);
    ota.begin(FIRMWARE_VERSION, "your-otamatic-service-key");
}

void loop() {
    ota.loop();
}
```

That's it: the device now checks for updates every 5 minutes and applies
them automatically (rebooting when done).

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
    ota.begin(FIRMWARE_VERSION, "your-otamatic-service-key");
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
  - [WiFiBasic](examples/WiFiBasic/WiFiBasic.ino) — basic setup with event logging
  - [SignatureExample](examples/SignatureExample/SignatureExample.ino) — firmware signature verification
  - [AdvancedExample](examples/AdvancedExample/AdvancedExample.ino) — manual update and restart control
  - [PortalExample](examples/PortalExample/PortalExample.ino) — local update portal for manual uploads
- See the `src/*.h` headers and the [extras](extras/) folder for the full API documentation.
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

