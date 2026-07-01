# Phase 3 - Kernel Service Management and Device Interface Connection

Date: 2026-07-02  
Status: Implementation complete; Release x64 build and static analysis verified

## Delivered implementation

- Added `kernel_service.h` and `kernel_service.cpp` as the Phase 3 lifecycle
  module and integrated them into the existing Visual Studio project/filters.
- Retained the Unicode C++20 Windows console architecture and verified the
  `Release|x64` output.
- Changed the Release x64 application manifest to `RequireAdministrator`, as
  creating and starting a kernel-driver service requires an elevated token.
- Used the fixed service identifier `ATHpEx_Diagnostic_Service` and fixed
  Phase 2 driver path
  `C:\Users\egory\Documents\hoyoverse\build\athpexnt_mod.sys`.
- Used the fixed device interface path `\\.\ATHpEx`.

## Service and device lifecycle

The diagnostic lifecycle now performs these operations in order:

1. Allocates a page-backed 4096-byte local test buffer, initializes it, and
   locks it with `VirtualLock`.
2. Opens the Service Control Manager with `OpenSCManagerW`.
3. Registers the driver with `CreateServiceW` as a demand-start
   `SERVICE_KERNEL_DRIVER`.
4. If the fixed service already exists, opens it through `OpenServiceW` and
   validates both its service type and exact binary path before treating it as
   a managed diagnostic registration. A name collision with a different type
   or path is left untouched and reported as an error.
5. Starts the driver through `StartServiceW` and polls
   `QueryServiceStatusEx` until `SERVICE_RUNNING` or a bounded 15-second
   timeout/failure occurs.
6. Opens `\\.\ATHpEx` with `CreateFileW`, `GENERIC_READ | GENERIC_WRITE`,
   exclusive sharing, `OPEN_EXISTING`, and `FILE_ATTRIBUTE_NORMAL`.
7. Logs service creation/open, completed startup, successful device handle
   acquisition, and cleanup transitions separately.

## Cleanup architecture

`DiagnosticSession` owns all SCM, service, and device handles. Its cleanup is
explicitly invoked on the success path and is also invoked by its destructor
on every early return or exception path. Cleanup is idempotent and runs in the
following order:

1. Close the device handle with `CloseHandle`.
2. Query the service state. If startup is still pending, wait for the pending
   transition before attempting control.
3. Stop an active service with
   `ControlService(..., SERVICE_CONTROL_STOP, ...)` and wait for
   `SERVICE_STOPPED`.
4. Call `DeleteService` even if the stop attempt reports a failure, so the
   registration is still marked for deletion when possible.
5. Close the service and SCM handles with `CloseServiceHandle`.
6. Zero, unlock with `VirtualUnlock`, and release the local test buffer.

Cleanup failures are logged and make the successful diagnostic path return a
nonzero process exit code. Windows can still refuse driver unload when the
driver does not support unloading; that OS/driver condition is surfaced rather
than hidden.

## Project integration

Updated files:

- `main.cpp`
- `kernel_service.h`
- `kernel_service.cpp`
- `vsproject\injectme\injectme\injectme.vcxproj`
- `vsproject\injectme\injectme\injectme.vcxproj.filters`

The existing Phase 1 pre-flight and Phase 2 workspace assembly remain in the
same application flow. Phase 3 begins only after the fixed Phase 2 output has
been assembled and the process elevation check has succeeded.

## Verification

- MSVC Release x64 rebuild: passed.
- Strict rebuild with warning level 4 and warnings-as-errors: passed.
- MSVC C/C++ code analysis: passed with no remaining diagnostics.
- PE machine: `8664 (x64)`.
- PE subsystem: Windows Console (`Windows CUI`).
- Embedded manifest contains `requestedExecutionLevel` and
  `requireAdministrator`.
- Import inspection confirmed `OpenSCManagerW`, `CreateServiceW`,
  `OpenServiceW`, `StartServiceW`, `QueryServiceStatusEx`, `ControlService`,
  `DeleteService`, `CreateFileW`, `VirtualLock`, and `VirtualUnlock`.
- Final executable:
  `C:\Users\egory\Documents\hoyoverse\vsproject\injectme\injectme\x64\Release\injectme.exe`
- Final executable size: `75776` bytes.
- Final executable SHA-256:
  `0034DCB7888CE0950FB0A3C774C08445EB0640C0BDE0FDE8222B45E58E88DF36`.
- Source/project scan found no system driver-directory path, process/shell
  launch, `DeviceIoControl`, or DKOM implementation.

The executable was not launched during development verification. Launching it
would perform the explicitly privileged state changes (service creation and
driver start) and attempt to load the Phase 2-modified driver. Actual service
startup remains dependent on the driver's validity, signing policy, HVCI state,
and whether it publishes the expected `\\.\ATHpEx` symbolic link.

## Constraint status

- No files are copied or written to `C:\Windows\System32\drivers`.
- No DKOM, randomized service naming, registry evasion, or footprint-hiding
  behavior is present.
- No GUI redesign or graphical layer was added.
- No shell, PowerShell, CMD, or bash execution was added to the application.
- No provider-native calls, MCP execution, external environment configuration,
  secrets, or production-server control were added.
