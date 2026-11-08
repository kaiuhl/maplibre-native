#pragma once

#include <mln/style/terrain.hpp>
#include <mln/util/immutable.hpp>
#include <string>

namespace mln {
namespace style {

class Terrain::Impl {
public:
    Impl()
        : sourceID(""),
          exaggeration(1.0f) {}
    Impl(const std::string& sourceID_, float exaggeration_)
        : sourceID(sourceID_),
          exaggeration(exaggeration_) {}

    std::string sourceID;
    float exaggeration;
    Terrain::Wave wave;

    bool operator==(const Impl& other) const {
        return sourceID == other.sourceID && exaggeration == other.exaggeration && wave == other.wave;
    }
};

} // namespace style
} // namespace mln
