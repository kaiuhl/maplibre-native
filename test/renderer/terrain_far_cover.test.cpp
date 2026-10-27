#include <mln/test/util.hpp>
#include <mln/test/stub_file_source.hpp>
#include <mln/test/map_adapter.hpp>

#include <mln/gfx/headless_frontend.hpp>
#include <mln/map/bound_options.hpp>
#include <mln/map/camera.hpp>
#include <mln/map/map_options.hpp>
#include <mln/map/mode.hpp>
#include <mln/math/angles.hpp>
#include <mln/storage/resource.hpp>
#include <mln/style/image.hpp>
#include <mln/style/style.hpp>
#include <mln/util/image.hpp>
#include <mln/util/run_loop.hpp>

#include <cmath>
#include <map>
#include <sstream>
#include <string>
#include <tuple>

using namespace mln;

namespace {

// A summit shaped like Mt Hood's: 3429 m at the top, falling away with distance the way Hood's
// flanks do (2664 m 600 m out, the 8737 ft of Pulpit Rock in the report; 1800 m at 3.5 km; 1270 m
// at 10 km; 980 m at 20 km).
const LatLng summitPeak{45.3736, -121.6959};

double summitHeight(double latitude, double longitude) {
    const double north = (latitude - summitPeak.latitude()) * 110540.0;
    const double east = (longitude - summitPeak.longitude()) * 111320.0 *
                        std::cos(util::deg2rad(summitPeak.latitude()));
    return 300.0 + 3129.0 / (1.0 + std::pow(std::hypot(north, east) / 3105.0, 0.686));
}

// The summit as a 256 px Terrarium tile: (R * 256 + G + B / 256) - 32768
std::string summitDEMTile(int z, int x, int y) {
    constexpr uint32_t size = 256;
    PremultipliedImage image({size, size});
    const double tiles = std::exp2(z);
    for (uint32_t row = 0; row < size; ++row) {
        const double my = (y + (row + 0.5) / size) / tiles;
        const double latitude = util::rad2deg(std::atan(std::sinh(M_PI * (1.0 - 2.0 * my))));
        for (uint32_t col = 0; col < size; ++col) {
            const double longitude = (x + (col + 0.5) / size) / tiles * 360.0 - 180.0;
            const double value = summitHeight(latitude, longitude) + 32768.0;
            uint8_t* px = image.data.get() + (row * size + col) * 4;
            px[0] = static_cast<uint8_t>(std::floor(value / 256.0));
            px[1] = static_cast<uint8_t>(static_cast<int>(std::floor(value)) % 256);
            px[2] = static_cast<uint8_t>(std::floor((value - std::floor(value)) * 256.0));
            px[3] = 255;
        }
    }
    return encodePNG(image);
}

// Glassing's terrain source (256 px Terrarium to z15) under a red polygon on a white page, and blue
// map pins over the mountain's flanks out to ~15 km. The page is drawn under the terrain, not
// draped, so wherever no terrain is drawn the view shows white.
std::string farCoverStyle() {
    std::ostringstream pins;
    bool first = true;
    for (double lat = 45.26; lat <= 45.50; lat += 0.012) {
        for (double lon = -121.86; lon <= -121.46; lon += 0.016) {
            pins << (first ? "" : ",")
                 << R"({"type":"Feature","properties":{},"geometry":{"type":"Point","coordinates":[)" << lon << ","
                 << lat << "]}}";
            first = false;
        }
    }
    return R"({
  "version": 8,
  "sources": {
    "dem": {
      "type": "raster-dem",
      "tiles": ["http://example.com/{z}-{x}-{y}.png"],
      "encoding": "terrarium",
      "maxzoom": 15,
      "tileSize": 256
    },
    "land": {
      "type": "geojson",
      "data": {"type": "Polygon", "coordinates": [[[-179, -80], [179, -80], [179, 80], [-179, 80], [-179, -80]]]}
    },
    "pins": {"type": "geojson", "data": {"type": "FeatureCollection", "features": [)" +
           pins.str() + R"(]}}
  },
  "terrain": {"source": "dem", "exaggeration": 1.0},
  "layers": [
    {"id": "page", "type": "background", "paint": {"background-color": "#ffffff"}},
    {"id": "land", "type": "fill", "source": "land", "paint": {"fill-color": "#ff0000"}},
    {"id": "pins", "type": "symbol", "source": "pins",
     "layout": {"icon-image": "pin", "icon-allow-overlap": true, "icon-ignore-placement": true}}
  ]
})";
}

const uint8_t* pixelAt(const PremultipliedImage& image, uint32_t x, uint32_t y) {
    return image.data.get() + (static_cast<size_t>(y) * image.size.width + x) * 4;
}

bool isPage(const uint8_t* px) {
    return px[0] > 230 && px[1] > 230 && px[2] > 230;
}

bool isPin(const uint8_t* px) {
    return px[2] > 150 && px[0] < 90 && px[1] < 90;
}

// A phone at the app's tilt cap, at its deepest zoom, standing on Mt Hood's north flank by Pulpit
// Rock and looking ESE down the mountain: the view Kyle reported, with ~45% of the top of the screen
// blank and map pins floating in it.
struct SummitView {
    util::RunLoop loop;
    std::shared_ptr<StubFileSource> fileSource = std::make_shared<StubFileSource>(ResourceOptions::Default(),
                                                                                  ClientOptions());
    std::map<std::tuple<int, int, int>, std::string> tiles;
    std::map<int, int> requestsByZoom;
    HeadlessFrontend frontend{{402, 874}, 1};
    MapAdapter map;

