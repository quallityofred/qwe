# Phase 1 — Environment Auditing & Pre-Flight Core Validation

Date: 2026-07-02  
Status: Complete

## Delivered files

- `environment_check.h`
- `main.cpp`

The source files are located at the workspace root. The pre-existing Visual Studio
project metadata was not changed.

## Implementation status

### HVCI audit

- Opens the required HKLM registry location with `RegOpenKeyExW` and read-only
  access to the 64-bit registry view.
- Reads `Enabled` with `RegQueryValueExW`.
- Validates both the registry value type (`REG_DWORD`) and returned byte count.
- Uses an RAII registry handle so `RegCloseKey` runs on every opened-key path.
- Reports enabled, disabled, not configured, and query failure as separate states.
- Treats any nonzero valid `Enabled` DWORD as enabled.

### Process elevation audit

- Opens the current process token with `OpenProcessToken` and `TOKEN_QUERY`.
- Calls `GetTokenInformation` with `TokenElevation`.
- Validates the returned structure size and requires `TokenIsElevated` to be
  nonzero.
- Uses an RAII token handle so `CloseHandle` runs on every opened-token path.
- Fails closed if either token API fails or the process is not fully elevated.

### Absolute file-path audit

- Uses the UTF-16 absolute path
  `C:\Users\egory\Documents\hoyoverse\athpexnt.sys`.
- Confirms that the path is absolute.
- Checks it with the non-throwing `std::filesystem::exists` overload and reports
  filesystem errors separately from a missing path.

### Unicode and runtime behavior

- Uses `wmain`, wide string literals, wide console streams, and explicit Windows
  `W` registry APIs.
- The compile configuration defines both `UNICODE` and `_UNICODE`.
- The program evaluates every audit, emits concise pass/info/error messages, and
  returns `0` only when elevation, HVCI query integrity, and file existence checks
  succeed.

## Verification

MSVC compilation completed successfully with an x64 developer environment and
the following effective settings:

- C++20 (`/std:c++20`)
- x64 target
- Release optimization (`/O2`, `/GL`, `/LTCG`)
- Strict conformance (`/permissive-`)
- Level 4 warnings treated as errors (`/W4`, `/WX`)
- Unicode definitions (`/DUNICODE`, `/D_UNICODE`)

The resulting executable was inspected as PE machine `8664 (x64)` with Windows
Console subsystem. The final rebuild exited with code `0`.

Runtime validation on 2026-07-02 produced:

- Process token: fully elevated — pass
- HVCI: disabled — audit succeeded
- Required file: exists — pass
- Program exit code: `0`

Build artifacts are isolated under `workspace/build/phase1`.

## Technical conclusion

Phase 1 is implemented and verified. No GUI, command-shell execution logic,
provider-native integrations, MCP execution, environment-layer access, git
operations, or server lifecycle changes were introduced into the application.
