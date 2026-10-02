// Screen-wide post-process filters for plugins: colour grading, white balance, tint, bloom, vignette and the rest of
// the engine's FPostProcessSettings.
//
// Each plugin that sets one gets a PostProcessVolume of its own, unbound (it covers the whole world, wherever the camera
// is), at a high priority so it wins over the map's own volumes. A setting is written into the volume's Settings by its
// name, with its bOverride_ flag set (the engine only applies overridden settings); the volume's blend weight fades the
// whole filter from none (0) to full (1). What a plugin set is kept, and put on a new volume when the map changes. The
// volume goes when the plugin stops or clears it. Game thread only.
#pragma once
#include <string>

namespace postprocess {

void Frame();                           // a new map: the plugins' volumes are made again with what they set
void RemoveOwner(int owner);            // a plugin stopped: its volume goes

// Sets a setting of FPostProcessSettings by its name (ColorSaturation, WhiteTemp, VignetteIntensity, ...): a float
// takes x; a colour or vector takes x, y, z, w; a bool or byte takes x. False (and why in `error`) if there is no
// such setting or it isn't one of those types.
bool Set(int owner, const std::string& name, double x, double y, double z, double w, std::string* error);
// The filter's strength, 0 (none) to 1 (full).
bool Weight(int owner, double weight);
// Takes everything this plugin set off again (its volume goes).
void Clear(int owner);

}  // namespace postprocess
