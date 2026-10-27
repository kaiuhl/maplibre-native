#include <mln/test/util.hpp>
#include <mln/test/stub_file_source.hpp>
#include <mln/test/map_adapter.hpp>

#include <mln/gfx/headless_frontend.hpp>
#include <mln/map/camera.hpp>
#include <mln/map/map_options.hpp>
#include <mln/style/image.hpp>
#include <mln/style/layer.hpp>
#include <mln/style/style.hpp>
#include <mln/util/chrono.hpp>
#include <mln/util/image.hpp>
#include <mln/util/projection.hpp>
#include <mln/util/run_loop.hpp>

#include <cmath>
#include <functional>
#include <string>

using namespace mln;

namespace {

constexpr uint32_t demTileSize = 256;

// A Terrain-RGB DEM tile whose height at each pixel is `heightAt` the pixel's latitude
std::string makeDEMTile(const CanonicalTileID& id, const std::function<double(double)>& heightAt) {
    PremultipliedImage image({demTileSize, demTileSize});
    const double tiles = std::ldexp(1.0, id.z);
    for (uint32_t y = 0; y < demTileSize; ++y) {
        const double worldY = (id.y + (y + 0.5) / demTileSize) / tiles * util::tileSize_D;
        const double lat = Projection::unproject({0.0, worldY}, 1.0).latitude();
        const auto value = static_cast<uint32_t>(std::lround((heightAt(lat) + 10000.0) * 10.0));
        for (uint32_t x = 0; x < demTileSize; ++x) {
            uint8_t* px = image.data.get() + (y * demTileSize + x) * 4;
            px[0] = static_cast<uint8_t>(value >> 16);
            px[1] = static_cast<uint8_t>(value >> 8);
            px[2] = static_cast<uint8_t>(value);
            px[3] = 255;
        }
    }
    return encodePNG(image);
}

// A 1500 m ridge running east-west between 46.010°N and 46.014°N, on ground at sea level
double ridge(double lat) {
    return lat >= 46.010 && lat <= 46.014 ? 1500.0 : 0.0;
}

// The camera looks north across the ridge from 46°N, tilted so the ground well past it would be
// in view were the ridge not there. Red things stand past the ridge, green ones short of it, a
// white dot on top of it; yellow and cyan blocks past it and short of it. The ground is draped
// blue. The GeoJSON sources have no tile buffer: the ridge lies on the boundary between two z12
// tiles, and a tile draws the points and blocks in its buffer as well as its own (points only off
// MapMode::Continuous, CircleLayout), standing them on the edge of its DEM, here the ridge top:
// copies 1500 m above where they belong.
constexpr auto ridgeStyle = R"({
  "version": 8,
  "sources": {
    "dem": {
      "type": "raster-dem",
      "tiles": ["http://example.com/dem/{z}/{x}/{y}.png"],
      "encoding": "mapbox",
      "tileSize": 256,
      "maxzoom": 12
    },
    "points": {
      "type": "geojson",
      "maxzoom": 14,
      "buffer": 0,
      "data": {"type": "FeatureCollection", "features": [
        {"type": "Feature", "properties": {"kind": "dot", "color": "#ff0000"},
         "geometry": {"type": "Point", "coordinates": [11.0, 46.025]}},
        {"type": "Feature", "properties": {"kind": "dot", "color": "#00ff00"},
         "geometry": {"type": "Point", "coordinates": [10.996, 46.004]}},
        {"type": "Feature", "properties": {"kind": "dot", "color": "#ffffff"},
         "geometry": {"type": "Point", "coordinates": [11.004, 46.012]}},
        {"type": "Feature", "properties": {"kind": "icon", "icon": "red-box"},
         "geometry": {"type": "Point", "coordinates": [11.006, 46.025]}},
        {"type": "Feature", "properties": {"kind": "icon", "icon": "green-box"},
         "geometry": {"type": "Point", "coordinates": [11.004, 46.004]}}
      ]}
    },
    "blocks": {
      "type": "geojson",
      "maxzoom": 14,
      "buffer": 0,
      "data": {"type": "FeatureCollection", "features": [
        {"type": "Feature", "properties": {"color": "#ffff00"},
         "geometry": {"type": "Polygon", "coordinates": [[[10.990, 46.028], [10.996, 46.028], [10.996, 46.032],
                                                          [10.990, 46.032], [10.990, 46.028]]]}},
        {"type": "Feature", "properties": {"color": "#00ffff"},
         "geometry": {"type": "Polygon", "coordinates": [[[11.006, 46.005], [11.010, 46.005], [11.010, 46.007],
                                                          [11.006, 46.007], [11.006, 46.005]]]}}
      ]}
    }
  },
  "terrain": {"source": "dem"},
  "layers": [
    {"id": "page", "type": "background", "paint": {"background-color": "#0000ff"}},
    {"id": "blocks", "type": "fill-extrusion", "source": "blocks",
     "paint": {"fill-extrusion-color": ["get", "color"], "fill-extrusion-height": 300}},
    {"id": "dots", "type": "circle", "source": "points", "filter": ["==", ["get", "kind"], "dot"],
     "paint": {"circle-radius": 10, "circle-color": ["get", "color"]}},
    {"id": "icons", "type": "symbol", "source": "points", "filter": ["==", ["get", "kind"], "icon"],
     "layout": {"icon-image": ["get", "icon"], "icon-allow-overlap": true, "icon-ignore-placement": true}}
  ]
})";

