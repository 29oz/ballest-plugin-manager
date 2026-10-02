#include "postprocess.hpp"

#include <array>
#include <cstring>
#include <map>
#include <vector>

#include "engine.hpp"
#include "game.hpp"
#include "layout.hpp"
#include "log.hpp"

namespace postprocess {
namespace {

using eng::Obj;

struct Value {
    std::string name;
    std::array<double, 4> v;
};

struct Filter {
    eng::Weak volume;
    std::vector<Value> values;          // in the order set (a later one of the same name replaces it)
    double weight = 1;
};

std::map<int, Filter> gFilters;         // by plugin
int gGeneration = -1;

constexpr float kPriority = 10000;      // above any volume a map has

struct Transform {                      // FTransform: rotation quaternion, translation, scale (doubles, 16-aligned)
    double q[4];
    double t[4];
    double s[4];
};

Obj SpawnVolume() {
    Obj cls = eng::FindClass("PostProcessVolume");
    Obj controller = game::PlayerController();
    Obj statics = eng::FindCdo("GameplayStatics");
    if (!cls || !controller || !statics) return nullptr;
    Transform t{{0, 0, 0, 1}, {0, 0, 0, 0}, {1, 1, 1, 0}};
    eng::Params begin(eng::FunctionOn(statics, "BeginDeferredActorSpawnFromClass"));
    begin.Set("WorldContextObject", controller);
    begin.Set("ActorClass", cls);
    begin.Set("SpawnTransform", t);
    begin.Set("CollisionHandlingOverride", uint8_t{1});     // always spawn
    begin.Set("TransformScaleMethod", uint8_t{1});
    eng::Invoke(statics, begin);
    Obj actor = begin.ReturnObj();
    if (!actor) return nullptr;
    eng::Params finish(eng::FunctionOn(statics, "FinishSpawningActor"));
    finish.Set("Actor", actor);
    finish.Set("SpawnTransform", t);
    finish.Set("TransformScaleMethod", uint8_t{1});
    eng::Invoke(statics, finish);
    eng::WriteBool(actor, "bUnbound", true);
    eng::WriteBool(actor, "bEnabled", true);
    eng::WriteBytes(actor, "Priority", &kPriority, sizeof kPriority);
    return actor;
}

// FPostProcessSettings in PostProcessVolume: where it sits, and the struct.
bool Settings(int32_t* offset, Obj* structure) {
    Obj cls = eng::FindClass("PostProcessVolume");
    eng::Prop p = cls ? eng::FindProp(cls, "Settings") : eng::Prop{};
    *structure = p ? eng::StructOf(p) : nullptr;
    if (!*structure) return false;
    *offset = p.offset;
    return true;
}

void SetBit(uint8_t* base, const eng::Prop& p, bool on) {
    uint8_t& byte = base[p.offset + p.field[layout::kFBoolPropertyByteIndexOffset]];
    const uint8_t mask = p.field[layout::kFBoolPropertyBitMaskOffset];
    byte = on ? (byte | mask) : static_cast<uint8_t>(byte & ~mask);
}

// Writes one setting into a volume's Settings, and its override flag; with no volume, only checks it can be.
bool Write(Obj volume, const std::string& name, const std::array<double, 4>& v, std::string* error) {
    int32_t offset = 0;
    Obj structure = nullptr;
    if (!Settings(&offset, &structure)) {
        if (error) *error = "this game has no post-process settings";
        return false;
    }
    uint8_t scratch[64] = {};               // where a check without a volume "writes"
    uint8_t* base = volume ? volume + offset : nullptr;
    const eng::Prop p = eng::FindProp(structure, name);
    if (!p) {
        if (error) *error = "there is no post-process setting '" + name + "'";
        return false;
    }
    const std::string kind = eng::KindOf(p);
    if (p.size > static_cast<int32_t>(sizeof scratch)) {
        if (error) *error = "'" + name + "' can't be set this way";
        return false;
    }
    uint8_t* at = base ? base + p.offset : scratch;
    if (kind == "FloatProperty") {
        const float f = static_cast<float>(v[0]);
        std::memcpy(at, &f, sizeof f);
    } else if (kind == "DoubleProperty") {
        std::memcpy(at, &v[0], sizeof(double));
    } else if (kind == "BoolProperty") {
        if (base) SetBit(base, p, v[0] != 0);
    } else if (kind == "ByteProperty" || kind == "EnumProperty") {
        if (p.size != 1) {
            if (error) *error = "'" + name + "' isn't a setting that can be set this way";
            return false;
        }
        *at = static_cast<uint8_t>(v[0]);
    } else if (kind == "IntProperty") {
        const int32_t i = static_cast<int32_t>(v[0]);
        std::memcpy(at, &i, sizeof i);
    } else if (kind == "StructProperty") {
        const std::string type = eng::ObjName(eng::StructOf(p));
        if (type == "LinearColor") {
            const float c[4] = {static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2]), static_cast<float>(v[3])};
            std::memcpy(at, c, sizeof c);
        } else if (type == "Vector4") {
            std::memcpy(at, v.data(), 4 * sizeof(double));
        } else if (type == "Vector2f") {
            const float c[2] = {static_cast<float>(v[0]), static_cast<float>(v[1])};
            std::memcpy(at, c, sizeof c);
        } else if (type == "Vector2D") {
            std::memcpy(at, v.data(), 2 * sizeof(double));
        } else {
            if (error) *error = "'" + name + "' is a " + type + ", which can't be set this way";
            return false;
        }
    } else {
        if (error) *error = "'" + name + "' is a " + kind + ", which can't be set this way";
        return false;
    }
    const eng::Prop flag = eng::FindProp(structure, "bOverride_" + name);
    if (flag && base) SetBit(base, flag, true);
    return true;
}

