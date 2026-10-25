#import <MapLibre.h>
#import <XCTest/XCTest.h>

// Keeps what the terrain looked like to the layer the last time it drew.
@interface MLNTerrainRecordingLayer : MLNCustomStyleLayer
@property (nonatomic, nullable) MLNTerrainDrawingContext *drawnTerrain;
@property (nonatomic) BOOL drew;
@end

@implementation MLNTerrainRecordingLayer

- (void)drawInMapView:(MLNMapView *)mapView withContext:(MLNStyleLayerDrawingContext)context {
  self.drew = YES;
  self.drawnTerrain = self.terrain;
}

@end

@interface MLNCustomStyleLayerTerrainTests : XCTestCase <MLNMapViewDelegate>

@property (nonatomic) UIWindow *window;
@property (nonatomic) MLNMapView *mapView;
@property (nonatomic) MLNTerrainRecordingLayer *layer;
@property (nonatomic) NSURL *tileDirectory;
@property (nonatomic, nullable) XCTestExpectation *renderExpectation;
@property (nonatomic) BOOL expectsTerrain;

@end

@implementation MLNCustomStyleLayerTerrainTests

// A flat 1000 m plateau in Terrain-RGB: -10000 + (R * 65536 + G * 256 + B) * 0.1, so
// RGB(1, 173, 176).
- (NSData *)plateauTile {
  CGSize size = CGSizeMake(256, 256);
  UIGraphicsImageRendererFormat *format = [UIGraphicsImageRendererFormat defaultFormat];
  format.scale = 1;
  UIGraphicsImageRenderer *renderer = [[UIGraphicsImageRenderer alloc] initWithSize:size
                                                                             format:format];
  return [renderer PNGDataWithActions:^(UIGraphicsImageRendererContext *context) {
    [[UIColor colorWithRed:1 / 255.0 green:173 / 255.0 blue:176 / 255.0 alpha:1] setFill];
    [context fillRect:CGRectMake(0, 0, size.width, size.height)];
  }];
}

- (void)setUp {
  [super setUp];

  self.tileDirectory = [NSURL
      fileURLWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString]
          isDirectory:YES];
  [NSFileManager.defaultManager createDirectoryAtURL:self.tileDirectory
                         withIntermediateDirectories:YES
                                          attributes:nil
                                               error:NULL];
  [[self plateauTile] writeToURL:[self.tileDirectory URLByAppendingPathComponent:@"plateau.png"]
                      atomically:YES];
}

// Shows a map over the plateau, with or without the plateau as its terrain, and waits until the
// layer has drawn on a fully rendered map.
- (void)showMapWithTerrain:(BOOL)withTerrain {
  NSString *tiles = [self.tileDirectory URLByAppendingPathComponent:@"plateau.png"].absoluteString;
  NSMutableDictionary *style = [@{
    @"version" : @8,
    @"sources" : @{
      @"dem" : @{
        @"type" : @"raster-dem",
        @"tiles" : @[ tiles ],
        @"encoding" : @"mapbox",
        @"tileSize" : @256,
        @"maxzoom" : @4
      }
    },
    @"layers" : @[ @{@"id" : @"background", @"type" : @"background"} ]
  } mutableCopy];
  if (withTerrain) {
    style[@"terrain"] = @{@"source" : @"dem", @"exaggeration" : @1};
  }
  NSURL *styleURL = [self.tileDirectory URLByAppendingPathComponent:@"style.json"];
  [[NSJSONSerialization dataWithJSONObject:style options:0
                                     error:NULL] writeToURL:styleURL atomically:YES];

  self.window = [[UIWindow alloc] initWithFrame:CGRectMake(0, 0, 256, 256)];
  self.mapView = [[MLNMapView alloc] initWithFrame:self.window.bounds styleURL:styleURL];
  self.mapView.delegate = self;
  [self.mapView setCenterCoordinate:CLLocationCoordinate2DMake(47.2692, 11.4041)
                          zoomLevel:4
                           animated:NO];
  [self.window addSubview:self.mapView];
  [self.window makeKeyAndVisible];

  self.expectsTerrain = withTerrain;
  self.renderExpectation = [self expectationWithDescription:@"The layer drew on a rendered map."];
  [self waitForExpectationsWithTimeout:20 handler:nil];
}

- (void)tearDown {
  [self.mapView removeFromSuperview];
  self.mapView = nil;
  self.window = nil;
  [NSFileManager.defaultManager removeItemAtURL:self.tileDirectory error:NULL];
  [super tearDown];
}

- (void)mapView:(MLNMapView *)mapView didFinishLoadingStyle:(MLNStyle *)style {
  self.layer = [[MLNTerrainRecordingLayer alloc] initWithIdentifier:@"recorder"];
  [style addLayer:self.layer];
}

- (void)mapViewDidFinishRenderingMap:(MLNMapView *)mapView fullyRendered:(BOOL)fullyRendered {
  if (fullyRendered && self.layer.drew &&
      (!self.expectsTerrain || self.layer.drawnTerrain.tiles.count > 0)) {
    [self.renderExpectation fulfill];
    self.renderExpectation = nil;
  }
}

- (void)testCustomLayerSeesTheDrawnTerrain {
  [self showMapWithTerrain:YES];

  MLNTerrainDrawingContext *terrain = self.layer.drawnTerrain;
  XCTAssertEqual(terrain.exaggeration, 1);
  XCTAssertEqual(terrain.meshSize, 128u);
  XCTAssertEqualWithAccuracy(terrain.demUnpack.x, 6553.6, 1e-3);
  XCTAssertEqualWithAccuracy(terrain.demUnpack.w, 10000, 1e-3);
  for (MLNTerrainTile *tile in terrain.tiles) {
    XCTAssertNotNil(tile.demTexture);
    XCTAssertGreaterThan(tile.demCoords.w, 1, @"the plateau's DEM is bound, not the placeholder");
    XCTAssertEqual(tile.wrap, 0);
  }
  // Only while the layer draws.
  XCTAssertNil(self.layer.terrain);
}

- (void)testCustomLayerSeesNoTerrainWithoutIt {
  [self showMapWithTerrain:NO];

  XCTAssertTrue(self.layer.drew);
  XCTAssertNil(self.layer.drawnTerrain);
}

@end
