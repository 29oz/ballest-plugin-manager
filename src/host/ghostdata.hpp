// Leaderboard ghost replays as data: reading the game's replay JSON and working out which way a run went. No engine
// calls, so it is tested on its own (tools/ghostdata_test.cpp) against replays saved by the game.
//
// A replay is the JSON the game attaches to a leaderboard entry (and keeps in Saved\Ghosts and
// Saved\GhostSwarmCache), read from the game's files: levelName, bestTime, username, steamId {result}, timestamp,
// locations [{x,y,z}], elapsedTime [], rotation, velocities, controlRotations (one of each per sample, about ten a
// second) and "?CheckpointSplits" (the time each checkpoint was taken, in the order they were taken, but not which
// checkpoint each was).
#pragma once
#include <string>
#include <vector>

namespace ghostdata {

struct Point {
    double x = 0, y = 0, z = 0;
};

struct Sample {
    double t = 0;
    Point at;
    double pitch = 0, yaw = 0;          // the player's control rotation (their camera's direction), degrees
    double ballPitch = 0, ballYaw = 0, ballRoll = 0;    // the ball's own rotation ("rotation"), degrees
    Point velocity;                     // "velocities", cm/s
};

// What the player's ball looked like (the replay's skin fields; object paths as the game writes them, e.g.
// "/Script/Engine.MaterialInstanceConstant'/Game/Art/.../MI_LBall05.MI_LBall05'", or "None").
struct Look {
    std::string skinMaterial, ghostSkinMaterial, specialSkinClass, accessory, accessoryGhostMaterial;
    bool basicTexture = false, basicGloss = false;
    double textureSlider = 0;
};

struct Replay {
    std::string level, name, steamId, timestamp;
    double time = 0;                    // bestTime, seconds
    std::vector<double> splits;         // seconds from the start, in the order the checkpoints were taken
    std::vector<Sample> samples;        // by time
    bool hasView = false;               // controlRotations were recorded
    Look look;
};

// An object path out of the game's "/Script/Engine.Class'/Game/Path.Object'" form ("" for "None" or nothing).
std::string ObjectPath(const std::string& written);

// The sample at or just before t (the ball's rotation and velocity there).
const Sample* SampleAt(const Replay& replay, double t);

// The replay in `text`: the JSON itself, or a file of the game's ghost cache (a short binary header, then the JSON).
bool Parse(const std::string& text, Replay* out, std::string* error);

// Where the ball was at time t (between samples: along the straight line between them; before the first or after
// the last: that sample).
Point At(const Replay& replay, double t);

// Where the player's camera pointed at time t (their control rotation, pitch -90..90 and yaw), between samples
// turning the short way round. False if the replay has none.
bool ViewAt(const Replay& replay, double t, double* pitch, double* yaw);

// The checkpoints in the order the run took them, as indexes into `checkpoints` (their positions): for each split,
// the checkpoint nearest to where the ball was at that moment, each checkpoint used once. -1 for a split with no
// checkpoint left. Empty when the replay has no splits.
std::vector<int> CheckpointOrder(const Replay& replay, const std::vector<Point>& checkpoints);

}  // namespace ghostdata
