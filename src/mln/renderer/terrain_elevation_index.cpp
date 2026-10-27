#include <mln/renderer/terrain_elevation_index.hpp>

#include <mln/geometry/dem_data.hpp>
#include <mln/math/clamp.hpp>
#include <mln/map/transform_state.hpp>
#include <mln/util/hash.hpp>
#include <mln/util/projection.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace mln {

TerrainElevationIndex::Grid::Grid(const DEMData& dem)
    : dim(dem.dim) {
    const int32_t side = dim + 1;
    heights.resize(static_cast<size_t>(side) * side);
    for (int32_t y = 0; y < side; ++y) {
        for (int32_t x = 0; x < side; ++x) {
            const int32_t h = util::clamp(dem.get(x, y),
                                          static_cast<int32_t>(std::numeric_limits<int16_t>::min()),
                                          static_cast<int32_t>(std::numeric_limits<int16_t>::max()));
            heights[static_cast<size_t>(y) * side + x] = static_cast<int16_t>(h);
            maxHeight = std::max(maxHeight, static_cast<int16_t>(h));
        }
    }
}

TerrainElevationIndex::TerrainElevationIndex(std::vector<Tile> tiles_, float exaggeration_)
    : tiles(std::move(tiles_)),
      exaggeration(exaggeration_) {
    for (const auto& tile : tiles) {
        const uint8_t z = tile.id.canonical.z;
        auto level = std::find_if(levels.begin(), levels.end(), [z](const Level& l) { return l.z == z; });
        if (level == levels.end()) {
            levels.push_back(Level{z, {}});
            level = std::prev(levels.end());
        }
        const int64_t across = int64_t{1} << z;
        const int64_t x = static_cast<int64_t>(tile.id.wrap) * across + tile.id.canonical.x;
        level->tiles.emplace(key(x, tile.id.canonical.y), &tile);
        maxElevation = std::max(maxElevation, static_cast<double>(tile.grid->maxHeight) * exaggeration);
    }
    std::sort(levels.begin(), levels.end(), [](const Level& a, const Level& b) { return a.z > b.z; });
}

std::optional<double> TerrainElevationIndex::getElevation(const LatLng& latLng) const {
    for (const auto& level : levels) {
        // The integer-zoom overload projects into tile units (world size 2^z), not pixels.
        const Point<double> world = Projection::project(latLng, level.z);
        const double fx = std::floor(world.x);
        const double fy = std::floor(world.y);
        const auto found = level.tiles.find(key(static_cast<int64_t>(fx), static_cast<int64_t>(fy)));
        if (found == level.tiles.end()) {
            continue;
        }
        // Bilinear interpolation of the DEM texels, as RenderTerrain::getElevation
        const Grid& grid = *found->second->grid;
        const auto dim = static_cast<double>(grid.dim);
        const double px = util::clamp((world.x - fx) * dim, 0.0, dim - 1.0);
        const double py = util::clamp((world.y - fy) * dim, 0.0, dim - 1.0);
        const auto x0 = static_cast<int32_t>(std::floor(px));
        const auto y0 = static_cast<int32_t>(std::floor(py));
        const double tx = px - x0;
        const double ty = py - y0;
        const double tl = grid.get(x0, y0);
        const double tr = grid.get(x0 + 1, y0);
        const double bl = grid.get(x0, y0 + 1);
        const double br = grid.get(x0 + 1, y0 + 1);
        const double top = tl + (tr - tl) * tx;
        const double bottom = bl + (br - bl) * tx;
        return (top + (bottom - top) * ty) * exaggeration;
    }
    return std::nullopt;
}