std::unique_ptr<style::Image> solidImage(const std::string& id, uint8_t r, uint8_t g, uint8_t b) {
    PremultipliedImage image({20, 20});
    for (uint32_t i = 0; i < 20 * 20; ++i) {
        uint8_t* px = image.data.get() + i * 4;
        px[0] = r;
        px[1] = g;
        px[2] = b;
        px[3] = 255;
    }
    return std::make_unique<style::Image>(id, std::move(image), 1.0f);
}

struct Counts {
    int red = 0, green = 0, white = 0, yellow = 0, cyan = 0;
};

Counts countColours(const PremultipliedImage& image) {
    Counts counts;
    for (uint32_t i = 0; i < image.size.width * image.size.height; ++i) {
        const uint8_t* px = image.data.get() + i * 4;
        const int r = px[0], g = px[1], b = px[2];
        if (r > 200 && g < 60 && b < 60) counts.red++;
        if (g > 200 && r < 60 && b < 60) counts.green++;
        if (r > 200 && g > 200 && b > 200) counts.white++;
        if (r > 100 && g > 100 && b < 40) counts.yellow++;
        if (g > 100 && b > 100 && r < 40) counts.cyan++;
    }
    return counts;
}

struct RidgeTest {
    util::RunLoop loop;
    std::shared_ptr<StubFileSource> fileSource = std::make_shared<StubFileSource>(ResourceOptions::Default(),
                                                                                  ClientOptions());
    HeadlessFrontend frontend;
    MapAdapter map;

    RidgeTest(const std::string& style, const std::function<double(double)>& heightAt, Size size = {512, 512})
        : frontend(size, 1),
          map(frontend,
              MapObserver::nullObserver(),
              fileSource,
              MapOptions().withMapMode(MapMode::Static).withSize(frontend.getSize())) {
        fileSource->tileResponse = [heightAt](const Resource& resource) -> std::optional<Response> {
            Response res;
            const auto& tile = resource.tileData;
            res.data = std::make_shared<std::string>(makeDEMTile(CanonicalTileID(tile->z, tile->x, tile->y), heightAt));
            return res;
        };
        map.getStyle().loadJSON(style);
        map.getStyle().addImage(solidImage("red-box", 255, 0, 0));
        map.getStyle().addImage(solidImage("green-box", 0, 255, 0));
    }

    PremultipliedImage render(const CameraOptions& camera) {
        map.jumpTo(camera);
        // The second render draws with the heights the first loaded (the map side's height copy)
        frontend.render(map);
        return frontend.render(map).image;
    }
};

// From about 5 km up, 8.8 km south of the middle of the map, tilted 60 degrees, the most the
// tile covers keep one zoom at: the ridge's top shows as a band some 20 degrees below the camera,
// so the dot on it is seen at a clear angle, and the ground past the ridge is behind its face.
const CameraOptions acrossTheRidge =
    CameraOptions().withCenter(LatLng{46.0, 11.0}).withZoom(12.0).withPitch(60.0).withBearing(0.0);

} // namespace

