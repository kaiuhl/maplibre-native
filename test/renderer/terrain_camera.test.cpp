#include <mln/test/util.hpp>
#include <mln/test/stub_file_source.hpp>
#include <mln/test/map_adapter.hpp>

#include <mln/gfx/headless_frontend.hpp>
#include <mln/map/bound_options.hpp>
#include <mln/map/camera.hpp>
#include <mln/map/map_options.hpp>
#include <mln/map/transform.hpp>
#include <mln/math/angles.hpp>
#include <mln/style/style.hpp>
#include <mln/style/terrain.hpp>
#include <mln/util/constants.hpp>
#include <mln/util/image.hpp>
#include <mln/util/run_loop.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iterator>
#include <thread>
#include <utility>
#include <vector>

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

// The camera's height on each frame of a rise from one plateau to the other, and when the frame was
// rendered. Frames come as fast as the machine renders them, so how far the camera rose on any one
// of them depends on the machine; how long the rise took doesn't.
struct Rise {
    std::vector<std::pair<TimePoint, double>> frames;

    void add(double height) { frames.emplace_back(Clock::now(), height); }

    // Milliseconds from the last frame at the height the camera started at to the first at the height
    // it ended at, to within a metre: an ease's duration at least, however far apart frames come, and
    // one frame for a jump. None if it didn't rise.
    double milliseconds() const {
        if (frames.empty() || std::abs(frames.back().second - frames.front().second) <= mercatorMetre) {
            return 0.0;
        }
        const double from = frames.front().second;
        const double to = frames.back().second;
        const auto arrived = std::find_if(
            frames.begin(), frames.end(), [&](const auto& frame) { return std::abs(frame.second - to) <= mercatorMetre; });
        const auto left = std::find_if(std::make_reverse_iterator(arrived), frames.rend(), [&](const auto& frame) {
            return std::abs(frame.second - from) <= mercatorMetre;
        });
        if (left == frames.rend()) {
            return 0.0;
        }
        return std::chrono::duration<double, std::milli>(arrived->first - left->first).count();
    }
};

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
    Rise rise;
    rise.add(test.eye()[2]);
    ASSERT_TRUE(test.runTransition(
        [&](const AnimationOptions& animation) {
            test.map.easeTo(CameraOptions().withCenter(test.westTarget).withCenterAltitude(plateauMeters), animation);
        },
        [&](bool) {
            rise.add(test.eye()[2]);
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
    // Eased: the 1000 m rise takes most of the second the move had left, not one frame.
    EXPECT_GE(rise.milliseconds(), 500.0);
}

// An animated move that leaves the centre where it is (a tilt, as the app gives one when 3D comes
// on with the centre still at sea level) eases the centre altitude it was given over the move.
// The path was no length, and the altitude, which moved by the distance along it, stayed put
// until the last frame and then jumped there: the whole view jumped as the tilt ended.
TEST(TerrainCamera, TiltInPlaceEasesTheCentreAltitude) {
    TerrainTransitionTest test;
    ASSERT_TRUE(test.settleOnPlateau());
    // Off the ground, as a camera is when 3D has just come on; nothing moves it back while the
    // centre stays put.
    test.map.jumpTo(CameraOptions().withCenterAltitude(0.0));
    test.renderFrame();
    ASSERT_NEAR(test.altitude(), 0.0, 1.0);
    const LatLng center = *test.map.getCameraOptions({}).center;

    // The centre altitude rather than the camera's height, which the tilt changes too; in the
    // mercator units Rise takes heights in.
    Rise rise;
    rise.add(test.altitude() * mercatorMetre);
    ASSERT_TRUE(test.runTransition(
        [&](const AnimationOptions& animation) {
            test.map.easeTo(CameraOptions().withCenter(center).withPitch(30.0).withCenterAltitude(plateauMeters),
                            animation);
        },
        [&](bool) { rise.add(test.altitude() * mercatorMetre); }));

    EXPECT_NEAR(test.altitude(), plateauMeters, 5.0);
    // Eased: the 1000 m rise takes most of the 1.5 s move, not its last frame.
    EXPECT_GE(rise.milliseconds(), 750.0);
}

// Heights that arrive in the last frames of a move were eased onto over what was left of it, a frame
// or two, and the view moved by hundreds of points on one of them (Timberline, 351 pt, heights 0.1 s
// before the end). The rest of the way takes 0.4 s at least instead, the move running on past its
// end until it is on the ground.
TEST(TerrainCamera, GroundArrivingAtTheEndOfAMoveIsEasedOnto) {
    TerrainTransitionTest test;
    test.holdWest = true;
    ASSERT_TRUE(test.settleOnPlateau());

    const auto start = Clock::now();
    bool released = false;
    Rise rise;
    rise.add(test.eye()[2]);
    ASSERT_TRUE(test.runTransition(
        [&](const AnimationOptions& animation) {
            test.map.easeTo(CameraOptions().withCenter(test.westTarget).withCenterAltitude(plateauMeters), animation);
        },
        [&](bool) {
            rise.add(test.eye()[2]);
            // 0.25 s before the 1.5 s move ends, the plateau's tiles arrive: their heights reach the
            // map a few frames later.
            if (!released && Clock::now() - start >= Milliseconds(1250)) {
                test.holdWest = false;
                released = true;
            }
            std::this_thread::sleep_for(Milliseconds(16));
        }));
    ASSERT_TRUE(released);

    // On the plateau as the move ended, the 1000 m rise eased over 0.4 s, where it took what was left
    // of the move, about 0.2 s.
    EXPECT_GE(rise.milliseconds(), 300.0);
    test.expectEyeAtEnd();
    EXPECT_NEAR(test.altitude(), westPlateauMeters, 5.0);
}

// A transition's frames are drawn at the times the map gives them, all at once in a still render.
// Whether new ground came too late for what is left of the move to carry the centre onto it, and
// the 0.4 s the centre takes onto it then, go by those times rather than by the wall clock.
TEST(TerrainCamera, LateGroundEasesOnTheFramesClock) {
    Transform transform;
    transform.resize({512, 512});
    transform.jumpTo(CameraOptions().withCenter(LatLng(0.0, 0.0)).withZoom(10.0).withCenterAltitude(0.0));
    transform.easeTo(CameraOptions().withCenter(LatLng(0.0, 0.01)).withCenterAltitude(0.0),
                     AnimationOptions(Seconds(1)));
    const TimePoint start = transform.getTransitionStart();
    transform.updateTransitions(start + Milliseconds(900));
    transform.retargetTransitionCenterAltitude(1000.0);

    // At the move's end, a quarter of the way through the 0.4 s ease...
    transform.updateTransitions(start + Milliseconds(1000));
    EXPECT_TRUE(transform.inTransition());
    EXPECT_LT(transform.getState().getCenterAltitude(), 500.0);
    // ...and on the ground 0.4 s after it came.
    transform.updateTransitions(start + Milliseconds(1300));
    EXPECT_FALSE(transform.inTransition());
    EXPECT_DOUBLE_EQ(transform.getState().getCenterAltitude(), 1000.0);
}

// Ground that changes again while the centre eases onto late ground (a finer tile arriving) is eased
// onto from where the altitude has got to, at the pace it has, within the same 0.4 s. Restarted from
// rest, the altitude stopped, and each change kept the move going 0.4 s more.
TEST(TerrainCamera, LateGroundChangingAgainKeepsTheEase) {
    Transform transform;
    transform.resize({512, 512});
    transform.jumpTo(CameraOptions().withCenter(LatLng(0.0, 0.0)).withZoom(10.0).withCenterAltitude(0.0));
    transform.easeTo(CameraOptions().withCenter(LatLng(0.0, 0.01)).withCenterAltitude(0.0),
                     AnimationOptions(Seconds(1)));
    const TimePoint start = transform.getTransitionStart();
    transform.updateTransitions(start + Milliseconds(900));
    transform.retargetTransitionCenterAltitude(1000.0);
    // Half way through the 0.4 s ease, the ground changes again.
    transform.updateTransitions(start + Milliseconds(1100));
    const double halfWay = transform.getState().getCenterAltitude();
    ASSERT_NEAR(halfWay, 500.0, 1.0);
    transform.retargetTransitionCenterAltitude(1200.0);

    transform.updateTransitions(start + Milliseconds(1150));
    EXPECT_GT(transform.getState().getCenterAltitude() - halfWay, 100.0) << "on the move";
    transform.updateTransitions(start + Milliseconds(1300));
    EXPECT_FALSE(transform.inTransition());
    EXPECT_DOUBLE_EQ(transform.getState().getCenterAltitude(), 1200.0);
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

// A setCenterAltitudeKeepingView that is refused moves nothing, and leaves the transition it would
// have cut short running: the clamp still keeps the centre that transition was given.
TEST(TerrainCamera, RefusedAltitudeChangeDuringATransitionKeepsItsCentre) {
    TerrainTransitionTest test;
    ASSERT_TRUE(test.settleOnPlateau());

    bool refused = false;
    ASSERT_TRUE(test.easeWest([&](bool finished) {
        // Well over the 2000 m plateau, so the clamp has a new height waiting.
        if (!finished && !refused && test.map.getCameraOptions({}).center->longitude() < -5.0) {
            // Already at that altitude: nothing to move.
            EXPECT_FALSE(test.map.setCenterAltitudeKeepingView(test.altitude()));
            refused = true;
        }
    }));
    ASSERT_TRUE(refused);

    const auto camera = test.map.getCameraOptions({});
    EXPECT_NEAR(camera.center->latitude(), test.westTarget.latitude(), 1e-6);
    EXPECT_NEAR(camera.center->longitude(), test.westTarget.longitude(), 1e-6);
    EXPECT_NEAR(*camera.centerAltitude, westPlateauMeters, 5.0);
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
