#import "MLNCustomStyleLayer.h"
#import "MLNCustomStyleLayer_Private.h"

#import "MLNGeometry_Private.h"
#import "MLNMapView_Private.h"
#import "MLNStyleLayer_Private.h"
#import "MLNStyle_Private.h"

#if MLN_RENDER_BACKEND_METAL
#import <MetalKit/MetalKit.h>
#endif

#include <mln/gfx/context.hpp>
#include <mln/math/wrap.hpp>
#include <mln/style/layers/custom_layer.hpp>

#if MLN_RENDER_BACKEND_METAL
#include <mln/mtl/texture2d.hpp>
#include <mln/style/layers/mtl/custom_layer_render_parameters.hpp>
#endif

class MLNCustomLayerHost;

@implementation MLNTerrainTile

- (instancetype)initWithTile:(const mln::style::CustomLayerTerrainTile &)tile {
  if (self = [super init]) {
    _zoomLevel = tile.z;
    _x = tile.x;
    _y = tile.y;
    _wrap = tile.wrap;
    _matrix = MLNMatrix4Make(tile.matrix);
    _demCoords = simd_make_float4(tile.demCoords[0], tile.demCoords[1], tile.demCoords[2],
                                  tile.demCoords[3]);
#if MLN_RENDER_BACKEND_METAL
    if (tile.demTexture) {
      _demTexture =
          (__bridge id<MTLTexture>)(static_cast<const mln::mtl::Texture2D &>(*tile.demTexture)
                                        .getMetalTexture());
    }
#endif
  }
  return self;
}

@end

@implementation MLNTerrainDrawingContext

- (instancetype)initWithTerrain:(const mln::style::CustomLayerTerrain &)terrain {
  if (self = [super init]) {
    _exaggeration = terrain.exaggeration;
    _demUnpack = simd_make_float4(terrain.unpack[0], terrain.unpack[1], terrain.unpack[2],
                                  terrain.unpack[3]);
    _meshSize = terrain.meshSize;
    NSMutableArray<MLNTerrainTile *> *tiles =
        [NSMutableArray arrayWithCapacity:terrain.tiles.size()];
    for (const auto &tile : terrain.tiles) {
      [tiles addObject:[[MLNTerrainTile alloc] initWithTile:tile]];
    }
    _tiles = tiles;
  }
  return self;
}

@end

@interface MLNCustomStyleLayer ()
@property (nonatomic, readonly) mln::style::CustomLayer *rawLayer;
@property (nonatomic, readonly, nullable) MLNMapView *mapView;
@property (nonatomic, weak, readwrite) MLNStyle *style;
@property (nonatomic, readwrite, nullable) MLNTerrainDrawingContext *terrain;
@end

@implementation MLNCustomStyleLayer

- (instancetype)initWithIdentifier:(NSString *)identifier {
  auto layer = std::make_unique<mln::style::CustomLayer>(
      identifier.UTF8String, std::make_unique<MLNCustomLayerHost>(self));
  return self = [super initWithPendingLayer:std::move(layer)];
}

- (mln::style::CustomLayer *)rawLayer {
  return (mln::style::CustomLayer *)super.rawLayer;
}

- (MLNMapView *)mapView {
  if ([self.style.stylable isKindOfClass:[MLNMapView class]]) {
    return (MLNMapView *)self.style.stylable;
  }
  return nil;
}

#if TARGET_OS_IPHONE
- (EAGLContext *)context {
  return self.mapView.context;
}
#else
- (CGLContextObj)context {
  return self.mapView.context;
}
#endif

- (void)addToStyle:(MLNStyle *)style belowLayer:(MLNStyleLayer *)otherLayer {
  self.style = style;
  self.style.customLayers[self.identifier] = self;
  [super addToStyle:style belowLayer:otherLayer];
}

- (void)removeFromStyle:(MLNStyle *)style {
  [super removeFromStyle:style];
  self.style.customLayers[self.identifier] = nil;
  self.style = nil;
}

- (void)didMoveToMapView:(MLNMapView *)mapView {
}

- (void)willMoveFromMapView:(MLNMapView *)mapView {
}

- (void)preDrawInMapView:(MLNMapView *)mapView withContext:(MLNStyleLayerDrawingContext)context {
}

- (void)drawInMapView:(MLNMapView *)mapView withContext:(MLNStyleLayerDrawingContext)context {
}

- (void)setNeedsDisplay {
  [self.mapView setNeedsRerender];
}

@end

