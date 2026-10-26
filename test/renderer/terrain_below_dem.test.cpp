#include <mln/test/util.hpp>
#include <mln/test/stub_file_source.hpp>
#include <mln/test/map_adapter.hpp>

#include <mln/gfx/headless_frontend.hpp>
#include <mln/map/camera.hpp>
#include <mln/map/map_options.hpp>
#include <mln/style/style.hpp>
#include <mln/util/image.hpp>
#include <mln/util/run_loop.hpp>

#include <string>

using namespace mln;

namespace {

// Flat ground at 0 m, Terrain-RGB
std::string makeFlatDEMTile() {
    constexpr uint32_t size = 64;
    PremultipliedImage image({size, size});
    for (uint32_t i = 0; i < size * size; ++i) {
        uint8_t* px = image.data.get() + i * 4;
        px[0] = 1;
        px[1] = 134;
        px[2] = 160;
        px[3] = 255;
    }
    return encodePNG(image);
}

// A red polygon over the world, draped on terrain whose DEM starts at z9, on a blue page (the
// background is drawn under the terrain, not draped)
constexpr auto drapedStyle = R"({
  "version": 8,
  "sources": {
    "dem": {
      "type": "raster-dem",
      "tiles": ["http://example.com/{z}-{x}-{y}.png"],
      "encoding": "mapbox",
      "minzoom": 9,
      "maxzoom": 12,
      "tileSize": 64
    },
    "land": {
      "type": "geojson",
      "data": {"type": "Polygon", "coordinates": [[[-179, -80], [179, -80], [179, 80], [-179, 80], [-179, -80]]]}
    }
  },
  "terrain": {"source": "dem"},
  "layers": [
    {"id": "page", "type": "background", "paint": {"background-color": "#0000ff"}},
    {"id": "land", "type": "fill", "source": "land", "paint": {"fill-color": "#ff0000"}}
  ]
})";

bool isRed(const PremultipliedImage& image, uint32_t x, uint32_t y) {
    const uint8_t* px = image.data.get() + (y * image.size.width + x) * 4;
    return px[0] > 200 && px[1] < 50 && px[2] < 50;
}

} // namespace

// Zoomed out past the DEM's lowest level the source holds no tiles. The terrain is still the
// view's cover, drawn flat, so the map draped on it shows; before, terrain drew nothing there,
// and a zoom out past it left the last zoom's tiles on screen.
TEST(TerrainBelowDEM, DrapesTheMapWithNoDEMTiles) {
    util::RunLoop loop;
    auto fileSource = std::make_shared<StubFileSource>(ResourceOptions::Default(), ClientOptions());
    const std::string tile = makeFlatDEMTile();
    fileSource->tileResponse = [&](const Resource&) {
        Response res;
        res.data = std::make_shared<std::string>(tile);
        return res;
    };
    HeadlessFrontend frontend{{256, 256}, 1};
    MapAdapter map(frontend,
                   MapObserver::nullObserver(),
                   fileSource,
                   MapOptions().withMapMode(MapMode::Static).withSize(frontend.getSize()));
    map.getStyle().loadJSON(drapedStyle);

    for (const double zoom : {10.0, 5.0}) {
        map.jumpTo(CameraOptions().withCenter(LatLng{45.37, -121.70}).withZoom(zoom));
        const auto image = frontend.render(map).image;
        EXPECT_TRUE(isRed(image, 128, 128)) << "zoom " << zoom;
        EXPECT_TRUE(isRed(image, 8, 248)) << "zoom " << zoom;
    }
}
