#pragma once

#include "core/path_resolver.h"

#include <string>
#include <string_view>
#include <vector>

namespace pm::pmd {

struct NoteEntry {
    std::wstring name;
    std::wstring path;
};

class NotesController {
public:
    void Load(const core::PathResolver& resolver);
    void Save(int index, std::wstring_view raw_crlf_text) const;
    std::wstring ReadCrlf(int index) const;
    int Create(const core::PathResolver& resolver);
    bool Delete(const core::PathResolver& resolver, int index);
    bool Rename(int index, std::wstring_view new_name);
    bool HasContent(int index) const;

    const std::vector<NoteEntry>& notes() const noexcept { return notes_; }
    int active() const noexcept { return active_note_; }
    void set_active(int index) noexcept { active_note_ = index; }

private:
    std::vector<NoteEntry> notes_;
    int active_note_ = 0;
};

} // namespace pm::pmd
