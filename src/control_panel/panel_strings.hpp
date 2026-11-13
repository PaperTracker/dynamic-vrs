#pragma once
#include <initializer_list>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>

namespace dvr::panel {
// UI text from control_panel_strings.tsv, which is compiled into the executable as a resource.
class Strings {
public:
    using Value = std::variant<double, std::wstring>;
    using Argument = std::pair<std::wstring_view, Value>;

    // Picks the column for language, falling back to en_US.
    void load(std::wstring_view language);
    // Missing keys show as the key itself, so a gap is visible rather than blank.
    const std::wstring& get(std::wstring_view key) const;
    // Fills {name} and {name:.Nf} placeholders.
    std::wstring format(std::wstring_view key, std::initializer_list<Argument> arguments) const;

private:
    std::unordered_map<std::wstring, std::wstring> text_;
    mutable std::wstring missing_;
};
} // namespace dvr::panel
