#include "draw.hpp"

#include <cmath>
#include <cstring>
#include <map>
#include <memory>

#include "cosmetics.hpp"

#include "game.hpp"
#include "log.hpp"
#include "models.hpp"

namespace draw {
namespace {

using eng::Obj;

struct Shape {
    int owner = -1;
    eng::Weak actor;
    eng::Weak material;                 // its dynamic material, once Glow has found it
    std::shared_ptr<models::Model> model;   // a model's (Model): what it is, and its parts built on the actor
    std::shared_ptr<models::Built> built;
};
std::map<int, Shape> gShapes;
int gNextId = 1;
int gGeneration = -1;

int gCameraOwner = -1;
eng::Weak gCamera;

struct Vec3 {
    double x, y, z;
};
struct Rot {
    double pitch, yaw, roll;
};

Obj Actor(int owner, int id) {
    auto it = gShapes.find(id);
    return it != gShapes.end() && it->second.owner == owner ? eng::Get(it->second.actor) : nullptr;
}

int Keep(int owner, Obj actor) {
    if (!actor) return 0;
    const int id = gNextId++;
    gShapes[id] = {owner, eng::MakeWeak(actor), {}, nullptr, nullptr};
    return id;
}

// A shape's actor and a model's parts go together.
void Destroy(Shape& shape) {
    if (shape.built) models::Destroy(*shape.built);
    if (Obj a = eng::Get(shape.actor)) eng::Call(a, "K2_DestroyActor");
}

struct Vec3d {
    double x, y, z;
};

Obj Pawn() {
    Obj controller = game::PlayerController();
    return controller ? eng::Call(controller, "K2_GetPawn").ReturnObj() : nullptr;
}

void ViewThrough(Obj target) {
    Obj controller = game::PlayerController();
    if (!controller || !target) return;
    eng::Params p(eng::FunctionOn(controller, "SetViewTargetWithBlend"));
    p.Set("NewViewTarget", target);
    p.Set("BlendTime", 0.0f);
    eng::Invoke(controller, p);
}

}  // namespace

void Frame() {
    // Models' spinning groups and animations (no ball: as still).
    for (auto& [id, shape] : gShapes)
        if (shape.built && shape.model && eng::Get(shape.actor)) models::Animate(*shape.model, *shape.built, nullptr, game::Seconds());
    if (game::Generation() == gGeneration) return;
    gGeneration = game::Generation();
    gShapes.clear();                    // their actors went with the old map
    gCamera = {};
    gCameraOwner = -1;
}

void RemoveOwner(int owner) {
    Clear(owner);
    ReleaseCamera(owner);
}

int Tube(int owner, const std::vector<std::array<double, 3>>& path, double radius, float r, float g, float b, bool glow, float opacity) {
    return Keep(owner, models::SpawnTube(path, radius, {r, g, b, glow, 8, opacity}));
}

int Ball(int owner, double radius, float r, float g, float b, bool glow) {
    return Keep(owner, models::SpawnBall(radius, {r, g, b, glow, 8}));
}

bool Move(int owner, int id, double x, double y, double z) {
    Obj a = Actor(owner, id);
    if (!a) return false;
    eng::Params p(eng::FunctionOn(a, "K2_SetActorLocation"));
    p.Set("NewLocation", Vec3{x, y, z});
    p.Set("bSweep", uint8_t{0});
    p.Set("bTeleport", uint8_t{1});
    return eng::Invoke(a, p);
}

bool Glow(int owner, int id, float r, float g, float b, float bright) {
    auto it = gShapes.find(id);
    if (it == gShapes.end() || it->second.owner != owner) return false;
    Obj material = eng::Get(it->second.material);
    if (!material) {
        material = models::GlowMaterial(eng::Get(it->second.actor));
        it->second.material = eng::MakeWeak(material);
    }
    return material && models::SetGlow(material, r, g, b, bright);
}

bool Fade(int owner, int id, float opacity) {
    auto it = gShapes.find(id);
    if (it == gShapes.end() || it->second.owner != owner) return false;
    Obj material = eng::Get(it->second.material);
    if (!material) {
        material = models::GlowMaterial(eng::Get(it->second.actor));      // the shape's dynamic material, glass or glow
        it->second.material = eng::MakeWeak(material);
    }
    return material && models::SetOpacity(material, opacity);
}

int Model(int owner, const std::string& text, std::string* error) {
    auto model = std::make_shared<models::Model>();
    if (!models::Parse(text, model.get(), error)) return 0;
    Obj holder = models::SpawnHolder();
    Obj root = holder ? eng::ReadObj(holder, "DynamicMeshComponent") : nullptr;
    if (!root) {
        *error = "the model's actor could not be made";
        return 0;
    }
    eng::Call(root, "SetCollisionEnabled", uint8_t{0});
    const int id = Keep(owner, holder);
    gShapes[id].model = model;
    gShapes[id].built = std::make_shared<models::Built>(models::Build(*model, root));
    return id;
}

bool Turn(int owner, int id, double pitch, double yaw, double roll) {
    Obj a = Actor(owner, id);
    if (!a) return false;
    eng::Params p(eng::FunctionOn(a, "K2_SetActorRotation"));
    p.Set("NewRotation", Rot{pitch, yaw, roll});
    p.Set("bTeleportPhysics", uint8_t{1});
    return eng::Invoke(a, p);
}

bool Scale(int owner, int id, double scale) {
    Obj a = Actor(owner, id);
    return a && eng::Call(a, "SetActorScale3D", Vec3d{scale, scale, scale}).Invoked();
}

bool Effect(const std::string& system, double x, double y, double z, double scale, double nx, double ny, double nz) {
    Obj asset = system.empty() ? nullptr : cosmetics::LoadAsset(eng::Widen(system));
    Obj controller = game::PlayerController();
    if (!asset || !controller) return false;
    // Up along the normal: pitch and roll from it (yaw left as it is).
    const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
    Rot rot{0, 0, 0};
    if (len > 0) {
        rot.pitch = std::atan2(nx / len, nz / len) * 57.29577951308232;
        rot.roll = -std::atan2(ny / len, std::sqrt(nx * nx + nz * nz) / len) * 57.29577951308232;
    }
    Obj lib = eng::FindCdo("NiagaraFunctionLibrary");
    eng::Params p(eng::FunctionOn(lib, "SpawnSystemAtLocation"));
    p.Set("WorldContextObject", controller);
    p.Set("SystemTemplate", asset);
    p.Set("Location", Vec3d{x, y, z});
    p.Set("Rotation", rot);
    p.Set("Scale", Vec3d{scale, scale, scale});
    p.Set("bAutoDestroy", uint8_t{1});
    p.Set("bAutoActivate", uint8_t{1});
    return eng::Invoke(lib, p) && p.ReturnObj();
}

bool Sound(const std::string& sound, double volume, double pitch) {
    Obj asset = sound.empty() ? nullptr : cosmetics::LoadAsset(eng::Widen(sound));
    Obj controller = game::PlayerController();
    if (!asset || !controller) return false;
    Obj statics = eng::FindCdo("GameplayStatics");
    eng::Params p(eng::FunctionOn(statics, "PlaySound2D"));
    p.Set("WorldContextObject", controller);
    p.Set("Sound", asset);
    p.Set("VolumeMultiplier", static_cast<float>(volume));
    p.Set("PitchMultiplier", static_cast<float>(pitch));
    return eng::Invoke(statics, p);
}

bool Shake(double scale) {
    static eng::Weak shakeClass;
    Obj cls = eng::Get(shakeClass);
    if (!cls) {
        cls = cosmetics::LoadAsset(L"/Game/Art/VFX/CameraShake/Shake_BallestCam.Shake_BallestCam_C");
        if (cls) cosmetics::KeepAlive(cls);
        shakeClass = eng::MakeWeak(cls);
    }
    Obj controller = game::PlayerController();
    Obj manager = controller ? eng::ReadObj(controller, "PlayerCameraManager") : nullptr;
    if (!cls || !manager) return false;
    eng::Params p(eng::FunctionOn(manager, "StartCameraShake"));
    p.Set("ShakeClass", cls);
    p.Set("Scale", static_cast<float>(scale));
    return eng::Invoke(manager, p);
}

int Adopt(int owner, Obj actor) { return Keep(owner, actor); }
Obj ActorOf(int owner, int id) { return Actor(owner, id); }

bool Show(int owner, int id, bool shown) {
    Obj a = Actor(owner, id);
    if (!a) return false;
    auto it = gShapes.find(id);
    if (it->second.built)                           // a model's parts are actors of their own, attached to it
        for (Obj part : models::Actors(*it->second.built)) eng::Call(part, "SetActorHiddenInGame", static_cast<uint8_t>(!shown));
    return eng::Call(a, "SetActorHiddenInGame", static_cast<uint8_t>(!shown)).Invoked();
}

void Remove(int owner, int id) {
    auto it = gShapes.find(id);
    if (it == gShapes.end() || it->second.owner != owner) return;
    Destroy(it->second);
    gShapes.erase(it);
}

void Clear(int owner) {
    for (auto it = gShapes.begin(); it != gShapes.end();) {
        if (it->second.owner == owner) {
            Destroy(it->second);
            it = gShapes.erase(it);
        } else {
            ++it;
        }
    }
}

bool Project(double x, double y, double z, double* sx, double* sy) {
    Obj controller = game::PlayerController();
    if (!controller) return false;
    Obj layout = eng::FindCdo("WidgetLayoutLibrary");
    eng::Params p(eng::FunctionOn(layout, "ProjectWorldLocationToWidgetPosition"));
    p.Set("PlayerController", controller);
    p.Set("WorldLocation", Vec3{x, y, z});
    p.Set("bPlayerViewportRelative", uint8_t{0});
    if (!eng::Invoke(layout, p) || !p.ReturnBool()) return false;
    struct {
        double x, y;
    } screen{};
    const uint8_t* s = p.Get("ScreenPosition");
    if (!s) return false;
    std::memcpy(&screen, s, sizeof screen);
    *sx = screen.x;
    *sy = screen.y;
    return true;
}

bool TakeCamera(int owner) {
    if (gCameraOwner >= 0 && gCameraOwner != owner) return false;
    if (Obj camera = eng::Get(gCamera)) {
        ViewThrough(camera);
        gCameraOwner = owner;
        return true;
    }
    Obj controller = game::PlayerController();
    Obj cls = eng::FindClass("CameraActor");
    if (!controller || !cls) return false;
    Obj statics = eng::FindCdo("GameplayStatics");
    // Where the game is looking now, so taking the camera doesn't jump.
    Obj manager = eng::ReadObj(controller, "PlayerCameraManager");
    const Vec3 at = manager ? eng::Call(manager, "GetCameraLocation").ReturnAs<Vec3>() : Vec3{0, 0, 0};
    const Rot facing = manager ? eng::Call(manager, "GetCameraRotation").ReturnAs<Rot>() : Rot{0, 0, 0};
    struct Transform {
        uint8_t bytes[96];
    } t{};
    const Vec3 one{1, 1, 1};
    const eng::Params made = eng::Call(eng::FindCdo("KismetMathLibrary"), "MakeTransform", at, facing, one);
    if (const uint8_t* r = made.Return()) std::memcpy(t.bytes, r, sizeof t.bytes);
    eng::Params begin(eng::FunctionOn(statics, "BeginDeferredActorSpawnFromClass"));
    begin.Set("WorldContextObject", controller);
    begin.Set("ActorClass", cls);
    begin.Set("SpawnTransform", t);
    begin.Set("CollisionHandlingOverride", uint8_t{1});
    begin.Set("TransformScaleMethod", uint8_t{1});
    eng::Invoke(statics, begin);
    Obj camera = begin.ReturnObj();
    if (!camera) return false;
    eng::Params finish(eng::FunctionOn(statics, "FinishSpawningActor"));
    finish.Set("Actor", camera);
    finish.Set("SpawnTransform", t);
    finish.Set("TransformScaleMethod", uint8_t{1});
    eng::Invoke(statics, finish);
    gCamera = eng::MakeWeak(camera);
    gCameraOwner = owner;
    // A CameraActor keeps a 16:9 picture by default, which puts black bars at the sides of wider screens.
    if (Obj component = eng::ReadObj(camera, "CameraComponent")) eng::Call(component, "SetConstraintAspectRatio", uint8_t{0});
    ViewThrough(camera);
    hostlog::Info("draw: camera taken");
    return true;
}

bool SetCamera(int owner, double x, double y, double z, double pitch, double yaw, double fov) {
    Obj camera = gCameraOwner == owner ? eng::Get(gCamera) : nullptr;
    if (!camera) return false;
    eng::Params p(eng::FunctionOn(camera, "K2_SetActorLocationAndRotation"));
    p.Set("NewLocation", Vec3{x, y, z});
    p.Set("NewRotation", Rot{pitch, yaw, 0});
    p.Set("bSweep", uint8_t{0});
    p.Set("bTeleport", uint8_t{1});
    eng::Invoke(camera, p);
    if (Obj component = eng::ReadObj(camera, "CameraComponent")) eng::Call(component, "SetFieldOfView", static_cast<float>(fov));
    return true;
}

void ReleaseCamera(int owner) {
    if (gCameraOwner != owner) return;
    gCameraOwner = -1;
    if (Obj pawn = Pawn()) ViewThrough(pawn);
    if (Obj camera = eng::Get(gCamera)) eng::Call(camera, "K2_DestroyActor");
    gCamera = {};
    hostlog::Info("draw: camera released");
}

bool HasCamera(int owner) { return gCameraOwner == owner && eng::Get(gCamera); }

}  // namespace draw
