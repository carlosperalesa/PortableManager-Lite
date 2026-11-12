#pragma once
#include <windows.h>
#include <string>

namespace pm::runtime {
// Serializes preparation, launching and restoration. An abandoned lock is acquired
// normally: callers reload the manifest and inspect actual resources before acting.
class SessionLock {
public:
    explicit SessionLock(const std::wstring& name) {
        handle_ = ::CreateMutexW(nullptr, FALSE, name.c_str());
        if (!handle_) { error_ = ::GetLastError(); return; }
        const DWORD wait = ::WaitForSingleObject(handle_, INFINITE);
        owned_ = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
        if (!owned_) error_ = ::GetLastError();
    }
    ~SessionLock() {
        if (owned_) ::ReleaseMutex(handle_);
        if (handle_) ::CloseHandle(handle_);
    }
    SessionLock(const SessionLock&) = delete;
    SessionLock& operator=(const SessionLock&) = delete;
    bool ok() const noexcept { return owned_; }
    DWORD error() const noexcept { return error_; }
private:
    HANDLE handle_ = nullptr;
    bool owned_ = false;
    DWORD error_ = ERROR_SUCCESS;
};
}
