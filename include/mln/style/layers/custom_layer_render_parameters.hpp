#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace mln {

class PaintParameters;

namespace gfx {
class Texture2D;
} // namespace gfx

namespace style {

/**
 * A tile of the 3D terrain as it is drawn this frame. A custom layer draws on the terrain by
 * drawing the terrain's own surface for each tile: a grid of `CustomLayerTerrain::meshSize`
 * cells a side over tile coordinates 0..8192, each cell split from its top-right to its
 * bottom-left corner, with every vertex lifted by the DEM as the terrain shader lifts it
 * (`get_elevation` in the shader prelude), then transformed by `matrix`.
 */
struct CustomLayerTerrainTile {
    uint8_t z;
    uint32_t x;
    uint32_t y;
    /// World copy: 0 for the primary world, -1 west of it, 1 east of it, and so on.
    int16_t wrap;
    /// Projects tile coordinates (x, y in 0..8192, z = elevation in metres) to clip space, in the
    /// same convention as `CustomLayerRenderParameters::projectionMatrix`.
    std::array<double, 16> matrix;
    /// Maps a tile coordinate `pos` to a texel of `demTexture`: `(pos * demCoords[0] + {demCoords[1],
    /// demCoords[2]}) * demCoords[3] + border`, where demCoords[3] is the DEM's size without its
    /// border and the border is half the rest of the texture's width. The DEM may be an ancestor
    /// tile's while this tile's own loads. Sample it unfiltered and interpolate the decoded
    /// heights, as packed elevations cannot be filtered.
    std::array<float, 4> demCoords;
    /// The DEM this tile is lifted by; a 1x1 texture at sea level while no DEM covers it yet.
    std::shared_ptr<gfx::Texture2D> demTexture;
};

/// The 3D terrain as it is drawn this frame, when the style has terrain.
struct CustomLayerTerrain {
    /// Multiplies the DEM's heights, as the terrain is drawn.
    float exaggeration;
    /// Decodes a DEM texel to metres: `dot(rgb * 255, unpack.rgb) - unpack.a`.
    std::array<float, 4> unpack;
    /// The cells a side of each tile's grid.
    uint32_t meshSize;
    std::vector<CustomLayerTerrainTile> tiles;
};

/**
 * Parameters that define the current camera position for a
 * `CustomLayerHost::render()` function.
 */
struct CustomLayerRenderParameters {
    double width;
    double height;
    double latitude;
    double longitude;
    double zoom;
    double bearing;
    double pitch;
    double fieldOfView;

    /// Standard projection matrix (nearZ = 1 tile unit).
    /// Use this for 2D/flat custom geometry.
    std::array<double, 16> projectionMatrix;

    /// A 4×4 matrix representing the map view’s current near clip projection state.
    std::array<double, 16> nearClippedProjectionMatrix;

    /// The 3D terrain, when the style has terrain, so the layer can draw on it rather than at sea
    /// level. In `render` the terrain has been drawn this frame and its depth written; in
    /// `preRender` it is the terrain the previous frame drew, as the terrain updates after layers.
    std::optional<CustomLayerTerrain> terrain;

    CustomLayerRenderParameters(const PaintParameters&);
};

} // namespace style
} // namespace mln
