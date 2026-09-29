// Offline test of ghostdata (replay parsing and checkpoint order) against replays the game saved, and against the
// checkpoint orders Will worked out for the same players (his Leth Trial viewer's data, ghosts.js).
//   ghostdata_test <folder of .replay files> <ghosts.js>
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "../../src/host/ghostdata.hpp"
#include "../../src/host/json.hpp"

static std::string Read(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int main(int argc, char** argv) {
    if (argc < 3) return std::printf("usage: ghostdata_test <replay folder> <ghosts.js>\n"), 2;
    // Will's data: window.LETH_DATA={...}
    std::string js = Read(argv[2]);
    json::Value will;
    std::string error;
    if (!json::Parse(js.substr(js.find('{'), js.rfind('}') + 1 - js.find('{')), will, error)) return std::printf("ghosts.js: %s\n", error.c_str()), 1;
    std::vector<ghostdata::Point> checkpoints;
    std::vector<std::string> checkpointIds;
    for (const auto& [id, p] : will.Get("checkpoints")->members) {
        checkpoints.push_back({p.items[0].number, p.items[1].number, p.items[2].number});
        checkpointIds.push_back(id);
    }
    std::map<std::string, std::pair<double, std::string>> willOrder;   // name -> time, "1,2,3..."
    for (const auto& p : will.Get("players")->items) {
        std::string o;
        for (const auto& c : p.Get("order")->items) o += (o.empty() ? "" : ",") + std::to_string(static_cast<int>(c.number));
        willOrder[p.Str("name")] = {p.Get("time")->number, o};
    }
    int parsed = 0, failed = 0, compared = 0, same = 0;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((std::string(argv[1]) + "/*.replay").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return std::printf("no .replay files\n"), 1;
    do {
        ghostdata::Replay r;
        if (!ghostdata::Parse(Read(std::string(argv[1]) + "/" + fd.cFileName), &r, &error)) {
            ++failed;
            std::printf("FAIL %s: %s\n", fd.cFileName, error.c_str());
            continue;
        }
        ++parsed;
        std::string mine;
        for (int c : ghostdata::CheckpointOrder(r, checkpoints)) mine += (mine.empty() ? "" : ",") + (c < 0 ? std::string("?") : checkpointIds[static_cast<size_t>(c)]);
        // Sanity: samples ordered in time, first sample near the start, last near the end time.
        bool ordered = true;
        for (size_t i = 1; i < r.samples.size(); ++i) ordered &= r.samples[i].t >= r.samples[i - 1].t;
        const auto it = willOrder.find(r.name);
        // Only the same run: Will's snapshot is older, and a player's newer best can take another route.
        if (it != willOrder.end() && std::fabs(it->second.first - r.time) < 0.002) {
            ++compared;
            same += it->second.second == mine;
            if (it->second.second != mine)
                std::printf("DIFF %-20s %.3f mine %s  will %s\n", r.name.c_str(), r.time, mine.c_str(), it->second.second.c_str());
        } else if (it != willOrder.end()) {
            std::printf("newer run: %-20s %.3f (Will's %.3f)\n", r.name.c_str(), r.time, it->second.first);
        }
        if (!ordered) std::printf("UNORDERED %s\n", r.name.c_str());
        if (parsed <= 3)
            std::printf("  %s %.3f s, %zu samples (%.2f..%.2f s), %zu splits, order %s\n", r.name.c_str(), r.time, r.samples.size(),
                        r.samples.front().t, r.samples.back().t, r.splits.size(), mine.c_str());
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::printf("parsed %d, failed %d; compared with Will's orders: %d, the same: %d\n", parsed, failed, compared, same);
    return failed == 0 && same == compared ? 0 : 1;
}
