# Otamatic Server API

This document describes the HTTP API that an **Otamatic server** must expose to
be used with the OtamaticClient library. It is the official reference for
implementing a self-hosted server.

The client talks plain **HTTP/1.1** (or HTTPS, depending on the `Client`
transport you bind) to a single host. By default the host is `otamatic.eu` on
port `80`; when self-hosting, point the device at your server with:

```cpp
ota.setServer("your-server.example.com", 80);
```

There are exactly **two endpoints**. Both use `GET` and both require the same
authentication header.

## Authentication

Every request must carry the service key configured in `begin()`:

```
Authorization: Bearer <service-key>
```

- The service key is exactly **48 characters** (enforced by the client at
  `begin()`).
- Any other status code than `200` is treated as a failure by the client.

Example: with service key `0123456789abcdef0123456789abcdef0123456789abcdef`:

```
Authorization: Bearer 0123456789abcdef0123456789abcdef0123456789abcdef
```

---

## 1. Check for update

Called periodically (every 5 minutes by default) to find out whether a newer
firmware is available.

```
GET /api/v1/ota/latest?currentVersion=<uint32>&deviceId=<uint64>
```

### Query parameters

| Parameter | Type | Description |
|---|---|---|
| `currentVersion` | unsigned 32-bit integer (decimal) | Firmware version currently running on the device |
| `deviceId` | unsigned 64-bit integer (decimal) | Device identifier (derived from MAC, or custom via `setDeviceId()`) |

Example:

```
GET /api/v1/ota/latest?currentVersion=1&deviceId=332657491620279 HTTP/1.1
```

### Expected response payload

Status **200** with a JSON body:

```json
{
  "version": 2,
  "size": 524288,
  "integrity": "a3f1c8d94b2e7f605143a2b8c9d0e1f2a3b4c5d6e7f8091a2b3c4d5e6f708192",
  "signature": "30450221008f1e2d3c4b5a697867564534231201ffeeddccbbaa99887766554433221102200a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90"
}
```

| Field | Type | Required | Description |
|---|---|---|---|
| `version` | unsigned 32-bit integer | yes | Version of the available firmware. If it is **greater than** `currentVersion`, the client proceeds to download the new firmware.|
| `size` | integer (bytes) | yes | Size of the firmware binary. |
| `integrity` | string | yes | SHA-256 of the firmware binary, **hex-encoded**, exactly **64 characters** (case-insensitive). |
| `signature` | string | no | ECDSA P-256 signature of the firmware. Required only when `setRequireSignature(true)`. |

---

## 2. Download firmware

Called after the check reports a newer version, to fetch the binary.

```
GET /api/v1/ota/download?deviceId=<uint64>
```

### Query parameters

| Parameter | Type | Description |
|---|---|---|
| `deviceId` | unsigned 64-bit integer (decimal) | Device identifier (derived from MAC, or custom via `setDeviceId()`) |

Example:

```
GET /api/v1/ota/download?deviceId=332657491620279 HTTP/1.1
```

### Expected response payload

Status **200** with the **raw firmware binary** as body (no JSON):

- The body must be correspond to the `size` in bytes announced by the check
  endpoint.
- The SHA-256 of the body must match the `integrity` field returned by the
  check endpoint.
