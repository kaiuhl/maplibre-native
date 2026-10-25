#include <mln/test/util.hpp>
#include <mln/test/stub_file_source.hpp>
#include <mln/test/map_adapter.hpp>

#include <mln/gfx/headless_frontend.hpp>
#include <mln/map/bound_options.hpp>
#include <mln/map/camera.hpp>
#include <mln/map/map_options.hpp>
#include <mln/map/transform_state.hpp>
#include <mln/renderer/renderer.hpp>
#include <mln/style/image.hpp>
#include <mln/style/style.hpp>
#include <mln/util/mat4.hpp>
#include <mln/util/projection.hpp>
#include <mln/util/run_loop.hpp>

#include <optional>
#include <string>

using namespace mln;

namespace {

// 0.5 + 0.5 * cameraToCenterDistance / w at a location, as symbol placement works it out
double perspectiveRatio(const TransformState& state, const LatLng& location) {
    mat4 projection;
    state.getProjMatrix(projection);
    const Point<double> world = Projection::project(location, state.getScale());
    vec4 p{{world.x, world.y, 0, 1}};
    matrix::transformMat4(p, p, projection);
    return 0.5 + 0.5 * state.getCameraToCenterDistance() / p[3];
}

std::string pinsStyle(const LatLng& near, const LatLng& far) {
    auto feature = [](const LatLng& at, const char* name) {
        return std::string(R"({"type": "Feature", "properties": {"name": ")") + name +
               R"("}, "geometry": {"type": "Point", "coordinates": [)" + std::to_string(at.longitude()) + ", " +
               std::to_string(at.latitude()) + "]}}";
    };
    return R"({"version": 8, "sources": {"pins": {"type": "geojson", "data": {"type": "FeatureCollection",
        "features": [)" +
           feature(near, "near") + ", " + feature(far, "far") + R"(]}}},
        "layers": [{"id": "pin", "type": "symbol", "source": "pins",
          "layout": {"icon-image": "dot", "icon-allow-overlap": true}}]})";
}

} // namespace

// As in gl-js, a symbol so far from the camera that its perspective ratio is under 0.6 is not
// placed: at a high pitch such labels shrink to nothing and crowd the horizon. One that is nearer
// is placed, and both are allowed to overlap, so only the distance tells them apart.
TEST(CollisionIndex, SymbolsFarFromTheCameraAreNotPlaced) {
    util::RunLoop loop;
    auto fileSource = std::make_shared<StubFileSource>(ResourceOptions::Default(), ClientOptions());
    HeadlessFrontend frontend{{512, 512}, 1};
    MapAdapter map(frontend,
                   MapObserver::nullObserver(),
                   fileSource,
                   MapOptions().withMapMode(MapMode::Static).withSize(frontend.getSize()));
    map.setBounds(BoundOptions().withMaxPitch(85.0));
    const LatLng centre{0.0, 0.0};
    map.jumpTo(CameraOptions().withCenter(centre).withZoom(12.0).withPitch(85.0));
    const TransformState state = map.getTransformState();

    // North from the centre, away from the camera: the first point nearer than the cutoff (ratio
    // 0.7) and the first farther (0.57; it nears 0.5 at the horizon)
    auto pointAt = [&](double ratio) -> std::optional<LatLng> {
        for (double latitude = 0; latitude < 2; latitude += 0.0005) {
            const LatLng at{latitude, 0.0};
            if (perspectiveRatio(state, at) < ratio) return at;
        }
        return std::nullopt;
    };
    const auto near = pointAt(0.7);
    const auto far = pointAt(0.57);
    ASSERT_TRUE(near && far);

    map.getStyle().loadJSON(pinsStyle(*near, *far));
    map.getStyle().addImage(std::make_unique<style::Image>("dot", PremultipliedImage({8, 8}), 1.0f));
    for (int i = 0; i < 4; ++i) {
        loop.runOnce();
        frontend.render(map);
    }

    const RenderedQueryOptions options{std::vector<std::string>{"pin"}};
    auto found = [&](const LatLng& at) {
        return frontend.getRenderer()->queryRenderedFeatures(map.pixelForLatLng(at), options).size();
    };
    EXPECT_EQ(1u, found(*near));
    EXPECT_EQ(0u, found(*far));
}
