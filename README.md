<div align="center">

# OtamaticClient

**Managed OTA updates for ESP32 — Arduino framework and plain ESP-IDF.**

[![Arduino Library](https://img.shields.io/badge/Arduino-Library_Manager-00878F?logo=arduino&logoColor=white)](https://docs.arduino.cc/libraries/otamaticclient/)
[![PlatformIO](https://img.shields.io/badge/PlatformIO-Library-1372D3?logo=platformio&logoColor=white)](https://registry.platformio.org/libraries/timothyfran/OtamaticClient)
[![MCU](https://img.shields.io/badge/MCU-ESP32-E7352C?logo=espressif&logoColor=white)](https://www.espressif.com/)
[![License](https://img.shields.io/badge/License-BUSL--1.1-blue.svg)](LICENSE)

[Examples](examples/) · [Requirements](#requirements) · [Install](#installation) · [Configure](#configuration) · [Self-hosted API](docs/API.md)

</div>

Your device periodically checks the **[Otamatic](https://www.otamatic.eu)** platform,
downloads the new firmware over HTTPS and installs it — safely.

- 🌐 **Any network** — Wi-Fi, Ethernet, GSM… anything with a TCP/IP stack
- 🔒 **Secure by default** — HTTPS with certificate validation, SHA-256 integrity checks, optional ECDSA P-256 firmware signatures
- ↩️ **Rollback protection** — the new firmware must prove itself at boot, or it is rolled back automatically
- 🎛️ **You're in control** — fully automatic or fully manual lifecycle, with an event callback for every step

> **[Create your account on otamatic.eu](https://www.otamatic.eu)** and manage your device fleet —
> multiple codebases, release channels, scheduled and staged-rollout releases — in **under 5 minutes**.

---

## Requirements

- An **ESP32** board with either:
  - the **Arduino framework** (arduino-esp32 core **3.x or later**), or
  - a plain **ESP-IDF** project (**≥ 5.1**)
- An **OTA-compatible partition scheme** (two app slots, e.g. `ota_0` / `ota_1`)
- **Network connectivity** brought up before starting the client
- A **synchronized clock** — TLS validates certificate dates, so without a time source (NTP, SNTP, GPS…) every HTTPS connection fails

> 💡 The dependency on [ArduinoJson](https://arduinojson.org) is declared in the library
> manifests and is resolved automatically by every supported toolchain.

## Installation

### Arduino IDE

**Sketch → Include Library → Manage Libraries…** → search **OtamaticClient** by
*Timothy Franceschi (TimothyFran)* → **Install**.

### PlatformIO

Add it to `lib_deps` in your `platformio.ini`:

```ini
[env:esp32dev]
platform = espressif32
framework = arduino
lib_deps = timothyfran/OtamaticClient @ ^1.0.0
```

### ESP-IDF (component)

Add the library as an external component in your project's root `CMakeLists.txt`:

```cmake
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../OtamaticClient")
```

then require it from your `main` component:

```cmake
idf_component_register(SRCS "main.c" REQUIRES OtamaticClient ...)
```

Dropping the library folder into your project's `components/` directory works too.
Either way, the ESP-IDF Component Manager pulls in ArduinoJson on the next build.

## Quick start

Once your network is up and the clock is synced, three steps are enough:

```cpp
#include <OtamaticClient.h>   // ESP-IDF: #include "otamatic/otamatic_client.hpp"

OtamaticClient client;

void setup() {
    // 1. Connect your network (Wi-Fi / Ethernet / GSM) and sync the clock.

    // 2. Configure the client
    OtamaticConfig config;
    config.apiKey = "your-otamatic-service-key";
    config.firmwareVersion = 1;        // monotonically increasing integer
    config.versionName = "1.0.0";      // human-readable version

    // 3. (Optional) observe the update lifecycle — register callbacks before
    //    begin(): boot-time events (post-boot verification) are delivered
    //    during begin().
    client.onEvent(onOtaEvent);

    client.begin(config);              // first update check runs immediately
}

void loop() {
    // Nothing to do: the client runs its own auto-check task.
}
```

That's all the integration code you need. For complete, working setups —
network bring-up, NTP sync, event logging, manual update control — pick one of
the [examples](examples/) and adapt it.

## Configuration

Every knob lives in [`OtamaticConfig`](src/otamatic/otamatic_config.hpp).
Only `apiKey` is mandatory; everything else has a sensible default.

| Option | Default | What it does |
|---|---|---|
| `apiKey` | — | Service key of your Otamatic account (**required**) |
| `firmwareVersion` | `0` | Version of the running firmware, compared against the server's |
| `versionName` | `nullptr` | Human-readable version (e.g. `"1.0.0"`), reported to the platform |
| `deviceId` | `0` | Unique device identifier; when `0`, it is derived from the MAC address |
| `apiBaseUrl` | `https://otamatic.eu/api` | API endpoint — change only for self-hosted servers |
| `checkInterval` | `300000` | Milliseconds between update checks (first check runs at `begin()`) |
| `enableAutoCheckMode` | `true` | Start the auto-check task at `begin()`. Set `false` for fully manual operation |
| `autoUpdateEnabled` | `true` | When a new version is found, download and install it without asking |
| `autoRestartEnabled` | `true` | Restart the device once an update is installed |
| `rollbackVerificationEnabled` | `true` | Confirm the new firmware at boot, with automatic rollback on failure. Customise the check with [`onUpdateVerification()`](#post-boot-verification) |
| `failedVersionGuardEnabled` | `true` | Keep skipping a version that already failed, to avoid update loops |
| `maxUpdateFailedAttempts` | `3` | Failed attempts before the guard blocks a version |
| `requireSignedUpdates` | `false` | Accept only firmware signed by the server |
| `publicKey` | `nullptr` | ECDSA P-256 public key (PEM or base64/DER) used to verify signatures |
| `autoCheckTaskStackSize` | `10240` | Stack size (bytes) of the auto-check task |

## How updates work

**Automatic mode** *(default)* — `begin()` starts a background task that checks
for updates every `checkInterval`, downloads what it finds, installs it and
restarts the device. You never touch it again.

**Manual mode** — disable `enableAutoCheckMode`, `autoUpdateEnabled` and/or
`autoRestartEnabled`, then drive the lifecycle yourself with `checkForUpdate()`
and `applyUpdate()`: download when the device is idle, restart when it suits you.

**One operation at a time** — the client serializes its operations. A manual
call while another operation (e.g. the auto-check) is running fails with the
`Busy` error — simply retry later — and the auto-check skips its cycle while a
manual operation is in progress.

### Post-boot verification

With `rollbackVerificationEnabled` (the default) every freshly installed
firmware must **prove itself at boot**: on the first boot after an update
`begin()` runs your custom verification code before anything else. Register it
with `onUpdateVerification()` **before** `begin()`:

```cpp
bool verifyNewFirmware() {
    // Your self-tests: peripherals, sensors, stored configuration...
    return selfTestPassed;   // true: confirm, false: roll back
}

client.onEvent(onOtaEvent);
client.onUpdateVerification(verifyNewFirmware);  // also: (callback, userData)
client.begin(config);
```

- The callback returns `true` to **confirm** the new firmware: the rollback is
  cancelled and the `UpdateConfirmed` event is emitted.
- The callback returns `false` to **roll back**: the `UpdateRolledBack` event is
  emitted (`VerificationFailed`), then the device restarts into the previous
  firmware.
- **No callback registered → always considered successful**: the new firmware is
  confirmed and no rollback can happen.
- `rollbackVerificationEnabled = false` skips your code entirely: the firmware
  is confirmed immediately.

The verification runs only on the first boot after an update — regular boots do
nothing — and requires a bootloader built with application rollback and
application-controlled confirmation:

| Framework | Required setting |
|---|---|
| Arduino-ESP32 core 3.x | Nothing to do: the official core ships a rollback-enabled bootloader |
| ESP-IDF 5.x | `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` |
| ESP-IDF 6.x | `CONFIG_BOOTLOADER_APP_ROLLBACK_CONFIRM_BY_APP` — the `CONFIRM_ON_STARTUP` default confirms the app before `app_main`, so the verification callback would never run |

The partition table must have two app slots + `otadata` (e.g. `ota_0`/`ota_1`).
Rollback protection is implemented jointly by the bootloader and the
application: on devices already in the field with a bootloader built *without*
rollback support, the safety net is inactive until the bootloader is rebuilt
and re-flashed. See the
[`verification_example`](examples/esp-idf/verification_example/) for a
complete, minimal setup.

### Events

Register a callback with `onEvent()` to follow every step. The callback runs
synchronously in the context of the task that generated the event: keep it
short and don't call back into the client from inside it. A second overload,
`onEvent(callback, userData)`, forwards a context pointer for use with classes.

| Event | Fired when |
|---|---|
| `CheckingForUpdate` | A new update check starts |
| `UpdateAvailable` | A newer firmware is available (carries version and size) |
| `UpdateNotNeeded` | The device already runs the latest version |
| `UpdateIgnored` | The update was skipped by the failed-version guard |
| `UpdateStarted` | The firmware download has started |
| `UpdateProgress` | Download progress (0–100 in `event.progress`) |
| `UpdateCompleted` | The firmware was downloaded, verified and installed |
| `UpdateConfirmed` | The new firmware was confirmed at boot (rollback cancelled) |
| `UpdateRolledBack` | Post-boot verification failed and the device rolled back |
| `OperationFailed` | An operation failed (error code in `event.error`) |

<details>
<summary><b>Error codes</b> (carried by <code>OperationFailed</code> events)</summary>

| Code | Meaning |
|---|---|
| `ConfigInvalid` | Invalid or incomplete configuration |
| `ConnectionFailed` | The HTTP connection could not be established |
| `HttpFailed` | The server answered with a non-200 status |
| `InvalidCheckData` | The check response is missing fields or malformed |
| `InvalidFirmware` | The advertised image is invalid (bad size, doesn't fit the partition) |
| `BeginFailed` | The OTA update could not be started |
| `DownloadFailed` | The download failed (connection dropped, stalled, size mismatch) |
| `WriteFailed` | The firmware could not be written to flash |
| `EndFailed` | The update could not be finalized (partition switch failed) |
| `IntegrityFailed` | The SHA-256 integrity verification failed |
| `SignatureFailed` | The firmware signature is missing or invalid |
| `InvalidImage` | The firmware image is not a valid app image |
| `VerificationFailed` | The post-boot firmware verification failed |
| `Busy` | Another operation is already in progress |

`OtamaticClient::getEventName()` and `OtamaticClient::getErrorName()` turn
these enums into human-readable strings.

</details>

## Examples

The [examples](examples/) folder is the reference for concrete implementations —
start from the one closest to your use case:

| Example | Shows |
|---|---|
| [`basic_example` (Arduino)](examples/arduino/basic_example/) | Minimal setup with event logging |
| [`basic_example` (ESP-IDF)](examples/esp-idf/basic_example/) | The same as a plain ESP-IDF project |
| [`verification_example` (Arduino)](examples/arduino/verification_example/) | Minimal post-boot verification: custom self-tests with rollback |
| [`verification_example` (ESP-IDF)](examples/esp-idf/verification_example/) | The same as a plain ESP-IDF project (with the rollback sdkconfig) |
| [`advanced_example` (Arduino)](examples/arduino/advanced_example/) | Manual update & restart control, custom `deviceId`, 60 s check interval |
| [`advanced_example` (ESP-IDF)](examples/esp-idf/advanced_example/) | The same as a plain ESP-IDF project |

## Reference

- Full API: the [`src/OtamaticClient.h`](src/OtamaticClient.h) header and its includes
- Self-hosting the server: the HTTP API specification in [docs/API.md](docs/API.md)
- Manual firmware upload page (optional, host it anywhere): [extras/index.html](extras/index.html)

## License

Licensed under the **Business Source License 1.1** (SPDX: `BUSL-1.1`) — see the [LICENSE](LICENSE) file.

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

These restrictions only apply if you are competing directly with the Licensor's
own managed OTA offering. If in doubt, contact us.

For commercial competitor licensing, contact the Licensor: **Timothy Franceschi &lt;timothy@franceschi.es&gt;**.