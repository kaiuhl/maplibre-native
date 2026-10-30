#include <mln/test/util.hpp>

#include <mln/renderer/terrain_cache_policy.hpp>

using namespace mln::terrain;

TEST(TerrainCachePolicy, RebindsToABetterTier) {
    EXPECT_TRUE(shouldRebind({0, -1}, {1, 6}));  // placeholder -> ancestor
    EXPECT_TRUE(shouldRebind({1, 12}, {2, 13})); // ancestor -> own DEM
    EXPECT_TRUE(shouldRebind({0, -1}, {2, 12})); // placeholder -> own DEM
}

TEST(TerrainCachePolicy, NeverRebindsToAWorseTier) {
    EXPECT_FALSE(shouldRebind({2, 12}, {1, 11}));
    EXPECT_FALSE(shouldRebind({1, 6}, {0, -1}));
}

TEST(TerrainCachePolicy, RebindsToADeeperAncestor) {
    // The regression: a z13 tile first built on the z6 ancestor stayed flat after z12 loaded.
    EXPECT_TRUE(shouldRebind({1, 6}, {1, 12}));
    EXPECT_TRUE(shouldRebind({1, 11}, {1, 12}));
}

TEST(TerrainCachePolicy, KeepsTheSameOrADeeperAncestor) {
    EXPECT_FALSE(shouldRebind({1, 12}, {1, 12}));
    EXPECT_FALSE(shouldRebind({1, 12}, {1, 6})); // a deeper ancestor was evicted: keep what is bound
    EXPECT_FALSE(shouldRebind({2, 12}, {2, 12}));
    EXPECT_FALSE(shouldRebind({0, -1}, {0, -1}));
}

namespace {
std::vector<DrapeGroupFallback> groups(std::initializer_list<int16_t> deficits) {
    std::vector<DrapeGroupFallback> out;
    int32_t layerIndex = 0;
    for (const auto deficit : deficits) {
        out.push_back({layerIndex++, deficit});
    }
    return out;
}
} // namespace

TEST(TerrainCachePolicy, SameDrapeContentIsNotWorse) {
    EXPECT_FALSE(drapeFallbackStrictlyWorse(groups({0, 1, -1}), groups({0, 1, -1})));
}

TEST(TerrainCachePolicy, LosingContentIsWorse) {
    // A tile briefly dropped out of the render set: keep the bake instead of flashing it empty.
    EXPECT_TRUE(drapeFallbackStrictlyWorse(groups({0, -1, 0}), groups({0, 1, 0})));
}

TEST(TerrainCachePolicy, CoarserFallbackIsWorse) {
    EXPECT_TRUE(drapeFallbackStrictlyWorse(groups({0, 3, 0}), groups({0, 1, 0})));
}

TEST(TerrainCachePolicy, AnyGroupImprovingIsNotWorse) {
    // The regression: hillshade moves from a z6 to its z12 DEM (deficit 7 -> 1) while vector groups
    // fall back a level. Summed over groups the new content scored worse (10 -> 16 in the field
    // log) and the hillshade-less bake was kept for the life of the target.
    EXPECT_FALSE(drapeFallbackStrictlyWorse(groups({1, 3, 3, 3}), groups({7, 0, 0, 0})));
    // A group gaining content at all is an improvement too.
    EXPECT_FALSE(drapeFallbackStrictlyWorse(groups({1, 4}), groups({-1, 0})));
}

TEST(TerrainCachePolicy, DifferentGroupSetsAreNotComparable) {
    EXPECT_FALSE(drapeFallbackStrictlyWorse(groups({-1, -1}), groups({0, 0, 0})));
    auto renumbered = groups({-1, -1});
    renumbered[1].layerIndex = 7;
    EXPECT_FALSE(drapeFallbackStrictlyWorse(renumbered, groups({0, 0})));
}

TEST(TerrainCachePolicy, ChangedDataIsNotWorse) {
    // The regression: a GeoJSON line selected in 3D (its tile's bucket replaced in place) while
    // a landcover group dropped the z11 ancestor it had been standing in with. The loss alone
    // read as worse and the target kept the unselected line for as long as it lived.
    auto now = groups({0, -1});
    auto baked = groups({0, 4});
    now[0].contents = 2;
    baked[0].contents = 1;
    EXPECT_FALSE(drapeFallbackStrictlyWorse(now, baked));
    // Same data, same loss: still the brief reload the hold exists for.
    now[0].contents = 1;
    EXPECT_TRUE(drapeFallbackStrictlyWorse(now, baked));
    // A group with no usable tile has no data to compare.
    auto lost = groups({-1, -1});
    auto had = groups({-1, 0});
    lost[0].contents = 5;
    EXPECT_TRUE(drapeFallbackStrictlyWorse(lost, had));
}