std::vector<float> TerrainElevationIndex::sampleGrid(const UnwrappedTileID& tile,
                                                     const int32_t dim,
                                                     const int32_t border) const {
    const int32_t side = dim + 2 * border;
    std::vector<float> heights(static_cast<size_t>(side) * side, 0.0f);
    const uint8_t z = tile.canonical.z;
    // The tile's corner in tile units at its own zoom, wrapped as the index keys its tiles
    const double tileX = static_cast<double>(tile.wrap) * std::ldexp(1.0, z) + tile.canonical.x;
    const double tileY = tile.canonical.y;
    // Neighbouring samples nearly always fall in the tile the last one did: remember it per level
    struct Last {
        uint64_t key = 0;
        const Tile* tile = nullptr;
        bool looked = false;
    };
    std::vector<Last> last(levels.size());
    std::vector<double> scales(levels.size());
    for (size_t l = 0; l < levels.size(); ++l) {
        scales[l] = std::ldexp(1.0, static_cast<int>(levels[l].z) - static_cast<int>(z));
    }
    for (int32_t j = 0; j < side; ++j) {
        const double v = tileY + static_cast<double>(j - border) / dim;
        for (int32_t i = 0; i < side; ++i) {
            const double u = tileX + static_cast<double>(i - border) / dim;
            for (size_t l = 0; l < levels.size(); ++l) {
                const double wx = u * scales[l];
                const double wy = v * scales[l];
                const double fx = std::floor(wx);
                const double fy = std::floor(wy);
                const uint64_t k = key(static_cast<int64_t>(fx), static_cast<int64_t>(fy));
                if (!last[l].looked || last[l].key != k) {
                    const auto found = levels[l].tiles.find(k);
                    last[l] = {k, found == levels[l].tiles.end() ? nullptr : found->second, true};
                }
                if (!last[l].tile) {
                    continue;
                }
                // Bilinear interpolation of the DEM texels, as getElevation
                const Grid& grid = *last[l].tile->grid;
                const auto gridDim = static_cast<double>(grid.dim);
                const double px = util::clamp((wx - fx) * gridDim, 0.0, gridDim - 1.0);
                const double py = util::clamp((wy - fy) * gridDim, 0.0, gridDim - 1.0);
                const auto x0 = static_cast<int32_t>(std::floor(px));
                const auto y0 = static_cast<int32_t>(std::floor(py));
                const double tx = px - x0;
                const double ty = py - y0;
                const double top = grid.get(x0, y0) + (grid.get(x0 + 1, y0) - grid.get(x0, y0)) * tx;
                const double bottom = grid.get(x0, y0 + 1) + (grid.get(x0 + 1, y0 + 1) - grid.get(x0, y0 + 1)) * tx;
                heights[static_cast<size_t>(j) * side + i] = static_cast<float>(top + (bottom - top) * ty);
                break;
            }
        }
    }
    return heights;
}

TerrainElevationIndex::Cover TerrainElevationIndex::cover(const UnwrappedTileID& tile) const {
    Cover result;
    for (const auto& candidate : tiles) {
        if (candidate.id == tile || candidate.id.isChildOf(tile) || tile.isChildOf(candidate.id)) {
            util::hash_combine(result.signature, std::hash<UnwrappedTileID>{}(candidate.id));
            util::hash_combine(result.signature, reinterpret_cast<std::uintptr_t>(candidate.grid.get()));
            result.deepest = std::max(result.deepest, static_cast<int>(candidate.id.canonical.z));
        }
    }
    return result;
}

std::optional<LatLng> pickTerrainSurface(const TransformState& state,
                                         const ScreenCoordinate& pixel,
                                         const std::function<std::optional<double>(const LatLng&)>& elevationAt,
                                         double maxElevation) {
    // TransformState unprojects y-up screen points; queries and the platform views hand us y-down
    ScreenCoordinate flipped = pixel;
    flipped.y = state.getSize().height - pixel.y;
    const auto camera = state.getFreeCameraOptions().getLocation();
    if (!camera || camera->altitude <= 0.0) {
        return std::nullopt;
    }
    // Terrain height (exaggerated metres, the projection's z unit) where the ray crosses the
    // horizontal plane at `planeHeight`
    const auto surfaceHeightAt = [&](double planeHeight, LatLng& out) {
        out = state.screenCoordinateToLatLng(flipped, planeHeight);
        return elevationAt(out).value_or(0.0);
    };
    constexpr int steps = 64;
    // Nothing is higher than the highest terrain, so start just above it, unless the camera is lower
    const double top = std::min(camera->altitude * 0.999, maxElevation + 1.0);
    if (top <= 0.0) {
        return std::nullopt; // no terrain above sea level
    }
    double above = top; // last plane height known to be above the surface
    LatLng probe;
    if (surfaceHeightAt(above, probe) >= above) {
        return std::nullopt; // camera inside the terrain
    }
    for (int i = 1; i <= steps; ++i) {
        const double h = top * (1.0 - static_cast<double>(i) / steps);
        if (surfaceHeightAt(h, probe) < h) {
            above = h;
            continue;
        }
        // Crossed between `above` and `h`: bisect onto the surface
        double below = h;
        for (int j = 0; j < 8; ++j) {
            const double mid = 0.5 * (above + below);
            if (surfaceHeightAt(mid, probe) < mid) {
                above = mid;
            } else {
                below = mid;
            }
        }
        return state.screenCoordinateToLatLng(flipped, 0.5 * (above + below), LatLng::Wrapped);
    }
    return std::nullopt;
}

} // namespace mln
