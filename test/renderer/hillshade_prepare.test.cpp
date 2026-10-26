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

std::string makeSlopedDEMTile() {
    constexpr uint32_t size = 64;
    PremultipliedImage image({size, size});
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            uint8_t* px = image.data.get() + (y * size + x) * 4;
            px[0] = 1;
            px[1] = static_cast<uint8_t>(100 + x);
            px[2] = static_cast<uint8_t>(y * 3);
            px[3] = 255;
        }
    }
    return encodePNG(image);
}

constexpr auto hillshadeStyle = R"({
  "version": 8,
  "sources": {
    "dem": {
      "type": "raster-dem",
      "tiles": ["http://example.com/{z}-{x}-{y}.png"],
      "encoding": "mapbox",
      "maxzoom": 12,
      "tileSize": 64
    }
  },
  "layers": [{"id": "hillshade", "type": "hillshade", "source": "dem"}]
})";

} // namespace

// The prepare pass turns a tile's DEM into slopes, which never change for that tile, so
// it runs on the frame a tile first draws and is skipped after. Before, every frame ran it
// again for every tile.
TEST(HillshadePrepare, RunsOncePerTile) {
    util::RunLoop loop;
    auto fileSource = std::make_shared<StubFileSource>(ResourceOptions::Default(), ClientOptions());
    const std::string tile = makeSlopedDEMTile();
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
    map.getStyle().loadJSON(hillshadeStyle);
    map.jumpTo(CameraOptions().withCenter(LatLng{45.37, -121.70}).withZoom(10.0));

    // The stats are running totals.
    const int first = frontend.render(map).stats.numOffscreenTargetsRendered;
    frontend.render(map);
    const int third = frontend.render(map).stats.numOffscreenTargetsRendered;

    ASSERT_GT(first, 0);
    EXPECT_EQ(third, first);
}
