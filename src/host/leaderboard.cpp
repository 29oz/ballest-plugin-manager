#include "leaderboard.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

#include "engine.hpp"
#include "game.hpp"
#include "log.hpp"
#include "widgets.hpp"

namespace leaderboard {
namespace {

using eng::Obj;
namespace w = ui::widgets;

std::string gNote;
int gNoteOwner = -1;

struct Titled {
    eng::Weak title;                            // the TXT_HEader text block
    std::string original, shown;
};
std::vector<Titled> gTitles;

std::string gOverallNote;
int gOverallNoteOwner = -1;
std::vector<Titled> gOverallLabels;

// The in-map leaderboard: a WBP_Leaderboard_C inside the race UI (menus have others, for the track picker).
Obj InMapBoard() {
    static double lastLook = -100;
    static eng::Weak cached;
    if (Obj board = eng::Get(cached)) return board;
    if (game::Seconds() - lastLook < 1) return nullptr;
    lastLook = game::Seconds();
    Obj cls = eng::FindClass("WBP_Leaderboard_C"), race = eng::FindClass("WBP_RaceUIManager_C");
    if (!cls || !race) return nullptr;
    Obj found = nullptr;
    eng::ForEachObject([&](Obj o) {
        if (eng::ClassOf(o) != cls || eng::IsDefaultObject(o)) return true;
        for (Obj outer = eng::OuterOf(o); outer; outer = eng::OuterOf(outer))
            if (eng::ClassOf(outer) == race && !eng::IsDefaultObject(outer)) {
                found = o;
                return false;
            }
        return true;
    });
    cached = eng::MakeWeak(found);
    return found;
}

// The main menu's overall leaderboard bar, if one is on screen.
Obj HeaderBoard() {
    static double lastLook = -100;
    static eng::Weak cached;
    if (Obj board = eng::Get(cached)) return board;
    if (game::Seconds() - lastLook < 1) return nullptr;
    lastLook = game::Seconds();
    Obj cls = eng::FindClass("WBP_HeaderSeasonScore_C");
    if (!cls) return nullptr;
    Obj found = nullptr;
    // the live bar (under /Engine/Transient), not the template in WBP_Header's class (measured: it's found first)
    eng::ForEachObject([&](Obj o) {
        if (eng::ClassOf(o) == cls && !eng::IsDefaultObject(o) && eng::PathOf(o).rfind("/Engine/Transient", 0) == 0) found = o;
        return found == nullptr;
    });
    cached = eng::MakeWeak(found);
    return found;
}

// The bar's "overall" label: the text block named "TextBlock" in its widget tree (found once per bar).
Obj OverallLabel(Obj board) {
    static eng::Weak cachedBoard, cachedLabel;
    if (eng::Get(cachedBoard) == board)
        if (Obj label = eng::Get(cachedLabel)) return label;
    Obj cls = eng::FindClass("TextBlock");
    Obj found = nullptr;
    eng::ForEachObject([&](Obj o) {
        if (eng::ClassOf(o) != cls || eng::ObjName(o) != "TextBlock") return true;
        for (Obj outer = eng::OuterOf(o); outer; outer = eng::OuterOf(outer))
            if (outer == board) {
                found = o;
                return false;
            }
        return true;
    });
    cachedBoard = eng::MakeWeak(board);
    cachedLabel = eng::MakeWeak(found);
    return found;
}

int CountEntries(uint64_t handle);

// An int kept in a Blueprint's event graph: its variables aren't properties of the object but of the graph's function
// (ExecuteUbergraph_<class>), in a frame the object points to through its UberGraphFrame property.
bool GraphInt(Obj o, const char* function, const char* name, int32_t* out) {
    const eng::Prop frameProp = eng::FindProp(eng::ClassOf(o), "UberGraphFrame");
    Obj fn = eng::FindFunction(eng::ClassOf(o), function);
    const eng::Prop var = fn ? eng::FindProp(fn, name) : eng::Prop{};
    if (!frameProp || !var || var.size != 4) return false;
    uint8_t* frame = nullptr;
    std::memcpy(&frame, o + frameProp.offset, sizeof frame);
    if (!frame) return false;
    std::memcpy(out, frame + var.offset, sizeof *out);
    return true;
}

}  // namespace

int Players() {
    Obj board = InMapBoard();
    if (!board || !eng::Call(board, "IsVisible").ReturnBool()) return -1;
    eng::Prop handleProp;
    const int offset = eng::NestedOffset(eng::ClassOf(board), {"NativeActiveLeaderboardRecord", "LeaderboardHandle"}, &handleProp);
    if (offset < 0 || handleProp.size != 8) return -1;
    uint64_t handle = 0;
    std::memcpy(&handle, board + offset, sizeof handle);
    return CountEntries(handle);
}

int OverallPlayers() {
    Obj board = HeaderBoard();
    if (!board || !eng::Call(board, "IsVisible").ReturnBool()) return -1;
    int32_t id = 0;
    if (!GraphInt(board, "ExecuteUbergraph_WBP_HeaderSeasonScore", "Temp_int_Variable", &id) || id <= 0) return -1;
    return CountEntries(static_cast<uint64_t>(id));
}

void SetOverallNote(int owner, const std::string& note) {
    gOverallNote = note;
    gOverallNoteOwner = note.empty() ? -1 : owner;
}

namespace {
// Steam's entry count for a leaderboard, or -1.
int CountEntries(uint64_t handle) {
    if (!handle) return -1;
    Obj steam = eng::FindCdo("SIK_UserStatsLibrary");
    if (!steam) return -1;
    // Measured: the function takes the handle as a 4-byte LeaderboardID (the game's handles fit: 0x013BAB71).
    eng::Params p(eng::FunctionOn(steam, "GetLeaderboardEntryCount"));
    const int32_t idSize = p.SizeOf("LeaderboardID");
    if ((idSize != 4 && idSize != 8) || (idSize == 4 && handle > 0xFFFFFFFFull) || !p.SetArg(0, &handle, static_cast<size_t>(idSize))) return -1;
    eng::Invoke(steam, p);
    size_t size = 0;
    const uint8_t* r = p.Return(&size);
    if (!r || size < 4) return -1;
    int32_t count = 0;
    std::memcpy(&count, r, 4);
    return count > 0 ? count : -1;
}
}  // namespace

void SetTitleNote(int owner, const std::string& note) {
    gNote = note;
    gNoteOwner = note.empty() ? -1 : owner;
}

void RemoveOwner(int owner) {
    if (owner == gNoteOwner) SetTitleNote(owner, "");
    if (owner == gOverallNoteOwner) SetOverallNote(owner, "");
}

void Frame() {
    static double lastLook = -100;
    if (game::Seconds() - lastLook < 0.25) return;
    lastLook = game::Seconds();
    Obj board = gNote.empty() && gTitles.empty() ? nullptr : InMapBoard();
    Obj title = board ? eng::ReadObj(board, "TXT_HEader") : nullptr;
    // A title seen for the first time is remembered as the game wrote it, to put the note after (and back without).
    if (title && std::none_of(gTitles.begin(), gTitles.end(), [&](const Titled& t) { return eng::Get(t.title) == title; }))
        gTitles.push_back({eng::MakeWeak(title), w::ReadText(title), ""});
    for (auto it = gTitles.begin(); it != gTitles.end();) {
        Obj text = eng::Get(it->title);
        if (!text) {
            it = gTitles.erase(it);
            continue;
        }
        const std::string wanted = gNote.empty() ? it->original : it->original + "  " + gNote;
        if (wanted != it->shown) {
            w::SetText(text, wanted);
            it->shown = wanted;
        }
        ++it;
    }
    // The overall label: the note goes before "overall".
    Obj header = gOverallNote.empty() && gOverallLabels.empty() ? nullptr : HeaderBoard();
    Obj label = header ? OverallLabel(header) : nullptr;
    if (label && std::none_of(gOverallLabels.begin(), gOverallLabels.end(), [&](const Titled& t) { return eng::Get(t.title) == label; }))
        gOverallLabels.push_back({eng::MakeWeak(label), w::ReadText(label), ""});
    for (auto it = gOverallLabels.begin(); it != gOverallLabels.end();) {
        Obj text = eng::Get(it->title);
        if (!text) {
            it = gOverallLabels.erase(it);
            continue;
        }
        const std::string wanted = gOverallNote.empty() ? it->original : gOverallNote + "  " + it->original;
        if (wanted != it->shown) {
            w::SetText(text, wanted);
            it->shown = wanted;
        }
        ++it;
    }
}

}  // namespace leaderboard
