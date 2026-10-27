#include <mln/test/util.hpp>
#include <mln/test/stub_file_source.hpp>
#include <mln/test/map_adapter.hpp>

#include <mln/gfx/headless_frontend.hpp>
#include <mln/map/bound_options.hpp>
#include <mln/map/camera.hpp>
#include <mln/map/map_options.hpp>
#include <mln/math/angles.hpp>
#include <mln/renderer/renderer.hpp>
#include <mln/style/layers/custom_layer.hpp>
#include <mln/style/layers/custom_layer_render_parameters.hpp>
#include <mln/style/style.hpp>
#include <mln/style/terrain.hpp>
#include <mln/util/constants.hpp>
#include <mln/util/projection.hpp>
#include <mln/util/image.hpp>
#include <mln/util/mat4.hpp>
#include <mln/util/run_loop.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

using namespace mln;

namespace {

constexpr uint32_t demTileSize = 64;
// Terrain-RGB encodes elevation as -10000 + (R * 65536 + G * 256 + B) * 0.1, so a flat
// 1000 m plateau is RGB(1, 173, 176): 65536 + 44288 + 176 = 110000.
constexpr double plateauMeters = 1000.0;
// One metre in the Mercator units of the camera position, at the fixture's latitude.
const double mercatorMetre = 1.0 / (util::M2PI * util::EARTH_RADIUS_M * std::cos(util::deg2rad(47.2692)));

std::string makeFlatDEMTile() {
    PremultipliedImage image({demTileSize, demTileSize});
    for (uint32_t i = 0; i < demTileSize * demTileSize; ++i) {
        uint8_t* px = image.data.get() + i * 4;
        px[0] = 1;
        px[1] = 173;
        px[2] = 176;
        px[3] = 255;
    }
    return encodePNG(image);
}

const char* terrainStyle = R"STYLE({
  "version": 8,
  "sources": {
    "dem": {
      "type": "raster-dem",
      "tiles": ["http://example.com/{z}-{x}-{y}.png"],
      "encoding": "mapbox",
      "maxzoom": 2,
      "tileSize": 64
    }
  },
  "terrain": {"source": "dem", "exaggeration": 1.0},
  "layers": []
})STYLE";

struct TerrainCameraTest {
    util::RunLoop loop;
    std::shared_ptr<StubFileSource> fileSource = std::make_shared<StubFileSource>(ResourceOptions::Default(),
                                                                                  ClientOptions());
    std::string tile = makeFlatDEMTile();
    HeadlessFrontend frontend{{256, 256}, 1};
    MapAdapter map;

    // Continuous runs camera transitions on the clock, for tests that do not render.
    explicit TerrainCameraTest(MapMode mode = MapMode::Static)
        : map(frontend,
              MapObserver::nullObserver(),
              fileSource,
              MapOptions().withMapMode(mode).withSize(frontend.getSize())) {
        fileSource->tileResponse = [this](const Resource&) {
            Response res;
            res.data = std::make_shared<std::string>(tile);
            return res;
        };
        map.getStyle().loadJSON(terrainStyle);
        map.jumpTo(CameraOptions().withCenter(LatLng{47.2692, 11.4041}).withZoom(2.0).withPitch(60.0));
    }

    double renderAndGetAltitude(int frames) {
        double altitude = 0.0;
        for (int i = 0; i < frames; ++i) {
            loop.runOnce();
            frontend.render(map);
            altitude = map.getCameraOptions({}).centerAltitude.value_or(0.0);
        }
        return altitude;
    }

    vec3 eye() const { return *map.getFreeCameraOptions().position; }

    // The camera is where it was, to within a metre.
    void expectEyeAt(const vec3& expected) const {
        const vec3 actual = eye();
        for (int i = 0; i < 3; ++i) {
            EXPECT_NEAR(actual[i], expected[i], mercatorMetre) << "axis " << i;
        }
    }
};

// A fill layer over a geojson source that covers far more tiles than the terrain mesh has,
// beside the DEM that gives the mesh.
const char* drapedFillStyle = R"STYLE({
  "version": 8,
  "sources": {
    "dem": {
      "type": "raster-dem",
      "tiles": ["http://example.com/{z}-{x}-{y}.png"],
      "encoding": "mapbox",
      "maxzoom": 6,
      "tileSize": 64
    },
    "area": {
      "type": "geojson",
      "data": {
        "type": "Feature",
        "properties": {},
        "geometry": {"type": "Polygon", "coordinates": [[[-60, 10], [80, 10], [80, 75], [-60, 75], [-60, 10]]]}
      }
    }
  },
  "terrain": {"source": "dem", "exaggeration": 1.0},
  "layers": [{"id": "area", "type": "fill", "source": "area", "paint": {"fill-color": "#c33"}}]
})STYLE";

struct DrapeMaskCounts {
    int targets = 0; // drape targets re-rendered
    int drawn = 0;   // masks drawn inside them
    int skipped = 0; // masks left out because the tile does not overlap the target
};

// Renders until the geojson tiles have arrived and drawn into the drape targets, then a few
// frames more, summing the counters the target passes add (RenderingStats accumulate across
// frames).
DrapeMaskCounts drapeMasksOverFrames(int maxFrames) {
    util::RunLoop loop;
    auto fileSource = std::make_shared<StubFileSource>(ResourceOptions::Default(), ClientOptions());
    const std::string tile = makeFlatDEMTile();
    fileSource->tileResponse = [&](const Resource&) {
        Response res;
        res.data = std::make_shared<std::string>(tile);
        return res;
    };
    HeadlessFrontend frontend{{390, 844}, 1};
    MapAdapter map(frontend,
                   MapObserver::nullObserver(),
                   fileSource,
                   MapOptions().withMapMode(MapMode::Static).withSize(frontend.getSize()));
    map.getStyle().loadJSON(drapedFillStyle);
    map.setBounds(BoundOptions().withMaxPitch(85.0));
    map.jumpTo(CameraOptions().withCenter(LatLng{47.2692, 11.4041}).withZoom(5.0).withPitch(75.0));

    gfx::RenderingStats before;
    DrapeMaskCounts counts;
    int settling = 0;
    for (int i = 0; i < maxFrames && settling < 4; ++i) {
        loop.runOnce();
        const auto stats = frontend.render(map).stats;
        counts.targets += stats.numDrapeTargetsRendered - before.numDrapeTargetsRendered;
        counts.drawn += stats.numDrapeMasksDrawn - before.numDrapeMasksDrawn;
        counts.skipped += stats.numDrapeMasksSkipped - before.numDrapeMasksSkipped;
        before = stats;
        if (counts.drawn > 0) ++settling;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return counts;
}

} // namespace

