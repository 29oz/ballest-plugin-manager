// The workshop as the game's track hub sees it, and more: searches with any of Steam's rankings, text, tags and time
// windows; a player's lists (published, played, favourited ...); everything Steam says about each map; who made it;
// and this player's own progress on it (best time and medal from the game's save, place on its leaderboard).
//
// Queries run through steam.hpp (the game's own steam_api64.dll) and their results are kept by id, so a plugin can ask
// about any map it has seen in any query. Game thread only.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "steam.hpp"

namespace workshop {

void Frame();

// A new query; its number, or -1 with `error` set (an unknown sort or list, or Steam isn't there). Results arrive
// later: State is "searching" until then, then "done" or "error: ...".
//   sort: relevance, top, trending, new, updated, played, subscribed, liked, unrated
//   list: published, played, favorited, subscribed, liked, disliked; listSort: new, old, title, updated, top, subscribed
int Find(const std::string& text, const std::string& sort, int page, int days, const std::vector<std::string>& with,
         const std::vector<std::string>& without, bool anyTag, std::string* error);
int FindList(const std::string& list, uint64_t user, const std::string& listSort, int page, std::string* error);
int FindIds(const std::vector<uint64_t>& ids, std::string* error);
std::string State(int query);
int Count(int query);
int Total(int query);
uint64_t IdAt(int query, int index);
void Forget(int query);

// A map seen in any query (null if none has returned it yet).
const steam::Details* Item(uint64_t id);
// Its preview picture on this computer: the game's hub cache or one downloaded here; "" until there is one.
std::string Image(uint64_t id);

std::string Name(uint64_t steamId);     // "" while Steam is still being asked
uint64_t Me();

// This player's progress, from the game's save (BP_BallerProfile_Save: PublishedFileIDHighscoreMap and
// PublishedFileIDMedalMap, written when a workshop map is finished).
double MyBest(uint64_t id);             // seconds; 0 if never finished
int MyMedal(uint64_t id);               // -1 never finished, else Enum_MedalGranted: 0 none, 1 bronze ... 4 author
std::vector<uint64_t> Finished();
// Place on the map's leaderboard: -1 while being looked up (asking starts it), 0 no time on it, else the rank; and
// how many players have a time on it (0 until known).
int MyRank(uint64_t id);
int Players(uint64_t id);

}  // namespace workshop
