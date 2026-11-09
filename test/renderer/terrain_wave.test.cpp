#include <mln/test/util.hpp>
#include <mln/test/stub_file_source.hpp>
#include <mln/test/map_adapter.hpp>

#include <mln/gfx/headless_frontend.hpp>
#include <mln/map/camera.hpp>
#include <mln/map/map_options.hpp>
#include <mln/renderer/render_terrain.hpp>
#include <mln/style/image.hpp>
#include <mln/style/layer.hpp>
#include <mln/style/style.hpp>
#include <mln/style/terrain.hpp>
#include <mln/util/constants.hpp>
#include <mln/util/image.hpp>
#include <mln/util/projection.hpp>
#include <mln/util/run_loop.hpp>

#include <cmath>
#include <functional>
#include <numbers>
#include <string>

using namespace mln;

// The wave the terrain rises in as 3D opens (style::Terrain::Wave): the ground and everything
// standing on it rise together, so a label, a circle or a building never floats over ground still
// lying flat.

TEST(TerrainWave, HeightFollowsTheShadersCurve) {
    const std::array<float, 4> wave{{0, 0, 400, 100}};
    EXPECT_EQ(RenderTerrain::waveHeight({500, 0}, wave, 1.2f, 0.4f), 0.0f) << "ahead of the front, flat";
    EXPECT_EQ(RenderTerrain::waveHeight({0, 0}, wave, 1.2f, 0.4f), 1.0f) << "behind the band, whole";
    EXPECT_NEAR(RenderTerrain::waveHeight({360, 0}, wave, 1.2f, 0.4f), 1.2f, 1e-5) << "the crest";
    EXPECT_GT(RenderTerrain::waveHeight({340, 0}, wave, 1.2f, 0.4f), 1.0f) << "settling from it";
    EXPECT_NEAR(RenderTerrain::waveHeight({0, 350}, wave, 0.0f, 0.0f), 0.5f, 1e-5) << "no crest: halfway, half";
    EXPECT_EQ(RenderTerrain::waveHeight({500, 0}, {{0, 0, 0, 0}}, 1.2f, 0.4f), 1.0f) << "no wave, whole";
}

TEST(TerrainWave, DrawableWaveIsInTheTilesOwnUnits) {
    auto impl = makeMutable<style::Terrain::Impl>("dem", 1.0f);
    impl->wave = {.latitude = 0, .longitude = 0, .front = 1000, .width = 2000, .peak = 1, .crest = 0};
    const RenderTerrain terrain(std::move(impl));

    // At z0 the world is one tile: the equator and prime meridian are its middle, and a metre is
    // EXTENT / (2 pi R) of it there.
    const auto world = terrain.getDrawableWave(UnwrappedTileID(0, CanonicalTileID(0, 0, 0)));
    const double perMetre = util::EXTENT / (2 * std::numbers::pi * util::EARTH_RADIUS_M);
    EXPECT_NEAR(world[0], util::EXTENT / 2.0, 1e-3);
    EXPECT_NEAR(world[1], util::EXTENT / 2.0, 1e-3);
    EXPECT_NEAR(world[2], 1000 * perMetre, 1e-4);
    EXPECT_NEAR(world[3], 2000 * perMetre, 1e-4);

    // At z1 the centre is the top-left tile's bottom-right corner, at twice the units per metre;
    // the copy of the world one wrap east sees it a world to its west.
    const auto quarter = terrain.getDrawableWave(UnwrappedTileID(0, CanonicalTileID(1, 0, 0)));
    EXPECT_NEAR(quarter[0], util::EXTENT, 1e-3);
    EXPECT_NEAR(quarter[1], util::EXTENT, 1e-3);
    EXPECT_NEAR(quarter[2], 2000 * perMetre, 1e-4);
    const auto east = terrain.getDrawableWave(UnwrappedTileID(1, CanonicalTileID(1, 0, 0)));
    EXPECT_NEAR(east[0], -util::EXTENT, 1e-3);

    const auto shape = terrain.getWaveShape();
    EXPECT_EQ(shape[0], 1.0f);
    EXPECT_EQ(shape[1], 0.0f);

    auto still = makeMutable<style::Terrain::Impl>("dem", 1.0f);
    const RenderTerrain flat(std::move(still));
    const auto none = flat.getDrawableWave(UnwrappedTileID(0, CanonicalTileID(5, 3, 4)));
    EXPECT_EQ(none, (std::array<float, 4>{{0, 0, 0, 0}})) << "no wave";
}