// With the clamp on (it is off by default), the centre rides the terrain instead of sea level,
// so pitching over high ground does not leave the camera inside the hillside.
TEST(TerrainCamera, CentreRidesTheTerrainSurface) {
    TerrainCameraTest test;
    test.map.setCenterClampedToGround(true);
    EXPECT_TRUE(test.map.getCenterClampedToGround());

    const double settled = test.renderAndGetAltitude(8);
    EXPECT_NEAR(settled, plateauMeters, 5.0);
}

// The historical worry about a terrain-anchored centre was that raising it re-samples a new
// elevation and runs away. Raising the centre moves the orbit plane, not the centre's lng/lat,
// so it must settle - and the centre must not drift while it does.
TEST(TerrainCamera, CentreElevationSettlesWithoutDrift) {
    TerrainCameraTest test;
    test.map.setCenterClampedToGround(true);

    test.renderAndGetAltitude(8);
    const auto afterSettling = test.map.getCameraOptions({});
    const double settledAltitude = *afterSettling.centerAltitude;
    // Guard against settling "stably" at sea level, which would make the rest vacuous.
    ASSERT_NEAR(settledAltitude, plateauMeters, 5.0);

    // Ten more frames with nothing else changing.
    const double later = test.renderAndGetAltitude(10);
    const auto atEnd = test.map.getCameraOptions({});

    EXPECT_NEAR(later, settledAltitude, 0.5);
    EXPECT_NEAR(atEnd.center->latitude(), afterSettling.center->latitude(), 1e-6);
    EXPECT_NEAR(atEnd.center->longitude(), afterSettling.center->longitude(), 1e-6);
    EXPECT_NEAR(*atEnd.zoom, *afterSettling.zoom, 1e-6);
}

// Turning it off leaves the camera where the caller put it.
TEST(TerrainCamera, ClampingCanBeTurnedOff) {
    TerrainCameraTest test;
    test.map.setCenterClampedToGround(false);
    EXPECT_FALSE(test.map.getCenterClampedToGround());

    EXPECT_NEAR(test.renderAndGetAltitude(8), 0.0, 0.001);
}

// An app can put the centre at any altitude without moving the camera, and restore a saved camera
// by setting its altitude before its centre and zoom: the same centre and zoom at another altitude
// are a different camera.
TEST(TerrainCamera, CentreAltitudeIsSetAndRestoredKeepingTheView) {
    TerrainCameraTest test;
    test.map.setCenterClampedToGround(false);
    test.map.jumpTo(CameraOptions().withZoom(10.0));

    const vec3 before = test.eye();
    const double zoomAtSeaLevel = *test.map.getCameraOptions({}).zoom;

    EXPECT_TRUE(test.map.setCenterAltitudeKeepingView(1500.0));
    const CameraOptions saved = test.map.getCameraOptions({});
    EXPECT_NEAR(*saved.centerAltitude, 1500.0, 0.001);
    EXPECT_GT(*saved.zoom, zoomAtSeaLevel);
    test.expectEyeAt(before);
    // Already there: nothing to move.
    EXPECT_FALSE(test.map.setCenterAltitudeKeepingView(1500.0));

    // Somewhere else, at sea level.
    EXPECT_TRUE(test.map.setCenterAltitudeKeepingView(0.0));
    test.map.jumpTo(CameraOptions().withCenter(LatLng{47.0, 11.0}).withZoom(9.0));

    // Restore: the altitude first, then the centre and zoom.
    test.map.setCenterAltitudeKeepingView(*saved.centerAltitude);
    test.map.jumpTo(CameraOptions().withCenter(*saved.center).withZoom(*saved.zoom));
    test.expectEyeAt(before);

    // Back at sea level, the original centre and zoom.
    EXPECT_TRUE(test.map.setCenterAltitudeKeepingView(0.0));
    EXPECT_NEAR(*test.map.getCameraOptions({}).zoom, zoomAtSeaLevel, 1e-3);
    test.expectEyeAt(before);
}

// The same restore while a return to sea level still waits, here for the next render of a still
// map after the terrain went: setting the altitude runs the return first, so the jump that follows
// applies the saved centre and zoom at the saved altitude, not at sea level.
TEST(TerrainCamera, CameraRestoredWhileTheReturnWaitsIsKept) {
    TerrainCameraTest test;
    test.map.setCenterClampedToGround(false);
    test.map.jumpTo(CameraOptions().withZoom(10.0));
    EXPECT_TRUE(test.map.setCenterAltitudeKeepingView(1500.0));
    const CameraOptions saved = test.map.getCameraOptions({});
    const vec3 savedEye = test.eye();

    // Somewhere else, anchored on the plateau, and then the terrain goes.
    EXPECT_TRUE(test.map.setCenterAltitudeKeepingView(0.0));
    test.map.jumpTo(CameraOptions().withCenter(LatLng{47.0, 11.0}).withZoom(9.0));
    // Lets the renderer report the plateau under the centre.
    ASSERT_NEAR(test.renderAndGetAltitude(8), 0.0, 0.001);
    test.map.anchorCenterOnTerrain();
    ASSERT_NEAR(*test.map.getCameraOptions({}).centerAltitude, plateauMeters, 0.5);
    test.map.getStyle().setTerrain(nullptr);

    test.map.setCenterAltitudeKeepingView(*saved.centerAltitude);
    test.map.jumpTo(CameraOptions().withCenter(*saved.center).withZoom(*saved.zoom));
    EXPECT_NEAR(*test.map.getCameraOptions({}).centerAltitude, *saved.centerAltitude, 0.001);
    test.expectEyeAt(savedEye);
}

// A gesture start anchors the centre on the ground being looked at without moving the camera,
// and removing the terrain brings it back to sea level, again without moving the camera:
// otherwise the flat map keeps orbiting a point up in the air.
TEST(TerrainCamera, AnchoredCentreReturnsToSeaLevelWhenTerrainIsRemoved) {
    TerrainCameraTest test;
    test.map.setCenterClampedToGround(false);
    test.map.jumpTo(CameraOptions().withZoom(10.0));
    // Lets the renderer report the plateau under the centre.
    ASSERT_NEAR(test.renderAndGetAltitude(8), 0.0, 0.001);

    const vec3 before = test.eye();
    const double zoomBefore = *test.map.getCameraOptions({}).zoom;

    test.map.anchorCenterOnTerrain();
    EXPECT_NEAR(*test.map.getCameraOptions({}).centerAltitude, plateauMeters, 0.5);
    EXPECT_GT(*test.map.getCameraOptions({}).zoom, zoomBefore);
    test.expectEyeAt(before);

    test.map.getStyle().setTerrain(nullptr);
    EXPECT_NEAR(test.renderAndGetAltitude(1), 0.0, 0.001);
    EXPECT_NEAR(*test.map.getCameraOptions({}).zoom, zoomBefore, 1e-3);
    test.expectEyeAt(before);

    // Without terrain there is nothing to anchor on, whatever height was last reported.
    test.map.anchorCenterOnTerrain();
    EXPECT_NEAR(test.map.getCameraOptions({}).centerAltitude.value_or(0.0), 0.0, 0.001);
}

