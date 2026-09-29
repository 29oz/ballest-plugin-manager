#include "ghostdata.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "json.hpp"

namespace ghostdata {
namespace {

double Number(const json::Value* v, double fallback = 0) { return v && v->type == json::Value::Number ? v->number : fallback; }

Point PointOf(const json::Value& v) { return {Number(v.Get("x")), Number(v.Get("y")), Number(v.Get("z"))}; }

double Distance(const Point& a, const Point& b) {
    const double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

}  // namespace

bool Parse(const std::string& text, Replay* out, std::string* error) {
    *out = Replay{};
    // A cache file starts with a binary header (which can hold a brace byte); the JSON is everything from its
    // opening, which is always the levelName key (measured on the game's files).
    size_t start = text.find("{\"levelName\"");
    if (start == std::string::npos) start = text.find('{');
    if (start == std::string::npos) {
        *error = "no JSON in it";
        return false;
    }
    json::Value root;
    if (!json::Parse(text.substr(start), root, *error)) return false;
    if (root.type != json::Value::Object) {
        *error = "not a JSON object";
        return false;
    }
    out->level = root.Str("levelName");
    out->name = root.Str("username");
    out->timestamp = root.Str("timestamp");
    out->time = Number(root.Get("bestTime"));
    if (const json::Value* id = root.Get("steamId")) {
        if (const json::Value* r = id->Get("result"); r && r->type == json::Value::Number) {
            // Steam ids are 64-bit; the JSON number is exact up to 2^53 only, so the text is rebuilt from the digits
            // the reader kept (ids fit: 7656119... is about 2^56, so the last digits can be off). Kept for display only.
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.0f", r->number);
            out->steamId = buf;
        }
    }
    const json::Value* locations = root.Get("locations");
    const json::Value* times = root.Get("elapsedTime");
    if (!locations || locations->type != json::Value::Array || !times || times->type != json::Value::Array) {
        *error = "no locations or elapsedTime";
        return false;
    }
    const size_t n = std::min(locations->items.size(), times->items.size());
    out->samples.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        Sample sample;
        sample.t = Number(&times->items[i]);
        sample.at = PointOf(locations->items[i]);
        out->samples.push_back(sample);
    }
    // controlRotations: {pitch, yaw, roll} per sample, pitch as 0..360 (328.85 is looking 31.15 degrees down).
    if (const json::Value* view = root.Get("controlRotations"); view && view->type == json::Value::Array && view->items.size() >= n) {
        out->hasView = n > 0;
        for (size_t i = 0; i < n; ++i) {
            double pitch = Number(view->items[i].Get("pitch"));
            if (pitch > 180) pitch -= 360;
            out->samples[i].pitch = pitch;
            out->samples[i].yaw = Number(view->items[i].Get("yaw"));
        }
    }
    if (const json::Value* rotation = root.Get("rotation"); rotation && rotation->type == json::Value::Array && rotation->items.size() >= n)
        for (size_t i = 0; i < n; ++i) {
            out->samples[i].ballPitch = Number(rotation->items[i].Get("pitch"));
            out->samples[i].ballYaw = Number(rotation->items[i].Get("yaw"));
            out->samples[i].ballRoll = Number(rotation->items[i].Get("roll"));
        }
    if (const json::Value* velocity = root.Get("velocities"); velocity && velocity->type == json::Value::Array && velocity->items.size() >= n)
        for (size_t i = 0; i < n; ++i) out->samples[i].velocity = PointOf(velocity->items[i]);
    out->look.skinMaterial = root.Str("skinMaterial");
    out->look.ghostSkinMaterial = root.Str("ghostSkinMaterial");
    out->look.specialSkinClass = root.Str("?SpecialSkinClass");
    out->look.accessory = root.Str("accessory");
    out->look.accessoryGhostMaterial = root.Str("accessoryGhostMaterial");
    if (const json::Value* prefs = root.Get("ballerSkinPrefs"); prefs && prefs->type == json::Value::Object) {
        const json::Value* texture = prefs->Get("bBasicBallTexture");
        const json::Value* gloss = prefs->Get("bBasicBallGloss");
        out->look.basicTexture = texture && texture->type == json::Value::Bool && texture->boolean;
        out->look.basicGloss = gloss && gloss->type == json::Value::Bool && gloss->boolean;
        out->look.textureSlider = Number(prefs->Get("basicBallTextureSliderValue"));
    }
    if (const json::Value* splits = root.Get("?CheckpointSplits"); splits && splits->type == json::Value::Array)
        for (const auto& s : splits->items) out->splits.push_back(Number(&s));
    return !out->samples.empty() || (*error = "no samples", false);
}

std::string ObjectPath(const std::string& written) {
    const size_t a = written.find('\''), b = written.rfind('\'');
    const std::string path = a != std::string::npos && b > a ? written.substr(a + 1, b - a - 1) : written;
    return path == "None" ? "" : path;
}

const Sample* SampleAt(const Replay& replay, double t) {
    const auto& s = replay.samples;
    if (s.empty()) return nullptr;
    if (t <= s.front().t) return &s.front();
    auto next = std::upper_bound(s.begin(), s.end(), t, [](double v, const Sample& a) { return v < a.t; });
    return &*(next - 1);
}

Point At(const Replay& replay, double t) {
    const auto& s = replay.samples;
    if (s.empty()) return {};
    if (t <= s.front().t) return s.front().at;
    if (t >= s.back().t) return s.back().at;
    const auto next = std::lower_bound(s.begin(), s.end(), t, [](const Sample& a, double v) { return a.t < v; });
    const auto prev = next - 1;
    const double span = next->t - prev->t, f = span > 0 ? (t - prev->t) / span : 0;
    return {prev->at.x + (next->at.x - prev->at.x) * f, prev->at.y + (next->at.y - prev->at.y) * f,
            prev->at.z + (next->at.z - prev->at.z) * f};
}

bool ViewAt(const Replay& replay, double t, double* pitch, double* yaw) {
    const auto& s = replay.samples;
    if (!replay.hasView || s.empty()) return false;
    if (t <= s.front().t || t >= s.back().t) {
        const Sample& end = t <= s.front().t ? s.front() : s.back();
        *pitch = end.pitch;
        *yaw = end.yaw;
        return true;
    }
    const auto next = std::lower_bound(s.begin(), s.end(), t, [](const Sample& a, double v) { return a.t < v; });
    const auto prev = next - 1;
    const double span = next->t - prev->t, f = span > 0 ? (t - prev->t) / span : 0;
    double turn = std::fmod(next->yaw - prev->yaw, 360.0);
    if (turn > 180) turn -= 360;
    if (turn < -180) turn += 360;
    *pitch = prev->pitch + (next->pitch - prev->pitch) * f;
    *yaw = prev->yaw + turn * f;
    return true;
}

std::vector<int> CheckpointOrder(const Replay& replay, const std::vector<Point>& checkpoints) {
    std::vector<int> order;
    std::vector<bool> used(checkpoints.size(), false);
    for (double split : replay.splits) {
        const Point at = At(replay, split);
        int best = -1;
        double bestDistance = 0;
        for (size_t c = 0; c < checkpoints.size(); ++c) {
            if (used[c]) continue;
            const double d = Distance(at, checkpoints[c]);
            if (best < 0 || d < bestDistance) {
                best = static_cast<int>(c);
                bestDistance = d;
            }
        }
        if (best >= 0) used[static_cast<size_t>(best)] = true;
        order.push_back(best);
    }
    return order;
}

}  // namespace ghostdata
