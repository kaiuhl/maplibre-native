#import <MapLibre.h>
#import <XCTest/XCTest.h>

@interface MLNMapViewCameraLocationTests : XCTestCase <MLNMapViewDelegate>

@property (nonatomic) MLNMapView *mapView;
@property (nonatomic) XCTestExpectation *styleLoadingExpectation;

@end

@implementation MLNMapViewCameraLocationTests

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

- (void)testCameraStandsWhereItIsPut {
  CLLocationCoordinate2D at = CLLocationCoordinate2DMake(45.2, -117.3);
  [self.mapView setCameraLocation:at altitude:2500 direction:30 pitch:60];

  XCTAssertEqualWithAccuracy(self.mapView.cameraLocation.latitude, at.latitude, 1e-6);
  XCTAssertEqualWithAccuracy(self.mapView.cameraLocation.longitude, at.longitude, 1e-6);
  XCTAssertEqualWithAccuracy(self.mapView.cameraAltitude, 2500, 0.01);
  XCTAssertEqualWithAccuracy(self.mapView.direction, 30, 1e-6);
  XCTAssertEqualWithAccuracy(self.mapView.camera.pitch, 60, 1e-6);
  // Looking down and north-east, the centre is ahead of the camera.
  XCTAssertGreaterThan(self.mapView.centerCoordinate.latitude, at.latitude);
  XCTAssertGreaterThan(self.mapView.centerCoordinate.longitude, at.longitude);
}

- (void)testCameraLooksUpPastLevel {
  self.mapView.maximumPitch = 170;
  CLLocationCoordinate2D at = CLLocationCoordinate2DMake(45.2, -117.3);
  [self.mapView setCameraLocation:at altitude:1000 direction:0 pitch:120];

  XCTAssertEqualWithAccuracy(self.mapView.camera.pitch, 120, 1e-6);
  XCTAssertEqualWithAccuracy(self.mapView.cameraAltitude, 1000, 0.01);
  // The line of sight climbs, so the centre stands north of the camera and above it.
  XCTAssertGreaterThan(self.mapView.centerCoordinate.latitude, at.latitude);
  XCTAssertGreaterThan(self.mapView.centerAltitude, 1000);
}

- (void)testPitchIsHeldToTheMaximum {
  [self.mapView setCameraLocation:CLLocationCoordinate2DMake(45.2, -117.3)
                         altitude:1000
                        direction:0
                            pitch:120];
  XCTAssertEqualWithAccuracy(self.mapView.camera.pitch, self.mapView.maximumPitch, 1e-6);
}

- (void)testInvalidLocationLeavesTheCamera {
  CLLocationCoordinate2D before = self.mapView.centerCoordinate;
  [self.mapView setCameraLocation:kCLLocationCoordinate2DInvalid
                         altitude:1000
                        direction:0
                            pitch:45];
  XCTAssertEqual(self.mapView.centerCoordinate.latitude, before.latitude);
  XCTAssertEqual(self.mapView.centerCoordinate.longitude, before.longitude);
}

- (void)testVerticalFieldOfView {
  XCTAssertEqualWithAccuracy(self.mapView.verticalFieldOfView, 36.87, 0.01);
  self.mapView.verticalFieldOfView = 50;
  XCTAssertEqualWithAccuracy(self.mapView.verticalFieldOfView, 50, 1e-4);
}

@end