// The plugin's volume, made (and given what the plugin set) if it has none in this map.
Obj VolumeOf(Filter& f) {
    if (Obj v = eng::Get(f.volume)) return v;
    Obj v = SpawnVolume();
    if (!v) return nullptr;
    f.volume = eng::MakeWeak(v);
    const float w = static_cast<float>(f.weight);
    eng::WriteBytes(v, "BlendWeight", &w, sizeof w);
    for (const Value& value : f.values) Write(v, value.name, value.v, nullptr);
    return v;
}

}  // namespace

void Frame() {
    if (game::Generation() != gGeneration) {
        gGeneration = game::Generation();
        for (auto& [owner, f] : gFilters) f.volume = {};    // the old volumes went with the old map
    }
    for (auto& [owner, f] : gFilters)
        if (!eng::Get(f.volume) && !f.values.empty() && game::PlayerController()) VolumeOf(f);
}

void RemoveOwner(int owner) { Clear(owner); }

bool Set(int owner, const std::string& name, double x, double y, double z, double w, std::string* error) {
    const std::array<double, 4> v{x, y, z, w};
    if (!Write(nullptr, name, v, error)) return false;      // a setting that can be set, before anything is made
    Filter& f = gFilters[owner];
    if (Obj volume = game::PlayerController() ? VolumeOf(f) : nullptr) Write(volume, name, v, nullptr);
    for (Value& value : f.values)
        if (value.name == name) {
            value.v = v;
            return true;
        }
    f.values.push_back({name, v});
    return true;
}

bool Weight(int owner, double weight) {
    Filter& f = gFilters[owner];
    f.weight = weight < 0 ? 0 : (weight > 1 ? 1 : weight);
    Obj volume = eng::Get(f.volume);
    if (!volume) return true;                   // used when the volume is made
    const float w = static_cast<float>(f.weight);
    return eng::WriteBytes(volume, "BlendWeight", &w, sizeof w);
}

void Clear(int owner) {
    auto it = gFilters.find(owner);
    if (it == gFilters.end()) return;
    if (Obj v = eng::Get(it->second.volume)) eng::Call(v, "K2_DestroyActor");
    gFilters.erase(it);
}

}  // namespace postprocess
