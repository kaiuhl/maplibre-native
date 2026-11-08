#pragma once

#include <mln/util/immutable.hpp>
#include <string>

namespace mln {
namespace style {

class TerrainObserver;

/**
 * @brief Terrain configuration for 3D terrain rendering
 *
 * Terrain allows the map to be draped over digital elevation model (DEM) data.
 * It requires a raster-dem source and provides an optional exaggeration multiplier.
 */
class Terrain {
public:
    Terrain();
    explicit Terrain(const std::string& sourceID, float exaggeration = 1.0f);
    ~Terrain();

    /**
     * @brief Get the ID of the raster-dem source providing elevation data
     */
    std::string getSource() const;

    /**
     * @brief Set the ID of the raster-dem source providing elevation data
     */
    void setSource(const std::string& sourceID);

    /**
     * @brief Get the elevation exaggeration multiplier
     * @return Exaggeration factor (default: 1.0)
     */
    float getExaggeration() const;

    /**
     * @brief Set the elevation exaggeration multiplier
     * @param exaggeration Multiplier for elevation values (e.g., 1.5 = 50% more dramatic)
     */
    void setExaggeration(float exaggeration);

    /**
     * @brief A wave of height rolling out over the terrain from a point.
     *
     * Behind its front, at `front` metres from the centre, the ground rises out of the flat map to
     * `peak` times its height within the first `crest` share of `width` metres, and settles to its
     * own height by `width` behind the front. Ahead of the front the ground lies flat. A `width` of
     * zero or less is no wave: the ground stands at its height everywhere.
     */
    struct Wave {
        double latitude = 0;
        double longitude = 0;
        float front = 0;
        float width = 0;
        float peak = 1;
        float crest = 0.2f;
        bool operator==(const Wave&) const = default;
    };

    Wave getWave() const;
    void setWave(const Wave& wave);

    // Internal implementation
    class Impl;
    Immutable<Impl> impl;
    explicit Terrain(Immutable<Impl>);
    Mutable<Impl> mutableImpl() const;

    TerrainObserver* observer = nullptr;
    void setObserver(TerrainObserver*);
};

} // namespace style
} // namespace mln
