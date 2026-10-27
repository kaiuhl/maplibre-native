#pragma once

#include <mln/annotation/annotation_manager.hpp>
#include <mln/map/map.hpp>
#include <mln/map/map_observer.hpp>
#include <mln/map/map_options.hpp>
#include <mln/map/mode.hpp>
#include <mln/map/transform.hpp>
#include <mln/renderer/renderer_frontend.hpp>
#include <mln/renderer/renderer_observer.hpp>
#include <mln/style/observer.hpp>
#include <mln/style/source.hpp>
#include <mln/style/style.hpp>
#include <mln/util/size.hpp>
#include <mln/tile/tile_operation.hpp>

#include <limits>
#include <numbers>

namespace mln {

class FileSource;
class ResourceTransform;

namespace gfx {
class ShaderRegistry;
} // namespace gfx

namespace util {
class ActionJournal;
} // namespace util

struct StillImageRequest {
    StillImageRequest(Map::StillImageCallback&& callback_)
        : callback(std::move(callback_)) {}

    Map::StillImageCallback callback;
};

class Map::Impl final : public TransformObserver, public style::Observer, public RendererObserver {
public:
    Impl(RendererFrontend&, MapObserver&, std::shared_ptr<FileSource>, const MapOptions&);
    ~Impl() final;

    // TransformObserver
    void onCameraWillChange(MapObserver::CameraChangeMode) final;
    void onCameraIsChanging() final;
    void onCameraDidChange(MapObserver::CameraChangeMode) final;

    // StyleObserver
    void onSourceChanged(style::Source&) final;
    void onUpdate() final;
    void onStyleLoading() final;
    void onStyleLoaded() final;
    void onStyleError(std::exception_ptr) final;
    void onSpriteLoaded(const std::optional<style::Sprite>&) final;
    void onSpriteError(const std::optional<style::Sprite>&, std::exception_ptr) final;
    void onSpriteRequested(const std::optional<style::Sprite>&) final;

    // RendererObserver
    void onInvalidate() final;
    void onResourceError(std::exception_ptr) final;
    void onWillStartRenderingFrame() final;
    void onDidFinishRenderingFrame(RenderMode, bool, bool, std::shared_ptr<gfx::RenderingStats>) final;
    void onWillStartRenderingMap() final;
    void onDidFinishRenderingMap() final;
    void onTerrainCenterElevationChanged(std::optional<double> elevationMeters) final;
    void onTerrainElevationIndexChanged(std::shared_ptr<const TerrainElevationIndex>) final;
    void onStyleImageMissing(const std::string&, const std::function<void()>&) final;
    void onRemoveUnusedStyleImages(const std::vector<std::string>&) final;
    void onRegisterShaders(gfx::ShaderRegistry&) final;

    void onPreCompileShader(shaders::BuiltIn, gfx::Backend::Type, const std::string&) final;
    void onPostCompileShader(shaders::BuiltIn, gfx::Backend::Type, const std::string&) final;
    void onShaderCompileFailed(shaders::BuiltIn, gfx::Backend::Type, const std::string&) final;
    void onGlyphsLoaded(const FontStack&, const GlyphRange&) final;
    void onGlyphsError(const FontStack&, const GlyphRange&, std::exception_ptr) final;
    void onGlyphsRequested(const FontStack&, const GlyphRange&) final;
    void onTileAction(TileOperation op, const OverscaledTileID&, const std::string&) final;
    void onRenderError(std::exception_ptr) final;
    void onSymbolError(const std::string&) final;

    // Map
    void jumpTo(const CameraOptions&);

    bool isRenderingStatsViewEnabled() const;
    void enableRenderingStatsView(bool value);

    MapObserver& observer;
    RendererFrontend& rendererFrontend;
    std::unique_ptr<util::ActionJournal> actionJournal;

    Transform transform;

    const MapMode mode;
    const float pixelRatio;
    const bool crossSourceCollisions;
    const bool fastPFOREnabled;
    const bool captureRenderedFeatures;

    MapDebugOptions debugOptions{MapDebugOptions::NoDebug};
    std::unique_ptr<gfx::RenderingStatsView> renderingStatsView;
    std::shared_ptr<gfx::RenderingStats> lastFrameStats;

    std::shared_ptr<FileSource> fileSource;

    std::unique_ptr<style::Style> style;
    AnnotationManager annotationManager;

    bool cameraMutated = false;

    uint8_t prefetchZoomDelta = util::DEFAULT_PREFETCH_ZOOM_DELTA;

    bool loading = false;
    bool rendererFullyLoaded;
    std::unique_ptr<StillImageRequest> stillImageRequest;

