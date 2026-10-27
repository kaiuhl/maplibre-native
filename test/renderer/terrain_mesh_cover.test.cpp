#include <mln/test/util.hpp>

#include <mln/map/camera.hpp>
#include <mln/map/mode.hpp>
#include <mln/map/transform.hpp>
#include <mln/math/angles.hpp>
#include <mln/renderer/render_terrain.hpp>
#include <mln/renderer/terrain_elevation_index.hpp>
#include <mln/tile/tile_id.hpp>
#include <mln/util/projection.hpp>
#include <mln/util/tile_cover.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>
#include <vector>

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

UnwrappedTileID z1(uint32_t x, uint32_t y) {
    return UnwrappedTileID(0, CanonicalTileID(1, x, y));
}

UnwrappedTileID z10(uint32_t x, uint32_t y) {
    return UnwrappedTileID(0, CanonicalTileID(10, x, y));
}
} // namespace

// Capped, the cover keeps the whole world: the z2 tiles farthest out for their size give way to their
// z1 parents, rather than the cover being cut down to its middle four.
TEST(TerrainMeshCover, CapCoarsensTheFarthestTilesInsteadOfDroppingThem) {
    const std::set<UnwrappedTileID> wholeZ1{z1(0, 0), z1(1, 0), z1(0, 1), z1(1, 1)};
    EXPECT_EQ(wholeZ1, RenderTerrain::capMeshCover(wholeZ2(), 4, worldCentre));
}

TEST(TerrainMeshCover, CapBreaksTiesByTileID) {
    // The four corner tiles lie as far out as each other; the one with the lowest ID gives way to
    // its parent first, so the same view always keeps the same tiles.
    std::set<UnwrappedTileID> expected = wholeZ2();
    for (const auto& id : {z2(0, 0), z2(1, 0), z2(0, 1), z2(1, 1)}) {
        expected.erase(id);
    }
    expected.insert(z1(0, 0));
    EXPECT_EQ(expected, RenderTerrain::capMeshCover(wholeZ2(), 13, worldCentre));
}

