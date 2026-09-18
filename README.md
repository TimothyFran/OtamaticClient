# OtamaticClient

OTA client for ESP32 with the Arduino framework.

## Project structure

```
OtamaticClient/
├── library.properties        # Metadata for the Arduino Library Manager
├── library.json              # Metadata for PlatformIO
├── LICENSE                   # Business Source License 1.1
├── README.md
├── keywords.txt              # Syntax highlighting for the IDE
├── src/                      # Library source code
├── examples/                 # Example sketches
└── extras/                   # Additional documentation, diagrams, etc.
```

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
lib_deps = timothyfran/OtamaticClient @ ^0.2.0
```

or install it from the PlatformIO Home → **Libraries** panel by searching for
**OtamaticClient**.

## Usage

OtamaticClient is transport agnostic: it implements plain HTTP/1.1 on top of
any `Client` implementation, so it works over Wi-Fi, Ethernet, GSM, in both HTTP and HTTPS (depending on the client class you pass in).
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

void onOtaEvent(OtamaticClientEventData eventData) {
    // Handle the update lifecycle events (see "Events" below)
}

void setup() {
    // ... connect your network first ...
    ota.setClient(&httpClient);
    ota.onEvent(onOtaEvent);
    ota.begin(FIRMWARE_VERSION, "your-otamatic-service-key");
    ota.setCheckInterval(5 * 60 * 1000);  // check every 5 minutes
}

void loop() {
    ota.loop();
}
```

A complete example is available in [examples/AdvancedExample](examples/AdvancedExample/AdvancedExample.ino).

### Configuration

- `setClient(client)` / `getClient()`: bind (or read) the transport. Must be
  called before `begin()`.
- `begin(firmwareVersion, serviceKey)`: initialize the library; returns `false`
  (with a `ConfigInvalid` error) if the transport is missing or the key is invalid.
- `setCheckInterval(interval)`: milliseconds between automatic checks (default: 5 min).
- `requestCheckNow(restartCounter)`: force an immediate check.
- `setDeviceId(id)` / `getDeviceId()`: device ID (default: ESP32 station MAC).
- `setAutoUpdate(enable)`: disable to only report updates via the
  `UpdateAvailable` event and apply them manually with `applyUpdate()`.
- `setAutoRestart(enable)`: disable to reboot manually with `ESP.restart()`
  when `UpdateCompleted` is received.
- `setPublicKey(key)`: ECDSA public key for signature verification
  (see "Firmware verification").
- `firmwareVersion()` / `getCheckData()` / `hasUpdateAvailable()`: inspect the
  running version and the result of the last check (not persisted across reboots).

### Local update portal

An alternative update transport: a minimal web page (no filesystem needed)
that lets you upload a firmware `.bin` directly from a browser.

```cpp
ota.startPortal(5 * 60 * 1000UL);  // timeout in ms (0 to disable it)
```

- Non-blocking: call `ota.loop()` as usual to service it.
- The portal stops itself after a successful upload; `stopPortal()` stops it at
  any time, `isPortalActive()` reports the state.
- All the update events are emitted as usual, but integrity and signature
  verification are skipped (the binary comes from the user, not the server).
- Periodic version checks are suspended while the portal is active.
- Works on Wi-Fi (station/soft-AP) and wired interfaces; not with an external
  modem (e.g. TinyGSM).
- With `setAutoRestart(true)` (default) the device reboots shortly after a
  successful upload.
- The event handler may run while the upload is in progress: do not block inside it.

### Events

`onEvent(callback)` registers a handler invoked with `OtamaticClientEventData`
(event type plus a `uint8_t` payload):

| Event | Payload | Meaning |
| ----- | ------- | ------- |
| `CheckingForUpdate` | – | A version check cycle started |
| `UpdateAvailable` | – | A new firmware version was found |
| `UpdateNotNeeded` | – | No update available |
| `UpdateStarted` | – | Writing of the new firmware started |
| `UpdateProgress` | Progress % (0-100) | Firmware write progress |
| `UpdateCompleted` | – | Update completed (device ready to reboot) |
| `OperationFailed` | `OtamaticClientError` code | Operation failed, see below |

Error codes reported with `OperationFailed`:

| Code | Meaning |
| ---- | ------- |
| `None` | No error (default payload) |
| `InvalidCheckData` | No valid version check result when applying the update |
| `ConnectionFailed` | Could not establish the connection to the server |
| `HttpFailed` | The server answered with an unexpected HTTP status |
| `InvalidFirmware` | The firmware binary size is invalid |
| `BeginFailed` | Could not reserve the OTA partition (no space, partition issue...) |
| `DownloadFailed` | Download stalled or connection dropped |
| `WriteFailed` | Error while writing the firmware into the OTA partition |
| `EndFailed` | Final image verification failed |
| `IntegrityFailed` | The integrity hash of the downloaded firmware does not match |
| `SignatureFailed` | The signature of the downloaded firmware does not match |
| `ConfigInvalid` | Invalid configuration (e.g. `begin()` called with an invalid service key, or before `setClient()`) |

### Firmware verification

The client always verifies the SHA-256 integrity hash of the downloaded firmware. If the server also provides a signature and you configured a public key with `setPublicKey()`, the signature is verified as well (ECDSA P-256 + SHA-256). With `setRequireSignature(true)`, updates without a signature are rejected outright.

### Examples

| Example | Description |
| ------- | ----------- |
| [WiFiBasic](examples/WiFiBasic/WiFiBasic.ino) | Basic Wi-Fi setup with automatic check and update |
| [SignatureExample](examples/SignatureExample/SignatureExample.ino) | Adds firmware signature verification with a public key |
| [AdvancedExample](examples/AdvancedExample/AdvancedExample.ino) | Signature verification with manual update and restart control via events |

### Transport notes

| Transport | Client class | Notes |
| --------- | ------------ | ----- |
| Wi-Fi (HTTP) | `WiFiClient` / `NetworkClient` | Plain HTTP |
| Wi-Fi (HTTPS) | `WiFiClientSecure` | Call `setCABundle()` (or `setInsecure()` for development only) |
| Ethernet | `EthernetClient` | Any `Client` with TLS support works for HTTPS |
| GSM/LTE | `TinyGsmClient` (TinyGSM) | Power on the modem before calling `ota.begin()` |

You own the `Client` instance: it must stay alive for the whole sketch lifetime. If the network changes, just re-bind it with `setClient()`.

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

