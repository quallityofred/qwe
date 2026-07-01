#pragma once

#include <Windows.h>

#pragma comment(lib, "Advapi32.lib")

namespace environment_check {

enum class HvciState {
    enabled,
    disabled,
    not_configured,
    query_failed
};

struct HvciAuditResult {
    HvciState state{HvciState::query_failed};
    LSTATUS error_code{ERROR_SUCCESS};

    [[nodiscard]] constexpr bool audit_succeeded() const noexcept {
        return state != HvciState::query_failed;
    }
};

class RegistryKey final {
public:
    RegistryKey() noexcept = default;

    RegistryKey(const RegistryKey&) = delete;
    RegistryKey& operator=(const RegistryKey&) = delete;

    ~RegistryKey() noexcept {
        if (key_ != nullptr) {
            RegCloseKey(key_);
        }
    }

    [[nodiscard]] HKEY* put() noexcept {
        return &key_;
    }

    [[nodiscard]] HKEY get() const noexcept {
        return key_;
    }

private:
    HKEY key_{nullptr};
};

[[nodiscard]] inline HvciAuditResult audit_hvci() noexcept {
    constexpr wchar_t hvci_key_path[] =
        L"SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\"
        L"HypervisorEnforcedCodeIntegrity";
    constexpr wchar_t enabled_value_name[] = L"Enabled";

    RegistryKey key;
    const LSTATUS open_status = RegOpenKeyExW(
        HKEY_LOCAL_MACHINE,
        hvci_key_path,
        0,
        KEY_QUERY_VALUE | KEY_WOW64_64KEY,
        key.put());

    if (open_status == ERROR_FILE_NOT_FOUND) {
        return {HvciState::not_configured, ERROR_SUCCESS};
    }

    if (open_status != ERROR_SUCCESS) {
        return {HvciState::query_failed, open_status};
    }

    DWORD value_type = 0;
    DWORD enabled_value = 0;
    DWORD value_size = sizeof(enabled_value);
    const LSTATUS query_status = RegQueryValueExW(
        key.get(),
        enabled_value_name,
        nullptr,
        &value_type,
        reinterpret_cast<BYTE*>(&enabled_value),
        &value_size);

    if (query_status == ERROR_FILE_NOT_FOUND) {
        return {HvciState::not_configured, ERROR_SUCCESS};
    }

    if (query_status != ERROR_SUCCESS) {
        return {HvciState::query_failed, query_status};
    }

    if (value_type != REG_DWORD || value_size != sizeof(enabled_value)) {
        return {HvciState::query_failed, ERROR_DATATYPE_MISMATCH};
    }

    return {
        enabled_value != 0 ? HvciState::enabled : HvciState::disabled,
        ERROR_SUCCESS};
}

}  // namespace environment_check