// Near grazing, the line of sight runs almost along the ground: keeping the view would slide the
// centre off towards the horizon, and close to 90 degrees past a pole. The anchor leaves the
// camera alone there instead.
TEST(TerrainCamera, AnchoringAtAGrazingPitchLeavesTheCameraAlone) {
    TerrainCameraTest test;
    test.map.setCenterClampedToGround(false);
    test.map.jumpTo(CameraOptions().withZoom(9.0));
    // Lets the renderer report the plateau under the centre.
    ASSERT_NEAR(test.renderAndGetAltitude(8), 0.0, 0.001);
    test.map.setBounds(BoundOptions().withMaxPitch(89.0));
    test.map.jumpTo(CameraOptions().withPitch(89.0));
    const vec3 before = test.eye();

    test.map.anchorCenterOnTerrain();
    const auto camera = test.map.getCameraOptions({});
    EXPECT_NEAR(*camera.centerAltitude, 0.0, 0.001);
    EXPECT_NEAR(*camera.zoom, 9.0, 1e-9);
    test.expectEyeAt(before);
}

// Anchoring raises the zoom. Past the maximum zoom, jumpTo would clamp it and the view would step
// back, so the anchor leaves the camera alone there.
TEST(TerrainCamera, AnchoringPastMaxZoomLeavesTheCameraAlone) {
    TerrainCameraTest test;
    test.map.setCenterClampedToGround(false);
    test.map.jumpTo(CameraOptions().withZoom(10.0));
    // Lets the renderer report the plateau under the centre.
    ASSERT_NEAR(test.renderAndGetAltitude(8), 0.0, 0.001);
    // Anchoring on the plateau raises the zoom by ~0.15.
    test.map.setBounds(BoundOptions().withMaxZoom(10.05));
    const vec3 before = test.eye();

    test.map.anchorCenterOnTerrain();
    const auto camera = test.map.getCameraOptions({});
    EXPECT_NEAR(*camera.centerAltitude, 0.0, 0.001);
    EXPECT_NEAR(*camera.zoom, 10.0, 1e-9);
    test.expectEyeAt(before);

    // With room to zoom in, the same anchor goes through.
    test.map.setBounds(BoundOptions().withMaxZoom(util::DEFAULT_MAX_ZOOM));
    test.map.anchorCenterOnTerrain();
    EXPECT_NEAR(*test.map.getCameraOptions({}).centerAltitude, plateauMeters, 0.5);
    test.expectEyeAt(before);
}

// The same for the bounds: they would hold the centre back from where the anchor moves it.
TEST(TerrainCamera, AnchoringPastTheBoundsLeavesTheCameraAlone) {
    TerrainCameraTest test;
    test.map.setCenterClampedToGround(false);
    test.map.jumpTo(CameraOptions().withZoom(10.0));
    // Lets the renderer report the plateau under the centre.
    ASSERT_NEAR(test.renderAndGetAltitude(8), 0.0, 0.001);
    // The anchor moves the centre ~1.7 km towards the camera, south here; the bounds end ~500 m
    // south of it.
    const LatLng center = *test.map.getCameraOptions({}).center;
    test.map.setBounds(BoundOptions().withLatLngBounds(LatLngBounds::hull(
        {center.latitude() - 0.0045, center.longitude() - 1.0}, {center.latitude() + 1.0, center.longitude() + 1.0})));
    const vec3 before = test.eye();

    test.map.anchorCenterOnTerrain();
    const auto camera = test.map.getCameraOptions({});
    EXPECT_NEAR(*camera.centerAltitude, 0.0, 0.001);
    EXPECT_NEAR(*camera.zoom, 10.0, 1e-9);
    EXPECT_NEAR(camera.center->latitude(), center.latitude(), 1e-9);
    EXPECT_NEAR(camera.center->longitude(), center.longitude(), 1e-9);
    test.expectEyeAt(before);
}

// A move across the antimeridian wraps the centre's longitude, which is no limit cutting it short.
TEST(TerrainCamera, AnchoringAcrossTheAntimeridianGoesThrough) {
    TerrainCameraTest test;
    test.map.setCenterClampedToGround(false);
    // Looking west from just west of the antimeridian: the camera, and the anchor's move towards
    // it, are east of the centre.
    test.map.jumpTo(CameraOptions().withCenter(LatLng{47.2692, 179.99}).withZoom(10.0).withBearing(270.0));
    // Lets the renderer report the plateau under the centre.
    ASSERT_NEAR(test.renderAndGetAltitude(8), 0.0, 0.001);

    test.map.anchorCenterOnTerrain();
    const auto camera = test.map.getCameraOptions({});
    EXPECT_NEAR(*camera.centerAltitude, plateauMeters, 0.5);
    EXPECT_LT(camera.center->longitude(), -179.9);
}

// Going back to sea level lowers the zoom. Past the minimum zoom the centre comes down where it is,
// at its zoom, as in gl-js, rather than stay up in the air over the flat map.
TEST(TerrainCamera, ReturnToSeaLevelAtMinZoomKeepsTheCentre) {
    TerrainCameraTest test;
    test.map.setCenterClampedToGround(false);
    test.map.jumpTo(CameraOptions().withZoom(10.0));
    // Lets the renderer report the plateau under the centre.
    ASSERT_NEAR(test.renderAndGetAltitude(8), 0.0, 0.001);
    test.map.anchorCenterOnTerrain();
    const auto anchored = test.map.getCameraOptions({});
    ASSERT_NEAR(*anchored.centerAltitude, plateauMeters, 0.5);
    // Keeping the view would take the zoom back to 10.
    test.map.setBounds(BoundOptions().withMinZoom(10.05));

    test.map.getStyle().setTerrain(nullptr);
    EXPECT_NEAR(test.renderAndGetAltitude(1), 0.0, 0.001);
    const auto camera = test.map.getCameraOptions({});
    EXPECT_NEAR(*camera.zoom, *anchored.zoom, 1e-9);
    EXPECT_NEAR(camera.center->latitude(), anchored.center->latitude(), 1e-9);
    EXPECT_NEAR(camera.center->longitude(), anchored.center->longitude(), 1e-9);
}

