#include "settings.hpp"

#include <windows.h>

#include <angelscript.h>
#include <scriptbuilder/scriptbuilder.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <set>

#include "log.hpp"
#include "storage.hpp"

namespace settings {
namespace {

std::vector<Setting> gSettings;
std::set<int> gChanged;                 // plugins whose OnSettingsChanged is due
std::set<size_t> gUnsaved;              // settings changed since the last save
ULONGLONG gLastSave = 0;
constexpr ULONGLONG kSaveEveryMs = 500;     // a dragged slider changes a value every frame

// `Setting name="Timer size" min=16 max=160 hidden` -> {"Setting": "", "name": "Timer size", "min": "16", ...}
std::vector<std::pair<std::string, std::string>> Attributes(const std::string& text) {
    std::vector<std::pair<std::string, std::string>> out;
    size_t i = 0;
    auto skipSpace = [&] {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    };
    while (skipSpace(), i < text.size()) {
        size_t start = i;
        while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i])) && text[i] != '=') ++i;
        std::string key = text.substr(start, i - start), value;
        skipSpace();
        if (i < text.size() && text[i] == '=') {
            ++i;
            skipSpace();
            if (i < text.size() && text[i] == '"') {
                const size_t end = text.find('"', i + 1);
                value = text.substr(i + 1, (end == std::string::npos ? text.size() : end) - i - 1);
                i = end == std::string::npos ? text.size() : end + 1;
            } else {
                start = i;
                while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i]))) ++i;
                value = text.substr(start, i - start);
            }
        }
        if (!key.empty()) out.emplace_back(key, value);
    }
    return out;
}

std::string Number(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.6g", v);
    return buf;
}

std::string Read(const Setting& s) {
    switch (s.kind) {
        case Kind::Bool: return *static_cast<bool*>(s.address) ? "true" : "false";
        case Kind::Int: return std::to_string(*static_cast<int32_t*>(s.address));
        case Kind::UInt: return std::to_string(*static_cast<uint32_t*>(s.address));
        case Kind::Float: return Number(*static_cast<float*>(s.address));
        case Kind::Double: return Number(*static_cast<double*>(s.address));
        case Kind::String: return *static_cast<std::string*>(s.address);
    }
    return "";
}

// Parses and writes; numbers are clamped to the setting's range. False if the text is not a value of the type.
bool Write(const Setting& s, const std::string& text) {
    if (s.kind == Kind::String) {
        if (!s.choices.empty() && std::find(s.choices.begin(), s.choices.end(), text) == s.choices.end()) return false;
        *static_cast<std::string*>(s.address) = text;
        return true;
    }
    if (s.kind == Kind::Bool) {
        if (text != "true" && text != "false") return false;
        *static_cast<bool*>(s.address) = text == "true";
        return true;
    }
    char* end = nullptr;
    double v = std::strtod(text.c_str(), &end);
    if (end == text.c_str()) return false;
    if (s.hasRange) v = std::clamp(v, s.min, s.max);
    switch (s.kind) {
        case Kind::Int: *static_cast<int32_t*>(s.address) = static_cast<int32_t>(v < 0 ? v - 0.5 : v + 0.5); break;
        case Kind::UInt: *static_cast<uint32_t*>(s.address) = static_cast<uint32_t>(std::max(0.0, v) + 0.5); break;
        case Kind::Float: *static_cast<float*>(s.address) = static_cast<float>(v); break;
        case Kind::Double: *static_cast<double*>(s.address) = v; break;
        default: break;
    }
    return true;
}

std::string Key(const Setting& s) { return "setting." + s.variable; }

bool KindOf(asIScriptEngine* engine, int typeId, Kind& kind) {
    switch (typeId) {
        case asTYPEID_BOOL: kind = Kind::Bool; return true;
        case asTYPEID_INT32: kind = Kind::Int; return true;
        case asTYPEID_UINT32: kind = Kind::UInt; return true;
        case asTYPEID_FLOAT: kind = Kind::Float; return true;
        case asTYPEID_DOUBLE: kind = Kind::Double; return true;
        default: break;
    }
    if (typeId == engine->GetTypeIdByDecl("string")) {
        kind = Kind::String;
        return true;
    }
    return false;
}

void SaveUnsaved() {
    for (size_t i : gUnsaved) {
        if (i >= gSettings.size()) continue;
        const Setting& s = gSettings[i];
        // A value at its default is not saved, so a later change of the default reaches this player too.
        if (Read(s) == s.defaultValue) storage::Erase(s.pluginId, Key(s));
        else storage::Set(s.pluginId, Key(s), Read(s));
    }
    gUnsaved.clear();
    gLastSave = GetTickCount64();
}

}  // namespace

