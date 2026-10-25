#import <MapLibre.h>
#import <XCTest/XCTest.h>

@interface MLNMapViewNearClipTests : XCTestCase

@property (nonatomic) MLNMapView *mapView;

@end

@implementation MLNMapViewNearClipTests

- (void)setUp {
  [super setUp];
  NSURL *styleURL = [[NSBundle bundleForClass:[self class]] URLForResource:@"one-liner"
                                                             withExtension:@"json"];
  self.mapView = [[MLNMapView alloc] initWithFrame:CGRectMake(0, 0, 400, 400) styleURL:styleURL];
}

- (void)tearDown {
  self.mapView = nil;
  [super tearDown];
}

- (void)testNearClipDistanceDefaultsToZero {
  XCTAssertEqual(self.mapView.nearClipDistance, 0);
}

- (void)testNearClipDistanceSets {
  self.mapView.nearClipDistance = 0.5;
  XCTAssertEqualWithAccuracy(self.mapView.nearClipDistance, 0.5, 1e-9);
}

- (void)testNonPositiveNearClipDistanceIsTheDefault {
  self.mapView.nearClipDistance = 0.5;
  self.mapView.nearClipDistance = -2;
  XCTAssertEqual(self.mapView.nearClipDistance, 0);
  self.mapView.nearClipDistance = 0.5;
  self.mapView.nearClipDistance = 0;
  XCTAssertEqual(self.mapView.nearClipDistance, 0);
}

@end
