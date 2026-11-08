#pragma once

#include <mln/shaders/layer_ubo.hpp>

namespace mln {
namespace shaders {

struct alignas(16) TerrainDrawableUBO {
    /*  0 */ std::array<float, 4 * 4> matrix;
    /* 64 */ std::array<float, 4> dem_coords; // scale, x offset, y offset into the bound DEM
                                              // tile ({1,0,0,0} unless an ancestor is bound)
    /* 80 */ std::array<float, 4> wave;       // the wave's centre x, y, front and width in this
                                              // tile's units (style::Terrain::Wave); width 0 = none
    /* 96 */
};
static_assert(sizeof(TerrainDrawableUBO) == 6 * 16);

// One entry per instance of the instanced GL terrain depth pass. The whole array is bound as
// the TerrainDrawableUBO block and indexed by gl_InstanceID in terrain_depth.vertex. Kept
// separate from TerrainDrawableUBO so the shared struct (and the mtl/vulkan/webgpu layouts
// mirroring it) stay untouched. std140: mat4 then two vec4s -> 6*16 bytes.
struct alignas(16) TerrainDepthInstanceUBO {
    /*  0 */ std::array<float, 4 * 4> matrix;
    /* 64 */ std::array<float, 4> dem_coords; // scale, x offset, y offset, dem_dim (in .w)
    /* 80 */ float dem_layer;                 // sampler2DArray layer of this tile's DEM
    /* 84 */ float pad1;
    /* 88 */ float pad2;
    /* 92 */ float pad3;
    /* 96 */
};
static_assert(sizeof(TerrainDepthInstanceUBO) == 6 * 16);

struct alignas(16) TerrainTilePropsUBO {
    /*  0 */ std::array<float, 2> dem_tl;
    /*  8 */ float dem_scale;
    /* 12 */ float pad1;
    /* 16 */
};
static_assert(sizeof(TerrainTilePropsUBO) == 16);

/// Evaluated properties that do not depend on the tile
struct alignas(16) TerrainEvaluatedPropsUBO {
    /*  0 */ std::array<float, 4> unpack; // DEM unpack vector for the source's encoding
    /* 16 */ float exaggeration;
    /* 20 */ float elevation_offset;
    /* 24 */ float wave_peak;  // style::Terrain::Wave::peak
    /* 28 */ float wave_crest; // style::Terrain::Wave::crest
    /* 32 */
};
static_assert(sizeof(TerrainEvaluatedPropsUBO) == 32);

} // namespace shaders
} // namespace mln
