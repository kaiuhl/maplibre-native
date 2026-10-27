#include <mln/test/util.hpp>
#include <mln/test/stub_file_source.hpp>
#include <mln/test/map_adapter.hpp>

#include <mln/gfx/headless_frontend.hpp>
#include <mln/map/bound_options.hpp>
#include <mln/map/camera.hpp>
#include <mln/map/map_options.hpp>
#include <mln/style/style.hpp>
#include <mln/util/image.hpp>
#include <mln/util/run_loop.hpp>

#include <string>

using namespace mln;

namespace {

// Flat ground at 2000 m (Mt Hood's flanks), Terrarium: (R * 256 + G + B / 256) - 32768
std::string makeFlatDEMTile() {
    constexpr uint32_t size = 256;
    PremultipliedImage image({size, size});
    for (uint32_t i = 0; i < size * size; ++i) {
        uint8_t* px = image.data.get() + i * 4;
        px[0] = 135;
        px[1] = 208;
        px[2] = 0;
        px[3] = 255;
    }
    return encodePNG(image);
}

// Glassing's terrain source: 256 px Terrarium to z15, also shaded as a hillshade
constexpr auto demStyle = R"({
  "version": 8,
  "sources": {
    "dem": {
      "type": "raster-dem",
      "tiles": ["http://example.com/{z}-{x}-{y}.png"],
      "encoding": "terrarium",
      "maxzoom": 15,
      "tileSize": 256
    }
  },
  "terrain": {"source": "dem", "exaggeration": 1.0},
  "layers": [{"id": "page", "type": "background", "paint": {"background-color": "#ffffff"}},
             {"id": "shade", "type": "hillshade", "source": "dem"}]
})";

struct DEMCounts {
    int created = 0;
    int released = 0;
};

// A phone-sized map at pixel ratio 3, its centre on the ground, on a DEM served at every zoom
struct TerrainDEMCacheTest {
    util::RunLoop loop;
    std::shared_ptr<StubFileSource> fileSource = std::make_shared<StubFileSource>(ResourceOptions::Default(),
                                                                                  ClientOptions());
    std::string tile;
    HeadlessFrontend frontend{{402, 874}, 3};
    MapAdapter map;
    gfx::RenderingStats last;

    TerrainDEMCacheTest()
        : tile(makeFlatDEMTile()),
          map(frontend,
              MapObserver::nullObserver(),
              fileSource,
              MapOptions().withMapMode(MapMode::Static).withSize(frontend.getSize())) {
        fileSource->tileResponse = [this](const Resource&) {
            Response res;
            res.data = std::make_shared<std::string>(tile);
            return res;
        };
        map.getStyle().loadJSON(demStyle);
        map.setBounds(BoundOptions().withMaxPitch(85.0));
        map.setTerrainLoadMode(TerrainLoadMode::Balanced);
        map.setCenterClampedToGround(true);
    }

    // DEM textures created and released over this many frames (RenderingStats accumulate)
    DEMCounts frames(int count) {
        DEMCounts counts;
        for (int i = 0; i < count; ++i) {
            loop.runOnce();
            const auto stats = frontend.render(map).stats;
            counts.created += stats.numDEMTexturesCreated - last.numDEMTexturesCreated;
            counts.released += stats.numDEMTexturesReleased - last.numDEMTexturesReleased;
            last = stats;
        }
        return counts;
    }
};

struct StaticView {
    const char* name;
    TerrainLoadMode mode;
    double zoom;
    double pitch;
};

} // namespace

// A view at rest must not rebuild DEM textures: once the tiles have loaded, idle frames create
// none. The DEM source loads tiles past the mesh's reach (the cover keeps only the tiles nearest the
// camera), and each of those used to get a texture every frame, dropped again in the same update as
// unrelated to the cover. Both views did so before the fix, the first past the DEM's maxzoom.
TEST(TerrainDEMCache, StaticViewCreatesNoDEMTexturesOnceSettled) {
    for (const auto& view : {StaticView{"z17 over a z15 DEM", TerrainLoadMode::Balanced, 17.0, 54.0},
                             StaticView{"z14 at a steep tilt", TerrainLoadMode::Performance, 14.0, 80.0}}) {
        TerrainDEMCacheTest test;
        test.map.setTerrainLoadMode(view.mode);
        test.map.jumpTo(CameraOptions()
                            .withCenter(LatLng{45.37, -121.70})
                            .withZoom(view.zoom)
                            .withPitch(view.pitch)
                            .withBearing(303.0));
        EXPECT_GT(test.frames(10).created, 0) << view.name;

        const DEMCounts idle = test.frames(20);
        EXPECT_EQ(idle.created, 0) << view.name;
        EXPECT_EQ(idle.released, 0) << view.name;
    }
}

// The cache still lets go of what the view no longer needs: a jump to the far side of the world
// releases every texture the old view held.
TEST(TerrainDEMCache, TexturesThatLeaveTheCoverAreReleased) {
    TerrainDEMCacheTest test;
    test.map.jumpTo(
        CameraOptions().withCenter(LatLng{45.37, -121.70}).withZoom(14.0).withPitch(54.0).withBearing(303.0));
    const DEMCounts settle = test.frames(10);
    const int held = settle.created - settle.released;
    ASSERT_GT(held, 0);

    test.map.jumpTo(CameraOptions().withCenter(LatLng{-45.0, 60.0}));
    const DEMCounts moved = test.frames(10);
    EXPECT_GE(moved.released, held);
    EXPECT_GT(moved.created, 0);
    // And the new view settles too
    EXPECT_EQ(test.frames(10).created, 0);
}
