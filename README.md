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
any Arduino `Client` implementation, so it works over Wi-Fi, Ethernet and GSM,
in both HTTP and HTTPS (depending on the client class you pass in).

The client is constructed unbound and the transport is attached afterwards
with `setClient()`: this makes it possible to switch transport at runtime
(e.g. after an Ethernet -> Wi-Fi fallback where the network client is
destroyed and recreated) without losing any configuration (host, service key,
check interval, event handler, ...).

```cpp
#include <Arduino.h>
#include <OtamaticClient.h>
#include <WiFi.h>

#define FIRMWARE_VERSION 1

WiFiClient httpClient;               // HTTP over Wi-Fi
// WiFiClientSecure httpClient;      // HTTPS over Wi-Fi (setCABundle/setInsecure)
// EthernetClient httpClient;        // HTTP over Ethernet (Arduino Ethernet)
// TinyGsmClient httpClient(modem);  // HTTP over GSM/LTE (TinyGSM)

OtamaticClient ota;                  // transport attached in setup() via setClient()

void onOtaEvent(OtamaticClientEventData eventData) {
    // Handle the update lifecycle events (see "Events" below)
}

void setup() {
    // ... connect your network first ...
    ota.setClient(&httpClient);
    // Register the event handler BEFORE begin(): events emitted by begin()
    // (e.g. ConfigInvalid) are delivered to it.
    ota.onEvent(onOtaEvent);
    ota.begin(FIRMWARE_VERSION, "your-otamatic-service-key");
    ota.setCheckInterval(5 * 60 * 1000);  // check every 5 minutes
}

void loop() {
    ota.loop();  // performs a version check on every check interval
}
```

A complete example is available in [examples/AdvancedExample](examples/AdvancedExample/AdvancedExample.ino).

### Configuration

- `setClient(client)`: bind the transport to use. Must be called before
  `begin()`; it can also be called again at any time to rebind to a different
  `Client` instance (e.g. on a network interface change).
- `getClient()`: returns the currently bound `Client` (or `nullptr` if none).
- `begin(firmwareVersion, serviceKey)`: initialize the library. Returns
  `false` — and emits an `OperationFailed` event with the `ConfigInvalid`
  error code — if no client is bound or the service key is invalid.
- `setCheckInterval(interval)`: milliseconds between two automatic checks
  (default: 5 minutes).
- `requestCheckNow(restartCounter)`: force an immediate check; pass `false` to
  keep the interval counter untouched.
- `setDeviceId(id)` / `getDeviceId()`: by default the device ID is derived from
  the ESP32 station MAC address; a custom ID can be set if needed for your custom logic.
- `setAutoUpdate(enable)`: when disabled (default: enabled), an available
  update is only reported through the `UpdateAvailable` event and must be
  applied manually with `applyUpdate()`.
- `setAutoRestart(enable)`: when disabled (default: enabled), the device is not
  rebooted automatically after a successful update; restart it with
  `ESP.restart()` when the `UpdateCompleted` event is received.
- `setPublicKey(key)`: ECDSA public key (PEM or Base64-encoded DER, SPKI) used
  to verify the firmware signature. See "Firmware verification".
- `firmwareVersion()` / `getCheckData()` / `hasUpdateAvailable()`: inspect the
  running version and the result of the last version check. The check data is
  populated when an update is found and cleared at the beginning of every new
  check; it is not persisted across reboots.

### Local update portal

The library also provides an alternative update transport: a minimal web page
(stored in the firmware image, **no filesystem needed**) served by an
`AsyncWebServer` on port 80 that allows the upload of a firmware `.bin`
directly from a browser.

```cpp
// Non blocking: starts the server and returns immediately.
// timeout: inactivity timeout in ms after which the portal stops itself
// (0 to disable it). Requires an active Wi-Fi connection.
ota.startPortal(5 * 60 * 1000UL);
```

- Call `ota.loop()` as usual: it services the portal (timeout enforcement,
  stalled upload recovery, post-upload restart).
- The portal stops itself after a successful upload; `stopPortal()` stops it
  at any time and aborts any upload in progress. `isPortalActive()` reports
  the current state.
- The upload is written into the OTA partition by the same pipeline used by
  `applyUpdate()`: all the update events (`UpdateStarted`, `UpdateProgress`,
  `UpdateCompleted`, `OperationFailed`) are emitted as usual.
- Integrity and signature verification are skipped: the binary arrives
  directly from the user, without a version check payload. The firmware size
  is not part of the HTTP multipart request, so the page fills a hidden field
  from the browser `File API`: when available, the size is used to validate
  the image against the OTA partition before writing it and to report an exact
  `UpdateProgress` percentage; otherwise the percentage is estimated from the
  request size. Uploads without the field (`curl`, JavaScript disabled...) are
  still applied.
- Periodic version checks are suspended while the portal is active.
- The event handler is invoked from the AsyncWebServer task: do not block
  inside it.
- The portal is transport agnostic like the rest of the library: it works on
  any network interface handled by the SoC network stack (Wi-Fi station or
  soft-AP, wired interfaces), regardless of the `Client` bound with
  `setClient()`. Being an inbound server implemented with AsyncTCP, it cannot
  work when the connection is provided by an external modem (e.g. TinyGSM).
- `setAutoRestart(true)` (default) reboots the device ~2s after a successful
  upload, once the HTTP response has been delivered to the browser.

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

After the download, the client always verifies the SHA-256 integrity hash of
the firmware against the value provided by the server. When the server also
provides a signature and a public key has been configured with
`setPublicKey()`, the firmware signature is verified as well (ECDSA over
NIST P-256 with SHA-256). If a signature is provided but no public key is set,
the signature check is skipped and a warning is logged.

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

The library never allocates a client for you: you own the `Client` instance
and it must stay alive for the whole sketch lifetime.

When the network interface changes (e.g. Ethernet -> Wi-Fi fallback where the
client object is destroyed and recreated), rebind it at runtime: all the
previously configured settings are preserved.

```cpp
if (ota.getClient() != &newHttpClient) {
    ota.setClient(&newHttpClient);
}
```

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