// A running transition re-applies the altitude it started with on every frame, so the return to
// sea level waits for it to end (here, to be cancelled), then keeps the view.
TEST(TerrainCamera, RemovingTerrainMidTransitionReturnsAfterIt) {
    TerrainCameraTest test{MapMode::Continuous};
    test.map.setCenterClampedToGround(false);
    test.map.jumpTo(CameraOptions().withZoom(10.0).withCenterAltitude(plateauMeters));
    test.map.easeTo(CameraOptions().withBearing(90.0), AnimationOptions(Seconds(10)));

    test.map.getStyle().setTerrain(nullptr);
    EXPECT_NEAR(*test.map.getCameraOptions({}).centerAltitude, plateauMeters, 0.001);

    const vec3 before = test.eye();
    test.map.cancelTransitions();
    EXPECT_NEAR(*test.map.getCameraOptions({}).centerAltitude, 0.0, 0.001);
    test.expectEyeAt(before);
}

// A camera the app sets while the return to sea level waits is the app's to keep: the return
// comes first, from the camera being replaced, and must not move the new one afterwards.
TEST(TerrainCamera, CameraSetWhileTheReturnWaitsIsKept) {
    TerrainCameraTest test{MapMode::Continuous};
    test.map.setCenterClampedToGround(false);
    test.map.jumpTo(CameraOptions().withZoom(10.0).withCenterAltitude(plateauMeters));
    test.map.easeTo(CameraOptions().withBearing(90.0), AnimationOptions(Seconds(10)));
    // The return waits for the ease.
    test.map.getStyle().setTerrain(nullptr);

    const LatLng target{47.3, 11.5};
    test.map.jumpTo(CameraOptions().withCenter(target).withZoom(12.0));
    const auto camera = test.map.getCameraOptions({});
    EXPECT_NEAR(*camera.centerAltitude, 0.0, 0.001);
    EXPECT_NEAR(*camera.zoom, 12.0, 1e-9);
    EXPECT_NEAR(camera.center->latitude(), target.latitude(), 1e-9);
    EXPECT_NEAR(camera.center->longitude(), target.longitude(), 1e-9);
}

// The same on a still map, where the return waits for the next render.
TEST(TerrainCamera, CameraSetBeforeTheNextStillRenderIsKept) {
    TerrainCameraTest test;
    test.map.setCenterClampedToGround(false);
    test.map.jumpTo(CameraOptions().withZoom(10.0));
    // Lets the renderer report the plateau under the centre.
    ASSERT_NEAR(test.renderAndGetAltitude(8), 0.0, 0.001);
    test.map.anchorCenterOnTerrain();
    test.map.getStyle().setTerrain(nullptr);

    const LatLng target{47.3, 11.5};
    test.map.jumpTo(CameraOptions().withCenter(target).withZoom(12.0));
    EXPECT_NEAR(test.renderAndGetAltitude(1), 0.0, 0.001);
    const auto camera = test.map.getCameraOptions({});
    EXPECT_NEAR(*camera.zoom, 12.0, 1e-9);
    EXPECT_NEAR(camera.center->latitude(), target.latitude(), 1e-9);
    EXPECT_NEAR(camera.center->longitude(), target.longitude(), 1e-9);
}

namespace {

// A flat Terrain-RGB tile at `meters`, a multiple of 0.1 m.
std::string makeDEMTile(double meters) {
    const auto value = static_cast<uint32_t>(std::lround((meters + 10000.0) * 10.0));
    PremultipliedImage image({demTileSize, demTileSize});
    for (uint32_t i = 0; i < demTileSize * demTileSize; ++i) {
        uint8_t* px = image.data.get() + i * 4;
        px[0] = static_cast<uint8_t>(value >> 16);
        px[1] = static_cast<uint8_t>(value >> 8);
        px[2] = static_cast<uint8_t>(value);
        px[3] = 255;
    }
    return encodePNG(image);
}

constexpr double westPlateauMeters = 2000.0;

// Continuous mode, so camera transitions run on the clock while frames render, with the
// clamp on. The z2 DEM tiles between 90°W and the prime meridian (x = 1) are a 2000 m
// plateau, the rest 1000 m, so a move west from Innsbruck crosses a step in the terrain.
struct TerrainTransitionTest {
    util::RunLoop loop;
    std::shared_ptr<StubFileSource> fileSource = std::make_shared<StubFileSource>(ResourceOptions::Default(),
                                                                                  ClientOptions());
    std::string lowTile = makeDEMTile(plateauMeters);
    std::string highTile = makeDEMTile(westPlateauMeters);
    HeadlessFrontend frontend{{256, 256}, 1};
    MapAdapter map;

    TerrainTransitionTest()
        : map(frontend,
              MapObserver::nullObserver(),
              fileSource,
              MapOptions().withMapMode(MapMode::Continuous).withSize(frontend.getSize())) {
        fileSource->tileResponse = [this](const Resource& resource) -> std::optional<Response> {
            const bool west = resource.tileData && resource.tileData->z == 2 && resource.tileData->x == 1;
            if (west && holdWest) {
                return std::nullopt; // still on its way
            }
            Response res;
            res.data = std::make_shared<std::string>(west ? highTile : lowTile);
            return res;
        };
        map.getStyle().loadJSON(terrainStyle);
        map.setCenterClampedToGround(true);
        map.jumpTo(CameraOptions().withCenter(LatLng{47.2692, 11.4041}).withZoom(2.0).withPitch(60.0));
    }

    void renderFrame() {
        loop.runOnce();
        frontend.renderFrame();
    }

    double altitude() { return map.getCameraOptions({}).centerAltitude.value_or(0.0); }

    // Renders until the clamp has put the centre on the 1000 m plateau under Innsbruck.
    bool settleOnPlateau() {
        const auto deadline = Clock::now() + Seconds(10);
        while (Clock::now() < deadline) {
            renderFrame();
            if (std::abs(altitude() - plateauMeters) < 5.0) {
                return true;
            }
        }
        return false;
    }

    // Runs the transition `start` begins with `animation`, calling `whileRunning` after each of
    // its frames, until it has finished and a few more frames rendered. `eyeAtEnd` and
    // `centerAtEnd` keep the camera as it left it, before the clamp acted.
    template <typename Start, typename Fn>
    bool runTransition(Start start, Fn whileRunning) {
        bool finished = false;
        AnimationOptions animation(Milliseconds(1500));
        animation.transitionFinishFn = [&] {
            finished = true;
            eyeAtEnd = eye();
            centerAtEnd = *map.getCameraOptions({}).center;
        };
        start(animation);

        const auto deadline = Clock::now() + Seconds(20);
        while (!finished && Clock::now() < deadline) {
            renderFrame();
            whileRunning(finished);
        }
        for (int i = 0; i < 10; ++i) {
            renderFrame();
        }
        return finished;
    }

