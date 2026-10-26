#include <mln/test/util.hpp>

#include <mln/map/camera.hpp>
#include <mln/map/mode.hpp>
#include <mln/map/transform.hpp>
#include <mln/renderer/render_terrain.hpp>
#include <mln/tile/tile_id.hpp>
#include <mln/util/tile_cover.hpp>

#include <set>

using namespace mln;

namespace {
UnwrappedTileID z2(uint32_t x, uint32_t y) {
    return UnwrappedTileID(0, CanonicalTileID(2, x, y));
}

// The whole z2 world: a 4x4 grid, small enough to work out distances by hand.
std::set<UnwrappedTileID> wholeZ2() {
    std::set<UnwrappedTileID> tiles;
    for (uint32_t y = 0; y < 4; ++y) {
        for (uint32_t x = 0; x < 4; ++x) {
            tiles.insert(z2(x, y));
        }
    }
    return tiles;
}

constexpr std::array<double, 2> worldCentre{{0.5, 0.5}};

// A 512 px map centred on the equator at 90°E (x = 0.75, y = 0.5 in normalized mercator).
void look(Transform& transform, double pitch, double bearing) {
    transform.resize({512, 512});
    transform.jumpTo(
        CameraOptions().withCenter(LatLng{0.0, 90.0}).withZoom(10.0).withPitch(pitch).withBearing(bearing));
}

// The camera's position from the free-camera API: where the ground under it is, worked out
// independently of RenderTerrain.
std::array<double, 2> cameraGround(const Transform& transform) {
    const vec3 position = *transform.getFreeCameraOptions().position;
    return {{position[0], position[1]}};
}

UnwrappedTileID z10At(double x, double y) {
    return UnwrappedTileID(0, CanonicalTileID(10, static_cast<uint32_t>(x * 1024), static_cast<uint32_t>(y * 1024)));
}
} // namespace

TEST(TerrainMeshCover, CapKeepsTheTilesNearestTheOrigin) {
    const std::set<UnwrappedTileID> middle{z2(1, 1), z2(2, 1), z2(1, 2), z2(2, 2)};
    EXPECT_EQ(middle, RenderTerrain::nearestMeshTiles(wholeZ2(), 4, worldCentre));
}

TEST(TerrainMeshCover, CapBreaksTiesByTileID) {
    // Past the middle four, the eight edge tiles beside them lie equally far from the centre;
    // the two with the lowest IDs stay, so the same view always keeps the same tiles.
    const std::set<UnwrappedTileID> expected{z2(1, 1), z2(2, 1), z2(1, 2), z2(2, 2), z2(0, 1), z2(0, 2)};
    EXPECT_EQ(expected, RenderTerrain::nearestMeshTiles(wholeZ2(), 6, worldCentre));
}

TEST(TerrainMeshCover, NoCapOrRoomToSpareKeepsEveryTile) {
    EXPECT_EQ(wholeZ2(), RenderTerrain::nearestMeshTiles(wholeZ2(), 0, worldCentre));
    EXPECT_EQ(wholeZ2(), RenderTerrain::nearestMeshTiles(wholeZ2(), 16, worldCentre));
    EXPECT_EQ(wholeZ2(), RenderTerrain::nearestMeshTiles(wholeZ2(), 64, worldCentre));
}

TEST(TerrainMeshCover, CapOriginWithoutTiltIsTheMapCentre) {
    Transform transform;
    transform.resize({512, 512});
    transform.jumpTo(CameraOptions().withCenter(LatLng{0.0, 90.0}).withZoom(10.0));

    const auto origin = RenderTerrain::meshTileCapOrigin(transform.getState());
    EXPECT_NEAR(0.75, origin[0], 1e-9);
    EXPECT_NEAR(0.5, origin[1], 1e-9);
}

TEST(TerrainMeshCover, CapOriginIsTheGroundUnderATiltedCamera) {
    for (const double bearing : {0.0, 90.0, 225.0}) {
        Transform transform;
        look(transform, 60.0, bearing);

        const auto origin = RenderTerrain::meshTileCapOrigin(transform.getState());
        const auto camera = cameraGround(transform);
        // 1e-7 of the world is a few metres, well inside a z10 tile.
        EXPECT_NEAR(camera[0], origin[0], 1e-7) << "bearing " << bearing;
        EXPECT_NEAR(camera[1], origin[1], 1e-7) << "bearing " << bearing;
    }
}

TEST(TerrainMeshCover, CapKeepsTheTileUnderATiltedCamera) {
    // Facing north at a 60° tilt, the camera stands south of the centre. Of two tiles as far
    // from the centre, one under the camera and one as far ahead of the centre, the cap keeps
    // the one under the camera: the biggest on screen, where the other is near the horizon.
    Transform transform;
    look(transform, 60.0, 0.0);
    const auto origin = RenderTerrain::meshTileCapOrigin(transform.getState());
    const double behind = origin[1] - 0.5;
    ASSERT_GT(behind, 1.0 / 1024) << "the camera should stand more than a z10 tile behind the centre";

    const UnwrappedTileID underTheCamera = z10At(0.75, 0.5 + behind);
    const UnwrappedTileID ahead = z10At(0.75, 0.5 - behind);
    const std::set<UnwrappedTileID> kept = RenderTerrain::nearestMeshTiles({underTheCamera, ahead}, 1, origin);
    EXPECT_EQ(std::set<UnwrappedTileID>{underTheCamera}, kept);
}

