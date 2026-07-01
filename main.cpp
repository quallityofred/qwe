#include <Windows.h>

#include <filesystem>
#include <iostream>
#include <system_error>
#include <string>
#include <array>

#include "binary_assembly.h"
#include "environment_check.h"
#include "kernel_service.h"

namespace {

class TokenHandle final {
public:
    TokenHandle() noexcept = default;

    TokenHandle(const TokenHandle&) = delete;
    TokenHandle& operator=(const TokenHandle&) = delete;

    ~TokenHandle() noexcept {
        if (handle_ != nullptr) {
            CloseHandle(handle_);
        }
    }

    [[nodiscard]] HANDLE* put() noexcept {
        return &handle_;
    }

    [[nodiscard]] HANDLE get() const noexcept {
        return handle_;
    }

private:
    HANDLE handle_{nullptr};
};

struct ElevationCheckResult {
    bool is_elevated{false};
    DWORD error_code{ERROR_SUCCESS};
};

[[nodiscard]] ElevationCheckResult check_process_elevation() noexcept {
    TokenHandle process_token;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, process_token.put()) == FALSE) {
        return {false, GetLastError()};
    }

    TOKEN_ELEVATION elevation{};
    DWORD bytes_returned = 0;
    if (GetTokenInformation(
            process_token.get(),
            TokenElevation,
            &elevation,
            sizeof(elevation),
            &bytes_returned) == FALSE) {
        return {false, GetLastError()};
    }

    if (bytes_returned != sizeof(elevation)) {
        return {false, ERROR_INVALID_DATA};
    }

    return {elevation.TokenIsElevated != 0, ERROR_SUCCESS};
}

void log_hvci_state(const environment_check::HvciAuditResult& result) {
    using environment_check::HvciState;

    switch (result.state) {
        case HvciState::enabled:
            std::wcout << L"[INFO] HVCI: enabled.\n";
            break;
        case HvciState::disabled:
            std::wcout << L"[INFO] HVCI: disabled.\n";
            break;
        case HvciState::not_configured:
            std::wcout << L"[INFO] HVCI: not configured.\n";
            break;
        case HvciState::query_failed:
            std::wcerr << L"[ERROR] HVCI registry audit failed (Windows error "
                       << result.error_code << L").\n";
            break;
    }
}

    bool is_test_signing_enabled() {
        std::array<char, 128> buffer;
        std::string result;
        // Запускаем bcdedit для чтения текущего состояния
        std::unique_ptr<FILE, decltype(&_pclose)> pipe(_popen("bcdedit /enum {current}", "r"), _pclose);
        if (!pipe) return false;

        while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe.get()) != nullptr) {
            result += buffer.data();
        }
        // Если в выводе есть "testsigning Yes", значит режим активен
        return result.find("testsigning             Yes") != std::string::npos;
    }

    // Функция принудительно включает тестовый режим через реестр/команду
    bool enable_test_signing() {
        std::wcout << L"[INFO] Attempting to enable Windows Test Signing Mode...\n";
        int ret = std::system("bcdedit /set testsigning on");
        return ret == 0;
    }

}  // namespace