    // Eases west onto the 2000 m plateau, a transition given its centre.
    template <typename Fn>
    bool easeWest(Fn whileRunning) {
        return runTransition(
            [&](const AnimationOptions& animation) { map.easeTo(CameraOptions().withCenter(westTarget), animation); },
            whileRunning);
    }

    // Pans west onto the 2000 m plateau the way a fling's deceleration does: a move relative to
    // the view, with no centre to arrive at.
    bool flingWest() {
        return runTransition([&](const AnimationOptions& animation) { map.moveBy({200.0, 0.0}, animation); },
                             [](bool) {});
    }

    vec3 eye() const { return *map.getFreeCameraOptions().position; }

    // The camera is where the transition left it, to within a metre.
    void expectEyeAtEnd() const {
        const vec3 actual = eye();
        for (int i = 0; i < 3; ++i) {
            EXPECT_NEAR(actual[i], eyeAtEnd[i], mercatorMetre) << "axis " << i;
        }
    }

    const LatLng westTarget{47.2692, -30.0};
    // While set, the plateau's tiles don't arrive.
    bool holdWest = false;
    vec3 eyeAtEnd{};
    LatLng centerAtEnd;
};

} // namespace

// Across relief the height under the centre changes every frame of a camera transition. The
// clamp waits for the transition to end instead of cancelling it with a jump, then puts the
// centre it was given on the ground there.
TEST(TerrainCamera, ClampWaitsForTransitionToEnd) {
    TerrainTransitionTest test;
    ASSERT_TRUE(test.settleOnPlateau());

    ASSERT_TRUE(test.easeWest([](bool) {}));

    const auto camera = test.map.getCameraOptions({});
    EXPECT_NEAR(camera.center->latitude(), test.westTarget.latitude(), 1e-6);
    EXPECT_NEAR(camera.center->longitude(), test.westTarget.longitude(), 1e-6);
    EXPECT_NEAR(*camera.centerAltitude, westPlateauMeters, 5.0);
}

// A fling arrives with the centre still at the height it set out from, 1000 m below the plateau
// it now looks at, and has no centre of its own to keep. The clamp keeping the centre and zoom
// would lift the camera 1000 m on the frame the motion stops, jumping the whole view; the centre
// moves along the line of sight onto the ground instead, and the camera stays where it was.
TEST(TerrainCamera, ClampAfterAFlingKeepsTheCamera) {
    TerrainTransitionTest test;
    ASSERT_TRUE(test.settleOnPlateau());

    ASSERT_TRUE(test.flingWest());
    // Arrived over the plateau with the centre's height from before the fling
    ASSERT_LT(test.centerAtEnd.longitude(), -5.0);

    test.expectEyeAtEnd();
    const auto camera = test.map.getCameraOptions({});
    EXPECT_NEAR(*camera.centerAltitude, westPlateauMeters, 5.0);
    // Towards the camera, south at bearing 0, by the 1000 m rise times tan(60 degrees)
    const double southMetres = util::deg2rad(test.centerAtEnd.latitude() - camera.center->latitude()) *
                               util::EARTH_RADIUS_M;
    EXPECT_NEAR(southMetres, 1000.0 * std::tan(util::deg2rad(60.0)), 50.0);

    // On the ground already, so the clamp has nothing left to do frames later.
    for (int i = 0; i < 10; ++i) {
        test.renderFrame();
    }
    test.expectEyeAtEnd();
}

// Heights that change once a fling has come to rest (a finer DEM tile landing; here the terrain
// grows by a tenth) are the ground changing under a camera that is not moving. The clamp keeping
// the centre and zoom would move the camera by the change and jump the whole view a frame after it
// stopped: the camera stays, and the centre moves along the line of sight onto the new ground.
TEST(TerrainCamera, HeightsChangingAfterAFlingKeepTheCamera) {
    TerrainTransitionTest test;
    ASSERT_TRUE(test.settleOnPlateau());
    ASSERT_TRUE(test.flingWest());
    ASSERT_NEAR(test.altitude(), westPlateauMeters, 5.0);
    const vec3 rested = test.eye();
    const double zoomRested = *test.map.getCameraOptions({}).zoom;

    test.map.getStyle().getTerrain()->setExaggeration(1.1f);
    for (int i = 0; i < 20; ++i) {
        test.renderFrame();
    }

    EXPECT_NEAR(test.altitude(), westPlateauMeters * 1.1, 5.0);
    const vec3 actual = test.eye();
    for (int i = 0; i < 3; ++i) {
        EXPECT_NEAR(actual[i], rested[i], mercatorMetre) << "axis " << i;
    }
    // Nearer the ground by 200 m, so further in
    EXPECT_GT(*test.map.getCameraOptions({}).zoom, zoomRested);
}

// A fling that ends uphill at the maximum zoom: keeping the camera would put the centre on ground
// close enough to zoom past it, and the clamp keeping the centre and zoom instead jumped the whole
// view up (Rocky Butte at z16.8, 130 pt). The camera stays, and the centre goes as far along the
// line of sight as the maximum zoom allows.
TEST(TerrainCamera, FlingEndingPastMaxZoomKeepsTheCamera) {
    TerrainTransitionTest test;
    ASSERT_TRUE(test.settleOnPlateau());
    // At z2 the 1000 m rise takes the camera 0.0006 zoom levels nearer the ground.
    const double maxZoom = *test.map.getCameraOptions({}).zoom + 0.0002;
    test.map.setBounds(BoundOptions().withMaxZoom(maxZoom));

    ASSERT_TRUE(test.flingWest());
    ASSERT_LT(test.centerAtEnd.longitude(), -5.0);

    test.expectEyeAtEnd();
    const auto camera = test.map.getCameraOptions({});
    EXPECT_NEAR(*camera.zoom, maxZoom, 1e-5);
    EXPECT_GT(*camera.centerAltitude, plateauMeters + 100.0);
    EXPECT_LT(*camera.centerAltitude, westPlateauMeters - 100.0);
}

