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
    /// Brings the zoom and pitch within the bounds where the camera is, at once, without a
    /// transition and leaving a running one to go on: its frames keep within the bounds as they are.
    void clampToBounds();

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
    /// Whether a transition's frame is running, its callbacks (onCameraIsChanging) included, and the
    /// transition is still under way: inTransition() is false then, updateTransitions holding it.
    bool isUpdatingTransition() const;
    void updateTransitions(const TimePoint& now);
    TimePoint getTransitionStart() const { return transitionStart; }
    Duration getTransitionDuration() const { return transitionDuration; }
    void cancelTransitions();
    /// How long, and with what easing, a centre altitude eases onto ground that arrives too late
    /// for the move under way to carry it there, or when no move is under way.
    static constexpr Duration centerAltitudeEaseDuration = std::chrono::milliseconds(400);
    static constexpr util::UnitBezier centerAltitudeEasing{0.42, 0.0, 0.58, 1.0};
    /// The centre a running transition was given to arrive at, as it arrives there (for moveBy, the
    /// one the offset moves to); nullopt without a transition or for one given no centre (rotateBy,
    /// a zoom or turn around an anchor).
    std::optional<LatLng> getTransitionCenter() const;
    /// The zoom a running transition with a centre of its own arrives at; nullopt where
    /// getTransitionCenter is.
    std::optional<double> getTransitionZoom() const;
    /// Moves the centre altitude a running transition arrives at to `altitudeMeters`, carrying on
    /// from the altitude it has reached so that nothing jumps (gl-js's Camera._updateElevation):
    /// over what is left of the transition, or, with less than 0.4 s left, over 0.4 s, the
    /// transition lingering past its end for it. A target that changes again in those 0.4 s goes
    /// on within them. Nothing without a transition.
    void retargetTransitionCenterAltitude(double altitudeMeters);
    /// Eases the centre altitude to `altitudeMeters` over centerAltitudeEaseDuration, keeping the
    /// rest of the camera, as a late target is eased onto: a transition given the centre, so that
    /// a new target on the way goes on within the same time (retargetTransitionCenterAltitude).
    void easeCenterAltitudeTo(double altitudeMeters);

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

    struct CenterAltitudeEase;
    /// Runs `frame` over `duration`, the centre altitude easing as `altitudeEase` says
    /// (transitionCenterAltitude).
    void startTransition(const CameraOptions&,
                         const AnimationOptions&,
                         const std::function<void(double)>& frame,
                         const Duration& duration,
                         const CenterAltitudeEase& altitudeEase);

    // We don't want to show horizon: limit max pitch based on edge insets.
    double getMaxPitchForEdgeInsets(const EdgeInsets& insets) const;

    TimePoint transitionStart;
    Duration transitionDuration;
    /// The time the running transition's frame is drawing, or last drew: its start until the first.
    TimePoint transitionFrameTime;
    /// The centre altitude the running transition eases to, how far along it is (the fraction its
    /// frame was last given), and the centre and zoom it was given. A new target carries on from the
    /// altitude reached at `fromProgress`, from `from`. Set late, the progress is that of an ease
    /// over centerAltitudeEaseDuration from `clockStart` instead (clockProgress), the transition
    /// lingering past its end until it is through (lingersAt).
    struct CenterAltitudeEase {
        double from = 0.0;
        double target = 0.0;
        double fromProgress = 0.0;
        double progress = 0.0;
        std::optional<LatLng> center;
        double zoom = 0.0;
        std::optional<TimePoint> clockStart;
        double at(double k, TimePoint now) const;
        double clockProgress(TimePoint now) const;
        bool lingersAt(TimePoint now) const;
    };
    CenterAltitudeEase centerAltitudeEase;
    /// The running transition's centre altitude at progress `k`, which a new target carries on from.
    double transitionCenterAltitude(double k);
    std::function<bool(const TimePoint)> transitionFrameFn;
    std::function<void()> transitionFinishFn;
    /// Counts the transitions started and cancelled, so that updateTransitions can tell that a
    /// camera set from its frame's callbacks (onCameraIsChanging) ended the transition it runs.
    uint64_t transitionGeneration = 0;
    /// The generation of the transition whose frame updateTransitions is running, if any.
    std::optional<uint64_t> updatingTransitionGeneration;
};

} // namespace mln
