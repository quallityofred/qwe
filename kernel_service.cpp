#include "kernel_service.h"

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <iostream>
#include <string_view>
#include <vector>

#pragma comment(lib, "Advapi32.lib")

namespace kernel_service {
namespace {

constexpr DWORD kServiceTransitionTimeoutMs = 15'000;

[[nodiscard]] std::wstring_view strip_optional_quotes(
    std::wstring_view path) noexcept {
    if (path.size() >= 2 && path.front() == L'\"' && path.back() == L'\"') {
        path.remove_prefix(1);
        path.remove_suffix(1);
    }
    return path;
}

[[nodiscard]] bool paths_equal(
    std::wstring_view left,
    std::wstring_view right) noexcept {
    left = strip_optional_quotes(left);
    right = strip_optional_quotes(right);

    return left.size() == right.size() &&
           _wcsnicmp(left.data(), right.data(), left.size()) == 0;
}

[[nodiscard]] bool query_service_status(
    SC_HANDLE service,
    SERVICE_STATUS_PROCESS& status,
    DWORD& error_code) noexcept {
    DWORD bytes_needed = 0;
    if (QueryServiceStatusEx(
            service,
            SC_STATUS_PROCESS_INFO,
            reinterpret_cast<BYTE*>(&status),
            sizeof(status),
            &bytes_needed) == FALSE) {
        error_code = GetLastError();
        return false;
    }

    error_code = ERROR_SUCCESS;
    return true;
}

}  // namespace

LockedTestBuffer::~LockedTestBuffer() noexcept {
    if (buffer_ == nullptr) {
        return;
    }

    SecureZeroMemory(buffer_, kBufferSize);

    if (locked_) {
        if (VirtualUnlock(buffer_, kBufferSize) == FALSE) {
            std::wcerr << L"[WARN] VirtualUnlock failed during buffer cleanup "
                       << L"(Windows error " << GetLastError() << L").\n";
        }
    }

    if (VirtualFree(buffer_, 0, MEM_RELEASE) == FALSE) {
        std::wcerr << L"[WARN] VirtualFree failed during buffer cleanup "
                   << L"(Windows error " << GetLastError() << L").\n";
    }
}

bool LockedTestBuffer::prepare(DWORD& error_code) noexcept {
    if (buffer_ != nullptr) {
        error_code = ERROR_ALREADY_INITIALIZED;
        return false;
    }

    buffer_ = VirtualAlloc(
        nullptr,
        kBufferSize,
        MEM_RESERVE | MEM_COMMIT,
        PAGE_READWRITE);
    if (buffer_ == nullptr) {
        error_code = GetLastError();
        return false;
    }

    std::memset(buffer_, 0xA5, kBufferSize);
    if (VirtualLock(buffer_, kBufferSize) == FALSE) {
        error_code = GetLastError();
        SecureZeroMemory(buffer_, kBufferSize);
        VirtualFree(buffer_, 0, MEM_RELEASE);
        buffer_ = nullptr;
        return false;
    }

    locked_ = true;
    error_code = ERROR_SUCCESS;
    return true;
}

DiagnosticSession::~DiagnosticSession() noexcept {
    (void)cleanup();
}

bool DiagnosticSession::create_or_open_service(
    const std::filesystem::path& driver_path,
    DWORD& error_code) {
    if (service_manager_ != nullptr || service_ != nullptr) {
        error_code = ERROR_ALREADY_INITIALIZED;
        return false;
    }

    cleanup_complete_ = false;
    service_manager_ = OpenSCManagerW(
        nullptr,
        nullptr,
        SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
    if (service_manager_ == nullptr) {
        error_code = GetLastError();
        return false;
    }

    constexpr DWORD service_access =
        DELETE | SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS |
        SERVICE_START | SERVICE_STOP;

    service_ = CreateServiceW(
        service_manager_,
        kServiceName,
        kServiceDisplayName,
        service_access,
        SERVICE_KERNEL_DRIVER,
        SERVICE_DEMAND_START,
        SERVICE_ERROR_NORMAL,
        driver_path.c_str(),
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr);

    if (service_ != nullptr) {
        managed_registration_ = true;
        error_code = ERROR_SUCCESS;
        std::wcout << L"[SERVICE] Created service '" << kServiceName
                   << L"' for " << driver_path.c_str() << L".\n";
        return true;
    }

    const DWORD create_error = GetLastError();
    if (create_error != ERROR_SERVICE_EXISTS &&
        create_error != ERROR_DUPLICATE_SERVICE_NAME) {
        error_code = create_error;
        return false;
    }

    service_ = OpenServiceW(service_manager_, kServiceName, service_access);
    if (service_ == nullptr) {
        error_code = GetLastError();
        return false;
    }

    if (!existing_configuration_matches(driver_path, error_code)) {
        std::wcerr << L"[ERROR] Existing service '" << kServiceName
                   << L"' does not match the required kernel-driver path and type; "
                   << L"it will not be modified.\n";
        return false;
    }

    managed_registration_ = true;
    error_code = ERROR_SUCCESS;
    std::wcout << L"[SERVICE] Opened matching existing service '"
               << kServiceName << L"'.\n";
    return true;
}

bool DiagnosticSession::existing_configuration_matches(
    const std::filesystem::path& driver_path,
    DWORD& error_code) const {
    DWORD bytes_needed = 0;
    const BOOL size_query_result =
        QueryServiceConfigW(service_, nullptr, 0, &bytes_needed);
    const DWORD size_query_error = GetLastError();
    if (size_query_result != FALSE) {
        error_code = ERROR_INVALID_DATA;
        return false;
    }

    if (size_query_error != ERROR_INSUFFICIENT_BUFFER ||
        bytes_needed == 0) {
        error_code = size_query_error;
        return false;
    }

    std::vector<BYTE> storage(bytes_needed);
    auto* configuration =
        reinterpret_cast<QUERY_SERVICE_CONFIGW*>(storage.data());
    if (QueryServiceConfigW(
            service_,
            configuration,
            bytes_needed,
            &bytes_needed) == FALSE) {
        error_code = GetLastError();
        return false;
    }

    if (configuration->dwServiceType != SERVICE_KERNEL_DRIVER ||
        configuration->lpBinaryPathName == nullptr ||
        !paths_equal(configuration->lpBinaryPathName, driver_path.native())) {
        error_code = ERROR_SERVICE_EXISTS;
        return false;
    }

    error_code = ERROR_SUCCESS;
    return true;
}

bool DiagnosticSession::wait_for_service_state(
    DWORD desired_state,
    DWORD timeout_ms,
    DWORD& error_code) const noexcept {
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;

    while (true) {
        SERVICE_STATUS_PROCESS status{};
        if (!query_service_status(service_, status, error_code)) {
            return false;
        }

        if (status.dwCurrentState == desired_state) {
            error_code = ERROR_SUCCESS;
            return true;
        }

        if (desired_state == SERVICE_RUNNING &&
            status.dwCurrentState == SERVICE_STOPPED) {
            error_code = status.dwWin32ExitCode != ERROR_SUCCESS
                ? status.dwWin32ExitCode
                : ERROR_SERVICE_NOT_ACTIVE;
            return false;
        }

        if (GetTickCount64() >= deadline) {
            error_code = ERROR_TIMEOUT;
            return false;
        }

        const DWORD wait_hint = status.dwWaitHint == 0 ? 100 : status.dwWaitHint / 10;
        Sleep((std::clamp)(wait_hint, 50UL, 500UL));
    }
}

bool DiagnosticSession::start_service(DWORD& error_code) noexcept {
    if (service_ == nullptr || !managed_registration_) {
        error_code = ERROR_INVALID_HANDLE;
        return false;
    }

    if (StartServiceW(service_, 0, nullptr) == FALSE) {
        const DWORD start_error = GetLastError();
        if (start_error != ERROR_SERVICE_ALREADY_RUNNING) {
            error_code = start_error;
            return false;
        }
    }

    if (!wait_for_service_state(
            SERVICE_RUNNING,
            kServiceTransitionTimeoutMs,
            error_code)) {
        return false;
    }

    std::wcout << L"[SERVICE] Startup complete for '" << kServiceName << L"'.\n";
    error_code = ERROR_SUCCESS;
    return true;
}

bool DiagnosticSession::connect_device(DWORD& error_code) noexcept {
    if (device_ != INVALID_HANDLE_VALUE) {
        error_code = ERROR_ALREADY_INITIALIZED;
        return false;
    }

    device_ = CreateFileW(
        kDevicePath,
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (device_ == INVALID_HANDLE_VALUE) {
        error_code = GetLastError();
        return false;
    }

    std::wcout << L"[DEVICE] Acquired read/write handle to "
               << kDevicePath << L".\n";
    error_code = ERROR_SUCCESS;
    return true;
}

bool DiagnosticSession::stop_service() noexcept {
    SERVICE_STATUS_PROCESS current_status{};
    DWORD query_error = ERROR_SUCCESS;
    if (!query_service_status(service_, current_status, query_error)) {
        std::wcerr << L"[WARN] Service status query failed during cleanup "
                   << L"(Windows error " << query_error << L").\n";
        return false;
    }

    if (current_status.dwCurrentState == SERVICE_STOPPED) {
        return true;
    }

    if (current_status.dwCurrentState == SERVICE_START_PENDING) {
        DWORD transition_error = ERROR_SUCCESS;
        (void)wait_for_service_state(
            SERVICE_RUNNING,
            kServiceTransitionTimeoutMs,
            transition_error);

        if (!query_service_status(service_, current_status, query_error)) {
            std::wcerr << L"[WARN] Service status query failed after startup "
                       << L"wait (Windows error " << query_error << L").\n";
            return false;
        }

        if (current_status.dwCurrentState == SERVICE_STOPPED) {
            return true;
        }
    }

    if (current_status.dwCurrentState == SERVICE_STOP_PENDING) {
        DWORD wait_error = ERROR_SUCCESS;
        if (!wait_for_service_state(
                SERVICE_STOPPED,
                kServiceTransitionTimeoutMs,
                wait_error)) {
            std::wcerr << L"[WARN] Timed out waiting for service shutdown "
                       << L"(Windows error " << wait_error << L").\n";
            return false;
        }
        return true;
    }

    SERVICE_STATUS stop_status{};
    if (ControlService(service_, SERVICE_CONTROL_STOP, &stop_status) == FALSE) {
        const DWORD stop_error = GetLastError();
        if (stop_error != ERROR_SERVICE_NOT_ACTIVE) {
            std::wcerr << L"[WARN] ControlService(SERVICE_CONTROL_STOP) failed "
                       << L"(Windows error " << stop_error << L").\n";
            return false;
        }
        return true;
    }

    DWORD wait_error = ERROR_SUCCESS;
    if (!wait_for_service_state(
            SERVICE_STOPPED,
            kServiceTransitionTimeoutMs,
            wait_error)) {
        std::wcerr << L"[WARN] Service did not reach the stopped state "
                   << L"(Windows error " << wait_error << L").\n";
        return false;
    }

    std::wcout << L"[CLEANUP] Stopped service '" << kServiceName << L"'.\n";
    return true;
}

bool DiagnosticSession::cleanup() noexcept {
    if (cleanup_complete_) {
        return cleanup_succeeded_;
    }
    cleanup_complete_ = true;

    if (device_ != INVALID_HANDLE_VALUE) {
        if (CloseHandle(device_) == FALSE) {
            std::wcerr << L"[WARN] Device handle close failed (Windows error "
                       << GetLastError() << L").\n";
            cleanup_succeeded_ = false;
        } else {
            std::wcout << L"[CLEANUP] Closed device handle.\n";
        }
        device_ = INVALID_HANDLE_VALUE;
    }

    if (service_ != nullptr && managed_registration_) {
        if (!stop_service()) {
            cleanup_succeeded_ = false;
        }
        if (DeleteService(service_) == FALSE) {
            const DWORD delete_error = GetLastError();
            if (delete_error != ERROR_SERVICE_MARKED_FOR_DELETE) {
                std::wcerr << L"[WARN] DeleteService failed (Windows error "
                           << delete_error << L").\n";
                cleanup_succeeded_ = false;
            }
        } else {
            std::wcout << L"[CLEANUP] Deleted service entry '"
                       << kServiceName << L"'.\n";
        }
        managed_registration_ = false;
    }

    if (service_ != nullptr) {
        if (CloseServiceHandle(service_) == FALSE) {
            std::wcerr << L"[WARN] Service handle close failed (Windows error "
                       << GetLastError() << L").\n";
            cleanup_succeeded_ = false;
        }
        service_ = nullptr;
    }

    if (service_manager_ != nullptr) {
        if (CloseServiceHandle(service_manager_) == FALSE) {
            std::wcerr << L"[WARN] SCM handle close failed (Windows error "
                       << GetLastError() << L").\n";
            cleanup_succeeded_ = false;
        }
        service_manager_ = nullptr;
    }

    return cleanup_succeeded_;
}

}  // namespace kernel_service