TEST(TerrainMeshCover, NoCapOrRoomToSpareKeepsEveryTile) {
    EXPECT_EQ(wholeZ2(), RenderTerrain::capMeshCover(wholeZ2(), 0, worldCentre));
    EXPECT_EQ(wholeZ2(), RenderTerrain::capMeshCover(wholeZ2(), 16, worldCentre));
    EXPECT_EQ(wholeZ2(), RenderTerrain::capMeshCover(wholeZ2(), 64, worldCentre));
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

TEST(TerrainMeshCover, CapKeepsTheGroundUnderATiltedCameraAtItsZoom) {
    // Facing north tilted, the camera stands south of the map centre (its ground is the cap's
    // origin, CapOriginIsTheGroundUnderATiltedCamera), here 3 z10 tiles south. Of a block of z10
    // tiles under the camera and one 2 tiles ahead of the centre, capped to five tiles, the block
    // ahead becomes its z9 parent and the one under the camera, the biggest on screen, keeps its
    // zoom. Measured from the centre, the block under the camera is the farther one.
    const std::array<double, 2> camera{{0.75, 0.5 + 3.0 / 1024}};
    const auto block = [](double x, double y) {
        const auto tx = static_cast<uint32_t>(x * 1024) & ~1u;
        const auto ty = static_cast<uint32_t>(y * 1024) & ~1u;
        return std::set<UnwrappedTileID>{z10(tx, ty), z10(tx + 1, ty), z10(tx, ty + 1), z10(tx + 1, ty + 1)};
    };
    const auto underTheCamera = block(camera[0], camera[1]);
    const auto ahead = block(0.75, 0.5 - 2.0 / 1024);
    std::set<UnwrappedTileID> cover = underTheCamera;
    cover.insert(ahead.begin(), ahead.end());
    ASSERT_EQ(8u, cover.size());

    std::set<UnwrappedTileID> expected = underTheCamera;
    expected.emplace(0, ahead.begin()->canonical.scaledTo(9));
    EXPECT_EQ(expected, RenderTerrain::capMeshCover(cover, 5, camera));

    std::set<UnwrappedTileID> fromTheCentre = ahead;
    fromTheCentre.emplace(0, underTheCamera.begin()->canonical.scaledTo(9));
    EXPECT_EQ(fromTheCentre, RenderTerrain::capMeshCover(cover, 5, {{0.75, 0.5}}));
}

// The DEM source loads the own DEM tile of every mesh tile coarser than its maxzoom (z15 here),
// keyed as the source keys its tiles below the maxzoom; a tile at or past it binds a maxzoom tile
// the source's cover holds.
TEST(TerrainMeshCover, CoarseMeshTilesAskForTheirOwnDEMTiles) {
    const std::set<UnwrappedTileID> cover{UnwrappedTileID(0, CanonicalTileID(13, 1308, 2930)),
                                          UnwrappedTileID(1, CanonicalTileID(14, 2618, 5862)),
                                          UnwrappedTileID(0, CanonicalTileID(15, 5236, 11722)),
                                          UnwrappedTileID(0, CanonicalTileID(17, 20945, 46889))};
    const std::vector<OverscaledTileID> expected{OverscaledTileID(13, 0, CanonicalTileID(13, 1308, 2930)),
                                                 OverscaledTileID(14, 1, CanonicalTileID(14, 2618, 5862))};
    EXPECT_EQ(expected, RenderTerrain::coarseMeshDEMTiles(cover, 15));
    EXPECT_TRUE(RenderTerrain::coarseMeshDEMTiles(cover, 13).empty());
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

TEST(TerrainMeshCover, ViewJustPastTheDEMMaxZoomOverzoomsAtTheRoundedViewZoom) {
    // Past the DEM's maxzoom the cover zoom comes from the 512 px drape tile, not the 256 px DEM
    // tile: a z15.25 view stays at z15 (not z16, rounded from 16.25), and z15.6 goes on to z16.
    for (const auto& [zoom, expected] : {std::pair{15.25, uint8_t{15}}, std::pair{15.6, uint8_t{16}}}) {
        Transform transform;
        transform.resize({402, 874});
        transform.jumpTo(CameraOptions().withCenter(LatLng{45.33, -121.70}).withZoom(zoom).withPitch(54.0));

        const util::TileCoverParameters params{.transformState = transform.getState()};
        EXPECT_EQ(std::set<uint8_t>{expected},
                  coverZooms(RenderTerrain::idealMeshCover(params, zoom, 256, demMaxZoom)))
            << "zoom " << zoom;
    }
}

namespace {
// The phone's own raster-dem source stops at z14, a zoom short of the simulator's.
constexpr uint8_t phoneDEMMaxZoom = 14;

// The deepest zoom of Glassing's 256 px DEM cover on a 402x874 pt phone over Mt Hood (its x3 pixel
// ratio leaves the cover, chosen in points, unchanged), looking at `pitch`, for each LOD mode.
// `covers` gets each mode's cover, and `clamped` the cover clamped to the DEM as it was before.
std::vector<uint8_t> phoneCoverMaxZooms(double zoom,
                                        double pitch,
                                        double shift = 0.0,
                                        std::vector<std::set<UnwrappedTileID>>* covers = nullptr,
                                        std::vector<std::set<UnwrappedTileID>>* clamped = nullptr) {
    std::vector<uint8_t> maxZooms;
    for (const auto mode : {TileLodMode::Default, TileLodMode::Adaptive}) {
        Transform transform;
        transform.resize({402, 874});
        transform.jumpTo(
            CameraOptions().withCenter(LatLng{45.37, -121.70}).withZoom(zoom).withPitch(pitch).withBearing(45.0));
        util::TileCoverParameters params{.transformState = transform.getState()};
        params.tileLodMode = mode;
        const auto cover = RenderTerrain::idealMeshCover(params, zoom + shift, 256, phoneDEMMaxZoom);
        maxZooms.push_back(cover.empty() ? 0 : *coverZooms(cover).rbegin());
        if (covers) {
            covers->push_back(cover);
        }
        if (clamped) {
            clamped->push_back(coverClampedToTheDEM(params, zoom + shift, 256, phoneDEMMaxZoom));
        }
    }
    return maxZooms;
}
} // namespace

TEST(TerrainMeshCover, PhoneViewAtZ15CoversNoDeeperThanZ15) {
    // On the phone's z14 DEM a z15 view covered at z16 (round(15 + 1) for the 256 px DEM): ~40%
    // more tiles and 1.5-2x the drape renders, +0.6 ms GPU on a tilted pan. The drape's 512 px
    // tile keeps it at z15, overhead and at the 54 deg tilt cap.
    for (const double pitch : {0.0, 54.0}) {
        EXPECT_EQ((std::vector<uint8_t>{15, 15}), phoneCoverMaxZooms(15.0, pitch)) << "pitch " << pitch;
    }
}

TEST(TerrainMeshCover, PhoneViewJustPastTheDEMCoversAsBefore) {
    // At z14.4 the view is past the z14 DEM but rounds back to it, so the cover is the one the
    // engine had before overzooming: clamped to z14.
    for (const double pitch : {0.0, 54.0}) {
        std::vector<std::set<UnwrappedTileID>> covers;
        std::vector<std::set<UnwrappedTileID>> clamped;
        EXPECT_EQ((std::vector<uint8_t>{14, 14}), phoneCoverMaxZooms(14.4, pitch, 0.0, &covers, &clamped))
            << "pitch " << pitch;
        EXPECT_EQ(clamped, covers) << "pitch " << pitch;
    }
}

TEST(TerrainMeshCover, PhoneViewAtZ17CoversAtZ17) {
    // Well past the DEM the cover still follows the view, so z17 stays sharp.
    for (const double pitch : {0.0, 54.0}) {
        EXPECT_EQ((std::vector<uint8_t>{17, 17}), phoneCoverMaxZooms(17.0, pitch)) << "pitch " << pitch;
    }
    // Overhead the whole cover is at z17.
    std::vector<std::set<UnwrappedTileID>> covers;
    phoneCoverMaxZooms(17.0, 0.0, 0.0, &covers);
    for (const auto& cover : covers) {
        EXPECT_EQ(std::set<uint8_t>{17}, coverZooms(cover));
    }
}

TEST(TerrainMeshCover, PhoneOverzoomFollowsTheLodShift) {
    // A tileLodZoomShift moves the overzoomed cover by the same amount: z15 shifted by +1 covers
    // at z16 (round(16)), not z17 (round(16 + 1) for the 256 px DEM).
    for (const double pitch : {0.0, 54.0}) {
        EXPECT_EQ((std::vector<uint8_t>{16, 16}), phoneCoverMaxZooms(15.0, pitch, 1.0)) << "pitch " << pitch;
    }
}

namespace {

// A summit shaped like Mt Hood's: 3429 m at the top, falling away with distance the way Hood's
// flanks do (2664 m 600 m out, the 8737 ft of Pulpit Rock in the report; 1800 m at 3.5 km; 1270 m
// at 10 km; 980 m at 20 km).
struct Summit {
    LatLng peak{45.3736, -121.6959};

    double operator()(const LatLng& p) const {
        const double north = (p.latitude() - peak.latitude()) * 110540.0;
        const double east = (p.longitude() - peak.longitude()) * 111320.0 * std::cos(util::deg2rad(peak.latitude()));
        return 300.0 + 3129.0 / (1.0 + std::pow(std::hypot(north, east) / 3105.0, 0.686));
    }
};

// The summit's heights as the DEM source would report them: each tile's lowest and highest point
class SummitElevation : public util::TileElevationProvider {
public:
    Summit summit;

    std::optional<Range<double>> getTileElevationRange(const CanonicalTileID& id) const override {
        const double tiles = std::exp2(id.z);
        const auto lon = [&](double x) {
            return x / tiles * 360.0 - 180.0;
        };
        const auto lat = [&](double y) {
            return util::rad2deg(std::atan(std::sinh(M_PI * (1.0 - 2.0 * y / tiles))));
        };
        const double west = lon(id.x), east = lon(id.x + 1.0), north = lat(id.y), south = lat(id.y + 1.0);
        // Heights fall away from the peak: highest at the tile's point nearest it, lowest at a corner
        const LatLng nearest{std::clamp(summit.peak.latitude(), south, north),
                             std::clamp(summit.peak.longitude(), west, east)};
        double lowest = summit(nearest);
        for (const auto& corner :
             {LatLng{north, west}, LatLng{north, east}, LatLng{south, west}, LatLng{south, east}}) {
            lowest = std::min(lowest, summit(corner));
        }
        return Range<double>{lowest, summit(nearest)};
    }
};

bool coverHoldsLatLng(const std::set<UnwrappedTileID>& cover, const LatLng& latLng) {
    return std::ranges::any_of(cover, [&](const UnwrappedTileID& tile) {
        const auto point = Projection::project(latLng, tile.canonical.z);
        return tile.wrap == 0 && std::floor(point.x) == tile.canonical.x && std::floor(point.y) == tile.canonical.y;
    });
}

} // namespace

// At z17 on a summit, tilted to the app's cap, a phone looks kilometres down the mountain: every
// row of the screen is ground, as far as 4.8 km off at the top edge (7.3 km at 60 deg). The mesh
// cover has to hold the ground under every pixel within the load mode's tile budget. Before, it
// kept one zoom for the whole view (no LOD below a 60 deg tilt), 115 z17 tiles at the app's cap,
// and the cap kept the 64 nearest the camera: the ground under the top ~50 rows was left out.
TEST(TerrainMeshCover, SummitViewCoversTheGroundUnderEveryPixel) {
    const LatLng pulpitRock{45.3790, -121.6960};
    // The app's tilt cap on an 874 pt view is 56.6 deg (TerrainTilt: 90 - 15 - the 18.4 deg the
    // view looks above its centre); 54 the cap of Kyle's earlier phones; 60 the app's ceiling.
    for (const double pitch : {54.0, 56.57, 60.0}) {
        for (const auto mode : {TerrainLoadMode::Balanced, TerrainLoadMode::Performance}) {
            SummitElevation elevation;
            Transform transform;
            transform.resize({402, 874});
            transform.jumpTo(CameraOptions()
                                 .withCenter(pulpitRock)
                                 .withZoom(17.0)
                                 .withPitch(pitch)
                                 .withBearing(116.0)
                                 .withCenterAltitude(elevation.summit(pulpitRock)));
            const auto& state = transform.getState();
            // The app's LOD settings: the default mode, which the terrain covers with as gl-js does
            const util::TileCoverParameters params{.transformState = state, .elevationProvider = &elevation};
            const size_t budget = terrainLoadBudget(mode).maxMeshTiles;
            const auto cover = RenderTerrain::meshCover(params, 17.0, 256, 15, budget);
            const std::string name = "pitch " + std::to_string(pitch) +
                                     (mode == TerrainLoadMode::Balanced ? ", Balanced" : ", Performance");
            EXPECT_LE(cover.size(), budget) << name;
            EXPECT_EQ(17, *coverZooms(cover).rbegin()) << name << ": the near field stays at the view's zoom";

            int uncovered = 0;
            int lowestUncoveredRow = -1;
            for (int y = 0; y < 874; y += 8) {
                for (const double x : {0.0, 100.0, 201.0, 300.0, 401.0}) {
                    const auto ground = pickTerrainSurface(
                        state,
                        {x, static_cast<double>(y)},
                        [&](const LatLng& p) { return std::optional<double>(elevation.summit(p)); },
                        3430.0);
                    ASSERT_TRUE(ground) << name << ": every pixel of the view looks at the summit's ground";
                    if (!coverHoldsLatLng(cover, *ground)) {
                        ++uncovered;
                        lowestUncoveredRow = y;
                    }
                }
            }
            EXPECT_EQ(0, uncovered) << name << ": ground left out of the cover down to row " << lowestUncoveredRow;
        }
    }
}

// The terrain covers with maplibre-gl-js's per-tile zoom unless the map chose a variable zoom itself.
TEST(TerrainMeshCover, TerrainCoversWithTheGLJSTileZoomByDefault) {
    EXPECT_EQ(TileLodMode::Adaptive, RenderTerrain::coverLodMode(TileLodMode::Default));
    EXPECT_EQ(TileLodMode::Adaptive, RenderTerrain::coverLodMode(TileLodMode::Adaptive));
    EXPECT_EQ(TileLodMode::Distance, RenderTerrain::coverLodMode(TileLodMode::Distance));
}
