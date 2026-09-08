# OtamaticClient

OTA client for ESP32 with the Arduino framework.

## Project structure

```
OtamaticClient/
├── library.properties        # Metadata for the Arduino Library Manager
├── library.json              # Metadata for PlatformIO
├── LICENSE                   # Business Source License 1.1
├── README.md
├── keywords.txt              # Syntax highlighting for the Arduino IDE
├── src/                      # Library source code
├── examples/                 # Example sketches
└── extras/                   # Additional documentation, diagrams, etc.
```

## Usage

OtamaticClient is transport agnostic: it implements plain HTTP/1.1 on top of
any Arduino `Client` implementation, so it works over Wi-Fi, Ethernet and GSM,
in both HTTP and HTTPS (depending on the client class you pass in).

```cpp
#include <OtamaticClient.h>
#include <WiFi.h>

WiFiClient httpClient;               // HTTP over Wi-Fi
// WiFiClientSecure httpClient;      // HTTPS over Wi-Fi (setCABundle/setInsecure)
// EthernetClient httpClient;        // HTTP over Ethernet (Arduino Ethernet)
// TinyGsmClient httpClient(modem);  // HTTP over GSM/LTE (TinyGSM)

OtamaticClient ota;

void setup() {
    // ... connect your network first ...
    ota.begin(FIRMWARE_VERSION, httpClient, "ota.example.com", 80, "/api/v1/devices/check");
    ota.onEvent(onOtaEvent);
}

void loop() {
    ota.loop();
}
```

A complete example is available in [examples/WiFiBasic](examples/WiFiBasic/WiFiBasic.ino).

### Server API

The client asks the server whether a new firmware is available:

```
GET /api/v1/devices/check?version=<current>&chip=<model>
```

The endpoint must answer with `200 OK` and a JSON body:

```json
{ "version": 2, "url": "/api/v1/devices/firmware.bin" }
```

- `version`: the latest available firmware version (number). If it is greater
  than the running one, the client downloads and flashes the firmware.
- `url`: path (or absolute URL on the same host) of the firmware binary. The
  firmware is downloaded over the same `Client` connection, i.e. from the same
  host and port used for the check.

### Transport notes

| Transport | Client class | Notes |
| --------- | ------------ | ----- |
| Wi-Fi (HTTP) | `WiFiClient` / `NetworkClient` | Port 80 |
| Wi-Fi (HTTPS) | `WiFiClientSecure` | Port 443, call `setCABundle()` (or `setInsecure()` for development only) |
| Ethernet | `EthernetClient` | Any `Client` with TLS support works for HTTPS |
| GSM/LTE | `TinyGsmClient` (TinyGSM) | Power on the modem before calling `ota.begin()` |

The library never allocates a client for you: you own the `Client` instance
and it must stay alive for the whole sketch lifetime.

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





