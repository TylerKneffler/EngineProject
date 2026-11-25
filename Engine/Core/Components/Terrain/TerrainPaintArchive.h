#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// A map's baked material paint: every cell's layer weights and lushness per
// grid vertex, as if the whole map had been painted by hand. The terrain
// loads it as its paint (Terrain::weights / lushness); nothing repaints the
// ground by rules at runtime, and hand paint (a cell's .terrainedit) goes on
// top of it.
//   u64 magic "TRNRPNTA", u32 version 1, i32 extent, i32 cellsX, i32 cellsY,
//   u32 layer count,
//   (2 extent + 1)^2 cell entries, rows of cells along +z from (-extent,
//     -extent): u64 offset, u32 stored bytes, u32 raw bytes (0: no paint),
//   the cell blocks (zlib), raw: one plane per layer, then lushness, each
//     (cellsX + 1) * (cellsY + 1) u8 values (value / 255), rows along +z.
namespace Engine::Components
{
class TerrainPaintArchive
{
public:
    using Cell = std::pair<int, int>;
    struct Header
    {
        int extent = 0, cellsX = 32, cellsY = 32, layers = 1;
    };
    // A cell's paint: `layers` weights per vertex (vertex-major) and one
    // lushness per vertex. False: the cell has none.
    using CellPaint = std::function<bool(Cell, std::vector<float>& weights, std::vector<float>& lushness)>;

    // Writes an archive; `cellPaint` runs on worker threads (it must be
    // thread-safe). progress (optional, on the calling thread) receives cells
    // done and total.
    static bool Write(const std::string& path, const Header& header, const CellPaint& cellPaint,
        const std::function<void(size_t, size_t)>& progress = {});

    // Opens one for reading; null when missing or malformed.
    static std::shared_ptr<const TerrainPaintArchive> Open(const std::string& path);
    const Header& GetHeader() const { return m_header; }
    bool Covers(Cell cell) const;
    // A cell's paint (weights renormalized per vertex). Thread-safe.
    bool ReadCell(Cell cell, std::vector<float>& weights, std::vector<float>& lushness) const;
    // Changes whenever the file does (size and write time).
    uint64_t Signature() const { return m_signature; }

private:
    std::string m_path;
    Header m_header;
    struct Entry { uint64_t offset = 0; uint32_t stored = 0, raw = 0; };
    std::vector<Entry> m_entries;
    uint64_t m_signature = 0;
};
}
