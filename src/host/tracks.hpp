// Tracks to open from outside the game's menus: the game's own (its circuit, in order) and workshop tracks found by a
// search, opened the way the game opens them (read from BP_BallgameGameInstance):
//   * the game's own: TryEnterLevelByIndex(index in GetCircuitMapNames), which ends in BeginLevelTransition(level, 0)
//   * a workshop track: downloaded by Steam if it isn't installed (DownloadItem, as the game's browser does), made
//     into level data by the level manager (O_LevelManager.AddRuntimeLevelFromSteam), then read with
//     SKGMLEStatics.LoadMapFromFile(directory, map path) and BallestUGCPlaybackLibrary.ResolveUGCPlaybackBaselineFromJson,
//     and opened with BeginLevelTransition(level, 1, map JSON, title) (TryEnterLevelByIndex's workshop branch)
// Game thread only.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "steam.hpp"

namespace tracks {

void Frame();

// The game's own tracks, grouped as its playlists group them (read from the game's files): each circuit season
// (the game instance's CircuitSeasons.NumberedSeasons), then DA_CollabLevels_ShippingList ("collabs") and
// DA_TowerLevels_ShippingList ("trials": The Tower and The Tower CPs).
struct OfficialTrack {
    std::string level, group;           // group: "season 1", "season 2", ..., "collabs", "trials"
};
std::vector<OfficialTrack> OfficialTracks();
std::vector<std::string> Official();                    // level names, in the circuit's order (TryEnterLevelByIndex)
std::string Title(const std::string& level);            // its descriptive name ("01" for the circuit's), or ""
std::string Image(const std::string& level);            // its preview picture (a game texture path), or ""
bool OpenOfficial(int index);
bool Open(const std::string& level);                    // as the game's own play button does

// A workshop search: results arrive later (SearchState "searching" until then).
void Search(const std::string& text);
std::string SearchState();                              // "idle", "searching", "done", "error: ..."
const std::vector<steam::Item>& Results();
int Total();
// A result's preview picture on this computer: the game's own cache of them (Saved\HubPreviewCache\<id>_*.img,
// JPEG), or one downloaded here (Saved\PluginManager\cache\previews\<id>.jpg); "" until there is one (asking for it
// starts the download).
std::string ResultImage(size_t index);
// The same for any workshop item, by id and its preview's file handle (0 if unknown: then only cached pictures).
std::string PreviewImage(uint64_t id, uint64_t preview);

// Opens a workshop track by id, downloading it first if needed; OpenState tells how it is going.
bool OpenWorkshop(uint64_t id);
std::string OpenState();                                // "idle", "downloading", "opening", "open", "error: ..."

}  // namespace tracks