// A drag across a 1000 m step in the terrain. The clamp keeping the centre and zoom on every
// height a drag brings under the centre lifted the camera with the ground: 1000 m in one frame
// here, up to 124 m a frame dragging over a mountainside. The camera moves level instead, and
// stays where the drag left it once the finger lifts.
TEST(TerrainCamera, DragAcrossAStepKeepsTheCameraLevel) {
    TerrainTransitionTest test;
    ASSERT_TRUE(test.settleOnPlateau());

    test.map.setGestureInProgress(true);
    test.map.anchorCenterOnTerrain();
    double previous = test.eye()[2];
    double largestStep = 0.0;
    for (int i = 0; i < 30; ++i) {
        test.map.moveBy({10.0, 0.0});
        test.renderFrame();
        const double height = test.eye()[2];
        largestStep = std::max(largestStep, std::abs(height - previous));
        previous = height;
    }
    ASSERT_LT(test.map.getCameraOptions({}).center->longitude(), -5.0);
    EXPECT_LT(largestStep, mercatorMetre);

    const vec3 dragged = test.eye();
    test.map.setGestureInProgress(false);
    for (int i = 0; i < 10; ++i) {
        test.renderFrame();
    }
    const vec3 actual = test.eye();
    for (int i = 0; i < 3; ++i) {
        EXPECT_NEAR(actual[i], dragged[i], mercatorMetre) << "axis " << i;
    }
    EXPECT_NEAR(test.altitude(), westPlateauMeters, 5.0);
}

// The clamp waits for the finger to lift, as gl-js's does: the drag keeps the zoom and the height
// it began at throughout, and the centre goes onto the ground along the line of sight once, keeping
// the camera, when the gesture ends.
TEST(TerrainCamera, DragKeepsItsZoomUntilTheFingerLifts) {
    TerrainTransitionTest test;
    ASSERT_TRUE(test.settleOnPlateau());
    const double zoom = *test.map.getCameraOptions({}).zoom;

    test.map.setGestureInProgress(true);
    test.map.anchorCenterOnTerrain();
    for (int i = 0; i < 30; ++i) {
        test.map.moveBy({10.0, 0.0});
        test.renderFrame();
        ASSERT_NEAR(*test.map.getCameraOptions({}).zoom, zoom, 1e-12) << "frame " << i;
        ASSERT_NEAR(test.altitude(), plateauMeters, 0.5) << "frame " << i;
    }
    ASSERT_LT(test.map.getCameraOptions({}).center->longitude(), -5.0);

    const vec3 dragged = test.eye();
    test.map.setGestureInProgress(false);
    const vec3 lifted = test.eye();
    for (int i = 0; i < 3; ++i) {
        EXPECT_NEAR(lifted[i], dragged[i], mercatorMetre) << "axis " << i;
    }
    EXPECT_NEAR(test.altitude(), westPlateauMeters, 5.0);
    EXPECT_GT(*test.map.getCameraOptions({}).zoom, zoom);
}

// A centre the app jumped to is still the one the clamp keeps when the heights under it change:
// it is put on the new ground there, at the zoom it was given.
TEST(TerrainCamera, HeightsChangingAfterAJumpKeepTheCentre) {
    TerrainTransitionTest test;
    ASSERT_TRUE(test.settleOnPlateau());
    const LatLng target{47.2692, -30.0};
    test.map.jumpTo(CameraOptions().withCenter(target).withZoom(2.5));
    for (int i = 0; i < 20; ++i) {
        test.renderFrame();
    }
    ASSERT_NEAR(test.altitude(), westPlateauMeters, 5.0);

    test.map.getStyle().getTerrain()->setExaggeration(1.1f);
    for (int i = 0; i < 20; ++i) {
        test.renderFrame();
    }

    const auto camera = test.map.getCameraOptions({});
    EXPECT_NEAR(*camera.centerAltitude, westPlateauMeters * 1.1, 5.0);
    EXPECT_NEAR(*camera.zoom, 2.5, 1e-9);
    EXPECT_NEAR(camera.center->latitude(), target.latitude(), 1e-9);
    EXPECT_NEAR(camera.center->longitude(), target.longitude(), 1e-9);
}

// A transition cancelled short of its centre has no centre to keep either: the camera stays where
// the cancel left it.
TEST(TerrainCamera, ClampAfterACancelledTransitionKeepsTheCamera) {
    TerrainTransitionTest test;
    ASSERT_TRUE(test.settleOnPlateau());

    bool cancelled = false;
    ASSERT_TRUE(test.easeWest([&](bool finished) {
        // Well over the 2000 m plateau, so the clamp has a new height waiting.
        if (!finished && !cancelled && test.map.getCameraOptions({}).center->longitude() < -5.0) {
            test.map.cancelTransitions();
            cancelled = true;
        }
    }));
    ASSERT_TRUE(cancelled);

    test.expectEyeAtEnd();
    EXPECT_NEAR(*test.map.getCameraOptions({}).centerAltitude, westPlateauMeters, 5.0);
}

// A camera set without a transition (a turn here) cancels a running one too, and the height the
// clamp was holding for that transition's end must not then move the camera the app just set: the
// turn keeps the eye's height, and so does the clamp, putting the centre on the ground along the
// line of sight.
TEST(TerrainCamera, TurnDuringATransitionKeepsTheCamera) {
    TerrainTransitionTest test;
    ASSERT_TRUE(test.settleOnPlateau());

    bool turned = false;
    double eyeHeightBeforeTurn = 0.0;
    double eyeHeightAfterTurn = 0.0;
    ASSERT_TRUE(test.easeWest([&](bool finished) {
        // Well over the 2000 m plateau, so the clamp has a new height waiting.
        if (!finished && !turned && test.map.getCameraOptions({}).center->longitude() < -5.0) {
            eyeHeightBeforeTurn = test.eye()[2];
            test.map.jumpTo(CameraOptions().withBearing(*test.map.getCameraOptions({}).bearing + 10.0));
            eyeHeightAfterTurn = test.eye()[2];
            turned = true;
        }
    }));
    ASSERT_TRUE(turned);

    EXPECT_NEAR(eyeHeightAfterTurn, eyeHeightBeforeTurn, mercatorMetre);
    EXPECT_NEAR(test.eye()[2], eyeHeightBeforeTurn, mercatorMetre);
    EXPECT_NEAR(*test.map.getCameraOptions({}).centerAltitude, westPlateauMeters, 5.0);
}

