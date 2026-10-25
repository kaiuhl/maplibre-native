#include <mln/style/layers/custom_layer_render_parameters.hpp>
#include <mln/renderer/paint_parameters.hpp>
#include <mln/renderer/render_terrain.hpp>
#include <mln/util/mat4.hpp>

namespace mln {
namespace style {

CustomLayerRenderParameters::CustomLayerRenderParameters(const mln::PaintParameters& paintParameters) {
    const TransformState& state = paintParameters.state;
    width = state.getSize().width;
    height = state.getSize().height;
    latitude = state.getLatLng().latitude();
    longitude = state.getLatLng().longitude();
    zoom = state.getZoom();
    bearing = util::rad2deg(-state.getBearing());
    pitch = state.getPitch();
    fieldOfView = state.getFieldOfView();
    mat4 projMatrix;
    state.getProjMatrix(projMatrix);
    projectionMatrix = projMatrix;
    nearClippedProjectionMatrix = paintParameters.transformParams.nearClippedProjMatrix;

    const RenderTerrain* renderTerrain = paintParameters.terrain;
    if (!renderTerrain || !renderTerrain->isEnabled()) {
        return;
    }
    CustomLayerTerrain& drawn = terrain.emplace();
    drawn.exaggeration = renderTerrain->getExaggeration();
    drawn.unpack = renderTerrain->getDEMUnpackVector();
    drawn.meshSize = static_cast<uint32_t>(RenderTerrain::MESH_SIZE);
    for (const auto& tile : renderTerrain->getDrawnTiles()) {
        const UnwrappedTileID unwrapped = tile.id.toUnwrapped();
        drawn.tiles.push_back({unwrapped.canonical.z,
                               unwrapped.canonical.x,
                               unwrapped.canonical.y,
                               unwrapped.wrap,
                               paintParameters.matrixForTile(unwrapped),
                               tile.demCoords,
                               tile.demTexture});
    }
}

} // namespace style
} // namespace mln
