#include "tracks.hpp"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <map>

#include "engine.hpp"
#include "game.hpp"
#include "cosmetics.hpp"
#include "log.hpp"

namespace tracks {
namespace {

using eng::Obj;

std::string gSearchState = "idle", gOpenState = "idle";
std::vector<steam::Item> gResults;
int gTotal = 0, gSearchSerial = 0;
uint64_t gOpening = 0;
double gOpeningSince = 0;
constexpr double kDownloadTimeout = 120;

Obj GameInstance() {
    Obj controller = game::PlayerController();
    return controller ? eng::Call(eng::FindCdo("GameplayStatics"), "GetGameInstance", controller).ReturnObj() : nullptr;
}

std::string FString(const uint8_t* at) { return eng::ReadFString(at); }

std::string MakeFString(const std::string& text, std::wstring* keep, eng::FString* out) {
    *keep = eng::Widen(text);
    *out = eng::FString{keep->c_str(), static_cast<int32_t>(keep->size() + 1), static_cast<int32_t>(keep->size() + 1)};
    return text;
}

// The level data the game keeps for one of its tracks (its master level library).
Obj LibraryLevelData(const std::string& key) {
    Obj instance = GameInstance();
    Obj library = instance ? eng::ReadObj(instance, "LevelLibrary") : nullptr;
    Obj fn = library ? eng::FindFunction(eng::ClassOf(library), "Try Get Level Data By String") : nullptr;
    if (!fn) return nullptr;
    eng::Params p(fn);
    std::wstring keep;
    eng::FString name{};
    MakeFString(key, &keep, &name);
    p.Set("LevelName", name);
    if (!eng::Invoke(library, p)) return nullptr;
    return p.GetObj("LevelData");
}

// The level data the game keeps for one of its tracks. Mostly its master level library, keyed by level name; the
// tower trials aren't (read from the game's files): The Tower's level data (DA_Map_Climb01, LevelName Map_TheTower)
// is filed under the key "Map_Climb01", and The Tower CPs' (DA_Map_TheTowerCP) isn't in the library at all but in
// its map's folder, as /Game/Maps/COOKEDMAPS/<level>/DA_<level>.
Obj LevelData(const std::string& level) {
    if (Obj data = LibraryLevelData(level)) return data;
    if (level == "Map_TheTower")
        if (Obj data = LibraryLevelData("Map_Climb01")) return data;
    const std::wstring path = L"/Game/Maps/COOKEDMAPS/" + eng::Widen(level) + L"/DA_" + eng::Widen(level) + L".DA_" + eng::Widen(level);
    return cosmetics::LoadAsset(path);
}

std::string PlaylistLevels(Obj list, std::vector<std::string>* out) {
    uint8_t array[16] = {};
    if (!list || !eng::ReadBytes(list, "LevelNumberLevelNameMap", array, sizeof array)) return "";
    struct {
        const eng::FString* data;
        int32_t num, max;
    } a{};
    std::memcpy(&a, array, sizeof a);
    for (int32_t i = 0; i < a.num && a.data; ++i) out->push_back(eng::ReadFString(reinterpret_cast<const uint8_t*>(&a.data[i])));
    return "";
}

std::vector<std::string> StringArray(const uint8_t* at) {
    struct {
        const eng::FString* data;
        int32_t num, max;
    } array{};
    std::vector<std::string> out;
    if (!at) return out;
    std::memcpy(&array, at, sizeof array);
    for (int32_t i = 0; i < array.num && array.data; ++i) out.push_back(eng::ReadFString(reinterpret_cast<const uint8_t*>(&array.data[i])));
    return out;
}

// As the game's own track preview opens a workshop track (read from WBP_TrackPreview and O_LevelManager): the level
// manager makes level data of the installed item (ProcessDownloadedLevel: finds its .ballmap and registers it), and
// the game instance enters it (TryEnterLevelByString(level data's UGC map path, level data, "Map_LevelEditorMain"),
// which reads the map file, claims it with the track hub and begins the level transition).
bool Open(uint64_t id) {
    Obj instance = GameInstance();
    if (!instance) {
        gOpenState = "error: no game instance";
        return false;
    }
    Obj manager = nullptr;
    if (Obj get = eng::FindFunction(eng::ClassOf(instance), "GetLevelManager")) {
        eng::Params p(get);
        if (eng::Invoke(instance, p)) manager = p.GetObj("LevelManager");
    }
    Obj process = manager ? eng::FindFunction(eng::ClassOf(manager), "ProcessDownloadedLevel") : nullptr;
    if (!process) {
        gOpenState = "error: the game's level manager isn't there";
        return false;
    }
    eng::Params made(process);
    const int64_t fileId = static_cast<int64_t>(id);
    made.Set("PublishedFileId", &fileId, sizeof fileId);            // SIK_PublishedFileId: the id, 8 bytes
    Obj data = eng::Invoke(manager, made) ? made.GetObj("OutLevelData") : nullptr;
    const uint8_t* ok = made.Get("bSucess");
    if (!data || !ok || !*ok) {
        gOpenState = "error: the game couldn't read the track's files";
        hostlog::Warn("tracks: ProcessDownloadedLevel failed for workshop " + std::to_string(id));
        return false;
    }
    uint8_t mapPath[16] = {};
    eng::ReadBytes(data, "? UGCMap Path", mapPath, sizeof mapPath);
    const std::string path = FString(mapPath);
    Obj enter = eng::FindFunction(eng::ClassOf(instance), "TryEnterLevelByString");
    if (!enter) {
        gOpenState = "error: the game can't enter levels here";
        return false;
    }
    eng::Params p(enter);
    std::wstring keepPath, keepOverride;
    eng::FString name{}, override{};
    MakeFString(path, &keepPath, &name);
    MakeFString("Map_LevelEditorMain", &keepOverride, &override);
    p.Set("LevelPathOrName", name);
    p.Set("LevelData", data);
    p.Set("OverrideLevelName", override);
    if (!eng::Invoke(instance, p)) {
        gOpenState = "error: the game didn't open the track";
        return false;
    }
    const uint8_t* entered = p.Get("bSuccessful");
    const uint8_t* message = p.Get("Message");
    const std::string said = message ? FString(message) : "";
    hostlog::Info("tracks: opening workshop " + std::to_string(id) + " (" + path + "): " + (entered && *entered ? "ok" : "refused") +
                  (said.empty() ? "" : " - " + said));
    // "Waiting for Hub Claim Check" (measured): the game checks the track with its hub first and enters it itself
    // when the check comes back, so this is on its way rather than refused.
    const bool claiming = said.find("Hub Claim") != std::string::npos;
    if ((!entered || !*entered) && !claiming) {
        gOpenState = "error: " + (said.empty() ? std::string("the game refused to open it") : said);
        return false;
    }
    gOpenState = "open";
    return true;
}

}  // namespace

void Frame() {
    if (!gOpening) return;
    const uint32_t state = steam::ItemState(gOpening);
    if ((state & steam::kItemInstalled) && !(state & (steam::kItemDownloading | steam::kItemDownloadPending))) {
        const uint64_t id = gOpening;
        gOpening = 0;
        gOpenState = "opening";
        Open(id);
    } else if (game::Seconds() - gOpeningSince > kDownloadTimeout) {
        gOpening = 0;
        gOpenState = "error: the download didn't finish";
    }
}

std::vector<OfficialTrack> OfficialTracks() {
    std::vector<OfficialTrack> all;
    Obj instance = GameInstance();
    Obj seasons = instance ? eng::ReadObj(instance, "CircuitSeasons") : nullptr;
    int number = 0;
    for (Obj list : seasons ? eng::ReadObjArray(seasons, "NumberedSeasons") : std::vector<Obj>{}) {
        std::vector<std::string> levels;
        PlaylistLevels(list, &levels);
        ++number;
        for (const auto& level : levels) all.push_back({level, "season " + std::to_string(number)});
    }
    const std::pair<const wchar_t*, const char*> others[] = {
        {L"/Game/Maps/Playlists/DA_CollabLevels_ShippingList.DA_CollabLevels_ShippingList", "collabs"},
        {L"/Game/Maps/Playlists/DA_TowerLevels_ShippingList.DA_TowerLevels_ShippingList", "trials"}};
    for (const auto& [path, group] : others) {
        std::vector<std::string> levels;
        PlaylistLevels(cosmetics::LoadAsset(path), &levels);
        for (const auto& level : levels) all.push_back({level, group});
    }
    return all;
}

std::vector<std::string> Official() {
    Obj instance = GameInstance();
    Obj fn = instance ? eng::FindFunction(eng::ClassOf(instance), "GetCircuitMapNames") : nullptr;
    if (!fn) return {};
    eng::Params p(fn);
    if (!eng::Invoke(instance, p)) return {};
    return StringArray(p.Get("LevelNumberLevelNameMap"));
}

std::string Title(const std::string& level) {
    Obj data = LevelData(level);
    uint8_t name[16] = {};
    return data && eng::ReadBytes(data, "?OptionalDescriptiveName", name, sizeof name) ? FString(name) : "";
}

std::string Image(const std::string& level) {
    Obj data = LevelData(level);
    // LevelPreviewImage: a soft reference (40 bytes in the type dump): a weak pointer (8 bytes), then the
    // FSoftObjectPath {FTopLevelAssetPath {PackageName, AssetName}, SubPathString}.
    uint8_t soft[40] = {};
    if (!data || !eng::ReadBytes(data, "LevelPreviewImage", soft, sizeof soft)) return "";
    uint32_t package[2], asset[2];
    std::memcpy(package, soft + 8, sizeof package);
    std::memcpy(asset, soft + 16, sizeof asset);
    const std::string p = eng::Name(package[0], static_cast<int32_t>(package[1])), a = eng::Name(asset[0], static_cast<int32_t>(asset[1]));
    return p.empty() || a.empty() || p == "None" ? "" : p + "." + a;
}

bool Open(const std::string& level) {
    Obj instance = GameInstance();
    Obj data = LevelData(level);
    Obj fn = instance ? eng::FindFunction(eng::ClassOf(instance), "TryEnterLevelByString") : nullptr;
    if (!fn || !data) {
        hostlog::Warn("tracks: can't open " + level + (data ? "" : ": the game has no level data for it"));
        return false;
    }
    eng::Params p(fn);
    std::wstring keepLevel, keepOverride;
    eng::FString name{}, override{};
    MakeFString(level, &keepLevel, &name);
    MakeFString("Map_LevelEditorMain", &keepOverride, &override);
    p.Set("LevelPathOrName", name);
    p.Set("LevelData", data);
    p.Set("OverrideLevelName", override);
    if (!eng::Invoke(instance, p)) return false;
    uint8_t ok = 0;
    if (const uint8_t* r = p.Get("bSuccessful")) ok = *r;
    hostlog::Info("tracks: opening " + level + (ok ? "" : " (the game refused)"));
    return ok != 0;
}

bool OpenOfficial(int index) {
    Obj instance = GameInstance();
    Obj fn = instance ? eng::FindFunction(eng::ClassOf(instance), "TryEnterLevelByIndex") : nullptr;
    if (!fn) return false;
    eng::Params p(fn);
    p.Set("Index", static_cast<int32_t>(index));
    return eng::Invoke(instance, p);
}

void Search(const std::string& text) {
    const int serial = ++gSearchSerial;
    gSearchState = "searching";
    gResults.clear();
    gTotal = 0;
    steam::SearchWorkshop(text, 1, [serial](bool ok, std::vector<steam::Item> items, int total) {
        if (serial != gSearchSerial) return;
        gSearchState = ok ? "done" : "error: the workshop search failed";
        gResults = std::move(items);
        gTotal = total;
    });
}

std::string SearchState() { return gSearchState; }

namespace {
std::map<uint64_t, bool> gPreviewAsked;
std::wstring PreviewFile(uint64_t id) { return hostlog::DataDir() + L"\\cache\\previews\\" + std::to_wstring(id) + L".jpg"; }
bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }
}  // namespace

