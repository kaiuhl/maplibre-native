#include <mln/renderer/layers/terrain_layer_tweaker.hpp>

#include <mln/gfx/context.hpp>
#include <mln/gfx/drawable.hpp>
#include <mln/renderer/layer_group.hpp>
#include <mln/renderer/layer_tweaker.hpp>
#include <mln/renderer/paint_parameters.hpp>
#include <mln/renderer/render_terrain.hpp>
#include <mln/shaders/terrain_layer_ubo.hpp>
#include <mln/math/angles.hpp>
#include <mln/shaders/shader_defines.hpp>
#include <mln/util/constants.hpp>
#include <mln/util/convert.hpp>
#include <mln/util/mat4.hpp>
#include <mln/util/logging.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace mln {

using namespace shaders;

void TerrainLayerTweaker::execute(LayerGroupBase& layerGroup, const PaintParameters& parameters) {
    if (layerGroup.empty() || !terrain) {
        return;
    }

    auto& context = parameters.context;

#if defined(DEBUG)
    const auto label = layerGroup.getName() + "-update-uniforms";
    const auto debugGroup = parameters.encoder->createDebugGroup(label.c_str());
#endif

    // Get terrain properties; per the style spec, exaggeration is applied as-is
    // (default 1.0 renders true-scale elevation)
    const float exaggeration = terrain->getExaggeration();
    const style::Terrain::Wave wave = terrain->getImpl()->wave;

    // Skirt depth (u_ele_delta): the shader drops the mesh's skirt vertices by
    // this many metres into a curtain that hides the cracks between neighbouring
    // tiles at different zoom levels. ~1/5 of the tile's world width at this zoom,
    // matching maplibre-gl-js Terrain.getSkirtLength().
    const auto zoom = std::max(parameters.state.getZoom(), 0.0);
    const float elevationOffset = static_cast<float>(util::M2PI * util::EARTH_RADIUS_M / std::pow(2.0, zoom) / 5.0);

    // Populate layer-level UBO with terrain properties
    auto& layerUniforms = layerGroup.mutableUniformBuffers();
    const TerrainEvaluatedPropsUBO propsUBO = {.unpack = terrain->getDEMUnpackVector(),
                                               .exaggeration = exaggeration,
                                               .elevation_offset = elevationOffset,
                                               .wave_peak = wave.peak,
                                               .wave_crest = wave.crest};
    layerUniforms.createOrUpdate(idTerrainEvaluatedPropsUBO, &propsUBO, context);

    // The wave's centre in world tiles at z0 (Web Mercator), and how many z0 world tiles a
    // metre of ground spans there: 1 / (2 pi R cos(lat)), mercator stretching ground by 1/cos.
    const double waveLat = util::deg2rad(wave.latitude);
    const double waveX = (wave.longitude + 180.0) / 360.0;
    const double waveY = (1.0 - std::log(std::tan(waveLat) + 1.0 / std::cos(waveLat)) / std::numbers::pi) / 2.0;
    const double worldPerMetre = 1.0 / (util::M2PI * util::EARTH_RADIUS_M * std::cos(waveLat));
    // This tile's wave: its centre, front and width in the tile's own units (0..EXTENT).
    const auto tileWave = [&](const UnwrappedTileID& tile) -> std::array<float, 4> {
        if (wave.width <= 0.0f) {
            return {0, 0, 0, 0};
        }
        const double scale = std::pow(2.0, tile.canonical.z);
        const double units = util::EXTENT * scale;
        const double x = (waveX - (tile.canonical.x + tile.wrap * scale) / scale) * units;
        const double y = (waveY - tile.canonical.y / scale) * units;
        const double perMetre = worldPerMetre * units;
        return {static_cast<float>(x),
                static_cast<float>(y),
                static_cast<float>(wave.front * perMetre),
                static_cast<float>(wave.width * perMetre)};
    };

#if MLN_UBO_CONSOLIDATION
    int i = 0;
    std::vector<TerrainDrawableUBO> drawableUBOVector(layerGroup.getDrawableCount());
#endif

    // Visit each drawable to populate per-drawable UBOs
    visitLayerGroupDrawables(layerGroup, [&](gfx::Drawable& drawable) {
        if (!drawable.getTileID()) {
            return;
        }

        const UnwrappedTileID tileID = drawable.getTileID()->toUnwrapped();

        // Calculate transformation matrix for this terrain tile
        // This uses the same matrix calculation as other layers. The metres->world
        // pixels elevation scale (pixelsPerMeter) is baked into the projection matrix
        // in TransformState::getProjMatrix, so it applies here and to the elevated
        // symbol / circle layers consistently, matching maplibre-gl-js.
        mat4 matrix = parameters.matrixForTile(tileID);

        // matrixForTile builds a GL-convention projection (clip z in [-1, 1]); Vulkan,
        // Metal and WebGPU clip to [0, 1]. Remap clip z so terrain that projects into the
        // near half of the GL clip volume (z < 0) is not clipped away on those backends.
        // Matches LayerTweaker::getTileMatrix and clipMatrixForTile, which remap the draped /
        // RTT matrices for the same reason, and the fill-extrusion tweaker, whose walls
        // depth-test against this surface. Monotonic, so skirt-vs-surface depth ordering and
        // the packed depth texture used for symbol occlusion are preserved; GL unchanged.
        LayerTweaker::remapClipDepth(matrix);

#if !MLN_UBO_CONSOLIDATION
        auto& drawableUniforms = drawable.mutableUniformBuffers();
#endif

#if MLN_UBO_CONSOLIDATION
        drawableUBOVector[i] = {
#else
        const TerrainDrawableUBO drawableUBO = {
#endif
            .matrix = util::cast<float>(matrix),
            .dem_coords = terrain->getDrawableDemCoords(*drawable.getTileID()),
            .wave = tileWave(tileID)
        };

#if !MLN_UBO_CONSOLIDATION
        drawableUniforms.createOrUpdate(idTerrainDrawableUBO, &drawableUBO, context);
#endif

#if MLN_UBO_CONSOLIDATION
        drawable.setUBOIndex(i++);
#endif
    });

#if MLN_UBO_CONSOLIDATION
    const size_t drawableUBOVectorSize = sizeof(TerrainDrawableUBO) * drawableUBOVector.size();
    if (!drawableUniformBuffer || drawableUniformBuffer->getSize() < drawableUBOVectorSize) {
        drawableUniformBuffer = context.createUniformBuffer(
            drawableUBOVector.data(), drawableUBOVectorSize, false, true);
    } else {
        drawableUniformBuffer->update(drawableUBOVector.data(), drawableUBOVectorSize);
    }

    layerUniforms.set(idTerrainDrawableUBO, drawableUniformBuffer);
#endif
}

} // namespace mln
