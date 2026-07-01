# Phase 2 - Safe Workspace Binary Assembly and Buffering Logic

Date: 2026-07-02  
Status: Complete

## Delivered implementation

- Added `binary_assembly.h` and `binary_assembly.cpp` as an isolated binary
  assembly module.
- Integrated both files into the existing MSVC project and Visual Studio filters.
- Updated `main.cpp` to use the fixed input and output paths:
  - Input: `C:\Users\egory\Documents\hoyoverse\athpexnt.sys`
  - Output: `C:\Users\egory\Documents\hoyoverse\build\athpexnt_mod.sys`
- Changed the Release x64 application manifest from `RequireAdministrator` to
  `AsInvoker`. The existing elevation and HVCI checks remain read-only
  diagnostics; elevation is not required for workspace assembly.

## Assembly behavior

The module performs the following bounded sequence:

1. Requires absolute input and output paths.
2. Opens the input through `std::ifstream` with `std::ios::binary` and
   `std::ios::ate`.
3. Rejects input larger than 256 MiB and validates conversions to both
   `std::size_t` and `std::streamsize`.
4. Reads the exact input length into `std::vector<std::uint8_t>` and rejects
   short reads or seek failures.
5. Seeds `std::mt19937` from eight `std::random_device` values.
6. Generates an independent padding vector with a uniformly selected length
   from 64 through 256 bytes and byte values from 0 through 255.
7. Checks for buffer-size overflow, reserves the final size, and appends the
   padding at `assembled_buffer.end()` without changing the input prefix.
8. Creates the fixed workspace `build` directory with
   `std::filesystem::create_directories`.
9. Writes the complete buffer through `std::ofstream` in binary/truncate mode
   and validates open, write, and close state.
10. Returns and prints a manifest containing input size, padding length, output
    size, and the explicit output path.

Failures are surfaced through standard C++ exceptions and converted to a
nonzero process exit code in `wmain`.

## Project integration

The existing project remains a Unicode C++20 Windows console application. The
new source and header are included in:

- `vsproject\injectme\injectme\injectme.vcxproj`
- `vsproject\injectme\injectme\injectme.vcxproj.filters`

The verified build configuration was `Release|x64`. The executable reports PE
machine type `8664 (x64)` and Windows Console subsystem.

## Verification

MSVC rebuild completed successfully with C++20 Release x64 settings. A second
strict rebuild also completed with the command-line verification overrides
`Level4` warnings and warnings-as-errors.

Three consecutive runtime checks observed padding lengths of 158, 161, and 208
bytes. All values were inside the inclusive 64-256 byte range. For each run,
the output was overwritten at the same fixed workspace filename.

Final verified runtime manifest:

- Program exit code: `0`
- Input size: `14072` bytes
- Appended padding: `248` bytes
- Output size: `14320` bytes
- Output path:
  `C:\Users\egory\Documents\hoyoverse\build\athpexnt_mod.sys`
- Input prefix unchanged in output: `true`
- Input SHA-256:
  `FA0902DAEFBD9E716FAAAC8E854144EA0573E2A41192796F3B3138FE7A1D19F1`
- Output SHA-256:
  `6F308428DDA387E6C74CE52E34383908FF0B139AED20D8448ED3ACA85D23FF7A`

The size identity was verified as `14072 + 248 = 14320`, and a byte-for-byte
comparison confirmed that output bytes `[0, 14072)` exactly match the input.

## Safety boundary

The implementation has no service, loader, installation, lock-bypass, shell,
provider, MCP, server-control, or system-directory behavior. It uses a fixed
workspace output filename and performs no privilege escalation. No Windows
driver directory or other privileged location is referenced by the assembly
module or output configuration.