    explicit SummitView(TerrainLoadMode mode)
        : map(frontend,
              MapObserver::nullObserver(),
              fileSource,
              MapOptions().withMapMode(MapMode::Static).withSize(frontend.getSize())) {
        fileSource->tileResponse = [this](const Resource& resource) {
            Response res;
            const auto& t = *resource.tileData;
            requestsByZoom[t.z]++;
            auto& tile = tiles[{t.z, t.x, t.y}];
            if (tile.empty()) {
                tile = summitDEMTile(t.z, t.x, t.y);
            }
            res.data = std::make_shared<std::string>(tile);
            return res;
        };
        map.getStyle().loadJSON(farCoverStyle());
        PremultipliedImage pin({3, 3});
        for (size_t i = 0; i < 9; ++i) {
            uint8_t* px = pin.data.get() + i * 4;
            px[0] = 0;
            px[1] = 0;
            px[2] = 255;
            px[3] = 255;
        }
        map.getStyle().addImage(std::make_unique<style::Image>("pin", std::move(pin), 1.0f));
        map.setBounds(BoundOptions().withMaxPitch(85.0));
        map.setTerrainLoadMode(mode);
    }

    // A still image draws what one frame's budgets allow: the Balanced and Performance load modes
    // defer new tile builds and drape renders past theirs to later frames, as the app's map does
    // while it settles. So it is rendered again until it no longer changes, the view the app
    // settles on.
    PremultipliedImage look(double pitch) {
        const LatLng pulpitRock{45.3790, -121.6960};
        map.jumpTo(CameraOptions()
                       .withCenter(pulpitRock)
                       .withZoom(17.0)
                       .withPitch(pitch)
                       .withBearing(116.0)
                       .withCenterAltitude(summitHeight(pulpitRock.latitude(), pulpitRock.longitude())));
        PremultipliedImage image = frontend.render(map).image;
        // A frame can change nothing on screen while work is still deferred (a drape re-rendered
        // for ground hidden behind the mountain), so it is settled after three unchanged ones.
        int unchanged = 0;
        for (int frame = 0; frame < 60 && unchanged < 3; ++frame) {
            PremultipliedImage next = frontend.render(map).image;
            unchanged = next == image ? unchanged + 1 : 0;
            image = std::move(next);
        }
        return image;
    }
};

struct FarCoverCase {
    const char* name;
    TerrainLoadMode mode;
    double pitch;
};

// The app's tilt cap on this 874 pt view is 56.6 deg (TerrainTilt: 90 - 15 - the 18.4 deg above
// the centre); 54 is the cap Kyle's earlier phones had, and 60 the app's ceiling.
const FarCoverCase farCoverCases[] = {{"Balanced, 54 deg", TerrainLoadMode::Balanced, 54.0},
                                      {"Balanced, app's cap", TerrainLoadMode::Balanced, 56.57},
                                      {"Balanced, 60 deg", TerrainLoadMode::Balanced, 60.0},
                                      {"Performance, app's cap", TerrainLoadMode::Performance, 56.57}};

} // namespace