    double tileLodMinRadius = 3;
    double tileLodScale = 1;
    double tileLodPitchThreshold = (60.0 / 180.0) * std::numbers::pi;
    double tileLodZoomShift = 0;
    TileLodMode tileLodMode = TileLodMode::Default;
    TerrainLoadMode terrainLoadMode = TerrainLoadMode::Quality;
    TerrainSkirtLength terrainSkirtLength = TerrainSkirtLength::Auto;
    /// Whether the clamp puts the centre on the terrain (Map::setCenterClampedToGround); off by
    /// default. It acts on the heights the renderer reports a frame behind, once no transition or
    /// gesture moves the camera, rather than during render setup as GL JS's recalculateZoomAndCenter
    /// does.
    bool centerClampedToGround = false;
    /// Terrain height under the centre, as the renderer last reported it: NaN until then, and while
    /// no DEM tile covers the centre, where the ground is unknown rather than at sea level.
    double terrainCenterElevation = std::numeric_limits<double>::quiet_NaN();
    void anchorCenterOnTerrain();
    /// Slides the centre along the line of sight to `altitudeMeters`, the zoom following, so the
    /// camera stays where it is. False, changing nothing, when there was nothing to move, the line
    /// of sight nearly grazes the plane at that altitude, the plane is (nearly) at the camera, or
    /// the zoom limits, the bounds or the constrain mode would cut the move short. With
    /// `stopAtZoomLimits`, a move past the zoom limits goes as far as they allow instead, the centre
    /// resting above or below that altitude by the rest.
    bool setCenterAltitudeKeepingView(double altitudeMeters, bool stopAtZoomLimits = false);
    /// Whether the style had terrain when last checked, and whether the centre has yet to come
    /// back to sea level since it was removed (a transition was in flight, or a still map has
    /// not rendered since).
    bool hadTerrain = false;
    bool centerAwaitingSeaLevel = false;
    void noteTerrainRemoval();
    /// Brings the centre down to sea level keeping the view or, where the limits refuse that,
    /// keeping the centre and zoom.
    void returnCenterToSeaLevel();
    /// Runs a return still waiting before the app sets a camera, so that the return does not move
    /// the app's camera afterwards.
    void settleCenterAwaitingSeaLevel();
    /// What every camera entry point does before it sets the camera: marks the camera as the app's
    /// (the style's default no longer applies), runs a return to sea level still waiting, and
    /// records whether the camera is being set to a centre (cameraSetToCenter).
    void beginCameraChange(bool toCenter);
    /// The ground changed while a transition or a gesture moved the camera; the clamp puts the
    /// centre on it once neither does (onUpdate).
    bool centerAwaitingTerrainAnchor = false;
    /// The clamp, keeping the camera, found no ground on the line of sight in the map-side heights;
    /// it tries again when they change (onTerrainElevationIndexChanged).
    bool centerAwaitingHeights = false;
    /// Whether the camera was last set to a centre (easeTo, flyTo or jumpTo with one) and not cut
    /// short of it, rather than moved relative to the view (moveBy, rotateBy, a zoom or turn around
    /// an anchor: a gesture's deceleration), cancelled, or set as a camera (setFreeCameraOptions,
    /// setCenterAltitudeKeepingView, a jump with no centre). Once a transition ends, the clamp keeps
    /// a centre it was given and otherwise keeps the camera (onUpdate).
    bool cameraSetToCenter = false;
    /// The ground under the centre as it is now: the map-side heights where they have it, else the
    /// height the renderer last reported.
    double groundUnderCenter() const;
    /// With the clamp on, has a running transition given a centre arrive at the ground there, from
    /// the map-side heights as they are now (Transform::retargetTransitionCenterAltitude).
    void easeTransitionOntoGround();
    /// The map-side height of the ground at `latLng`; nullopt where they have none.
    std::optional<double> groundAt(LatLng latLng) const;
    /// Height of the ground the line of sight from the camera through the centre meets first,
    /// from the map-side heights: where setCenterAltitudeKeepingView puts the centre on the
    /// terrain. Nullopt without heights there, or with the camera inside the terrain.
    std::optional<double> groundOnLineOfSight() const;
    /// Puts the centre on the ground once nothing is moving the camera: keeping a centre the camera
    /// was set to, and otherwise the camera. True when it moved anything.
    bool settleCenterOnGround();
    /// settleCenterOnGround now, or once no transition or gesture moves the camera.
    void settleCenterOnGroundWhenStill();
    /// Moves the centre altitude to `elevationMeters` keeping lng/lat, easing it there
    /// (Transform::easeCenterAltitudeTo) where it was held over unknown ground; false when the change is under the
    /// sub-metre threshold, or where `elevationMeters` is NaN: the ground is unknown, and the centre
    /// is held where it is.
    bool clampCenterToTerrain(double elevationMeters);
    /// The clamp found the ground under the centre unknown when it would have put the centre on it,
    /// and left the centre at the altitude it had; it eases the centre onto the ground once that
    /// arrives, rather than jumping the view there.
    bool centerHeldOverUnknownGround = false;
    /// The renderer's terrain heights, copied for this side (onTerrainElevationIndexChanged); null
    /// without terrain. Map's terrain projection reads it so it never waits on the renderer.
    std::shared_ptr<const TerrainElevationIndex> terrainElevationIndex;
    bool debugAboveGroundLog = false;
};

// Forward declaration of this method is required for the MapProjection class
CameraOptions cameraForLatLngs(const std::vector<LatLng>& latLngs,
                               const Transform& transform,
                               const EdgeInsets& padding);

} // namespace mln