// The control: on flat ground everything past where the ridge would be is in view, so the
// counts below are of things the ridge hides, not of things out of frame.
TEST(TerrainOcclusion, EverythingIsInViewWithoutTheRidge) {
    RidgeTest test(ridgeStyle, [](double) { return 0.0; });
    const auto counts = countColours(test.render(acrossTheRidge));
    EXPECT_GT(counts.red, 200) << "the dot and icon past the ridge's line";
    EXPECT_GT(counts.green, 200) << "the dot and icon short of it";
    EXPECT_GT(counts.yellow, 100) << "the block past it";
    EXPECT_GT(counts.cyan, 100) << "the block short of it";
}

// A ridge hides the circles, icons and extrusions behind it, as maplibre-gl-js does, and leaves
// those in front of it, and on top of it, drawn whole.
TEST(TerrainOcclusion, TheRidgeHidesWhatIsBehindIt) {
    RidgeTest test(ridgeStyle, ridge);
    const auto counts = countColours(test.render(acrossTheRidge));
    EXPECT_EQ(counts.red, 0) << "the dot and icon behind the ridge";
    EXPECT_EQ(counts.yellow, 0) << "the block behind the ridge";
    EXPECT_GT(counts.green, 200) << "the dot and icon in front of it";
    EXPECT_GT(counts.cyan, 100) << "the block in front of it";
    EXPECT_GT(counts.white, 100) << "the dot on top of it";
}

// A layer told to keep some opacity behind the terrain draws a faint copy there, red over the
// blue ground at a quarter strength, where the default hides it.
TEST(TerrainOcclusion, ALayerCanStayFaintBehindTheRidge) {
    RidgeTest test(ridgeStyle, ridge);
    const auto faintRed = [](const PremultipliedImage& image) {
        int count = 0;
        for (uint32_t i = 0; i < image.size.width * image.size.height; ++i) {
            const uint8_t* px = image.data.get() + i * 4;
            if (px[0] > 40 && px[0] < 110 && px[1] < 30 && px[2] > 140) count++;
        }
        return count;
    };
    EXPECT_EQ(faintRed(test.render(acrossTheRidge)), 0) << "hidden by default";

    for (const auto* id : {"dots", "icons"}) {
        test.map.getStyle().getLayer(id)->setTerrainOccludedOpacity(0.25f);
    }
    EXPECT_EQ(test.map.getStyle().getLayer("dots")->getTerrainOccludedOpacity(), 0.25f);
    const auto image = test.render(acrossTheRidge);
    EXPECT_GT(faintRed(image), 200) << "the dot and icon behind the ridge, faint";
    EXPECT_EQ(countColours(image).red, 0) << "and never at full strength";
}

namespace {

// The same dot, green from a source as deep as the DEM, red from one that stops at z8, far
// shallower than the z12 DEM tiles the view loads: no DEM tile covers the shallow one's tile.
constexpr auto plateauStyle = R"({
  "version": 8,
  "sources": {
    "dem": {
      "type": "raster-dem",
      "tiles": ["http://example.com/dem/{z}/{x}/{y}.png"],
      "encoding": "mapbox",
      "tileSize": 256,
      "minzoom": 11,
      "maxzoom": 12
    },
    "deep": {"type": "geojson", "maxzoom": 14,
             "data": {"type": "Point", "coordinates": [11.0, 46.01]}},
    "shallow": {"type": "geojson", "maxzoom": 8,
                "data": {"type": "Point", "coordinates": [11.0, 46.01]}}
  },
  "terrain": {"source": "dem"},
  "layers": [
    {"id": "page", "type": "background", "paint": {"background-color": "#0000ff"}},
    {"id": "deep", "type": "circle", "source": "deep", "paint": {"circle-radius": 8, "circle-color": "#00ff00"}},
    {"id": "shallow", "type": "circle", "source": "shallow", "paint": {"circle-radius": 8, "circle-color": "#ff0000"}}
  ]
})";

struct Centroid {
    int count = 0;
    double x = 0.0, y = 0.0;
};

