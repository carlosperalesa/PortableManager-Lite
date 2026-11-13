#include "pmd/pmd_notes.h"
#include "applications/app_paths.h"
#include "core/atomic_file.h"
#include "core/config_manager.h"

#include <algorithm>
#include <cwchar>
#include <filesystem>

namespace pm::pmd {
namespace fs = std::filesystem;

void NotesController::Load(const core::PathResolver& resolver) {
    notes_.clear();
    const auto dir_res = resolver.ResolvePmRelativePath(L"Data\\notes");
    if (!dir_res.ok) return;
    std::error_code ec;
    fs::create_directories(dir_res.resolved_path, ec);
    for (fs::directory_iterator it(dir_res.resolved_path, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        if (applications::Equal(it->path().extension().native(), L".txt")) {
            notes_.push_back({it->path().stem().native(), it->path().native()});
        }
    }
    std::sort(notes_.begin(), notes_.end(), [](const NoteEntry& a, const NoteEntry& b) {
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    if (notes_.empty()) {
        const auto p = (fs::path(dir_res.resolved_path) / L"Nota 1.txt").native();
        core::AtomicFile::WriteUtf8Text(p, "");
        notes_.push_back({L"Nota 1", p});
    }
    active_note_ = std::clamp(active_note_, 0, static_cast<int>(notes_.size()) - 1);
}

void NotesController::Save(int index, std::wstring_view raw_crlf_text) const {
    if (index < 0 || static_cast<std::size_t>(index) >= notes_.size()) return;
    std::wstring norm;
    norm.reserve(raw_crlf_text.size());
    for (std::size_t i = 0; i < raw_crlf_text.size(); ++i) {
        if (raw_crlf_text[i] == L'\r' && i + 1 < raw_crlf_text.size() && raw_crlf_text[i + 1] == L'\n') continue;
        norm.push_back(raw_crlf_text[i]);
    }
    core::AtomicFile::WriteUtf8Text(notes_[static_cast<std::size_t>(index)].path,
                                    core::IniDocument::WideToUtf8(norm));
}

std::wstring NotesController::ReadCrlf(int index) const {
    if (index < 0 || static_cast<std::size_t>(index) >= notes_.size()) return {};
    const auto read_res = core::AtomicFile::ReadUtf8Text(notes_[static_cast<std::size_t>(index)].path);
    const auto wide = read_res.ok ? core::IniDocument::Utf8ToWide(read_res.content) : L"";
    std::wstring crlf;
    crlf.reserve(wide.size() + 16);
    for (std::size_t i = 0; i < wide.size(); ++i) {
        if (wide[i] == L'\n' && (i == 0 || wide[i - 1] != L'\r')) crlf.push_back(L'\r');
        crlf.push_back(wide[i]);
    }
    return crlf;
}

int NotesController::Create(const core::PathResolver& resolver) {
    const auto dir_res = resolver.ResolvePmRelativePath(L"Data\\notes");
    if (!dir_res.ok) return active_note_;
    std::error_code ec;
    fs::create_directories(dir_res.resolved_path, ec);
    int n = static_cast<int>(notes_.size()) + 1;
    fs::path p;
    std::wstring title;
    do {
        title = L"Nota " + std::to_wstring(n++);
        p = fs::path(dir_res.resolved_path) / (title + L".txt");
    } while (fs::exists(p, ec));
    core::AtomicFile::WriteUtf8Text(p.native(), "");
    notes_.push_back({title, p.native()});
    active_note_ = static_cast<int>(notes_.size()) - 1;
    return active_note_;
}

bool NotesController::Delete(const core::PathResolver& resolver, int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= notes_.size()) return false;
    std::error_code ec;
    fs::remove(notes_[static_cast<std::size_t>(index)].path, ec);
    notes_.erase(notes_.begin() + index);
    if (notes_.empty()) {
        const auto dir_res = resolver.ResolvePmRelativePath(L"Data\\notes");
        const auto p = (fs::path(dir_res.resolved_path) / L"Nota 1.txt").native();
        core::AtomicFile::WriteUtf8Text(p, "");
        notes_.push_back({L"Nota 1", p});
        active_note_ = 0;
    } else if (active_note_ >= static_cast<int>(notes_.size())) {
        active_note_ = static_cast<int>(notes_.size()) - 1;
    }
    return true;
}

bool NotesController::Rename(int index, std::wstring_view new_name) {
    if (index < 0 || static_cast<std::size_t>(index) >= notes_.size() || new_name.empty()) return false;
    auto& entry = notes_[static_cast<std::size_t>(index)];
    const fs::path old_path(entry.path);
    const fs::path new_path = old_path.parent_path() / (std::wstring(new_name) + L".txt");
    std::error_code ec;
    if (!fs::exists(new_path, ec)) {
        fs::rename(old_path, new_path, ec);
        if (!ec) {
            entry.name.assign(new_name);
            entry.path = new_path.native();
            return true;
        }
    }
    return false;
}

bool NotesController::HasContent(int index) const {
    if (index < 0 || static_cast<std::size_t>(index) >= notes_.size()) return false;
    const auto r = core::AtomicFile::ReadUtf8Text(notes_[static_cast<std::size_t>(index)].path);
    return r.ok && !r.content.empty();
}

} // namespace pm::pmd
