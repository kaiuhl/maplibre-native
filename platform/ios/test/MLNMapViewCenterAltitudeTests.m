#import <MapLibre.h>
#import <XCTest/XCTest.h>

@interface MLNMapViewCenterAltitudeTests : XCTestCase <MLNMapViewDelegate>

@property (nonatomic) MLNMapView *mapView;
@property (nonatomic) XCTestExpectation *styleLoadingExpectation;

@end

@implementation MLNMapViewCenterAltitudeTests

- (void)setUp {
  [super setUp];

  NSURL *styleURL = [[NSBundle bundleForClass:[self class]] URLForResource:@"one-liner"
                                                             withExtension:@"json"];
  self.mapView = [[MLNMapView alloc] initWithFrame:CGRectMake(0, 0, 400, 400) styleURL:styleURL];
  self.mapView.delegate = self;

  if (!self.mapView.style) {
    _styleLoadingExpectation =
        [self expectationWithDescription:@"Map view should finish loading style."];
    [self waitForExpectationsWithTimeout:10 handler:nil];
  }
}

- (void)mapView:(MLNMapView *)mapView didFinishLoadingStyle:(MLNStyle *)style {
  [_styleLoadingExpectation fulfill];
}

- (void)tearDown {
  self.mapView = nil;
  [super tearDown];
}

- (MLNMapCamera *)cameraOverGroundAt:(CLLocationDistance)centerAltitude {
  MLNMapCamera *camera =
      [MLNMapCamera cameraLookingAtCenterCoordinate:CLLocationCoordinate2DMake(45.2, -117.2)
                                           altitude:3000
                                              pitch:60
                                            heading:30];
  camera.centerAltitude = centerAltitude;
  return camera;
}

- (void)testCameraCarriesTheCenterAltitude {
  MLNMapCamera *camera = [self cameraOverGroundAt:2150];
  self.mapView.camera = camera;

  XCTAssertEqualWithAccuracy(self.mapView.centerAltitude, 2150, 1e-6);
  XCTAssertEqualWithAccuracy(self.mapView.camera.centerAltitude, 2150, 1e-6);
  XCTAssertTrue([self.mapView.camera isEqualToMapCamera:camera]);
}

// The altitude is measured from the centre altitude, so the same camera raised onto the ground
// is at the same zoom level, not two levels closer.
- (void)testAltitudeIsMeasuredFromTheCenterAltitude {
  self.mapView.camera = [self cameraOverGroundAt:0];
  double zoomAtSeaLevel = self.mapView.zoomLevel;

  self.mapView.camera = [self cameraOverGroundAt:2150];
  XCTAssertEqualWithAccuracy(self.mapView.zoomLevel, zoomAtSeaLevel, 1e-6);
}

- (void)testCameraReadBackRestoresTheView {
  self.mapView.camera = [self cameraOverGroundAt:2150];
  MLNMapCamera *saved = self.mapView.camera;

  self.mapView.camera = [self cameraOverGroundAt:0];
  XCTAssertEqualWithAccuracy(self.mapView.centerAltitude, 0, 1e-6);

  self.mapView.camera = saved;
  XCTAssertEqualWithAccuracy(self.mapView.centerAltitude, 2150, 1e-6);
  XCTAssertTrue([self.mapView.camera isEqualToMapCamera:saved]);
}

- (void)testElevationIsNilWithoutTerrain {
  XCTAssertNil([self.mapView elevationAtCoordinate:CLLocationCoordinate2DMake(45.2, -117.2)]);
  XCTAssertNil([self.mapView elevationAtCoordinate:kCLLocationCoordinate2DInvalid]);
}

@end
