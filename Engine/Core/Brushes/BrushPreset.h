#pragma once
#include <algorithm>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

// A brush (.brush, XML): the tip every painting tool shares (image painter,
// mesh vertex paint, weight paint, terrain), plus optional sections the
// terrain tools read:
//  * relief: a height map the brush raises the terrain by (what terrain
//    stamps were: scanned rock shapes seen from above), with its facing;
//  * terrain: the tool the brush is made for (sculpt, paint or place) and
//    rules that limit where it acts (height and slope bands, paint layers
//    the ground has or lacks);
//  * placement: what a Place brush puts down (weighted assets), how densely,
//    how far apart, and how it stands them;
//  * foliage: the dense ground cover a Foliage brush paints (grass, ferns,
//    undergrowth): which foliage layers, how much of each, and how. The
//    plants themselves are generated where the density allows
//    (TerrainFoliage.h); sparse, large plants (bushes) are placed instead.
// Which paint layer a stroke paints and which object layer it places into
// are the tool's choice (the editor's toolbar, a script's call), not the
// brush's.
// Stroke colour, weight, target and bone selection live in the tool
// session, so changing a preset never changes the content being painted.
// Scripts can load a brush and stroke with it as the editor does
// (TerrainBrushStroke.h).
namespace Engine::Brushes
{
struct BrushRelief
{
    // Grayscale height map (16-bit PNG preferred), Assets-relative; empty:
    // no relief. 0 is the base the brush stands on, white its top.
    std::string mapPath;
    // Height of the map's white over the brush's half width: heights scale
    // with the brush radius, so the shape keeps its proportions.
    float heightRatio = 0.f;
    // The way the shape's face looks (tip space x, y; unit), and whether the
    // Stamp tool by default builds only against terrain behind it.
    float facingX = 0.f, facingY = 1.f;
    bool backed = false;
    bool Present() const { return !mapPath.empty() && heightRatio > 0.f; }
};

// Where a terrain brush acts, per point. Bands fade over `fade` (metres for
// height, degrees for slope). Paint layers are named by their material: an
// asset path, or a file name with or without extension ("grass" matches
// ".../grass.material"), so a brush means the same ground on any terrain
// whatever order its layers are in (BrushRules.h, ResolveBrushLayers).
struct BrushRules
{
    float minHeight = -std::numeric_limits<float>::infinity();
    float maxHeight = std::numeric_limits<float>::infinity();
    float heightFade = 2.f;
    float minSlope = 0.f, maxSlope = 90.f;   // degrees
    float slopeFade = 4.f;
    // The point needs at least `layerThreshold` of one of these layers (empty:
    // any ground), and loses what it has of the avoided ones.
    std::vector<std::string> onLayers, avoidLayers;
    float layerThreshold = .5f;
    // Edge band: past this share of the "on" layers the brush fades out (over
    // 0.1), so with a low threshold it acts only where they are partly
    // there, along their edge (a shrub belt where forest soil meets meadow).
    float maxLayerShare = 1.f;
    // Metres to keep from a trail's centre line (the terrain's contour paths),
    // fading in over trailFade; 0 ignores trails.
    float trailClearance = 0.f, trailFade = 2.f;
    bool Any() const
    {
        return minHeight > -std::numeric_limits<float>::max() ||
            maxHeight < std::numeric_limits<float>::max() || minSlope > 0.f ||
            maxSlope < 90.f || !onLayers.empty() || !avoidLayers.empty() ||
            maxLayerShare < 1.f || trailClearance > 0.f;
    }
};

struct BrushPlacementAsset
{
    std::string asset;            // prefab (or model) path
    float weight = 1.f;           // share of the picks
    float minScale = 1.f, maxScale = 1.f;
    // Heights (metres) the asset is picked at, its weight fading out over
    // heightFade past them: species by altitude (oaks low, spruce high).
    float minHeight = -std::numeric_limits<float>::infinity();
    float maxHeight = std::numeric_limits<float>::infinity();
    float heightFade = 20.f;
    float WeightAt(float height) const
    {
        const auto fade = [&](float past) {
            return heightFade > 0.f ? std::max(0.f, 1.f - past / heightFade) : 0.f;
        };
        if (height < minHeight) return weight * fade(minHeight - height);
        if (height > maxHeight) return weight * fade(height - maxHeight);
        return weight;
    }
};

struct BrushPlacement
{
    std::vector<BrushPlacementAsset> assets;
    float density = .05f;         // placements per square metre at full strength
    float minSpacing = 2.f;       // metres between placements in the target layer
    bool alignToSurface = false;
    bool randomYaw = true;
    float sink = 0.f;             // metres below the ground
    bool Any() const { return !assets.empty(); }
};

// A foliage layer a Foliage brush paints (named like the cells' ground-cover
// object layer, e.g. "Procedural meadow grass") and its amount: the density
// Set paints toward, or the share of the stroke's strength for Add and Erase.
struct BrushFoliageLayer
{
    std::string layer;
    float amount = 1.f;
};

struct BrushFoliage
{
    enum class Mode { Add = 0, Erase, Set };
    std::vector<BrushFoliageLayer> layers;
    Mode mode = Mode::Add;
    // Metres from the surface a dab reaches (any surface: open ground, a
    // cliff face, a cave ceiling).
    float shell = 1.5f;
    bool Any() const { return !layers.empty(); }
};

enum class BrushTool { Any = 0, Sculpt, Paint, Place, Foliage };

struct BrushTerrain
{
    BrushTool tool = BrushTool::Any;
    BrushRules rules;
    BrushPlacement placement;
    BrushFoliage foliage;
    bool Any() const
    {
        return tool != BrushTool::Any || rules.Any() || placement.Any() || foliage.Any();
    }
};

struct BrushPreset
{
    std::string name = "Round";
    std::string tipPath; // Assets-relative image path; empty means analytic tip.
    int shape = 0;       // circle, square
    int falloff = 2;     // hard, linear, smooth
    float size = 40.f;   // viewport pixels
    float hardness = .5f;
    float strength = .35f;
    float opacity = 1.f;
    float flow = 1.f;
    float spacing = .08f; // fraction of diameter
    float rotation = 0.f; // degrees
    float scatter = 0.f;  // fraction of radius
    float pressureSize = 0.f;
    float pressureStrength = 0.f;
    BrushRelief relief;
    BrushTerrain terrain;
};
// Version 1 holds the tip only; version 2 adds the optional <Relief>,
// <Terrain> (with <Rules>, <Placement> and <Foliage>) elements. Both load.
bool SaveBrushPreset(const std::filesystem::path& path,
    const BrushPreset& preset, std::string& error);
bool LoadBrushPreset(const std::filesystem::path& path,
    BrushPreset& preset, std::string& error);
}