// An animated move to a centre over ground whose heights arrive part-way through it, given the
// height it set out from as the app gives one where the ground ahead hasn't loaded. It eases the
// centre onto that ground as the heights arrive, as gl-js's Camera._updateElevation does, and
// arrives on it: the clamp has nothing left to do, where it lifted the camera 1000 m on the frame
// the move ended.
TEST(TerrainCamera, EaseOntoGroundLoadedDuringTheMoveEndsWithoutAJump) {
    TerrainTransitionTest test;
    test.holdWest = true;
    ASSERT_TRUE(test.settleOnPlateau());

    bool released = false;
    double releasedAt = 0.0;
    double largestStep = 0.0;
    double previous = test.eye()[2];
    ASSERT_TRUE(test.runTransition(
        [&](const AnimationOptions& animation) {
            test.map.easeTo(CameraOptions().withCenter(test.westTarget).withCenterAltitude(plateauMeters), animation);
        },
        [&](bool finished) {
            const double height = test.eye()[2];
            if (!finished) {
                largestStep = std::max(largestStep, std::abs(height - previous));
            }
            previous = height;
            // A third of the way there, the plateau's heights arrive.
            const double longitude = test.map.getCameraOptions({}).center->longitude();
            if (!released && longitude < 0.0) {
                test.holdWest = false;
                released = true;
                releasedAt = longitude;
            }
        }));
    ASSERT_TRUE(released);
    ASSERT_GT(releasedAt, -25.0);

    test.expectEyeAtEnd();
    const auto camera = test.map.getCameraOptions({});
    EXPECT_NEAR(*camera.centerAltitude, westPlateauMeters, 5.0);
    EXPECT_NEAR(camera.center->longitude(), test.westTarget.longitude(), 1e-6);
    // Eased: a 1000 m rise over the rest of the move, no step of it a jump.
    EXPECT_LT(largestStep, 400.0 * mercatorMetre);
}

// The same for a free camera set while a transition runs: it stays where it was put.
TEST(TerrainCamera, FreeCameraSetDuringATransitionIsKept) {
    TerrainTransitionTest test;
    ASSERT_TRUE(test.settleOnPlateau());

    bool set = false;
    vec3 eyeSet{};
    ASSERT_TRUE(test.easeWest([&](bool finished) {
        if (!finished && !set && test.map.getCameraOptions({}).center->longitude() < -5.0) {
            const FreeCameraOptions camera = test.map.getFreeCameraOptions();
            eyeSet = *camera.position;
            test.map.setFreeCameraOptions(camera);
            set = true;
        }
    }));
    ASSERT_TRUE(set);

    const vec3 actual = test.eye();
    for (int i = 0; i < 3; ++i) {
        EXPECT_NEAR(actual[i], eyeSet[i], mercatorMetre) << "axis " << i;
    }
}

// A jump to a centre while a transition runs arrives over other ground than the height the clamp
// was holding for the transition: the clamp puts that centre on the ground under it, not at the
// held height.
TEST(TerrainCamera, JumpToACentreDuringATransitionUsesTheGroundThere) {
    TerrainTransitionTest test;
    ASSERT_TRUE(test.settleOnPlateau());

    const LatLng east{47.2692, 20.0};
    bool jumped = false;
    double altitudeAfterJump = 0.0;
    ASSERT_TRUE(test.easeWest([&](bool finished) {
        if (!finished && !jumped && test.map.getCameraOptions({}).center->longitude() < -5.0) {
            test.map.jumpTo(CameraOptions().withCenter(east));
            altitudeAfterJump = test.altitude();
            jumped = true;
        }
    }));
    ASSERT_TRUE(jumped);

    EXPECT_NEAR(altitudeAfterJump, plateauMeters, 5.0);
    const auto camera = test.map.getCameraOptions({});
    EXPECT_NEAR(*camera.centerAltitude, plateauMeters, 5.0);
    EXPECT_NEAR(camera.center->longitude(), east.longitude(), 1e-9);
    EXPECT_NEAR(camera.center->latitude(), east.latitude(), 1e-9);
}

// A terrain removed while the clamp waits for a transition takes the waiting height with it:
// once the transition ends, the centre returns to sea level and the clamp does not raise it
// back onto ground that is no longer drawn.
TEST(TerrainCamera, ClampDoesNotApplyAfterTerrainRemoval) {
    TerrainTransitionTest test;
    ASSERT_TRUE(test.settleOnPlateau());

    bool removedMidTransition = false;
    ASSERT_TRUE(test.easeWest([&](bool finished) {
        // Well over the 2000 m plateau, so the clamp has a new height waiting.
        if (!finished && !removedMidTransition && test.map.getCameraOptions({}).center->longitude() < -5.0) {
            test.map.getStyle().setTerrain(nullptr);
            removedMidTransition = true;
        }
    }));
    ASSERT_TRUE(removedMidTransition);

    EXPECT_NEAR(test.altitude(), 0.0, 0.001);
}

// The terrain height under a position reads the loaded DEM, as drawn: nothing before a tile
// covers it, the plateau once one does, scaled by the exaggeration, and nothing without terrain.
TEST(TerrainElevation, QueryReadsTheTerrainAsDrawn) {
    TerrainCameraTest test;
    const LatLng innsbruck{47.2692, 11.4041};
    EXPECT_FALSE(test.frontend.getRenderer()->queryTerrainElevation(innsbruck));

    test.renderAndGetAltitude(8);
    const auto elevation = test.frontend.getRenderer()->queryTerrainElevation(innsbruck);
    ASSERT_TRUE(elevation);
    EXPECT_NEAR(*elevation, plateauMeters, 0.5);

    test.map.getStyle().setTerrain(std::make_unique<style::Terrain>("dem", 2.0f));
    test.renderAndGetAltitude(2);
    EXPECT_NEAR(test.frontend.getRenderer()->queryTerrainElevation(innsbruck).value_or(0.0), 2 * plateauMeters, 1.0);

    test.map.getStyle().setTerrain(nullptr);
    test.renderAndGetAltitude(2);
    EXPECT_FALSE(test.frontend.getRenderer()->queryTerrainElevation(innsbruck));
}

// The default layer manager registers custom layers on OpenGL only (darwin registers its own).
#if MLN_RENDER_BACKEND_OPENGL
namespace {

// Keeps what the terrain looked like to the custom layer the last time it drew.
class TerrainRecordingLayer : public style::CustomLayerHost {
public:
    explicit TerrainRecordingLayer(std::optional<style::CustomLayerTerrain>& terrain_)
        : terrain(terrain_) {}
    void initialize(const style::CustomLayerInitParameters&) override {}
    void render(const style::CustomLayerRenderParameters& parameters) override {
        rendered = true;
        terrain = parameters.terrain;
    }
    void contextLost() override {}
    void deinitialize() override {}

    bool rendered = false;
    std::optional<style::CustomLayerTerrain>& terrain;
};

} // namespace