std::string ResultImage(size_t index) {
    if (index >= gResults.size()) return "";
    const steam::Item& item = gResults[index];
    // The game's own cache of workshop previews: <id>_<hash>_<size>.img, JPEG (measured).
    const std::wstring saved = hostlog::DataDir() + L"\\..\\HubPreviewCache\\";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((saved + std::to_wstring(item.id) + L"_*.img").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        FindClose(h);
        const std::wstring full = saved + fd.cFileName;
        return eng::Narrow(full.c_str(), static_cast<int>(full.size()));
    }
    const std::wstring mine = PreviewFile(item.id);
    if (Exists(mine)) return eng::Narrow(mine.c_str(), static_cast<int>(mine.size()));
    if (!item.preview || gPreviewAsked[item.id]) return "";
    gPreviewAsked[item.id] = true;
    const uint64_t id = item.id;
    steam::DownloadFile(item.preview, [id](bool ok, std::string bytes) {
        if (!ok || bytes.empty()) return;
        const std::wstring dir = hostlog::DataDir() + L"\\cache";
        CreateDirectoryW(dir.c_str(), nullptr);
        CreateDirectoryW((dir + L"\\previews").c_str(), nullptr);
        if (FILE* f = _wfopen(PreviewFile(id).c_str(), L"wb")) {
            fwrite(bytes.data(), 1, bytes.size(), f);
            fclose(f);
        }
    });
    return "";
}
const std::vector<steam::Item>& Results() { return gResults; }
int Total() { return gTotal; }

bool OpenWorkshop(uint64_t id) {
    if (!steam::Available() || !id) {
        gOpenState = "error: Steam isn't available";
        return false;
    }
    const uint32_t state = steam::ItemState(id);
    if ((state & steam::kItemInstalled) && !(state & steam::kItemNeedsUpdate)) {
        gOpenState = "opening";
        return Open(id);
    }
    if (!steam::DownloadItem(id)) {
        gOpenState = "error: Steam wouldn't download it";
        return false;
    }
    gOpening = id;
    gOpeningSince = game::Seconds();
    gOpenState = "downloading";
    hostlog::Info("tracks: downloading workshop " + std::to_string(id));
    return true;
}

std::string OpenState() { return gOpenState; }

}  // namespace tracks
