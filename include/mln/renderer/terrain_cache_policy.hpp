#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mln {
namespace terrain {

/// The DEM a terrain mesh drawable was built on.
struct DEMBinding {
    /// 0 = flat placeholder, 1 = closest loaded ancestor DEM, 2 = the tile's own DEM
    uint8_t tier = 0;
    /// Zoom of the bound DEM tile (-1 for the placeholder)
    int8_t demZoom = -1;
};

/// Whether a terrain drawable built on `current` should be rebuilt now that `available` could be
/// bound instead. A better tier always wins. Within the ancestor tier a deeper ancestor wins as
/// well: a fresh wide view often builds its tiles on whatever coarse ancestor is loaded first (z6
/// under a z13 tile samples 1/128th of that DEM, which renders flat), and the deeper ancestors that
/// load moments later must replace it. maplibre-gl-js resolves the covering DEM tile every frame
/// (Terrain.getTerrainData -> TileManager.getSourceTile(tileID, true)), so it never keeps a
/// shallower one than it has.
inline bool shouldRebind(const DEMBinding& current, const DEMBinding& available) {
    if (available.tier != current.tier) {
        return available.tier > current.tier;
    }
    return available.tier == 1 && available.demZoom > current.demZoom;
}

/// How one draped layer group covers a drape target.
struct DrapeGroupFallback {
    int32_t layerIndex = 0;
    /// -1 = no usable tile, 0 = exact or deeper tiles, n > 0 = only an ancestor n zoom levels up
    int16_t deficit = -1;
    /// What the group draws into the target: its overlapping tiles and their buckets, summed.
    /// Moves when a tile's data is replaced in place, not when it merely reloads the same bucket.
    std::size_t contents = 0;
};

/// Whether the content a drape target would render now (`now`) is strictly worse than what it
/// baked (`baked`): no layer group improved and at least one lost its content or fell back to a
/// coarser ancestor. The drape cache keeps its baked texture in that case, so a tile briefly
/// dropping out of the render set (eviction, reload) does not flash the drape empty.
///
/// Compared per layer group, not as a sum over groups: a sum lets one group's regression outweigh
/// another group's improvement, and the improvement - typically hillshade arriving from its real
/// DEM tile - is then discarded for as long as the target lives. Group sets that differ (style
/// change) are not comparable and never count as worse, and neither is a group whose coverage
/// held but whose data changed.
inline bool drapeFallbackStrictlyWorse(const std::vector<DrapeGroupFallback>& now,
                                       const std::vector<DrapeGroupFallback>& baked) {
    if (now.size() != baked.size()) {
        return false;
    }
    bool worse = false;
    for (std::size_t i = 0; i < now.size(); ++i) {
        const auto& n = now[i];
        const auto& b = baked[i];
        if (n.layerIndex != b.layerIndex) {
            return false;
        }
        if (n.deficit == b.deficit) {
            // Same coverage, different data: a source changed what it draws here (a line
            // selected, a route extended, a recording grown). That is new content, not lost
            // detail, so a group elsewhere dropping a stale ancestor fallback at the same time
            // must not keep the old bake - the target would show the old data indefinitely.
            if (n.deficit >= 0 && n.contents != b.contents) {
                return false;
            }
            continue;
        }
        const bool improved = (b.deficit < 0) || (n.deficit >= 0 && n.deficit < b.deficit);
        if (improved) {
            return false;
        }
        worse = true;
    }
    return worse;
}

/// A draped layer group's deficit on a target (`DrapeGroupFallback::deficit`). Exact or deeper
/// drawables cover it. So does an exact or deeper tile that is loaded, complete and holds nothing
/// for the layer: its data is gone, not on its way. Deleting the last feature of a GeoJSON line in
/// a tile (a recording discarded, a track deleted) leaves exactly that, and counting it as no
/// coverage read the deletion as a reload losing content, so the target held the deleted line for
/// as long as it lived. Otherwise an ancestor `ancestorLevelsUp` levels up (-1 for none) stands in,
/// drawn or emptied the same way.
inline int16_t drapeGroupDeficit(bool exactDrawn, bool exactEmptied, int16_t ancestorLevelsUp) {
    if (exactDrawn || exactEmptied) {
        return 0;
    }
    return ancestorLevelsUp >= 0 ? ancestorLevelsUp : -1;
}

/// What an emptied tile contributes to a group's contents: its id under a seed no drawn tile's
/// hash starts from, so emptying a tile always moves the contents.
constexpr std::size_t drapeEmptiedTileSeed = 0x9e3779b97f4a7c15ull;

} // namespace terrain
} // namespace mln
