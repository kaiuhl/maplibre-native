#include <mln/layermanager/layer_manager.hpp>
#include <mln/map/map_impl.hpp>
#include <mln/math/wrap.hpp>
#include <mln/renderer/terrain_elevation_index.hpp>
#include <mln/renderer/update_parameters.hpp>
#include <mln/storage/file_source.hpp>
#include <mln/style/style_impl.hpp>
#include <mln/util/exception.hpp>
#include <mln/util/logging.hpp>
#include <mln/util/projection.hpp>
#include <mln/util/traits.hpp>
#include <mln/util/action_journal.hpp>
#include <mln/util/action_journal_impl.hpp>
#include <mln/gfx/rendering_stats.hpp>
#include <mln/util/unitbezier.hpp>

#include <chrono>
#include <cmath>
#include <utility>

namespace mln {

namespace {
// How long a centre held over unknown ground takes to ease onto the ground once its heights arrive.
constexpr auto groundEaseDuration = std::chrono::milliseconds(400);
} // namespace

#if !defined(NDEBUG)
namespace {
void logStyleDependencies(EventSeverity severity, Event event, const style::Style& style) {
    using Dependency = style::expression::Dependency;
    constexpr auto maskCount = underlying_type(Dependency::MaskCount);
    std::array<std::size_t, maskCount + 1> counts = {0};
    const auto layers = style.getLayers();
    for (const auto& layer : layers) {
        const auto deps = layer->getDependencies();
        if (deps == Dependency::None) {
            counts[0]++;
        } else {
            for (size_t i = 0; i < maskCount; ++i) {
                if (deps & Dependency{1u << i}) {
                    counts[i + 1]++;
                }
            }
        }
    }
    std::ostringstream ss;
    ss << "Style '" << style.getName() << "' has " << layers.size() << " layers:\n";
    ss << "  " << Dependency::None << ": " << counts[0] << "\n";
    for (size_t i = 0; i < maskCount; ++i) {
        if (counts[i + 1]) {
            ss << "  " << Dependency{1u << i} << ": " << counts[i + 1] << "\n";
        }
    }
    Log::Record(severity, event, ss.str());
}
} // namespace
#endif

Map::Impl::Impl(RendererFrontend& frontend_,
                MapObserver& observer_,
                std::shared_ptr<FileSource> fileSource_,
                const MapOptions& mapOptions)
    : observer(observer_),
      rendererFrontend(frontend_),
      transform(*this, mapOptions.constrainMode(), mapOptions.viewportMode()),
      mode(mapOptions.mapMode()),
      pixelRatio(mapOptions.pixelRatio()),
      crossSourceCollisions(mapOptions.crossSourceCollisions()),
      fastPFOREnabled(mapOptions.fastPFOREnabled()),
      captureRenderedFeatures(mapOptions.renderedFeatureInfo()),
      fileSource(std::move(fileSource_)),
      style(std::make_unique<style::Style>(fileSource, pixelRatio, frontend_.getThreadPool())),
      annotationManager(*style) {
    transform.setNorthOrientation(mapOptions.northOrientation());
    style->impl->setObserver(this);
    rendererFrontend.setObserver(*this);
    transform.resize(mapOptions.size());
}

Map::Impl::~Impl() {
    // Explicitly reset the RendererFrontend first to ensure it releases
    // All shared resources (AnnotationManager)
    rendererFrontend.reset();
};

void Map::Impl::onCameraWillChange(MapObserver::CameraChangeMode cameraMode) {
    observer.onCameraWillChange(cameraMode);

    if (actionJournal) {
        actionJournal->impl->onCameraWillChange(cameraMode);
    }
}

void Map::Impl::onCameraIsChanging() {
    observer.onCameraIsChanging();

    if (actionJournal) {
        actionJournal->impl->onCameraIsChanging();
    }
}

void Map::Impl::onCameraDidChange(MapObserver::CameraChangeMode cameraMode) {
    observer.onCameraDidChange(cameraMode);

    if (actionJournal) {
        actionJournal->impl->onCameraDidChange(cameraMode);
    }
}

// MARK: - Map::Impl StyleObserver

void Map::Impl::onSourceChanged(style::Source& source) {
    observer.onSourceChanged(source);

    if (actionJournal) {
        actionJournal->impl->onSourceChanged(source);
    }
}

void Map::Impl::onUpdate() {
    // Ahead of the still-mode return below: a camera the app sets before the next still render
    // has to know that the terrain went (see settleCenterAwaitingSeaLevel).
    noteTerrainRemoval();

    // Don't load/render anything in still mode until explicitly requested.
    if (mode != MapMode::Continuous && !stillImageRequest) {
        return;
    }

    TimePoint timePoint = mode == MapMode::Continuous ? Clock::now() : Clock::time_point::max();

    // Before the frame, so that it lands on the latest ground.
    easeTransitionOntoGround();
    transform.updateTransitions(timePoint);

    // Terrain removed: a centre that gestures anchored on it (or the clamp held there) would stay
    // up to kilometres above the now flat map, and pans, tile cover and label placement would
    // all work on that raised plane. Bring it back to sea level, keeping the view - once no
    // transition is running, since one in flight re-applies the altitude it started with.
    if (centerAwaitingSeaLevel && !transform.inTransition()) {
        returnCenterToSeaLevel();
    }

    // The clamp stood aside for a transition or a gesture (onTerrainCenterElevationChanged), or is
    // waiting for heights on the line of sight (settleCenterOnGround); now that neither is moving
    // the camera, put the centre on the ground: once, at the end of a drag. Where the ground under
    // the centre is unknown, the clamp holds the centre where the move left it. The reported
    // height outlives a terrain removed during the transition, so only apply it while there is
    // still terrain.
    if (centerAwaitingTerrainAnchor && !transform.inTransition() && !transform.isGestureInProgress()) {
        centerAwaitingTerrainAnchor = false;
        if (centerClampedToGround && style->impl->getTerrain()) {
            settleCenterOnGround();
        }
    }

    keepCameraAboveTerrain();

    std::optional<Immutable<style::Terrain::Impl>> terrainImpl;
    if (auto* terrain = style->impl->getTerrain()) {
        terrainImpl = terrain->impl;
    }

    UpdateParameters params = {.styleLoaded = style->impl->isLoaded(),
                               .mode = mode,
                               .pixelRatio = pixelRatio,
                               .debugOptions = debugOptions,
                               .timePoint = timePoint,
                               .transformState = transform.getState(),
                               .glyphURL = style->impl->getGlyphURL(),
                               .fontFaces = style->impl->getFontFaces(),
                               .spriteLoaded = style->impl->areSpritesLoaded(),
                               .transitionOptions = style->impl->getTransitionOptions(),
                               .light = style->impl->getLight()->impl,
                               .terrain = terrainImpl,
                               .images = style->impl->getImageImpls(),
                               .sources = style->impl->getSourceImpls(),
                               .layers = style->impl->getLayerImpls(),
                               .globalState = style->impl->getGlobalStateShared(),
                               .annotationManager = annotationManager.makeWeakPtr(),
                               .fileSource = fileSource,
                               .prefetchZoomDelta = prefetchZoomDelta,
                               .stillImageRequest = bool(stillImageRequest),
                               .crossSourceCollisions = crossSourceCollisions,
                               .fastPFOREnabled = fastPFOREnabled,
                               .captureRenderedFeatures = captureRenderedFeatures,
                               .tileLodMinRadius = tileLodMinRadius,
                               .tileLodScale = tileLodScale,
                               .tileLodPitchThreshold = tileLodPitchThreshold,
                               .tileLodZoomShift = tileLodZoomShift,
                               .tileLodMode = tileLodMode,
                               .terrainLoadMode = terrainLoadMode,
                               .terrainSkirtLength = terrainSkirtLength,
                               .debugAboveGroundLog = debugAboveGroundLog};

    rendererFrontend.update(std::make_shared<UpdateParameters>(std::move(params)));
}

void Map::Impl::onStyleLoading() {
    loading = true;
    rendererFullyLoaded = false;
    observer.onWillStartLoadingMap();

    if (actionJournal) {
        actionJournal->impl->onWillStartLoadingMap();
    }
}

void Map::Impl::onStyleLoaded() {
    if (!cameraMutated) {
        jumpTo(style->getDefaultCamera());
    }
    if (LayerManager::annotationsEnabled) {
        annotationManager.onStyleLoaded();
    }

    observer.onDidFinishLoadingStyle();

    if (actionJournal) {
        actionJournal->impl->onDidFinishLoadingStyle();
    }

#if !defined(NDEBUG)
    logStyleDependencies(EventSeverity::Info, Event::Style, *style);
#endif
}

void Map::Impl::onStyleError(std::exception_ptr error) {
    MapLoadError type;
    std::string description;

    try {
        std::rethrow_exception(error);
    } catch (const mln::util::StyleParseException& e) {
        type = MapLoadError::StyleParseError;
        description = e.what();
    } catch (const mln::util::StyleLoadException& e) {
        type = MapLoadError::StyleLoadError;
        description = e.what();
    } catch (const mln::util::NotFoundException& e) {
        type = MapLoadError::NotFoundError;
        description = e.what();
    } catch (const std::exception& e) {
        type = MapLoadError::UnknownError;
        description = e.what();
    }

    observer.onDidFailLoadingMap(type, description);

    if (actionJournal) {
        actionJournal->impl->onDidFailLoadingMap(type, description);
    }
}

void Map::Impl::onSpriteLoaded(const std::optional<style::Sprite>& sprite) {
    observer.onSpriteLoaded(sprite);

    if (actionJournal) {
        actionJournal->impl->onSpriteLoaded(sprite);
    }
}

void Map::Impl::onSpriteError(const std::optional<style::Sprite>& sprite, std::exception_ptr ex) {
    observer.onSpriteError(sprite, ex);

    if (actionJournal) {
        actionJournal->impl->onSpriteError(sprite, ex);
    }
}

void Map::Impl::onSpriteRequested(const std::optional<style::Sprite>& sprite) {
    observer.onSpriteRequested(sprite);

    if (actionJournal) {
        actionJournal->impl->onSpriteRequested(sprite);
    }
}

// MARK: - Map::Impl RendererObserver

void Map::Impl::onInvalidate() {
    onUpdate();
}

void Map::Impl::onResourceError(std::exception_ptr error) {
    if (mode != MapMode::Continuous && stillImageRequest) {
        auto request = std::move(stillImageRequest);
        request->callback(error);
    }
}

void Map::Impl::onWillStartRenderingFrame() {
    if (mode == MapMode::Continuous) {
        observer.onWillStartRenderingFrame();

        if (actionJournal) {
            actionJournal->impl->onWillStartRenderingFrame();
        }
    }
}

void Map::Impl::onDidFinishRenderingFrame(RenderMode renderMode,
                                          bool needsRepaint,
                                          bool placemenChanged,
                                          std::shared_ptr<gfx::RenderingStats> stats) {
    rendererFullyLoaded = renderMode == RenderMode::Full;

    if (renderingStatsView && style) {
        renderingStatsView->update(*style, *stats);
    }

    if (mode == MapMode::Continuous) {
        const MapObserver::RenderFrameStatus frameStatus{.mode = static_cast<MapObserver::RenderMode>(renderMode),
                                                         .needsRepaint = needsRepaint,
                                                         .placementChanged = placemenChanged,
                                                         .renderingStats = stats};
        observer.onDidFinishRenderingFrame(frameStatus);

        if (actionJournal) {
            actionJournal->impl->onDidFinishRenderingFrame(frameStatus);
        }

        if (needsRepaint || transform.inTransition()) {
            onUpdate();
        } else if (rendererFullyLoaded) {
            observer.onDidBecomeIdle();

            if (actionJournal) {
                actionJournal->impl->onDidBecomeIdle();
            }
        }
    } else if (stillImageRequest && rendererFullyLoaded) {
        const auto request = std::move(stillImageRequest);
        request->callback(nullptr);
    }

    lastFrameStats = std::move(stats);
}

void Map::Impl::onWillStartRenderingMap() {
    if (mode == MapMode::Continuous) {
        observer.onWillStartRenderingMap();

        if (actionJournal) {
            actionJournal->impl->onWillStartRenderingMap();
        }
    }
}

void Map::Impl::onDidFinishRenderingMap() {
    if (mode == MapMode::Continuous && loading) {
        observer.onDidFinishRenderingMap(MapObserver::RenderMode::Full);

        if (actionJournal) {
            actionJournal->impl->onDidFinishRenderingMap(MapObserver::RenderMode::Full);
        }

        if (loading) {
            loading = false;
            observer.onDidFinishLoadingMap();

            if (actionJournal) {
                actionJournal->impl->onDidFinishLoadingMap();
            }
        }
    }
};

void Map::Impl::anchorCenterOnTerrain() {
    // The last reported height outlives a removed terrain; the style says whether there is one.
    if (!style->impl->getTerrain() || std::isnan(terrainCenterElevation)) {
        return;
    }
    // The ground the line of sight meets, where the map-side heights have it, rather than the
    // height reported under the centre: the centre moves off that spot onto the line of sight, and
    // the clamp would then move the camera by the difference.
    if (setCenterAltitudeKeepingView(groundOnLineOfSight().value_or(terrainCenterElevation))) {
        onUpdate();
    }
}

bool Map::Impl::setCenterAltitudeKeepingView(double altitudeMeters) {
    const TransformState& state = transform.getState();
    const double rise = altitudeMeters - state.getCenterAltitude();
    if (std::abs(rise) < 1.0) {
        return false;
    }
    const CameraOptions camera = transform.getCameraOptions(std::nullopt);
    if (!camera.center || !camera.zoom) {
        return false;
    }
    const double pitch = util::deg2rad(camera.pitch.value_or(0.0));
    // Near grazing, the line of sight runs almost along the plane: the centre would slide off
    // towards the horizon, past a pole close to 90 degrees. gl-js stops at the same angle
    // (_distanceToCenterFromAltElevationPitch).
    if (std::cos(pitch) < 0.1) {
        return false;
    }
    const double bearing = util::deg2rad(camera.bearing.value_or(0.0));
    // In pixels at the current zoom, the unit of the camera-to-centre distance along the line
    // of sight.
    const double metresPerPixel = Projection::getMetersPerPixelAtLatitude(camera.center->latitude(), *camera.zoom);
    const double risePixels = rise / metresPerPixel;
    const double distance = state.getCameraToCenterDistance();
    // Up the line of sight by `rise` (down, for a negative one): rise/cos(pitch) closer to the
    // camera, rise·tan(pitch) nearer it on the ground.
    const double remaining = distance - risePixels / std::cos(pitch);
    if (remaining < distance * 0.05) {
        return false; // the ground is (nearly) at the camera: leave it
    }
    const double shift = risePixels * std::tan(pitch);
    // Towards the camera, which sits opposite the bearing; y grows southwards.
    const double scale = state.getScale();
    const Point<double> center = Projection::project(*camera.center, scale);
    const LatLng anchored = Projection::unproject(
        {center.x - shift * std::sin(bearing), center.y + shift * std::cos(bearing)}, scale);
    const double zoom = *camera.zoom + std::log2(distance / remaining);
    const auto keepingView = CameraOptions().withCenter(anchored).withZoom(zoom).withCenterAltitude(altitudeMeters);

    // The zoom limits, the bounds and the constrain mode cut off whatever part of a move they
    // don't allow, and a move cut short shifts the view (gl-js's moveCenterOntoTerrain undoes one
    // that passes maxZoom). Try it on a copy, and leave the camera alone unless all of it goes
    // through.
    Transform trial(state);
    trial.jumpTo(keepingView);
    const LatLng reached = trial.getLatLng();
    if (std::abs(trial.getZoom() - zoom) > 1e-9 || std::abs(reached.latitude() - anchored.latitude()) > 1e-9 ||
        std::abs(util::wrap(reached.longitude() - anchored.longitude(), -180.0, 180.0)) > 1e-9) {
        return false;
    }
    transform.jumpTo(keepingView);
    return true;
}

void Map::Impl::keepCameraAboveTerrain() {
    // A transition sets the camera every frame; moving it here would cancel the transition.
    if (!terrainElevationIndex || !style->impl->getTerrain() || transform.inTransition()) {
        return;
    }
    const TransformState& state = transform.getState();
    const auto eye = state.getFreeCameraOptions().getLocation();
    if (!eye) {
        return;
    }
    const auto ground = terrainElevationIndex->getElevation(eye->location);
    if (!ground || eye->altitude >= *ground) {
        return;
    }
    // From the eye put back on the ground, at the same place, to the same centre: the pitch is
    // the angle of the line between them off the vertical, and the zoom is the one that puts
    // the centre that far along it (gl-js's calculateCameraOptionsFromTo).
    //
    // In world units, as the transform places the eye. A height in metres is a different number
    // of them at the eye's latitude than at the centre's, kilometres away, and the eye's altitude
    // is read at its own (FreeCameraOptions::getLocation): worked out in metres at the centre, the
    // lift left an eye north of the centre short of the ground (1.6 m in the app), and lifted it
    // again on every frame after without getting it there. A millimetre over the ground, so that
    // rounding can't leave it under.
    const double worldSize = Projection::worldSize(state.getScale());
    const double cameraToCenter = state.getCameraToCenterDistance();
    const double across = cameraToCenter * std::sin(state.getPitch()) / worldSize;
    const double metresPerUnit = Projection::getMetersPerPixelAtLatitude(eye->location.latitude(), 0.0) *
                                 Projection::worldSize(1.0);
    const double rise = (*ground + 0.001) / metresPerUnit - state.getZ() / worldSize;
    if (rise <= 0.0) {
        return; // looking up at the centre, past what a pitch below 90 degrees can show
    }
    const double pitch = util::rad2deg(std::atan2(across, rise));
    const double zoom = std::log2(cameraToCenter / (std::hypot(across, rise) * util::tileSize_D));
    transform.jumpTo(CameraOptions().withPitch(pitch).withZoom(zoom));
}

void Map::Impl::returnCenterToSeaLevel() {
    centerAwaitingSeaLevel = false;
    centerHeldOverUnknownGround = false;
    // Where the limits refuse the move that keeps the view, keep the centre and zoom instead, as
    // gl-js does (applyTerrainChange), rather than leave the centre up in the air.
    if (!setCenterAltitudeKeepingView(0.0) && transform.getState().getCenterAltitude() != 0.0) {
        transform.jumpTo(CameraOptions().withCenterAltitude(0.0));
    }
}

void Map::Impl::noteTerrainRemoval() {
    const bool hasTerrain = style->impl->getTerrain() != nullptr;
    if (hadTerrain && !hasTerrain) {
        centerAwaitingSeaLevel = true;
    } else if (hasTerrain) {
        centerAwaitingSeaLevel = false;
    }
    hadTerrain = hasTerrain;
}

void Map::Impl::settleCenterAwaitingSeaLevel() {
    // A return still waiting, for a transition or for a still render, would move the camera the
    // app is about to set instead of the one it replaces. The transition would give way to that
    // camera anyway.
    noteTerrainRemoval();
    if (centerAwaitingSeaLevel) {
        transform.cancelTransitions();
        returnCenterToSeaLevel();
    }
}

void Map::Impl::onTerrainElevationIndexChanged(std::shared_ptr<const TerrainElevationIndex> index) {
    terrainElevationIndex = std::move(index);
}

void Map::Impl::onTerrainCenterElevationChanged(std::optional<double> elevationMeters) {
    // Unknown ground (no DEM tile covers the centre yet) is not sea level: the clamp holds the
    // centre where it is over it (clampCenterToTerrain).
    terrainCenterElevation = elevationMeters.value_or(std::numeric_limits<double>::quiet_NaN());
    if (!centerClampedToGround) {
        return;
    }
    // A running transition (an animated recenter, a fling) owns the camera: the jump below
    // would cancel it, and across relief the height changes every frame, so the move stopped
    // within a few kilometres of where it started. Let it finish; onUpdate applies the latest
    // height once it has.
    //
    // A gesture moves the camera too, a frame at a time without a transition. Under a drag across
    // relief the height changes every frame, and the camera rose and fell with it; kept instead,
    // the zoom would change every frame. As gl-js does (HandlerManager's _terrainMovement), the
    // centre stays at the height the gesture began on, the camera moving level, and is put on the
    // ground keeping the camera once the gesture ends.
    if (transform.inTransition() || transform.isGestureInProgress()) {
        centerAwaitingTerrainAnchor = true;
        return;
    }
    if (settleCenterOnGround()) {
        onUpdate();
    }
}

bool Map::Impl::settleCenterOnGround() {
    // A centre the camera was set to (a jump, or an animated move run to its end) is put on the
    // ground under it, keeping it and the zoom: the camera moves onto that ground.
    if (transitionNamedCenter) {
        return clampCenterToTerrain(groundUnderCenter());
    }
    // A move relative to the view (a fling, a drag, a pinch, an animation cut short) has no centre
    // to keep, and the same clamp would lift or drop the camera by however much the ground under
    // the centre changed since the move began, or since a finer DEM tile landed: the whole view
    // jumped as it came to rest. The camera stays where it is, and the centre moves along the line
    // of sight onto the ground it meets, the zoom following, as gl-js's recalculateZoomAndCenter
    // does at the end of a terrain gesture.
    const double before = transform.getState().getCenterAltitude();
    if (const auto ground = groundOnLineOfSight()) {
        // The camera stays, so there is nothing to ease for a centre that was held.
        centerHeldOverUnknownGround = false;
        anchorCenterOnLineOfSight(*ground);
        return transform.getState().getCenterAltitude() != before;
    }
    // No ground on the line of sight in the map-side heights: they haven't loaded there yet, or the
    // camera is inside the terrain. The clamp lifts a camera that is below the ground under the
    // centre (from the renderer, which also reads the DEM tiles it only has cached); otherwise
    // the camera stays put until the heights arrive.
    const auto eye = transform.getState().getFreeCameraOptions().getLocation();
    if (!eye || eye->altitude <= terrainCenterElevation) {
        return clampCenterToTerrain(groundUnderCenter());
    }
    centerAwaitingTerrainAnchor = true;
    return false;
}

void Map::Impl::easeTransitionOntoGround() {
    // An animated move to a centre was given the altitude of the ground there as far as anyone
    // knew when it started, the current one where that hadn't loaded. It eases the centre onto
    // the ground there as the heights for it arrive, as gl-js's Camera._updateElevation does, so
    // that the clamp has nothing left to move once the move ends: left at the altitude it was
    // given, the camera jumped onto ground that loaded during the move on the frame it ended.
    if (!centerClampedToGround || !transitionNamedCenter || !terrainElevationIndex || !style->impl->getTerrain()) {
        return;
    }
    auto center = transform.getTransitionCenter();
    if (!center) {
        return;
    }
    // Unwrapped for the shortest path from the centre, as groundOnLineOfSight does.
    center->unwrapForShortestPath(transform.getLatLng());
    if (const auto ground = terrainElevationIndex->getElevation(*center)) {
        transform.retargetTransitionCenterAltitude(*ground);
    }
}

std::optional<double> Map::Impl::groundOnLineOfSight() const {
    if (!terrainElevationIndex) {
        return std::nullopt;
    }
    const TerrainElevationIndex& index = *terrainElevationIndex;
    const TransformState& state = transform.getState();
    const auto surface = pickTerrainSurface(
        state,
        state.getEdgeInsets().getCenter(state.getSize().width, state.getSize().height),
        [&](const LatLng& latLng) { return index.getElevation(latLng); },
        index.getMaxElevation());
    if (!surface) {
        return std::nullopt;
    }
    // The pick answers wrapped. Unwrapped for the shortest path to the centre, as
    // Map::pixelForLatLngOnTerrain does: the index keys the DEM tiles by unwrapped x.
    LatLng ground = *surface;
    ground.unwrapForShortestPath(transform.getLatLng());
    return index.getElevation(ground);
}

void Map::Impl::anchorCenterOnLineOfSight(double ground) {
    if (setCenterAltitudeKeepingView(ground)) {
        return;
    }
    // Refused: under a metre (the centre is on that ground), or the zoom the camera's distance to
    // the ground comes to is past the zoom limits (a fling ending uphill at the app's maximum zoom
    // would have to zoom past it). The camera stays all the same, and the centre goes as far along
    // the line of sight as the limit allows, resting above or below the ground by the rest.
    const TransformState& state = transform.getState();
    const double rise = ground - state.getCenterAltitude();
    const double zoom = state.getZoom();
    const double limit = rise > 0.0 ? state.getMaxZoom() : state.getMinZoom();
    // setCenterAltitudeKeepingView's zoom, zoom + log2(d / (d - rise / cos(pitch))) in pixels at
    // this zoom, at the limit; a hair short of it, so that rounding doesn't take the move past it.
    const double metresPerPixel = Projection::getMetersPerPixelAtLatitude(state.getLatLng().latitude(), zoom);
    const double limitRise = state.getCameraToCenterDistance() * std::cos(state.getPitch()) *
                             (1.0 - std::exp2(zoom - limit)) * metresPerPixel * (1.0 - 1e-6);
    const double reachable = rise > 0.0 ? std::min(rise, limitRise) : std::max(rise, limitRise);
    setCenterAltitudeKeepingView(state.getCenterAltitude() + reachable);
}

double Map::Impl::groundUnderCenter() const {
    // The height reported was sampled under the centre of the last frame drawn, which a jump since
    // may have moved anywhere; the map-side heights answer for the centre as it is now.
    if (terrainElevationIndex) {
        if (const auto ground = terrainElevationIndex->getElevation(transform.getLatLng())) {
            return *ground;
        }
    }
    return terrainCenterElevation;
}

bool Map::Impl::clampCenterToTerrain(double elevationMeters) {
    // No DEM tile covers the centre yet (a move that ends over ground whose heights are still on
    // their way): the ground there is unknown. Put at sea level, the whole view dove hundreds of
    // points and jumped back up once the heights arrived, seconds later. The centre stays at the
    // altitude it has instead, and is eased onto the ground when that arrives.
    if (std::isnan(elevationMeters)) {
        centerHeldOverUnknownGround = true;
        return false;
    }
    const bool held = std::exchange(centerHeldOverUnknownGround, false);
    // Sub-metre differences are the terrain cover shifting under a camera that just moved,
    // not the ground actually changing height; acting on them would chase itself.
    constexpr double minimumChangeMeters = 0.5;
    if (std::abs(transform.getState().getCenterAltitude() - elevationMeters) < minimumChangeMeters) {
        return false;
    }
    // Raising the centre onto the terrain moves the orbit plane, not the centre's lng/lat,
    // so this settles rather than feeding back into the next frame's sample.
    if (!held) {
        transform.jumpTo(CameraOptions().withCenterAltitude(elevationMeters));
        return true;
    }
    // A held centre can be far off the ground that arrives (the altitude a move set out from):
    // eased there, from rest to rest, rather than jumped. Given the centre, so that a move with
    // one carries on onto the latest ground as it eases (easeTransitionOntoGround).
    AnimationOptions animation(groundEaseDuration);
    animation.easing.emplace(0.42, 0.0, 0.58, 1.0);
    transform.easeTo(CameraOptions().withCenter(transform.getLatLng()).withCenterAltitude(elevationMeters), animation);
    return true;
}

void Map::Impl::jumpTo(const CameraOptions& camera) {
    cameraMutated = true;
    settleCenterAwaitingSeaLevel();
    // A jump cancels a running transition, and the camera it sets decides what the clamp keeps
    // once heights arrive (onUpdate): a centre given, or else the camera.
    transitionNamedCenter = camera.center.has_value();
    transform.jumpTo(camera);
    onUpdate();
}

bool Map::Impl::isRenderingStatsViewEnabled() const {
    return !!renderingStatsView;
}

void Map::Impl::enableRenderingStatsView(bool value) {
    if (value) {
        if (!renderingStatsView) {
            renderingStatsView = std::make_unique<gfx::RenderingStatsView>();
            if (style) {
                renderingStatsView->create(*style);
            }
        }
    } else {
        if (renderingStatsView) {
            if (style) {
                renderingStatsView->destroy(*style);
            }
            renderingStatsView = nullptr;
        }
    }
}

void Map::Impl::onStyleImageMissing(const std::string& id, const std::function<void()>& done) {
    if (!style->getImage(id)) {
        observer.onStyleImageMissing(id);

        if (actionJournal) {
            actionJournal->impl->onStyleImageMissing(id);
        }
    }

    done();
    onUpdate();
}

void Map::Impl::onRemoveUnusedStyleImages(const std::vector<std::string>& unusedImageIDs) {
    for (const auto& unusedImageID : unusedImageIDs) {
        if (observer.onCanRemoveUnusedStyleImage(unusedImageID)) {
            style->removeImage(unusedImageID);
        }
    }
}

void Map::Impl::onRegisterShaders(gfx::ShaderRegistry& registry) {
    observer.onRegisterShaders(registry);

    if (actionJournal) {
        actionJournal->impl->onRegisterShaders(registry);
    }
}

void Map::Impl::onPreCompileShader(shaders::BuiltIn shaderID,
                                   gfx::Backend::Type type,
                                   const std::string& additionalDefines) {
    observer.onPreCompileShader(shaderID, type, additionalDefines);

    if (actionJournal) {
        actionJournal->impl->onPreCompileShader(shaderID, type, additionalDefines);
    }
}

void Map::Impl::onPostCompileShader(shaders::BuiltIn shaderID,
                                    gfx::Backend::Type type,
                                    const std::string& additionalDefines) {
    observer.onPostCompileShader(shaderID, type, additionalDefines);

    if (actionJournal) {
        actionJournal->impl->onPostCompileShader(shaderID, type, additionalDefines);
    }
}

void Map::Impl::onShaderCompileFailed(shaders::BuiltIn shaderID,
                                      gfx::Backend::Type type,
                                      const std::string& additionalDefines) {
    observer.onShaderCompileFailed(shaderID, type, additionalDefines);

    if (actionJournal) {
        actionJournal->impl->onShaderCompileFailed(shaderID, type, additionalDefines);
    }
}

void Map::Impl::onGlyphsLoaded(const FontStack& fontStack, const GlyphRange& ranges) {
    observer.onGlyphsLoaded(fontStack, ranges);

    if (actionJournal) {
        actionJournal->impl->onGlyphsLoaded(fontStack, ranges);
    }
}

void Map::Impl::onGlyphsError(const FontStack& fontStack, const GlyphRange& ranges, std::exception_ptr ex) {
    observer.onGlyphsError(fontStack, ranges, ex);

    if (actionJournal) {
        actionJournal->impl->onGlyphsError(fontStack, ranges, ex);
    }
}

void Map::Impl::onGlyphsRequested(const FontStack& fontStack, const GlyphRange& ranges) {
    observer.onGlyphsRequested(fontStack, ranges);

    if (actionJournal) {
        actionJournal->impl->onGlyphsRequested(fontStack, ranges);
    }
}

void Map::Impl::onTileAction(TileOperation op, const OverscaledTileID& id, const std::string& sourceID) {
    observer.onTileAction(op, id, sourceID);

    if (actionJournal) {
        actionJournal->impl->onTileAction(op, id, sourceID);
    }
}

void Map::Impl::onRenderError(std::exception_ptr error) {
    observer.onRenderError(error);

    if (actionJournal) {
        actionJournal->impl->onRenderError(error);
    }
}

void Map::Impl::onSymbolError(const std::string& message) {
    observer.onSymbolError(message);

    if (actionJournal) {
        actionJournal->impl->onSymbolError(message);
    }
}

} // namespace mln