// Where a setting is declared in the plugin's script files: {file, offset}, for showing settings in that order (the
// module lists its globals in its own order: measured, a float before a string declared above it).
std::pair<size_t, size_t> DeclaredAt(const std::vector<std::string>& sources, const std::string& variable) {
    for (size_t file = 0; file < sources.size(); ++file) {
        const std::string& text = sources[file];
        // the variable's name followed by = or ; (its declaration), as a whole word
        for (size_t at = text.find(variable); at != std::string::npos; at = text.find(variable, at + 1)) {
            const bool startsWord = at == 0 || !(std::isalnum(static_cast<unsigned char>(text[at - 1])) || text[at - 1] == '_');
            size_t after = at + variable.size();
            while (after < text.size() && (text[after] == ' ' || text[after] == '\t')) ++after;
            if (startsWord && after < text.size() && (text[after] == '=' || text[after] == ';')) return {file, at};
        }
    }
    return {sources.size(), 0};
}

void Collect(int plugin, const std::string& pluginId, asIScriptModule* module, CScriptBuilder& builder,
             const std::vector<std::string>& sources) {
    const size_t first = gSettings.size();
    for (asUINT i = 0; i < module->GetGlobalVarCount(); ++i) {
        for (const std::string& metadata : builder.GetMetadataForVar(static_cast<int>(i))) {
            const auto attributes = Attributes(metadata);
            if (attributes.empty() || attributes[0].first != "Setting") continue;
            const char* variable = nullptr;
            int typeId = 0;
            bool isConst = false;
            module->GetGlobalVar(i, &variable, nullptr, &typeId, &isConst);
            Setting s;
            if (isConst || !KindOf(module->GetEngine(), typeId, s.kind)) {
                hostlog::Write("warn", pluginId, std::string("setting ") + variable + ": only bool, int, uint, float, double and string");
                continue;
            }
            s.plugin = plugin;
            s.pluginId = pluginId;
            s.variable = variable;
            s.name = variable;
            s.address = module->GetAddressOfGlobalVar(i);
            bool hasMin = false, hasMax = false;
            for (const auto& [key, value] : attributes) {
                if (key == "name") s.name = value;
                else if (key == "description") s.description = value;
                else if (key == "hidden") s.hidden = true;
                else if (key == "min") hasMin = true, s.min = std::atof(value.c_str());
                else if (key == "max") hasMax = true, s.max = std::atof(value.c_str());
                else if (key == "choices") {
                    size_t from = 0;
                    while (from <= value.size()) {
                        size_t bar = value.find('|', from);
                        if (bar == std::string::npos) bar = value.size();
                        std::string choice = value.substr(from, bar - from);
                        while (!choice.empty() && std::isspace(static_cast<unsigned char>(choice.back()))) choice.pop_back();
                        while (!choice.empty() && std::isspace(static_cast<unsigned char>(choice.front()))) choice.erase(choice.begin());
                        if (!choice.empty()) s.choices.push_back(choice);
                        from = bar + 1;
                    }
                }
            }
            if (!s.choices.empty() && s.kind != Kind::String) {
                hostlog::Write("warn", pluginId, std::string("setting ") + variable + ": choices are for a string setting");
                s.choices.clear();
            }
            s.hasRange = hasMin && hasMax && s.max > s.min && s.kind != Kind::Bool && s.kind != Kind::String;
            s.defaultValue = Read(s);           // globals are initialised when the module is built
            const std::string saved = storage::Get(pluginId, Key(s), "");
            if (storage::Has(pluginId, Key(s)) && !Write(s, saved))
                hostlog::Write("warn", pluginId, "setting " + s.variable + ": saved value '" + saved + "' not usable, default kept");
            gSettings.push_back(s);
        }
    }
    std::stable_sort(gSettings.begin() + static_cast<std::ptrdiff_t>(first), gSettings.end(), [&](const Setting& a, const Setting& b) {
        return DeclaredAt(sources, a.variable) < DeclaredAt(sources, b.variable);
    });
}

void Forget(int plugin) {
    SaveUnsaved();                      // before the indices move
    gSettings.erase(std::remove_if(gSettings.begin(), gSettings.end(), [&](const Setting& s) { return s.plugin == plugin; }),
                    gSettings.end());
    gChanged.erase(plugin);
}

void Frame() {
    if (!gUnsaved.empty() && GetTickCount64() - gLastSave >= kSaveEveryMs) SaveUnsaved();
}

const std::vector<Setting>& List() { return gSettings; }

std::string Get(size_t index) { return index < gSettings.size() ? Read(gSettings[index]) : ""; }

bool Set(size_t index, const std::string& value) {
    if (index >= gSettings.size()) return false;
    const Setting& s = gSettings[index];
    const std::string before = Read(s);
    if (!Write(s, value)) return false;
    if (Read(s) != before) {
        gChanged.insert(s.plugin);
        gUnsaved.insert(index);
    }
    return true;
}

void Reset(size_t index) {
    if (index < gSettings.size()) Set(index, gSettings[index].defaultValue);
}

bool IsDefault(size_t index) { return index < gSettings.size() && Read(gSettings[index]) == gSettings[index].defaultValue; }

bool TakeChanged(int plugin) { return gChanged.erase(plugin) > 0; }

}  // namespace settings
