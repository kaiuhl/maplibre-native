#include <mln/test/util.hpp>

#include <mln/geometry/dem_data.hpp>
#include <mln/renderer/render_terrain.hpp>
#include <mln/renderer/terrain_elevation_index.hpp>
#include <mln/util/image.hpp>

#include <cmath>
#include <cstring>

using namespace mln;

namespace {

// The height DEMData reads back from a tile filled with `pixel`
int32_t decode(const std::array<uint8_t, 4>& pixel, Tileset::RasterEncoding encoding) {
    PremultipliedImage image({4, 4});
    for (size_t i = 0; i < 16; ++i) {
        std::memcpy(image.data.get() + i * 4, pixel.data(), 4);
    }
    const DEMData dem(image, encoding);
    return dem.get(0, 0);
}

std::array<float, 4> unpackVector(Tileset::RasterEncoding encoding) {
    PremultipliedImage image({4, 4});
    return DEMData(image, encoding).getUnpackVector();
}

TerrainElevationIndex::Tile flatTile(const UnwrappedTileID& id, int32_t metres) {
    const auto pixel = RenderTerrain::encodeDEMHeight(unpackVector(Tileset::RasterEncoding::Terrarium),
                                                      static_cast<float>(metres));
    PremultipliedImage image({4, 4});
    for (size_t i = 0; i < 16; ++i) {
        std::memcpy(image.data.get() + i * 4, pixel.data(), 4);
    }
    const DEMData dem(image, Tileset::RasterEncoding::Terrarium);
    return {id, std::make_shared<const TerrainElevationIndex::Grid>(dem)};
}

UnwrappedTileID z10(uint32_t x, uint32_t y) {
    return UnwrappedTileID(0, CanonicalTileID(10, x, y));
}

} // namespace

TEST(TerrainStandIn, EncodesTheHeightInEitherEncoding) {
    for (const auto encoding : {Tileset::RasterEncoding::Mapbox, Tileset::RasterEncoding::Terrarium}) {
        const auto unpack = unpackVector(encoding);
        for (const float metres : {0.0f, 1500.0f, 4392.0f, -86.0f}) {
            // Terrain-RGB steps 0.1 m, and DEMData::get truncates to a whole metre
            EXPECT_NEAR(metres, decode(RenderTerrain::encodeDEMHeight(unpack, metres), encoding), 1.0)
                << "encoding " << static_cast<int>(encoding) << ", " << metres << " m";
        }
    }
    // Sea level is not zero bytes: those read as -32768 m in Terrarium
    const std::array<uint8_t, 4> seaLevel{{128, 0, 0, 255}};
    EXPECT_EQ(seaLevel, RenderTerrain::encodeDEMHeight(unpackVector(Tileset::RasterEncoding::Terrarium), 0.0f));
}

// A tile coming into view before its DEM or an ancestor's has loaded is drawn flat. Below the
// ground loaded around it, it is a pit walled by its neighbours' skirts (the streaks and holes
// seen zooming out over the Cascades while streaming).
TEST(TerrainStandIn, SitsAmongTheLoadedNeighbours) {
    const TerrainElevationIndex index({flatTile(z10(163, 366), 1500), flatTile(z10(165, 366), 1700)}, 1.0f);
    const float height = RenderTerrain::standInElevation(&index, z10(164, 366));
    EXPECT_GE(height, 1500.0f);
    EXPECT_LE(height, 1700.0f);
}

TEST(TerrainStandIn, UsesTheChildrenAZoomOutLeaves) {
    const UnwrappedTileID parent(0, CanonicalTileID(9, 82, 183));
    const TerrainElevationIndex index({flatTile(z10(164, 366), 1200), flatTile(z10(165, 367), 1200)}, 1.0f);
    EXPECT_NEAR(1200.0f, RenderTerrain::standInElevation(&index, parent), 1.0f);
}

TEST(TerrainStandIn, IsUnexaggerated) {
    // The index holds exaggerated heights; the stand-in is a DEM, which the shader exaggerates
    const TerrainElevationIndex index({flatTile(z10(163, 366), 1000)}, 1.5f);
    EXPECT_NEAR(1000.0f, RenderTerrain::standInElevation(&index, z10(164, 366)), 1.0f);
}

TEST(TerrainStandIn, SeaLevelWithNothingLoaded) {
    EXPECT_EQ(0.0f, RenderTerrain::standInElevation(nullptr, z10(164, 366)));
    const TerrainElevationIndex far({flatTile(z10(500, 500), 1500)}, 1.0f);
    EXPECT_EQ(0.0f, RenderTerrain::standInElevation(&far, z10(164, 366)));
}