namespace {

constexpr uint32_t demTileSize = 256;

// A Terrain-RGB DEM tile standing `height` metres everywhere
std::string makeDEMTile(double height) {
    PremultipliedImage image({demTileSize, demTileSize});
    const auto value = static_cast<uint32_t>(std::lround((height + 10000.0) * 10.0));
    for (uint32_t i = 0; i < demTileSize * demTileSize; ++i) {
        uint8_t* px = image.data.get() + i * 4;
        px[0] = static_cast<uint8_t>(value >> 16);
        px[1] = static_cast<uint8_t>(value >> 8);
        px[2] = static_cast<uint8_t>(value);
        px[3] = 255;
    }
    return encodePNG(image);
}

const LatLng spot{46.01, 11.0};

// A green circle, a red icon and a cyan block on one spot, each shown alone in turn.
constexpr auto plateauStyle = R"({
  "version": 8,
  "sources": {
    "dem": {
      "type": "raster-dem",
      "tiles": ["http://example.com/dem/{z}/{x}/{y}.png"],
      "encoding": "mapbox",
      "tileSize": 256,
      "maxzoom": 12
    },
    "spot": {"type": "geojson", "maxzoom": 14, "data": {"type": "Point", "coordinates": [11.0, 46.01]}},
    "block": {"type": "geojson", "maxzoom": 14,
              "data": {"type": "Polygon", "coordinates": [[[10.999, 46.0095], [11.001, 46.0095], [11.001, 46.0105],
                                                           [10.999, 46.0105], [10.999, 46.0095]]]}}
  },
  "terrain": {"source": "dem"},
  "layers": [
    {"id": "page", "type": "background", "paint": {"background-color": "#0000ff"}},
    {"id": "block", "type": "fill-extrusion", "source": "block",
     "paint": {"fill-extrusion-color": "#00ffff", "fill-extrusion-height": 100}},
    {"id": "dot", "type": "circle", "source": "spot", "paint": {"circle-radius": 8, "circle-color": "#00ff00"}},
    {"id": "icon", "type": "symbol", "source": "spot",
     "layout": {"icon-image": "red-box", "icon-allow-overlap": true, "icon-ignore-placement": true}}
  ]
})";

struct Centroid {
    int count = 0;
    double x = 0.0, y = 0.0;
};

Centroid centroid(const PremultipliedImage& image, const std::function<bool(int, int, int)>& match) {
    Centroid c;
    for (uint32_t y = 0; y < image.size.height; ++y) {
        for (uint32_t x = 0; x < image.size.width; ++x) {
            const uint8_t* px = image.data.get() + (y * image.size.width + x) * 4;
            if (match(px[0], px[1], px[2])) {
                c.count++;
                c.x += x;
                c.y += y;
            }
        }
    }
    if (c.count) {
        c.x /= c.count;
        c.y /= c.count;
    }
    return c;
}

struct Thing {
    const char* layer;
    std::function<bool(int, int, int)> match;
};

const std::vector<Thing> things{
    {"dot", [](int r, int g, int b) { return g > 200 && r < 60 && b < 60; }},
    {"icon", [](int r, int g, int b) { return r > 200 && g < 60 && b < 60; }},
    {"block", [](int r, int g, int b) { return g > 100 && b > 100 && r < 40; }},
};

