#include "workshop.hpp"

#include <algorithm>
#include <cstring>
#include <deque>
#include <map>

#include "engine.hpp"
#include "game.hpp"
#include "layout.hpp"
#include "log.hpp"
#include "tracks.hpp"

namespace workshop {
namespace {

using eng::Obj;

struct QueryState {
    std::string state = "searching";
    std::vector<uint64_t> ids;
    int total = 0;
};
std::map<int, QueryState> gQueries;
int gNextQuery = 1;
constexpr size_t kMaxQueries = 256;     // the oldest are forgotten past this
std::map<uint64_t, steam::Details> gItems;

// Names: asked for at most kNamesPerSecond at a time, and again after kNameRetry if Steam hasn't answered.
struct NameState {
    std::string name;
    double askedAt = -1e9;
};
std::map<uint64_t, NameState> gNames;
constexpr int kNamesPerSecond = 20;
constexpr double kNameRetry = 30;
int gNamesAsked = 0;
double gNameSecond = 0;

int Start(const steam::Query& q) {
    if (gQueries.size() >= kMaxQueries) gQueries.erase(gQueries.begin());
    const int number = gNextQuery++;
    gQueries[number] = QueryState{};
    steam::QueryWorkshop(q, [number](bool ok, std::vector<steam::Details> items, int total) {
        auto it = gQueries.find(number);
        if (it == gQueries.end()) return;           // forgotten meanwhile
        it->second.state = ok ? "done" : "error: Steam didn't answer the search";
        it->second.total = total;
        for (auto& item : items) {
            it->second.ids.push_back(item.id);
            gItems[item.id] = std::move(item);
        }
    });
    return number;
}

const QueryState* QueryOf(int query) {
    auto it = gQueries.find(query);
    return it == gQueries.end() ? nullptr : &it->second;
}

// --- the save ----------------------------------------------------------------------------------------------------

Obj GameInstance() {
    Obj controller = game::PlayerController();
    return controller ? eng::Call(eng::FindCdo("GameplayStatics"), "GetGameInstance", controller).ReturnObj() : nullptr;
}

struct Progress {
    double best = 0;
    int medal = 0;
};
std::map<uint64_t, Progress> gProgress;
double gProgressReadAt = -1e9;
constexpr double kProgressEvery = 2;    // seconds between reads of the save (it changes when a map is finished)

// The struct member whose name starts with `prefix` (the game's structs carry a GUID after each name).
int MemberOffset(Obj structure, const std::string& prefix) {
    for (const auto& name : eng::PropertyNames(structure))
        if (name.rfind(prefix, 0) == 0) return eng::FindProp(structure, name).offset;
    return -1;
}

void ReadProgress() {
    if (game::Seconds() - gProgressReadAt < kProgressEvery) return;
    gProgressReadAt = game::Seconds();
    Obj instance = GameInstance();
    Obj profile = instance ? eng::Call(instance, "GetCleanBallerProfile").GetObj("CleanProfile") : nullptr;
    if (!profile) return;
    std::map<uint64_t, Progress> read;
    // PublishedFileIDHighscoreMap: Map<Int64, SLevelHighScore>; the struct's BestTime is a double (type dump).
    const eng::Prop scores = eng::FindProp(eng::ClassOf(profile), "PublishedFileIDHighscoreMap");
    int bestAt = -1;
    if (scores) {
        const uint8_t* valueProp = nullptr;
        std::memcpy(&valueProp, scores.field + layout::kFMapPropertyValueOffset, sizeof valueProp);
        if (Obj value = valueProp ? eng::StructOf(eng::Prop{const_cast<uint8_t*>(valueProp), 0, 0}) : nullptr)
            bestAt = MemberOffset(value, "BestTime");
    }
    if (bestAt >= 0)
        eng::ForEachMapEntry(profile, "PublishedFileIDHighscoreMap", 8, 8, [&](const uint8_t* key, const uint8_t* value) {
            int64_t id = 0;
            double best = 0;
            std::memcpy(&id, key, sizeof id);
            std::memcpy(&best, value + bestAt, sizeof best);
            if (id > 0) read[static_cast<uint64_t>(id)].best = best;
        });
    // PublishedFileIDMedalMap: Map<Int64, Byte<Enum_MedalGranted>>.
    eng::ForEachMapEntry(profile, "PublishedFileIDMedalMap", 8, 1, [&](const uint8_t* key, const uint8_t* value) {
        int64_t id = 0;
        std::memcpy(&id, key, sizeof id);
        if (id > 0) read[static_cast<uint64_t>(id)].medal = *value;
    });
    gProgress = std::move(read);
}

// --- leaderboard places --------------------------------------------------------------------------------------------

struct Rank {
    int rank = -1, players = 0;
    bool started = false;
};
std::map<uint64_t, Rank> gRanks;
std::deque<uint64_t> gRankQueue;
int gRanksInFlight = 0;
constexpr int kRanksAtOnce = 2;

// The map's leaderboard name, as the game's hub asks for it (WBP_HubListEntry: the static
// BallestUGCSubsystem.ResolveUGCLeaderboardNameFromMetadataJson(metadata, id, title, "Climb")).
std::string LeaderboardName(const steam::Details& item) {
    Obj cdo = eng::FindCdo("BallestUGCSubsystem");
    Obj fn = cdo ? eng::FindFunction(eng::ClassOf(cdo), "ResolveUGCLeaderboardNameFromMetadataJson") : nullptr;
    if (!fn) return "";
    eng::Params p(fn);
    const std::wstring metadata = eng::Widen(item.metadata), title = eng::Widen(item.title), mode = L"Climb";
    auto fstring = [](const std::wstring& s) {
        return eng::FString{s.c_str(), static_cast<int32_t>(s.size() + 1), static_cast<int32_t>(s.size() + 1)};
    };
    const int64_t id = static_cast<int64_t>(item.id);
    p.Set("MetadataJson", fstring(metadata));
    p.Set("ExpectedPublishedFileId", id);
    p.Set("StableLevelDisplayName", fstring(title));
    p.Set("GameMode", fstring(mode));
    if (!eng::Invoke(cdo, p) || !p.ReturnBool()) return "";
    const uint8_t* name = p.Get("LeaderboardNameOut");
    return name ? eng::ReadFString(name) : "";
}

void Finish(uint64_t id, int rank, int players) {
    gRanks[id].rank = rank;
    gRanks[id].players = players;
    --gRanksInFlight;
}

void LookUpRanks() {
    while (gRanksInFlight < kRanksAtOnce && !gRankQueue.empty()) {
        const uint64_t id = gRankQueue.front();
        gRankQueue.pop_front();
        const steam::Details* item = Item(id);
        const std::string name = item ? LeaderboardName(*item) : "";
        if (name.empty()) {
            gRanks[id].rank = 0;            // no leaderboard the game knows of
            continue;
        }
        ++gRanksInFlight;
        steam::FindLeaderboard(name, [id](uint64_t board) {
            if (!board) return Finish(id, 0, 0);
            steam::DownloadOwnEntry(board, [id, board](bool ok, std::vector<steam::Entry> entries) {
                Finish(id, ok && !entries.empty() ? entries[0].rank : 0, steam::EntryCount(board));
            });
        });
    }
}

}  // namespace

void Frame() {
    if (game::Seconds() - gNameSecond >= 1) {
        gNameSecond = game::Seconds();
        gNamesAsked = 0;
    }
    LookUpRanks();
}

int Find(const std::string& text, const std::string& sort, int page, int days, const std::vector<std::string>& with,
         const std::vector<std::string>& without, bool anyTag, std::string* error) {
    static const std::map<std::string, int> ranks = {
        {"relevance", steam::kRankTextSearch}, {"top", steam::kRankVote},           {"trending", steam::kRankTrend},
        {"new", steam::kRankPublished},        {"updated", steam::kRankUpdated},    {"played", steam::kRankPlaySessions},
        {"subscribed", steam::kRankSubscriptions}, {"liked", steam::kRankVotesUp}, {"unrated", steam::kRankNotYetRated}};
    auto it = ranks.find(sort);
    if (it == ranks.end()) {
        *error = "unknown sort \"" + sort + "\"";
        return -1;
    }
    if (!steam::Available()) {
        *error = "Steam isn't available";
        return -1;
    }
    steam::Query q;
    q.kind = steam::Query::kAll;
    q.rank = it->second;
    q.text = text;
    q.page = page;
    q.trendDays = days;
    q.with = with;
    q.without = without;
    q.anyTag = anyTag;
    return Start(q);
}

int FindList(const std::string& list, uint64_t user, const std::string& listSort, int page, std::string* error) {
    static const std::map<std::string, int> lists = {
        {"published", steam::kListPublished}, {"played", steam::kListPlayed},   {"favorited", steam::kListFavorited},
        {"subscribed", steam::kListSubscribed}, {"liked", steam::kListVotedUp}, {"disliked", steam::kListVotedDown}};
    static const std::map<std::string, int> sorts = {
        {"new", steam::kSortNewest},     {"old", steam::kSortOldest}, {"title", steam::kSortTitle},
        {"updated", steam::kSortUpdated}, {"top", steam::kSortTop},   {"subscribed", steam::kSortSubscribed}};
    auto l = lists.find(list);
    auto s = sorts.find(listSort);
    if (l == lists.end() || s == sorts.end()) {
        *error = l == lists.end() ? "unknown list \"" + list + "\"" : "unknown list sort \"" + listSort + "\"";
        return -1;
    }
    if (!user) user = Me();
    if (!steam::Available() || !user) {
        *error = "Steam isn't available";
        return -1;
    }
    steam::Query q;
    q.kind = steam::Query::kUser;
    q.account = static_cast<uint32_t>(user & 0xffffffffull);       // AccountID_t: the low 32 bits of a Steam id
    q.list = l->second;
    q.userSort = s->second;
    q.page = page;
    return Start(q);
}

int FindIds(const std::vector<uint64_t>& ids, std::string* error) {
    if (!steam::Available()) {
        *error = "Steam isn't available";
        return -1;
    }
    steam::Query q;
    q.kind = steam::Query::kIds;
    q.ids = ids;
    return Start(q);
}

std::string State(int query) {
    const QueryState* q = QueryOf(query);
    return q ? q->state : "";
}
int Count(int query) {
    const QueryState* q = QueryOf(query);
    return q ? static_cast<int>(q->ids.size()) : 0;
}
int Total(int query) {
    const QueryState* q = QueryOf(query);
    return q ? q->total : 0;
}
uint64_t IdAt(int query, int index) {
    const QueryState* q = QueryOf(query);
    return q && index >= 0 && static_cast<size_t>(index) < q->ids.size() ? q->ids[static_cast<size_t>(index)] : 0;
}
void Forget(int query) { gQueries.erase(query); }

const steam::Details* Item(uint64_t id) {
    auto it = gItems.find(id);
    return it == gItems.end() ? nullptr : &it->second;
}

std::string Image(uint64_t id) {
    const steam::Details* item = Item(id);
    return tracks::PreviewImage(id, item ? item->preview : 0);
}

std::string Name(uint64_t steamId) {
    if (!steamId) return "";
    NameState& n = gNames[steamId];
    if (!n.name.empty()) return n.name;
    n.name = steam::PersonaName(steamId);
    if (n.name.empty() && game::Seconds() - n.askedAt > kNameRetry && gNamesAsked < kNamesPerSecond) {
        n.askedAt = game::Seconds();
        ++gNamesAsked;
        // False when Steam already has everything about the user: then the name is read on the next ask.
        steam::RequestPersona(steamId);
    }
    return n.name;
}

uint64_t Me() { return steam::OwnSteamId(); }

double MyBest(uint64_t id) {
    ReadProgress();
    auto it = gProgress.find(id);
    return it == gProgress.end() ? 0 : it->second.best;
}

int MyMedal(uint64_t id) {
    ReadProgress();
    auto it = gProgress.find(id);
    return it == gProgress.end() ? -1 : it->second.medal;
}

std::vector<uint64_t> Finished() {
    ReadProgress();
    std::vector<uint64_t> ids;
    for (const auto& [id, progress] : gProgress) ids.push_back(id);
    return ids;
}

int MyRank(uint64_t id) {
    Rank& r = gRanks[id];
    if (!r.started && Item(id)) {
        r.started = true;
        gRankQueue.push_back(id);
    }
    return r.rank;
}

int Players(uint64_t id) {
    auto it = gRanks.find(id);
    return it == gRanks.end() ? 0 : it->second.players;
}

}  // namespace workshop