Centroid centroid(const PremultipliedImage& image, bool red) {
    Centroid c;
    for (uint32_t y = 0; y < image.size.height; ++y) {
        for (uint32_t x = 0; x < image.size.width; ++x) {
            const uint8_t* px = image.data.get() + (y * image.size.width + x) * 4;
            const bool match = red ? (px[0] > 200 && px[1] < 60) : (px[1] > 200 && px[0] < 60);
            if (match) {
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

} // namespace

// A circle from a source shallower than the DEM stands on the terrain where one from a deep source
// does, on a 1000 m plateau, rather than at sea level under it.
TEST(TerrainOcclusion, ShallowSourceStandsOnTheTerrain) {
    RidgeTest test(plateauStyle, [](double) { return 1000.0; });
    const auto camera = CameraOptions().withCenter(LatLng{46.0, 11.0}).withZoom(12.0).withPitch(50.0);

    test.map.getStyle().getLayer("shallow")->setVisibility(style::VisibilityType::None);
    const auto deep = centroid(test.render(camera), false);
    test.map.getStyle().getLayer("shallow")->setVisibility(style::VisibilityType::Visible);
    test.map.getStyle().getLayer("deep")->setVisibility(style::VisibilityType::None);
    const auto shallow = centroid(test.render(camera), true);

    ASSERT_GT(deep.count, 50);
    ASSERT_GT(shallow.count, 50) << "the shallow source's dot is drawn";
    EXPECT_NEAR(shallow.x, deep.x, 1.5);
    EXPECT_NEAR(shallow.y, deep.y, 1.5) << "at the deep source's height";
}

namespace {

// Where a 1000 m wall 100 m thick runs east-west: midway between two rows of pixels of the z8 DEM
// tile over 46.01°N, which then has none of it, while the z12 tiles have it four pixels thick.
double wallLatitude() {
    const double y = Projection::project(LatLng{46.01, 11.0}, 8).y; // in z8 tiles
    const double row = std::floor((y - std::floor(y)) * demTileSize - 0.5);
    const double between = std::floor(y) + (row + 1.0) / demTileSize;
    return Projection::unproject({0.0, between / std::ldexp(1.0, 8) * util::tileSize_D}, 1.0).latitude();
}

double wall(double lat) {
    return std::abs(lat - wallLatitude()) <= 0.00045 ? 1000.0 : 0.0;
}

// A moving map prefetches the DEM four zooms above the z12 tiles it shows: the z8 tile over the
// wall, which misses it. The shallow source stops at z10, between the two.
std::string wallStyle() {
    const std::string point = R"({"type": "Point", "coordinates": [11.0, )" + std::to_string(wallLatitude()) + "]}";
    return R"({
  "version": 8,
  "sources": {
    "dem": {
      "type": "raster-dem",
      "tiles": ["http://example.com/dem/{z}/{x}/{y}.png"],
      "encoding": "mapbox",
      "tileSize": 256,
      "maxzoom": 12
    },
    "deep": {"type": "geojson", "maxzoom": 14, "data": )" +
           point + R"(},
    "shallow": {"type": "geojson", "maxzoom": 10, "data": )" +
           point + R"(}
  },
  "terrain": {"source": "dem"},
  "layers": [
    {"id": "page", "type": "background", "paint": {"background-color": "#0000ff"}},
    {"id": "deep", "type": "circle", "source": "deep", "paint": {"circle-radius": 8, "circle-color": "#00ff00"}},
    {"id": "shallow", "type": "circle", "source": "shallow", "paint": {"circle-radius": 8, "circle-color": "#ff0000"}}
  ]
})";
}

struct MovingMapTest {
    util::RunLoop loop;
    std::shared_ptr<StubFileSource> fileSource = std::make_shared<StubFileSource>(ResourceOptions::Default(),
                                                                                  ClientOptions());
    HeadlessFrontend frontend{{512, 512}, 1};
    MapAdapter map{frontend,
                   MapObserver::nullObserver(),
                   fileSource,
                   MapOptions().withMapMode(MapMode::Continuous).withSize(frontend.getSize())};

    MovingMapTest(const std::string& style, const std::function<double(double)>& heightAt) {
        fileSource->tileResponse = [heightAt](const Resource& resource) -> std::optional<Response> {
            Response res;
            const auto& tile = resource.tileData;
            res.data = std::make_shared<std::string>(makeDEMTile(CanonicalTileID(tile->z, tile->x, tile->y), heightAt));
            return res;
        };
        map.getStyle().loadJSON(style);
    }

    // Draws frames until everything has loaded, and a few more for what the frames after a load
    // draw (the DEMs sampled a few a frame)
    PremultipliedImage render(const CameraOptions& camera) {
        map.jumpTo(camera);
        const auto deadline = Clock::now() + Seconds(10);
        int settled = 0;
        while (settled < 10 && Clock::now() < deadline) {
            loop.runOnce();
            frontend.renderFrame();
            settled = map.isFullyLoaded() ? settled + 1 : 0;
        }
        return frontend.readStillImage();
    }
};

} // namespace

// On a moving map a circle from a source shallower than the DEM stands on the heights the terrain
// is drawn with, as one from a deep source does, though a DEM tile zooms coarser, prefetched, has
// none of the 1000 m wall it stands on.
TEST(TerrainOcclusion, ShallowSourceStandsOnTheFineHeightsNotTheCoarseTile) {
    MovingMapTest test(wallStyle(), wall);
    const auto camera = CameraOptions().withCenter(LatLng{46.0, 11.0}).withZoom(12.0).withPitch(50.0);

    test.map.getStyle().getLayer("shallow")->setVisibility(style::VisibilityType::None);
    const auto deep = centroid(test.render(camera), false);
    test.map.getStyle().getLayer("shallow")->setVisibility(style::VisibilityType::Visible);
    test.map.getStyle().getLayer("deep")->setVisibility(style::VisibilityType::None);
    const auto shallow = centroid(test.render(camera), true);

    ASSERT_GT(deep.count, 50);
    ASSERT_GT(shallow.count, 50) << "the shallow source's dot is drawn";
    EXPECT_NEAR(shallow.x, deep.x, 1.5);
    EXPECT_NEAR(shallow.y, deep.y, 1.5) << "on the wall, at the deep source's height";
}

namespace {

// Red dots every 500 m north of 46°N, 20 km of them, up a tilted phone's view.
std::string farDotsStyle() {
    std::string features;
    for (int k = 1; k <= 40; ++k) {
        features += std::string(k > 1 ? "," : "") +
                    R"({"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": [11.0, )" +
                    std::to_string(46.0 + k * 0.0045) + "]}}";
    }
    return R"({
  "version": 8,
  "sources": {
    "dem": {
      "type": "raster-dem",
      "tiles": ["http://example.com/dem/{z}/{x}/{y}.png"],
      "encoding": "mapbox",
      "tileSize": 256,
      "maxzoom": 12
    },
    "dots": {"type": "geojson", "maxzoom": 14, "buffer": 0,
             "data": {"type": "FeatureCollection", "features": [)" +
           features + R"(]}}
  },
  "terrain": {"source": "dem"},
  "layers": [
    {"id": "page", "type": "background", "paint": {"background-color": "#0000ff"}},
    {"id": "dots", "type": "circle", "source": "dots",
     "paint": {"circle-radius": 5, "circle-color": "#ff0000", "circle-pitch-scale": "viewport"}}
  ]
})";
}

} // namespace

// Flat ground hides nothing on it, to the top of a phone-shaped view tilted 65 degrees, where the
// ground is some three times as far as the middle of the map: the terrain's packed depth comes back as
// far as it went in, not a few percent nearer, which faded the farthest labels in plain view.
TEST(TerrainOcclusion, FlatGroundHidesNothingToTheTopOfATallView) {
    RidgeTest test(farDotsStyle(), [](double) { return 0.0; }, {402, 874});
    const auto image = test.render(CameraOptions().withCenter(LatLng{46.0, 11.0}).withZoom(12.5).withPitch(65.0));

    int inView = 0;
    double farthest = 0.0;
    for (int k = 1; k <= 40; ++k) {
        const LatLng at{46.0 + k * 0.0045, 11.0};
        const auto p = test.map.pixelForLatLng(at);
        const auto x = static_cast<int>(std::lround(p.x));
        const auto y = static_cast<int>(std::lround(p.y));
        if (y < 8 || y >= static_cast<int>(image.size.height) - 8) {
            continue;
        }
        ++inView;
        farthest = std::max(farthest, (at.latitude() - 46.0) * 111.32);
        int strongest = 0;
        for (int dy = -2; dy <= 2; ++dy) {
            for (int dx = -2; dx <= 2; ++dx) {
                const uint8_t* px = image.data.get() + ((y + dy) * image.size.width + (x + dx)) * 4;
                if (px[1] < 40 && px[2] < 40) {
                    strongest = std::max<int>(strongest, px[0]);
                }
            }
        }
        EXPECT_GT(strongest, 240) << "the dot " << (at.latitude() - 46.0) * 111.32 << " km north is drawn whole";
    }
    EXPECT_GE(inView, 20);
    EXPECT_GT(farthest, 15.0) << "km north of the middle of the map, near the top of the view";
}
