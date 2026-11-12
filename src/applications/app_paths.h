#pragma once

#include "applications/applications_module.h"

#include <memory>

namespace pm::applications {

struct CloseHandle {
    void operator()(HANDLE handle) const { ::CloseHandle(handle); }
};
using Handle = std::unique_ptr<void, CloseHandle>;
using Pins = std::vector<Handle>;

bool Equal(std::wstring_view a, std::wstring_view b);
bool Within(std::wstring_view path, std::wstring_view parent);
bool SafeId(std::wstring_view id);
DWORD PinDirectory(const std::wstring& path, Pins& pins);
DWORD EnsureDirectory(const core::PathResolver& resolver, std::wstring_view relative, Pins& pins);
DWORD SaveApp(const core::PathResolver& resolver, App app, bool removed);
DWORD CheckFile(const std::wstring& path, Handle& file);
DWORD AcquireApp(const core::PathResolver& resolver, std::wstring_view directory, Handle& lease);
DWORD ValidateApp(const core::PathResolver& resolver, App& app, bool check_on_disk);

} // namespace pm::applications
