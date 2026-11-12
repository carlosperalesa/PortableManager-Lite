#include "guard/guard_supervisor.h"

namespace pm::guard {
namespace {
constexpr DWORD kRetryDelays[] = {0, 2000, 5000};
// A guardian that repeatedly exits right after reporting ready consumes the
// same retry budget. A stable guard starts a fresh incident on the next exit.
constexpr ULONGLONG kStableMs = 30000;
}

GuardSupervisor::~GuardSupervisor() {
    StopMonitoring();
    for (auto& [id, reservations] : reservations_) {
        (void)id;
        // If cleanup failed, a live PMG retains the remote reservations for
        // recovery after owner exit.
        for (const auto& reservation : reservations) ::CloseHandle(reservation.local);
    }
    if (wait_) ::CloseThreadpoolWait(wait_);
    if (timer_) ::CloseThreadpoolTimer(timer_);
}

void GuardSupervisor::Start(std::wstring origin, runtime::SessionManager& session, std::function<void()> notify) {
    std::lock_guard lock(mutex_);
    origin_ = std::move(origin);
    session_ = &session;
    notify_ = std::move(notify);
    wait_ = ::CreateThreadpoolWait(Exited, this, nullptr);
    timer_ = ::CreateThreadpoolTimer(Retry, this, nullptr);
    if (wait_ && timer_ && !StartGuard()) ArmRetry(kRetryDelays[0]);
}

void GuardSupervisor::Report(bool ready) {
    if (ready_.exchange(ready) != ready && notify_) notify_();
}

bool GuardSupervisor::StartGuard() {
    DWORD error = ERROR_SUCCESS;
    // Retire the exact owned image of the last instance before copying. A
    // failed retirement leaves its handle available for the next attempt.
    if (stopping_ || !process_.Retire(error)) return false;
    for (auto& entry : reservations_)
        for (auto& reservation : entry.second) reservation.remote = nullptr;
    const auto transfer = [&](HANDLE process) {
        for (auto& entry : reservations_)
            for (auto& reservation : entry.second)
                if (!Attach(process, reservation)) return false;
        return true;
    };
    if (!process_.Start(origin_, *session_, error, nullptr, cleanup::Artifacts::Remove, nullptr, transfer))
        return false;
    ready_since_ = ::GetTickCount64();
    ::SetThreadpoolWait(wait_, process_.handle(), nullptr);
    Report(true);
    return true;
}

void GuardSupervisor::ArmRetry(DWORD delay_ms) {
    if (stopping_) return;
    LARGE_INTEGER relative{};
    relative.QuadPart = delay_ms ? -static_cast<LONGLONG>(delay_ms) * 10000 : -1;
    FILETIME due{relative.LowPart, static_cast<DWORD>(relative.HighPart)};
    ::SetThreadpoolTimer(timer_, &due, 0, 0);
}

void CALLBACK GuardSupervisor::Exited(PTP_CALLBACK_INSTANCE, void* context, PTP_WAIT, TP_WAIT_RESULT) {
    auto& self = *static_cast<GuardSupervisor*>(context);
    std::lock_guard lock(self.mutex_);
    if (self.stopping_) return;
    self.Report(false);
    if (::GetTickCount64() - self.ready_since_ >= kStableMs) self.attempts_ = 0;
    if (self.attempts_ < std::size(kRetryDelays)) self.ArmRetry(kRetryDelays[self.attempts_]);
}

void CALLBACK GuardSupervisor::Retry(PTP_CALLBACK_INSTANCE, void* context, PTP_TIMER) {
    auto& self = *static_cast<GuardSupervisor*>(context);
    std::lock_guard lock(self.mutex_);
    if (self.stopping_ || self.attempts_ >= std::size(kRetryDelays)) return;
    ++self.attempts_;
    if (!self.StartGuard() && self.attempts_ < std::size(kRetryDelays))
        self.ArmRetry(kRetryDelays[self.attempts_]);
}

void GuardSupervisor::StopMonitoring() {
    BeginShutdown();
    {
        std::lock_guard lock(mutex_);
        if (wait_) ::SetThreadpoolWait(wait_, nullptr, nullptr);
        if (timer_) ::SetThreadpoolTimer(timer_, nullptr, 0, 0);
    }
    if (wait_) ::WaitForThreadpoolWaitCallbacks(wait_, TRUE);
    if (timer_) ::WaitForThreadpoolTimerCallbacks(timer_, TRUE);
}

bool GuardSupervisor::Attach(HANDLE process, Reservation& reservation) {
    return ::DuplicateHandle(::GetCurrentProcess(), reservation.local, process,
                             &reservation.remote, 0, FALSE, DUPLICATE_SAME_ACCESS) != FALSE;
}

DWORD GuardSupervisor::Protect(const std::wstring& app_id, std::span<const HANDLE> handles) {
    std::lock_guard lock(mutex_);
    if (stopping_ || !ready_ || ::WaitForSingleObject(process_.handle(), 0) != WAIT_TIMEOUT)
        return ERROR_NOT_READY;
    auto [entry, inserted] = reservations_.try_emplace(app_id);
    if (!inserted) return ERROR_ALREADY_EXISTS;
    auto& reservations = entry->second;
    reservations.reserve(handles.size());
    for (auto handle : handles) {
        if (!handle) continue;
        Reservation reservation{};
        if (::DuplicateHandle(::GetCurrentProcess(), handle, ::GetCurrentProcess(),
                              &reservation.local, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
            reservations.push_back(reservation);
            if (Attach(process_.handle(), reservations.back())) continue;
        }
        const DWORD error = ::GetLastError();
        Release(reservations);
        reservations_.erase(entry);
        return error;
    }
    return ERROR_SUCCESS;
}

void GuardSupervisor::Release(std::vector<Reservation>& reservations) {
    for (const auto& reservation : reservations) {
        if (reservation.remote && process_.handle())
            ::DuplicateHandle(process_.handle(), reservation.remote, nullptr, nullptr,
                              0, FALSE, DUPLICATE_CLOSE_SOURCE);
        ::CloseHandle(reservation.local);
    }
}

void GuardSupervisor::Release(const std::wstring& app_id) {
    std::lock_guard lock(mutex_);
    const auto entry = reservations_.find(app_id);
    if (entry == reservations_.end()) return;
    Release(entry->second);
    reservations_.erase(entry);
}

} // namespace pm::guard
