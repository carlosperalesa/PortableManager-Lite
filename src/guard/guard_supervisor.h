#pragma once

#include "guard/guard_process.h"

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <span>
#include <vector>

namespace pm::guard {

class GuardSupervisor {
public:
    ~GuardSupervisor();
    void Start(std::wstring origin, runtime::SessionManager& session, std::function<void()> notify = {});
    bool ready() const noexcept { return ready_.load(); }
    void BeginShutdown() noexcept { stopping_.store(true); }
    void StopMonitoring();
    GuardProcess& process() noexcept { return process_; }

    // Keep the application's leases alive in PMG across owner death. Keep
    // owner-side duplicates too, for replacing a failed PMG.
    DWORD Protect(const std::wstring& app_id, std::span<const HANDLE> reservations);
    void Release(const std::wstring& app_id);

private:
    struct Reservation { HANDLE local; HANDLE remote; };
    static void CALLBACK Exited(PTP_CALLBACK_INSTANCE, void* context, PTP_WAIT, TP_WAIT_RESULT);
    static void CALLBACK Retry(PTP_CALLBACK_INSTANCE, void* context, PTP_TIMER);
    void ArmRetry(DWORD delay_ms);
    bool StartGuard();
    static bool Attach(HANDLE process, Reservation& reservation);
    void Release(std::vector<Reservation>& reservations);
    void Report(bool ready);

    std::mutex mutex_;
    GuardProcess process_;
    runtime::SessionManager* session_ = nullptr;
    std::wstring origin_;
    std::function<void()> notify_;
    PTP_WAIT wait_ = nullptr;
    PTP_TIMER timer_ = nullptr;
    std::map<std::wstring, std::vector<Reservation>> reservations_;
    std::atomic<bool> ready_{false};
    std::atomic<bool> stopping_{false};
    unsigned attempts_ = 0;
    ULONGLONG ready_since_ = 0;
};

} // namespace pm::guard
