#include <mln/test/util.hpp>

#include <mln/geometry/dem_data.hpp>
#include <mln/renderer/render_terrain.hpp>
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
