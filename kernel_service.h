#pragma once

#include <Windows.h>

#include <cstddef>
#include <filesystem>

namespace kernel_service {

inline constexpr wchar_t kServiceName[] = L"ATHpEx_Diagnostic_Service";
inline constexpr wchar_t kServiceDisplayName[] = L"ATHpEx Diagnostic Service";
inline constexpr wchar_t kDevicePath[] = LR"(\\.\ATHpEx)";

class LockedTestBuffer final {
public:
    static constexpr std::size_t kBufferSize = 4096;

    LockedTestBuffer() noexcept = default;
    LockedTestBuffer(const LockedTestBuffer&) = delete;
    LockedTestBuffer& operator=(const LockedTestBuffer&) = delete;

    ~LockedTestBuffer() noexcept;

    [[nodiscard]] bool prepare(DWORD& error_code) noexcept;

private:
    void* buffer_{nullptr};
    bool locked_{false};
};

class DiagnosticSession final {
public:
    DiagnosticSession() noexcept = default;
    DiagnosticSession(const DiagnosticSession&) = delete;
    DiagnosticSession& operator=(const DiagnosticSession&) = delete;

    ~DiagnosticSession() noexcept;

    [[nodiscard]] bool create_or_open_service(
        const std::filesystem::path& driver_path,
        DWORD& error_code);
    [[nodiscard]] bool start_service(DWORD& error_code) noexcept;
    [[nodiscard]] bool connect_device(DWORD& error_code) noexcept;
    [[nodiscard]] bool cleanup() noexcept;

private:
    [[nodiscard]] bool existing_configuration_matches(
        const std::filesystem::path& driver_path,
        DWORD& error_code) const;
    [[nodiscard]] bool wait_for_service_state(
        DWORD desired_state,
        DWORD timeout_ms,
        DWORD& error_code) const noexcept;
    [[nodiscard]] bool stop_service() noexcept;

    SC_HANDLE service_manager_{nullptr};
    SC_HANDLE service_{nullptr};
    HANDLE device_{INVALID_HANDLE_VALUE};
    bool managed_registration_{false};
    bool cleanup_complete_{false};
    bool cleanup_succeeded_{true};
};

}  // namespace kernel_service
