#pragma once

#include <mln/map/camera.hpp>
#include <mln/map/projection_mode.hpp>
#include <mln/map/map_observer.hpp>
#include <mln/map/mode.hpp>
#include <mln/map/transform_state.hpp>
#include <mln/util/chrono.hpp>
#include <mln/util/geo.hpp>
#include <mln/util/noncopyable.hpp>

#include <cstdint>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>

namespace mln {

class TransformObserver {
public:
    virtual ~TransformObserver() = default;

    static TransformObserver& nullObserver() {
        static TransformObserver observer;
        return observer;
    }

    virtual void onCameraWillChange(MapObserver::CameraChangeMode) {}
    virtual void onCameraIsChanging() {}
    virtual void onCameraDidChange(MapObserver::CameraChangeMode) {}
};

class Transform : private util::noncopyable {
public:
    Transform(TransformObserver& = TransformObserver::nullObserver(),
              ConstrainMode = ConstrainMode::HeightOnly,
              ViewportMode = ViewportMode::Default);

    Transform(const TransformState& state_)
        : observer(TransformObserver::nullObserver()),
          state(state_) {}

    // Map view
    void resize(Size size);

    // Camera
    /** Returns the current camera options. */
    CameraOptions getCameraOptions(const std::optional<EdgeInsets>&) const;

    /** Instantaneously, synchronously applies the given camera options. */
    void jumpTo(const CameraOptions&);
    /** Asynchronously transitions all specified camera options linearly along
        an optional time curve. However, center coordinate is not transitioned
        linearly as, instead, ground speed is kept linear.*/
    void easeTo(const CameraOptions&, const AnimationOptions& = {});
    /** Asynchronously zooms out, pans, and zooms back into the given camera
        along a great circle, as though the viewer is riding a supersonic
        jetcopter.
        Parameter linearZoomInterpolation: when true, there is no additional
        zooming out as zoom is linearly interpolated from current to given
        camera zoom. This is used for easeTo.*/
    void flyTo(const CameraOptions&, const AnimationOptions& = {}, bool linearZoomInterpolation = false);

    // Position

    /** Pans the map by the given amount.
        @param offset The distance to pan the map by, measured in pixels from
            top to bottom and from left to right. */
    void moveBy(const ScreenCoordinate& offset, const AnimationOptions& = {});
    LatLng getLatLng(LatLng::WrapMode = LatLng::Wrapped) const;

    // Bounds

    void setLatLngBounds(LatLngBounds);
    void setMinZoom(double);
    void setMaxZoom(double);

    void setMinPitch(double);
    void setMaxPitch(double);

    // Zoom

    /** Returns the zoom level. */
    double getZoom() const;

    // Bearing

    void rotateBy(const ScreenCoordinate& first, const ScreenCoordinate& second, const AnimationOptions& = {});
    double getBearing() const;

    // Pitch

    double getPitch() const;
    double getRoll() const;
    double getFieldOfView() const;

    // North Orientation
    void setNorthOrientation(NorthOrientation);
    NorthOrientation getNorthOrientation() const;

    // Constrain mode
    void setConstrainMode(ConstrainMode);
    ConstrainMode getConstrainMode() const;

    // Viewport mode
    void setViewportMode(ViewportMode);
    ViewportMode getViewportMode() const;

    // Projection mode
    void setProjectionMode(const ProjectionMode&);
    ProjectionMode getProjectionMode() const;

    // Transitions
    bool inTransition() const;
    void updateTransitions(const TimePoint& now);
    TimePoint getTransitionStart() const { return transitionStart; }
    Duration getTransitionDuration() const { return transitionDuration; }
    void cancelTransitions();
    /// The centre a running transition was given to arrive at, as it arrives there; nullopt without
    /// a transition or for one with no centre of its own (moveBy, rotateBy, a zoom around an anchor).
    std::optional<LatLng> getTransitionCenter() const;
    /// Moves the centre altitude a running transition arrives at to `altitudeMeters`, carrying on
    /// from the altitude it has reached so that nothing jumps (gl-js's Camera._updateElevation).
    /// Nothing without a transition, or on its last frame.
    void retargetTransitionCenterAltitude(double altitudeMeters);

    // Gesture
    void setGestureInProgress(bool);
    bool isGestureInProgress() const { return state.isGestureInProgress(); }

    // Transform state
    const TransformState& getState() const { return state; }
    bool isRotating() const { return state.isRotating(); }
    bool isScaling() const { return state.isScaling(); }
    bool isPanning() const { return state.isPanning(); }

    // Conversion and projection
    ScreenCoordinate latLngToScreenCoordinate(const LatLng&) const;
    /// As above, for a point `elevationMeters` above sea level (draped 3D terrain).
    ScreenCoordinate latLngToScreenCoordinate(const LatLng&, double elevationMeters) const;
    LatLng screenCoordinateToLatLng(const ScreenCoordinate&, LatLng::WrapMode = LatLng::Wrapped) const;
    /// As above, unprojecting onto the plane `elevationMeters` above sea level.
    LatLng screenCoordinateToLatLng(const ScreenCoordinate&,
                                    double elevationMeters,
                                    LatLng::WrapMode = LatLng::Wrapped) const;

    FreeCameraOptions getFreeCameraOptions() const;
    void setFreeCameraOptions(const FreeCameraOptions& options);

    // Frustum
    void setFrustumOffset(const EdgeInsets&);
    EdgeInsets getFrustumOffset();

private:
    TransformObserver& observer;
    TransformState state;

    void startTransition(const CameraOptions&,
                         const AnimationOptions&,
                         const std::function<void(double)>&,
                         const Duration&);

    // We don't want to show horizon: limit max pitch based on edge insets.
    double getMaxPitchForEdgeInsets(const EdgeInsets& insets) const;

    TimePoint transitionStart;
    Duration transitionDuration;
    /// The centre altitude the running transition eases to, how far along it is (the fraction its
    /// frame was last given), and the centre it was given. A new target carries on from the
    /// altitude reached at `fromProgress`, from `from`.
    struct CenterAltitudeEase {
        double from = 0.0;
        double target = 0.0;
        double fromProgress = 0.0;
        double progress = 0.0;
        std::optional<LatLng> center;
        double at(double k) const;
    };
    std::shared_ptr<CenterAltitudeEase> centerAltitudeEase;
    std::function<bool(const TimePoint)> transitionFrameFn;
    std::function<void()> transitionFinishFn;
};

} // namespace mln