TEST(TerrainMeshCover, DilationAddsTheRingAroundOneTile) {
    const auto dilated = RenderTerrain::dilateMeshCover({z2(1, 1)});
    EXPECT_EQ(9u, dilated.size());
    EXPECT_TRUE(dilated.contains(z2(0, 0)));
    EXPECT_TRUE(dilated.contains(z2(2, 2)));
}

// A cover with an LOD mixes zooms: the ring of a deep tile can fall inside a shallow cover tile,
// and the ring of a shallow tile over deep ones. Overlapping mesh tiles draw two surfaces in one
// place, so no neighbour that overlaps the cover, or a deeper neighbour, joins it.
TEST(TerrainMeshCover, DilationNeverOverlapsTheCover) {
    const UnwrappedTileID shallow(0, CanonicalTileID(3, 2, 2));
    const UnwrappedTileID deep(0, CanonicalTileID(4, 6, 4)); // beside (3, 2, 2), east
    const auto dilated = RenderTerrain::dilateMeshCover({shallow, deep});
    for (const auto& a : dilated) {
        for (const auto& b : dilated) {
            EXPECT_FALSE(a != b && (a.isChildOf(b) || b.isChildOf(a)))
                << a.canonical.z << "/" << a.canonical.x << "/" << a.canonical.y << " overlaps " << b.canonical.z << "/"
                << b.canonical.x << "/" << b.canonical.y;
        }
    }
    // (4, 5, 4), west of the deep tile, is inside the shallow one and stays out; (3, 3, 2), east
    // of the shallow one, holds the deep tile and stays out, its other quarters taken by the deep
    // tile's ring instead.
    EXPECT_FALSE(dilated.contains(UnwrappedTileID(0, CanonicalTileID(4, 5, 4))));
    EXPECT_FALSE(dilated.contains(UnwrappedTileID(0, CanonicalTileID(3, 3, 2))));
    EXPECT_TRUE(dilated.contains(UnwrappedTileID(0, CanonicalTileID(4, 7, 4))));
    EXPECT_TRUE(dilated.contains(UnwrappedTileID(0, CanonicalTileID(4, 7, 5))));
    EXPECT_TRUE(dilated.contains(UnwrappedTileID(0, CanonicalTileID(3, 1, 1))));
}

// Balanced spreads the work over frames but reaches as far as Quality: a shorter cap drops the
// farthest tiles, which at a steep tilt left bare ground at the top edge of the map.
TEST(TerrainMeshCover, BalancedReachesAsFarAsQuality) {
    EXPECT_EQ(terrainLoadBudget(TerrainLoadMode::Quality).maxMeshTiles,
              terrainLoadBudget(TerrainLoadMode::Balanced).maxMeshTiles);
    EXPECT_LT(terrainLoadBudget(TerrainLoadMode::Performance).maxMeshTiles,
              terrainLoadBudget(TerrainLoadMode::Balanced).maxMeshTiles);
}

namespace {
// Glassing's raster-dem source stops at z15; the view goes well past it.
constexpr uint8_t demMaxZoom = 15;

std::set<uint8_t> coverZooms(const std::set<UnwrappedTileID>& cover) {
    std::set<uint8_t> zooms;
    for (const auto& id : cover) {
        zooms.insert(id.canonical.z);
    }
    return zooms;
}
} // namespace

TEST(TerrainMeshCover, OverzoomedViewCoversAtTheViewZoom) {
    // Past the DEM's maxzoom the mesh and its drape targets stay at the view's zoom, as in
    // maplibre-gl-js (TerrainSourceCache covers up to z22 whatever the DEM's maxzoom). Capped at
    // the DEM's maxzoom, one z15 drape was magnified 8x at z18 and the whole map went blocky.
    Transform transform;
    transform.resize({512, 512});
    transform.jumpTo(CameraOptions().withCenter(LatLng{45.0, -122.0}).withZoom(18.0));

    const util::TileCoverParameters params{.transformState = transform.getState()};
    const auto cover = RenderTerrain::idealMeshCover(params, 18.0, 512, demMaxZoom);
    ASSERT_FALSE(cover.empty());
    EXPECT_EQ(std::set<uint8_t>{18}, coverZooms(cover));
}