class MLNCustomLayerHost : public mln::style::CustomLayerHost {
public:
  MLNCustomLayerHost(MLNCustomStyleLayer *styleLayer) {
    layerRef = styleLayer;
    layer = nil;
  }

  void initialize(const mln::style::CustomLayerInitParameters &) override {
    if (layerRef == nil)
      return;
    else if (layer == nil)
      layer = layerRef;

    if (layer.mapView) {
      [layer didMoveToMapView:layer.mapView];
    }
  }

  void preRender(const mln::gfx::Context &context,
                 const mln::style::CustomLayerRenderParameters &parameters) override {
    if (!layer) return;

#if MLN_RENDER_BACKEND_METAL
    auto renderPassDesc =
        static_cast<const mln::style::mtl::CustomLayerRenderParameters &>(parameters)
            .renderPassDesc.get();
    MTL::CommandBuffer *cmdPtr =
        static_cast<const mln::style::mtl::CustomLayerRenderParameters &>(parameters)
            .commandBuffer.get();
    id<MTLCommandBuffer> commandBuffer = (__bridge id<MTLCommandBuffer>)cmdPtr;
    layer.commandBuffer = commandBuffer;
    layer.renderPassDesc = (__bridge MTLRenderPassDescriptor *)renderPassDesc;

#endif

    MLNStyleLayerDrawingContext drawingContext = {
        .size = CGSizeMake(parameters.width, parameters.height),
        .centerCoordinate = CLLocationCoordinate2DMake(parameters.latitude, parameters.longitude),
        .zoomLevel = parameters.zoom,
        .direction = mln::util::wrap(parameters.bearing, 0., 360.),
        .pitch = static_cast<CGFloat>(parameters.pitch),
        .fieldOfView = static_cast<CGFloat>(parameters.fieldOfView),
        .projectionMatrix = MLNMatrix4Make(parameters.projectionMatrix),
        .nearClippedProjectionMatrix = MLNMatrix4Make(parameters.nearClippedProjectionMatrix)};

    if (layer.mapView) {
      [layer preDrawInMapView:layer.mapView withContext:drawingContext];
    }
  }

  void render(const mln::style::CustomLayerRenderParameters &parameters) override {
    if (!layer) return;

#if MLN_RENDER_BACKEND_METAL
    MTL::CommandBuffer *cmdPtr =
        static_cast<const mln::style::mtl::CustomLayerRenderParameters &>(parameters)
            .commandBuffer.get();
    id<MTLCommandBuffer> commandBuffer = (__bridge id<MTLCommandBuffer>)cmdPtr;
    layer.commandBuffer = commandBuffer;
    MTL::RenderCommandEncoder *ptr =
        static_cast<const mln::style::mtl::CustomLayerRenderParameters &>(parameters).encoder.get();
    id<MTLRenderCommandEncoder> encoder = (__bridge id<MTLRenderCommandEncoder>)ptr;
    layer.renderEncoder = encoder;
#endif

    MLNStyleLayerDrawingContext drawingContext = {
        .size = CGSizeMake(parameters.width, parameters.height),
        .centerCoordinate = CLLocationCoordinate2DMake(parameters.latitude, parameters.longitude),
        .zoomLevel = parameters.zoom,
        .direction = mln::util::wrap(parameters.bearing, 0., 360.),
        .pitch = static_cast<CGFloat>(parameters.pitch),
        .fieldOfView = static_cast<CGFloat>(parameters.fieldOfView),
        .projectionMatrix = MLNMatrix4Make(parameters.projectionMatrix),
        .nearClippedProjectionMatrix = MLNMatrix4Make(parameters.nearClippedProjectionMatrix)};

    layer.terrain = parameters.terrain
                        ? [[MLNTerrainDrawingContext alloc] initWithTerrain:*parameters.terrain]
                        : nil;
    if (layer.mapView) {
      [layer drawInMapView:layer.mapView withContext:drawingContext];
    }
    layer.terrain = nil;
  }

  void contextLost() override {}

  void deinitialize() override {
    if (layer == nil) return;

    if (layer.mapView) {
      [layer willMoveFromMapView:layer.mapView];
    }
    layerRef = layer;
    layer = nil;
  }

private:
  __weak MLNCustomStyleLayer *layerRef;
  MLNCustomStyleLayer *layer = nil;
};

namespace mln {

MLNStyleLayer *CustomStyleLayerPeerFactory::createPeer(style::Layer *rawLayer) {
  return [[MLNCustomStyleLayer alloc] initWithRawLayer:rawLayer];
}

}  // namespace mln