struct PlateauTest {
    util::RunLoop loop;
    std::shared_ptr<StubFileSource> fileSource = std::make_shared<StubFileSource>(ResourceOptions::Default(),
                                                                                  ClientOptions());
    HeadlessFrontend frontend{{512, 512}, 1};
    MapAdapter map;

    explicit PlateauTest(double height)
        : map(frontend,
              MapObserver::nullObserver(),
              fileSource,
              MapOptions().withMapMode(MapMode::Static).withSize(frontend.getSize())) {
        fileSource->tileResponse = [height](const Resource&) -> std::optional<Response> {
            Response res;
            res.data = std::make_shared<std::string>(makeDEMTile(height));
            return res;
        };
        map.getStyle().loadJSON(plateauStyle);
        PremultipliedImage box({20, 20});
        for (uint32_t i = 0; i < 20 * 20; ++i) {
            uint8_t* px = box.data.get() + i * 4;
            px[0] = 255;
            px[1] = px[2] = 0;
            px[3] = 255;
        }
        map.getStyle().addImage(std::make_unique<style::Image>("red-box", std::move(box), 1.0f));
    }

    void roll(float front, float width) {
        auto terrain = std::make_unique<style::Terrain>("dem", 1.0f);
        terrain->setWave({.latitude = spot.latitude(),
                          .longitude = spot.longitude(),
                          .front = front,
                          .width = width,
                          .peak = 0,
                          .crest = 0});
        map.getStyle().setTerrain(std::move(terrain));
    }

    Centroid where(const Thing& thing) {
        for (const auto& other : things) {
            map.getStyle()
                .getLayer(other.layer)
                ->setVisibility(other.layer == thing.layer ? style::VisibilityType::Visible
                                                           : style::VisibilityType::None);
        }
        map.jumpTo(
            CameraOptions().withCenter(LatLng{46.0, 11.0}).withZoom(13.0).withPitch(50.0).withCenterAltitude(0.0));
        // The second render draws with the heights the first loaded
        frontend.render(map);
        return centroid(frontend.render(map).image, thing.match);
    }
};

void expectSamePlace(const Centroid& seen, const Centroid& expected, const char* layer, const char* what) {
    ASSERT_GT(seen.count, 20) << layer << " is drawn " << what;
    ASSERT_GT(expected.count, 20) << layer << " is drawn on the reference ground";
    EXPECT_NEAR(seen.x, expected.x, 1.5) << layer << " " << what;
    EXPECT_NEAR(seen.y, expected.y, 1.5) << layer << " " << what;
}

} // namespace

// Ahead of the front the ground lies flat, and the circle, icon and block on it stand at sea level
// with it rather than on the 1000 m plateau the ground will rise to.
TEST(TerrainWave, AheadOfTheFrontEverythingLiesFlat) {
    PlateauTest seaLevel(0.0), plateau(1000.0);
    plateau.roll(0, 2000);
    for (const auto& thing : things) {
        expectSamePlace(plateau.where(thing), seaLevel.where(thing), thing.layer, "ahead of the front");
    }
}

// Halfway through its rise the ground stands at half its height, with no crest, and so does
// everything on it.
TEST(TerrainWave, HalfRisenEverythingStandsHalfway) {
    PlateauTest half(500.0), plateau(1000.0);
    // A band 200 km wide, so the ground around the spot stands level at half its height rather
    // than as a dome on which the block's walls would show more than on level ground.
    plateau.roll(100000, 200000);
    for (const auto& thing : things) {
        expectSamePlace(plateau.where(thing), half.where(thing), thing.layer, "halfway through its rise");
    }
}

// Once the wave has passed, everything stands on the ground at its full height.
TEST(TerrainWave, PastTheWaveEverythingStandsWhole) {
    PlateauTest whole(1000.0), plateau(1000.0);
    plateau.roll(1.0e6f, 2000);
    for (const auto& thing : things) {
        expectSamePlace(plateau.where(thing), whole.where(thing), thing.layer, "once the wave has passed");
    }
}