TEST(TerrainMeshCover, OverzoomedTiltedViewNeverCoversDeeperThanTheView) {
    // With the per-tile LOD of a tilted view, the near field reaches the view's zoom (not the
    // DEM's maxzoom) and nothing goes deeper than it.
    Transform transform;
    transform.resize({512, 512});
    transform.jumpTo(CameraOptions().withCenter(LatLng{45.0, -122.0}).withZoom(18.0).withPitch(60.0));

    util::TileCoverParameters params{.transformState = transform.getState()};
    params.tileLodMode = TileLodMode::Adaptive;
    const auto zooms = coverZooms(RenderTerrain::idealMeshCover(params, 18.0, 512, demMaxZoom));
    ASSERT_FALSE(zooms.empty());
    EXPECT_EQ(18, *zooms.rbegin());
}

TEST(TerrainMeshCover, ViewWithinTheDEMZoomsCoversAtTheViewZoom) {
    Transform transform;
    transform.resize({512, 512});
    transform.jumpTo(CameraOptions().withCenter(LatLng{45.0, -122.0}).withZoom(12.0));

    const util::TileCoverParameters params{.transformState = transform.getState()};
    EXPECT_EQ(std::set<uint8_t>{12}, coverZooms(RenderTerrain::idealMeshCover(params, 12.0, 512, demMaxZoom)));
}

TEST(TerrainMeshCover, OverzoomedTileUsesItsMaxZoomAncestorDEM) {
    const UnwrappedTileID z18(0, CanonicalTileID(18, 41000, 91000));
    EXPECT_EQ(UnwrappedTileID(0, CanonicalTileID(15, 41000 >> 3, 91000 >> 3)), RenderTerrain::demTileFor(z18, 15));
    // Wrapped copies keep their wrap.
    const UnwrappedTileID wrapped(-1, CanonicalTileID(17, 8, 9));
    EXPECT_EQ(UnwrappedTileID(-1, CanonicalTileID(15, 2, 2)), RenderTerrain::demTileFor(wrapped, 15));
    // At or above the DEM's maxzoom a tile's DEM is its own.
    const UnwrappedTileID z15(0, CanonicalTileID(15, 5125, 11375));
    EXPECT_EQ(z15, RenderTerrain::demTileFor(z15, 15));
}

namespace {
// The cover as it was before overzooming: clamped to the DEM's maxzoom at every view zoom.
std::set<UnwrappedTileID> coverClampedToTheDEM(const util::TileCoverParameters& params,
                                               double zoom,
                                               uint16_t tileSize,
                                               uint8_t maxZoom) {
    std::set<UnwrappedTileID> cover;
    const int32_t overscaledZoom = util::coveringZoomLevel(zoom, style::SourceType::RasterDEM, tileSize);
    const auto idealZoom = static_cast<uint8_t>(std::min<int32_t>(maxZoom, overscaledZoom));
    for (const auto& id :
         util::tileCover(params, idealZoom, Range<uint8_t>{0, maxZoom}, static_cast<uint8_t>(overscaledZoom))) {
        cover.insert(id.toUnwrapped());
    }
    return cover;
}
} // namespace

TEST(TerrainMeshCover, TiltedViewWithinTheDEMZoomsCoversAsBefore) {
    // Glassing's 256 px DEM covers a zoom deeper than the view (z16 from z14.5), and a tilted
    // near field or a tileLodZoomShift deeper still. None of that may go past the DEM's maxzoom
    // while the view itself is within it: on a phone at the tilt cap that nearly tripled the
    // cover at z14.5-15 (16 -> 43 tiles), each a 1024 px drape, for drapes ~1.5x magnified.
    for (const auto mode : {TileLodMode::Default, TileLodMode::Adaptive}) {
        for (const uint16_t tileSize : {uint16_t{256}, uint16_t{512}}) {
            for (double zoom = 12.0; zoom <= 15.0; zoom += 0.25) {
                Transform transform;
                transform.resize({402, 874});
                transform.jumpTo(
                    CameraOptions().withCenter(LatLng{45.33, -121.70}).withZoom(zoom).withPitch(54.0).withBearing(45.0));
                util::TileCoverParameters params{.transformState = transform.getState()};
                params.tileLodMode = mode;
                for (const double shift : {0.0, 1.0}) {
                    const auto cover = RenderTerrain::idealMeshCover(params, zoom + shift, tileSize, demMaxZoom);
                    ASSERT_FALSE(cover.empty());
                    EXPECT_EQ(coverClampedToTheDEM(params, zoom + shift, tileSize, demMaxZoom), cover)
                        << "zoom " << zoom << " shift " << shift << " tile size " << tileSize;
                    EXPECT_LE(*coverZooms(cover).rbegin(), demMaxZoom);
                }
            }
        }
    }
}

TEST(TerrainMeshCover, ViewJustPastTheDEMMaxZoomOverzooms) {
    // Just past the DEM's maxzoom, the 256 px DEM's z15.25 view covers at z16 (rounded from 16.25).
    Transform transform;
    transform.resize({402, 874});
    transform.jumpTo(CameraOptions().withCenter(LatLng{45.33, -121.70}).withZoom(15.25).withPitch(54.0));

    const util::TileCoverParameters params{.transformState = transform.getState()};
    EXPECT_EQ(std::set<uint8_t>{16}, coverZooms(RenderTerrain::idealMeshCover(params, 15.25, 256, demMaxZoom)));
}
