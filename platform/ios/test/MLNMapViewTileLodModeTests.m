#import <MapLibre.h>
#import <XCTest/XCTest.h>

@interface MLNMapViewTileLodModeTests : XCTestCase

@property (nonatomic) MLNMapView *mapView;

@end

@implementation MLNMapViewTileLodModeTests

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

- (void)testTileLodModeDefaultsToDefault {
  XCTAssertEqual(self.mapView.tileLodMode, MLNTileLodModeDefault);
}

- (void)testTileLodModeSetsEachMode {
  self.mapView.tileLodMode = MLNTileLodModeDistance;
  XCTAssertEqual(self.mapView.tileLodMode, MLNTileLodModeDistance);
  self.mapView.tileLodMode = MLNTileLodModeAdaptive;
  XCTAssertEqual(self.mapView.tileLodMode, MLNTileLodModeAdaptive);
  self.mapView.tileLodMode = MLNTileLodModeDefault;
  XCTAssertEqual(self.mapView.tileLodMode, MLNTileLodModeDefault);
}

@end
