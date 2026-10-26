#import <MapLibre.h>
#import <XCTest/XCTest.h>

@interface MLNMapViewTerrainLoadModeTests : XCTestCase

@property (nonatomic) MLNMapView *mapView;

@end

@implementation MLNMapViewTerrainLoadModeTests

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

- (void)testTerrainLoadModeDefaultsToQuality {
  XCTAssertEqual(self.mapView.terrainLoadMode, MLNTerrainLoadModeQuality);
}

- (void)testTerrainLoadModeSetsEachMode {
  self.mapView.terrainLoadMode = MLNTerrainLoadModeBalanced;
  XCTAssertEqual(self.mapView.terrainLoadMode, MLNTerrainLoadModeBalanced);
  self.mapView.terrainLoadMode = MLNTerrainLoadModePerformance;
  XCTAssertEqual(self.mapView.terrainLoadMode, MLNTerrainLoadModePerformance);
  self.mapView.terrainLoadMode = MLNTerrainLoadModeQuality;
  XCTAssertEqual(self.mapView.terrainLoadMode, MLNTerrainLoadModeQuality);
}

@end