// A custom layer is handed the terrain as the terrain pass draws it, so it can draw on the same
// surface: each mesh tile with its matrix and DEM, and the DEM decoding.
TEST(TerrainCustomLayer, RenderParametersCarryTheDrawnTerrain) {
    TerrainCameraTest test;
    std::optional<style::CustomLayerTerrain> terrain;
    auto host = std::make_unique<TerrainRecordingLayer>(terrain);
    auto* recorder = host.get();
    test.map.getStyle().addLayer(std::make_unique<style::CustomLayer>("custom", std::move(host)));

    test.renderAndGetAltitude(8);
    ASSERT_TRUE(recorder->rendered);
    ASSERT_TRUE(terrain);
    EXPECT_FLOAT_EQ(terrain->exaggeration, 1.0f);
    EXPECT_EQ(terrain->meshSize, 128u);
    // Terrain-RGB ("mapbox"): (r * 65536 + g * 256 + b) * 0.1 - 10000.
    EXPECT_FLOAT_EQ(terrain->unpack[0], 6553.6f);
    EXPECT_FLOAT_EQ(terrain->unpack[3], 10000.0f);
    ASSERT_FALSE(terrain->tiles.empty());

    for (const auto& tile : terrain->tiles) {
        ASSERT_TRUE(tile.demTexture);
        EXPECT_GT(tile.demCoords[3], 1.0f) << "the plateau's DEM is bound, not the placeholder";
    }

    // The map centre (at sea level: the clamp is off) projects through its tile's matrix to the
    // middle of the view.
    const LatLng centre = *test.map.getCameraOptions({}).center;
    const auto inTile = [&](const style::CustomLayerTerrainTile& tile) {
        const auto point = Projection::project(centre, tile.z);
        return tile.wrap == 0 && std::floor(point.x) == tile.x && std::floor(point.y) == tile.y;
    };
    const auto centreTile = std::find_if(terrain->tiles.begin(), terrain->tiles.end(), inTile);
    ASSERT_NE(centreTile, terrain->tiles.end());
    const auto point = Projection::project(centre, centreTile->z);
    const vec4 local{(point.x - centreTile->x) * util::EXTENT, (point.y - centreTile->y) * util::EXTENT, 0, 1};
    vec4 clip;
    matrix::transformMat4(clip, local, centreTile->matrix);
    EXPECT_NEAR(clip[0] / clip[3], 0.0, 1e-3);
    EXPECT_NEAR(clip[1] / clip[3], 0.0, 1e-3);

    test.map.getStyle().setTerrain(nullptr);
    test.renderAndGetAltitude(2);
    EXPECT_FALSE(terrain);
}
#endif

// A drape target is drawn into only by the source tiles that overlap its own tile, so the
// masks it draws follow those tiles and not every tile the source covers.
TEST(TerrainDrape, MasksAreDrawnOnlyForTilesOverlappingTheTarget) {
    const auto counts = drapeMasksOverFrames(60);
    ASSERT_GT(counts.targets, 0);
    EXPECT_GT(counts.drawn, 0);
    EXPECT_GT(counts.skipped, 0);
    // The source's tiles are coarser than the mesh or the same size, so a target overlaps a
    // handful of them at most.
    EXPECT_LE(counts.drawn, 4 * counts.targets)
        << counts.drawn << " masks over " << counts.targets << " targets, " << counts.skipped << " skipped";
}

// Where the source has no DEM for a tile, the terrain draws its parent's, and the query reads that
// too rather than finding nothing.
TEST(TerrainElevation, QueryReadsTheParentDEMWhereATileHasNone) {
    TerrainCameraTest test;
    const LatLng innsbruck{47.2692, 11.4041};
    test.map.jumpTo(CameraOptions().withZoom(1.0));
    test.renderAndGetAltitude(4);
    ASSERT_NEAR(test.frontend.getRenderer()->queryTerrainElevation(innsbruck).value_or(0.0), plateauMeters, 0.5);

    test.fileSource->tileResponse = [&](const Resource& resource) {
        Response res;
        if (resource.url.find("/2-") != std::string::npos) {
            res.noContent = true;
        } else {
            res.data = std::make_shared<std::string>(test.tile);
        }
        return res;
    };
    test.map.jumpTo(CameraOptions().withZoom(2.0));
    test.renderAndGetAltitude(4);
    const auto elevation = test.frontend.getRenderer()->queryTerrainElevation(innsbruck);
    ASSERT_TRUE(elevation);
    EXPECT_NEAR(*elevation, plateauMeters, 0.5);
}

namespace {

constexpr double highGroundMeters = 1500.0;

// Ground higher than the camera of a sea-level centre at z17 stands above it (a few hundred
// metres in this 256 px viewport), so a centre left at sea level puts the camera inside it.
void expectCameraAboveTheGround(TerrainCameraTest& test) {
    EXPECT_NEAR(*test.map.getCameraOptions({}).centerAltitude, highGroundMeters, 5.0);
    EXPECT_GT(test.eye()[2] / mercatorMetre, highGroundMeters);
}

} // namespace

// The clamp puts the centre on the ground once the renderer has its height, with nothing else
// moving the camera: here the DEM under a z17 centre that starts at sea level, below the camera
// that the ground has swallowed.
TEST(TerrainCamera, CentreLandsOnTheGroundWhenTheDemLoadsAtHighZoom) {
    TerrainCameraTest test;
    test.tile = makeDEMTile(highGroundMeters);
    test.map.setCenterClampedToGround(true);
    test.map.jumpTo(CameraOptions().withZoom(17.0).withPitch(45.0).withCenterAltitude(0.0));
    ASSERT_LT(test.eye()[2] / mercatorMetre, highGroundMeters);

    test.renderAndGetAltitude(8);
    expectCameraAboveTheGround(test);
}

// The same when the terrain is turned on over an idle z17 map.
TEST(TerrainCamera, CentreLandsOnTheGroundWhenTerrainIsEnabledAtHighZoom) {
    TerrainCameraTest test;
    test.tile = makeDEMTile(highGroundMeters);
    test.map.setCenterClampedToGround(true);
    test.map.getStyle().setTerrain(nullptr);
    test.map.jumpTo(CameraOptions().withZoom(17.0).withPitch(45.0).withCenterAltitude(0.0));
    ASSERT_NEAR(test.renderAndGetAltitude(4), 0.0, 0.001);
    ASSERT_LT(test.eye()[2] / mercatorMetre, highGroundMeters);

    test.map.getStyle().setTerrain(std::make_unique<style::Terrain>("dem"));
    test.renderAndGetAltitude(8);
    expectCameraAboveTheGround(test);
}