// At z17 on a summit, tilted to the app's cap, the view looks kilometres down the mountain. Every
// row of it is ground, and the terrain has to draw all of it: before, the mesh kept only the 64
// tiles nearest the camera (24 in Performance), all at z17, and the farther ground showed the page.
TEST(TerrainFarCover, SummitViewDrawsTheGroundToTheTopEdge) {
    for (const auto& view : farCoverCases) {
        SummitView test(view.mode);
        const PremultipliedImage image = test.look(view.pitch);
        ASSERT_EQ(image.size.width, 402u);
        int blankRows = 0;
        int topmostBlank = -1;
        for (uint32_t y = 0; y < image.size.height; y += 4) {
            bool blank = false;
            for (const uint32_t x : {8u, 100u, 201u, 300u, 393u}) {
                blank |= isPage(pixelAt(image, x, y));
            }
            if (blank) {
                ++blankRows;
                if (topmostBlank < 0) topmostBlank = static_cast<int>(y);
            }
        }
        EXPECT_EQ(blankRows, 0) << view.name << ": the page shows through in " << blankRows << " of "
                                << image.size.height / 4 << " rows sampled, from row " << topmostBlank;
    }
}

// Map pins stand on the terrain: every pin drawn has ground drawn around it. Before, pins on the
// ground past the mesh's reach were placed and drawn over the blank page.
TEST(TerrainFarCover, PinsStandOnDrawnTerrain) {
    int allPins = 0;
    for (const auto& view : farCoverCases) {
        SummitView test(view.mode);
        const PremultipliedImage image = test.look(view.pitch);
        int pins = 0;
        int floating = 0;
        for (uint32_t y = 4; y + 4 < image.size.height; ++y) {
            for (uint32_t x = 4; x + 4 < image.size.width; ++x) {
                if (!isPin(pixelAt(image, x, y))) continue;
                ++pins;
                // The ground beside the 3 px pin, a few pixels out on each side
                if (isPage(pixelAt(image, x - 4, y)) || isPage(pixelAt(image, x + 4, y)) ||
                    isPage(pixelAt(image, x, y - 4)) || isPage(pixelAt(image, x, y + 4))) {
                    ++floating;
                }
            }
        }
        EXPECT_EQ(floating, 0) << view.name << ": " << floating << " of " << pins << " pin pixels over the page";
        allPins += pins;
    }
    // Pins farther than five times the camera-to-centre distance are not placed (CollisionIndex),
    // so a shallow view can show none
    EXPECT_GT(allPins, 0);
}

// The DEM source loads the DEM tiles of the mesh's coarse far tiles as well as its own cover's: in
// the Performance load mode the mesh draws the ground down the mountain with tiles coarser than
// the DEM's maxzoom, and without their own DEM tiles it drew them flat, away from the ground.
TEST(TerrainFarCover, CoarseMeshTilesLoadTheirDEMTiles) {
    SummitView test(TerrainLoadMode::Performance);
    test.look(56.57);
    std::string zooms;
    for (const auto& [z, n] : test.requestsByZoom) {
        zooms += " z" + std::to_string(z) + ":" + std::to_string(n);
    }
    ASSERT_FALSE(test.requestsByZoom.empty());
    EXPECT_LT(test.requestsByZoom.begin()->first, 15) << "DEM requests by zoom:" << zooms;
}
