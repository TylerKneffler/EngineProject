#pragma once
#include "Core/Components/Terrain/TerrainObjectLayer.h"
#include "Core/Serialization/Json.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// A map's baked object placements (trees, rocks, bushes...): every cell's
// object layers in one indexed file, so a large map loads its placements
// instead of generating them. Each layer name is stored once with its
// template (settings and per-asset LOD chains: a TerrainObjectLayer saved
// without placements); each cell holds its placements, compressed:
//   u64 magic "TRNROBJA", u32 version 2, i32 extent, i32 cellsX, i32 cellsY,
//   f32 cellWidth, f32 cellDepth,
//   u32 layer count, per layer: u32 name length, name, u32 template length,
//     template (binary asset, Json AssetWriteBinary),
//   u32 asset count, per asset: u32 length, path,
//   (2 extent + 1)^2 cell entries, rows of cells along +z from (-extent,
//     -extent): u64 offset, u32 stored bytes, u32 raw bytes (0: empty cell),
//   the cell blocks (zlib), raw: u16 layer count, per layer: u16 layer index,
//     u32 placement count n, then its placements (sorted by asset, then x)
//     as columns, each n values long, so like values compress together:
//     u16 asset, u16 x delta from the previous x (cell-local, 0..65535
//     across the cell), u16 z, i16 y (1/16 m), i8 normal x, i8 normal z
//     (normal.y from them), u8 yaw (256 steps), u8 scale (s / 40).
// TerrainManager::objectArchive names one; a cell's own .objects file (an
// edit) wins over its archive block.
namespace Engine::Components
{
class TerrainObjectArchive
{
public:
    using Cell = std::pair<int, int>;
    using JsonValue = Engine::Serialization::JsonValue;
    struct Layer
    {
        std::string name;
        std::vector<TerrainObjectLayer::Placement> placements;
    };
    struct Header
    {
        int extent = 0, cellsX = 32, cellsY = 32;
        float cellWidth = 1.f, cellDepth = 1.f;
    };

    // Writes an archive: templates by layer name (layers saved without
    // placements), and each cell's layers from `cellLayers` (called once per
    // cell in file order, from any thread the caller likes; empty: none).
    // progress (optional) receives cells done and total.
    static bool Write(const std::string& path, const Header& header,
        const std::vector<std::pair<std::string, JsonValue>>& templates,
        const std::function<std::vector<Layer>(Cell)>& cellLayers,
        const std::function<void(size_t, size_t)>& progress = {});

    // Opens one for reading; null when missing or malformed.
    static std::shared_ptr<const TerrainObjectArchive> Open(const std::string& path);
    const Header& GetHeader() const { return m_header; }
    bool Covers(Cell cell) const;
    // A cell's layers (placements in the cell's own space). Thread-safe.
    bool ReadCell(Cell cell, std::vector<Layer>& layers) const;
    // A cell's layers as saved object layers (templates with the cell's
    // placements), as TerrainObjectLayer::Serialize writes them.
    bool ReadCellLayers(Cell cell, JsonValue& layers) const;
    const JsonValue* Template(const std::string& layerName) const;
    // Changes whenever the file does (size and write time).
    uint64_t Signature() const { return m_signature; }

private:
    std::string m_path;
    Header m_header;
    std::vector<std::pair<std::string, JsonValue>> m_templates;
    std::vector<std::string> m_assets;
    struct Entry { uint64_t offset = 0; uint32_t stored = 0, raw = 0; };
    std::vector<Entry> m_entries;
    uint64_t m_signature = 0;
    mutable std::mutex m_mutex;
};
}