int wmain() {
    constexpr wchar_t driver_path_text[] =
        LR"(C:\Users\egory\Documents\hoyoverse\athpexnt.sys)";
    constexpr wchar_t output_path_text[] =
        LR"(C:\Users\egory\Documents\hoyoverse\build\athpexnt_mod.sys)";
    const std::filesystem::path driver_path{driver_path_text};
    const std::filesystem::path output_path{output_path_text};

    std::wcout << L"[INFO] Starting environment pre-flight validation.\n";

    bool preflight_passed = true;

    const ElevationCheckResult elevation = check_process_elevation();
    if (elevation.error_code != ERROR_SUCCESS) {
        std::wcerr << L"[WARN] Process token elevation check failed (Windows error "
                   << elevation.error_code << L").\n";
    } else if (!elevation.is_elevated) {
        std::wcout << L"[INFO] Process is not elevated; workspace assembly can "
                      L"proceed, but Phase 3 service operations will be blocked.\n";
    } else {
        std::wcout << L"[INFO] Process is elevated; Phase 3 service operations "
                      L"are permitted.\n";
    }

    const environment_check::HvciAuditResult hvci = environment_check::audit_hvci();
    log_hvci_state(hvci);

    std::error_code filesystem_error;
    const bool driver_exists =
        driver_path.is_absolute() && std::filesystem::exists(driver_path, filesystem_error);

    if (filesystem_error) {
        std::wcerr << L"[ERROR] Driver path validation failed (error "
                   << filesystem_error.value() << L").\n";
        preflight_passed = false;
    } else if (!driver_exists) {
        std::wcerr << L"[ERROR] Required file was not found at the absolute path: "
                   << driver_path.c_str() << L"\n";
        preflight_passed = false;
    } else {
        std::wcout << L"[PASS] Required file exists: " << driver_path.c_str() << L"\n";
    }

    if (!preflight_passed) {
        std::wcerr << L"[FAIL] Environment pre-flight validation failed.\n";
        return 1;
    }

    std::wcout << L"[PASS] Environment pre-flight validation completed.\n";

    try {
        const binary_assembly::AssemblyManifest manifest =
            binary_assembly::assemble_with_random_padding(driver_path, output_path);

        std::wcout << L"[MANIFEST] Input size: " << manifest.input_size << L" bytes\n"
                   << L"[MANIFEST] Appended padding: " << manifest.padding_size
                   << L" bytes\n"
                   << L"[MANIFEST] Output size: " << manifest.output_size << L" bytes\n"
                   << L"[MANIFEST] Output path: " << manifest.output_path.c_str()
                   << L"\n"
                   << L"[PASS] Workspace binary assembly completed.\n";
    } catch (const std::exception& error) {
        std::cerr << "[ERROR] Workspace binary assembly failed: "
                  << error.what() << '\n';
        return 1;
    }

    if (!elevation.is_elevated) {
        std::wcerr << L"[ERROR] Phase 3 requires an elevated process to manage "
                      L"the diagnostic kernel-driver service.\n";
        return 1;
    }

    kernel_service::LockedTestBuffer locked_buffer;
    DWORD phase3_error = ERROR_SUCCESS;
    if (!locked_buffer.prepare(phase3_error)) {
        std::wcerr << L"[ERROR] VirtualLock buffer preparation failed (Windows error "
                   << phase3_error << L").\n";
        return 1;
    }
    std::wcout << L"[MEMORY] Locked a "
               << kernel_service::LockedTestBuffer::kBufferSize
               << L"-byte diagnostic test buffer in RAM.\n";

    kernel_service::DiagnosticSession diagnostic_session;
    if (!diagnostic_session.create_or_open_service(output_path, phase3_error)) {
        std::wcerr << L"[ERROR] Service creation/open failed (Windows error "
                   << phase3_error << L").\n";
        return 1;
    }

    if (!is_test_signing_enabled()) {
        std::wcerr << L"[WARN] Windows Test Signing Mode is DISABLED (This will cause Error 577).\n";

        if (enable_test_signing()) {
            std::wcout << L"\n===========================================================\n";
            std::wcout << L"[SUCCESS] Test Signing Mode has been enabled successfully!\n";
            std::wcout << L"[REQUIRED] A system restart is required to apply changes.\n";
            std::wcout << L"Would you like to restart your PC now? (Y/N): ";

            wchar_t response;
            std::wcin >> response;
            if (response == L'Y' || response == L'y') {
                std::wcout << L"[INFO] Restarting system...\n";
                std::system("shutdown /r /t 5 /c \"Restarting for Kernel Driver Testing\"");
            }
            return 0;
        }
        else {
            std::wcerr << L"[ERROR] Failed to enable Test Signing. Please run this EXE as Administrator.\n";
            return 1;
        }
    }
    else {
        std::wcout << L"[PASS] Windows Test Signing Mode is active.\n";
    }

    if (!diagnostic_session.start_service(phase3_error)) {
        std::wcerr << L"[ERROR] Service startup failed (Windows error "
                   << phase3_error << L").\n";
        return 1;
    }

    if (!diagnostic_session.connect_device(phase3_error)) {
        std::wcerr << L"[ERROR] Device connection to "
                   << kernel_service::kDevicePath << L" failed (Windows error "
                   << phase3_error << L").\n";
        return 1;
    }

    std::wcout << L"[INFO] Phase 3 diagnostic operations completed; cleanup follows.\n";
    if (!diagnostic_session.cleanup()) {
        std::wcerr << L"[ERROR] Phase 3 cleanup did not complete cleanly.\n";
        return 1;
    }

    std::wcout << L"[PASS] Phase 3 diagnostic lifecycle completed.\n";

    return 0;
}
