// Compiles plugins against the host's real API without the game: the same AngelScript engine setup and the same
// registrations (api::Register), then each plugin's files as the host builds them, with its dependencies first.
// Nothing runs, so no game is needed; it catches everything the compiler would report in the host log.
//   ascheck <plugin folder>...            (a dependency is looked for next to the plugin's folder, as "<id>" or
//                                          "ballest-<id>")
// Exit code 0 when every plugin compiles without errors.
#include <windows.h>

#include <angelscript.h>
#include <scriptbuilder/scriptbuilder.h>

#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "../../src/host/api.hpp"

static int gErrors = 0, gWarnings = 0;

static void Message(const asSMessageInfo* m, void*) {
    const char* kind = m->type == asMSGTYPE_ERROR ? "error" : m->type == asMSGTYPE_WARNING ? "warning" : "info";
    if (m->type == asMSGTYPE_ERROR) ++gErrors;
    if (m->type == asMSGTYPE_WARNING) ++gWarnings;
    std::printf("%s (%d, %d): %s: %s\n", m->section, m->row, m->col, kind, m->message);
}

static std::string Read(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// The strings of a TOML array value: key = ["a", "b"].
static std::vector<std::string> List(const std::string& toml, const std::string& key) {
    std::vector<std::string> out;
    const size_t at = toml.find("\n" + key);
    if (at == std::string::npos) return out;
    const size_t open = toml.find('[', at), close = toml.find(']', open);
    if (open == std::string::npos || close == std::string::npos) return out;
    const std::string body = toml.substr(open + 1, close - open - 1);
    for (size_t q = body.find('"'); q != std::string::npos; q = body.find('"', q + 1)) {
        const size_t end = body.find('"', q + 1);
        if (end == std::string::npos) break;
        out.push_back(body.substr(q + 1, end - q - 1));
        q = end;
    }
    return out;
}

static std::string Folder(const std::string& path) {
    const size_t cut = path.find_last_of("/\\");
    return cut == std::string::npos ? "." : path.substr(0, cut);
}
static std::string Name(const std::string& path) {
    std::string p = path;
    while (!p.empty() && (p.back() == '/' || p.back() == '\\')) p.pop_back();
    const size_t cut = p.find_last_of("/\\");
    return cut == std::string::npos ? p : p.substr(cut + 1);
}

static std::map<std::string, bool> gBuilt;

static bool Build(asIScriptEngine* e, const std::string& dir, const std::string& id) {
    if (gBuilt.count(id)) return gBuilt[id];
    gBuilt[id] = false;
    const std::string toml = "\n" + Read(dir + "/info.toml");
    if (toml.size() < 2) return std::printf("%s: no info.toml in %s\n", id.c_str(), dir.c_str()), false;
    for (const auto& dep : List(toml, "dependencies")) {
        bool found = false;
        for (const std::string& candidate : {Folder(dir) + "/" + dep, Folder(dir) + "/ballest-" + dep})
            if (!Read(candidate + "/info.toml").empty()) {
                found = Build(e, candidate, dep);
                break;
            }
        if (!found) return std::printf("%s: dependency %s not found next to it\n", id.c_str(), dep.c_str()), false;
    }
    CScriptBuilder builder;
    if (builder.StartNewModule(e, id.c_str()) < 0) return false;
    for (const auto& file : List(toml, "files")) {
        const std::string code = Read(dir + "/" + file);
        if (code.empty()) return std::printf("%s: missing %s\n", id.c_str(), file.c_str()), false;
        builder.AddSectionFromMemory((id + "/" + file).c_str(), code.data(), static_cast<unsigned>(code.size()));
    }
    const int before = gErrors;
    const bool ok = builder.BuildModule() >= 0 && gErrors == before;
    asIScriptModule* m = builder.GetModule();
    if (ok && m->GetImportedFunctionCount() > 0 && m->BindAllImportedFunctions() < 0) {
        std::printf("%s: an imported function was not found in its dependencies\n", id.c_str());
        return false;
    }
    std::printf("%s: %s\n", id.c_str(), ok ? "compiles" : "does not compile");
    return gBuilt[id] = ok;
}

int main(int argc, char** argv) {
    if (argc < 2) return std::printf("usage: ascheck <plugin folder>...\n"), 2;
    asIScriptEngine* e = asCreateScriptEngine();
    e->SetMessageCallback(asFUNCTION(Message), nullptr, asCALL_CDECL);
    api::Register(e);
    if (gErrors) return std::printf("the host's API did not register (%d errors)\n", gErrors), 1;
    bool all = true;
    for (int i = 1; i < argc; ++i) {
        // The plugin's id is its folder's name without a "ballest-" prefix.
        std::string id = Name(argv[i]);
        if (id.rfind("ballest-", 0) == 0) id = id.substr(8);
        all &= Build(e, argv[i], id);
    }
    std::printf("%d error(s), %d warning(s)\n", gErrors, gWarnings);
    return all && gErrors == 0 ? 0 : 1;
}
