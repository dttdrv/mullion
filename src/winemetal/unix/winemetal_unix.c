#include <stdatomic.h>
#include <errno.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#import <Cocoa/Cocoa.h>
#import <IOKit/IOKitLib.h>
#import <ColorSync/ColorSync.h>
#import <CoreFoundation/CFRunLoop.h>
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#import <QuartzCore/QuartzCore.h>
#include "objc/objc-runtime.h"
#include <bootstrap.h>
#include <mach/mach_port.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <sys/sysctl.h>
#define WINEMETAL_API
#include "../winemetal_thunks.h"
#include "../airconv_thunks.h"

typedef int NTSTATUS;
#define STATUS_SUCCESS 0
#define STATUS_UNSUCCESSFUL 0xC0000001
// a call that makes an object returns it retained, and drains what Metal autoreleased while making it: the calling
// thread may have no pool, and then all of that, a texture's validation records with the texture among them, leaks
#define CREATES(name)                                                                                                \
  name##_made(void *obj);                                                                                            \
  static NTSTATUS name(void *obj) {                                                                                  \
    @autoreleasepool {                                                                                               \
      return name##_made(obj);                                                                                       \
    }                                                                                                                \
  }                                                                                                                  \
  static NTSTATUS name##_made

static int64_t test_late_ns;
static dispatch_queue_t test_late_commits;
static dispatch_semaphore_t test_late_timer;
static pthread_mutex_t test_late_mutex = PTHREAD_MUTEX_INITIALIZER;

__attribute__((constructor)) static void
init_test_late(void) {
  const char *value = getenv("MULLION_TEST_LATE");
  if (!value)
    return;
  char *end;
  errno = 0;
  long long ms = strtoll(value, &end, 10);
  if (errno || end == value || *end || ms < 0 || ms > INT64_MAX / (int64_t)NSEC_PER_MSEC) {
    fprintf(stderr, "winemetal: invalid MULLION_TEST_LATE: %s\n", value);
    abort();
  }
  test_late_ns = ms * NSEC_PER_MSEC;
  if (test_late_ns) {
    test_late_commits = dispatch_queue_create("mullion-test-late", DISPATCH_QUEUE_SERIAL);
    test_late_timer = dispatch_semaphore_create(0);
  }
}

void
execute_on_main(dispatch_block_t block) {
  if ([NSThread isMainThread]) {
    block();
  } else {
    dispatch_sync(dispatch_get_main_queue(), block);
  }
}

static NTSTATUS
_NSObject_retain(NSObject **obj) {
  [*obj retain];
  return STATUS_SUCCESS;
}

static NTSTATUS
_NSObject_release(NSObject **obj) {
  [*obj release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_NSArray_object(void *obj) {
  struct unixcall_generic_obj_uint64_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(NSArray *)params->handle objectAtIndex:params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
_NSArray_count(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(NSArray *)params->handle count];
  return STATUS_SUCCESS;
}

// MULLION_D3D12_GPU_ERRORS, or MULLION_TRACE_GPU=heavy as the process's trace calls say (_WMTTraceWrite): the error of
// a command buffer the GPU failed lists its encoders and what became of each
static bool encoder_errors;

__attribute__((constructor)) static void
init_encoder_errors(void) {
  encoder_errors = getenv("MULLION_D3D12_GPU_ERRORS") || getenv("DXMT_D3D12_GPU_ERRORS");
}

static NTSTATUS
_MTLCopyAllDevices(void *obj) {
  struct unixcall_generic_obj_ret *params = obj;
  params->ret = (obj_handle_t)MTLCopyAllDevices();
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_recommendedMaxWorkingSetSize(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLDevice>)params->handle recommendedMaxWorkingSetSize];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_currentAllocatedSize(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLDevice>)params->handle currentAllocatedSize];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_name(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(id<MTLDevice>)params->handle name];
  return STATUS_SUCCESS;
}

static NTSTATUS
_NSString_getCString(void *obj) {
  struct unixcall_nsstring_getcstring *params = obj;
  params->ret = (uint32_t)[(NSString *)params->str getCString:(char *)params->buffer_ptr
                                                    maxLength:params->max_length
                                                     encoding:params->encoding];
  return STATUS_SUCCESS;
}

static NTSTATUS
CREATES(_MTLDevice_newCommandQueue)(void *obj) {
  struct unixcall_generic_obj_uint64_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(id<MTLDevice>)params->handle newCommandQueueWithMaxCommandBufferCount:params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
_NSAutoreleasePool_alloc_init(void *obj) {
  struct unixcall_generic_obj_ret *params = obj;
  params->ret = (obj_handle_t)[[NSAutoreleasePool alloc] init];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandQueue_commandBuffer(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  if (__atomic_load_n(&encoder_errors, __ATOMIC_RELAXED)) {
    MTLCommandBufferDescriptor *descriptor = [[[MTLCommandBufferDescriptor alloc] init] autorelease];
    descriptor.errorOptions = MTLCommandBufferErrorOptionEncoderExecutionStatus;
    params->ret = (obj_handle_t)[(id<MTLCommandQueue>)params->handle commandBufferWithDescriptor:descriptor];
    return STATUS_SUCCESS;
  }
  params->ret = (obj_handle_t)[(id<MTLCommandQueue>)params->handle commandBuffer];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_commit(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  id<MTLCommandBuffer> buffer = (id<MTLCommandBuffer>)params->handle;
  if (test_late_ns) {
    dispatch_time_t deadline = dispatch_time(DISPATCH_TIME_NOW, test_late_ns);
    pthread_mutex_lock(&test_late_mutex);
    // MTLCommandBuffer.enqueue reserves queue order before the delayed commit (Apple, enqueue discussion).
    [buffer enqueue];
    dispatch_async(test_late_commits, ^{
      dispatch_semaphore_wait(test_late_timer, deadline);
      @autoreleasepool {
        [buffer commit];
      }
    });
    pthread_mutex_unlock(&test_late_mutex);
  } else {
    [buffer commit];
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_waitUntilCompleted(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  // Metal's wait must follow commit; the commit worker never waits for GPU work.
  if (test_late_ns)
    dispatch_sync(test_late_commits, ^{});
  [(id<MTLCommandBuffer>)params->handle waitUntilCompleted];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_status(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLCommandBuffer>)params->handle status];
  // callers must not mistake a delayed commit for a cancelled buffer and free its allocations.
  if (test_late_ns && params->ret == MTLCommandBufferStatusEnqueued)
    params->ret = MTLCommandBufferStatusCommitted;
  return STATUS_SUCCESS;
}

static NTSTATUS
CREATES(_MTLDevice_newSharedEvent)(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(id<MTLDevice>)params->handle newSharedEvent];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLSharedEvent_signaledValue(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLSharedEvent>)params->handle signaledValue];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_encodeSignalEvent(void *obj) {
  struct unixcall_generic_obj_obj_uint64_noret *params = obj;
  [(id<MTLCommandBuffer>)params->handle encodeSignalEvent:(id<MTLSharedEvent>)params->arg0 value:params->arg1];
  return STATUS_SUCCESS;
}

static NTSTATUS
CREATES(_MTLDevice_newBuffer)(void *obj) {
  struct unixcall_mtldevice_newbuffer *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  struct WMTBufferInfo *info = params->info.ptr;
  id<MTLBuffer> buffer;
  if (info->memory.ptr) {
    buffer = [device newBufferWithBytesNoCopy:info->memory.ptr
                                       length:info->length
                                      options:(enum MTLResourceOptions)info->options
                                  deallocator:NULL];
  } else {
    buffer = [device newBufferWithLength:info->length options:(enum MTLResourceOptions)info->options];
    info->memory.ptr = [buffer storageMode] == MTLStorageModePrivate ? NULL : [buffer contents];
  }
  params->ret = (obj_handle_t)buffer;
  info->gpu_address = [buffer gpuAddress];
  return STATUS_SUCCESS;
}

static NTSTATUS
CREATES(_MTLDevice_newSamplerState)(void *obj) {
  struct unixcall_mtldevice_newsamplerstate *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  struct WMTSamplerInfo *info = params->info.ptr;

  MTLSamplerDescriptor *sampler_desc = [[MTLSamplerDescriptor alloc] init];
  sampler_desc.borderColor = (MTLSamplerBorderColor)info->border_color;
  sampler_desc.rAddressMode = (MTLSamplerAddressMode)info->r_address_mode;
  sampler_desc.sAddressMode = (MTLSamplerAddressMode)info->s_address_mode;
  sampler_desc.tAddressMode = (MTLSamplerAddressMode)info->t_address_mode;
  sampler_desc.magFilter = (MTLSamplerMinMagFilter)info->mag_filter;
  sampler_desc.minFilter = (MTLSamplerMinMagFilter)info->min_filter;
  sampler_desc.mipFilter = (MTLSamplerMipFilter)info->mip_filter;
  sampler_desc.compareFunction = (MTLCompareFunction)info->compare_function;
  sampler_desc.lodMaxClamp = info->lod_max_clamp;
  sampler_desc.lodMinClamp = info->lod_min_clamp;
  sampler_desc.maxAnisotropy = info->max_anisotroy;
  sampler_desc.lodAverage = info->lod_average;
  sampler_desc.normalizedCoordinates = info->normalized_coords;
  sampler_desc.supportArgumentBuffers = info->support_argument_buffers;

  id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:sampler_desc];
  info->gpu_resource_id = info->support_argument_buffers ? [sampler gpuResourceID]._impl : 0;
  params->ret = (obj_handle_t)sampler;
  [sampler_desc release];
  return STATUS_SUCCESS;
}

static NTSTATUS
CREATES(_MTLDevice_newDepthStencilState)(void *obj) {
  struct unixcall_mtldevice_newdepthstencilstate *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  const struct WMTDepthStencilInfo *info = params->info.ptr;

  MTLDepthStencilDescriptor *desc = [[MTLDepthStencilDescriptor alloc] init];
  desc.depthCompareFunction = (MTLCompareFunction)info->depth_compare_function;
  desc.depthWriteEnabled = info->depth_write_enabled;

  if (info->front_stencil.enabled) {
    desc.frontFaceStencil.depthStencilPassOperation = (MTLStencilOperation)info->front_stencil.depth_stencil_pass_op;
    desc.frontFaceStencil.depthFailureOperation = (MTLStencilOperation)info->front_stencil.depth_fail_op;
    desc.frontFaceStencil.stencilFailureOperation = (MTLStencilOperation)info->front_stencil.stencil_fail_op;
    desc.frontFaceStencil.stencilCompareFunction = (MTLCompareFunction)info->front_stencil.stencil_compare_function;
    desc.frontFaceStencil.writeMask = info->front_stencil.write_mask;
    desc.frontFaceStencil.readMask = info->front_stencil.read_mask;
  }

  if (info->back_stencil.enabled) {
    desc.backFaceStencil.depthStencilPassOperation = (MTLStencilOperation)info->back_stencil.depth_stencil_pass_op;
    desc.backFaceStencil.depthFailureOperation = (MTLStencilOperation)info->back_stencil.depth_fail_op;
    desc.backFaceStencil.stencilFailureOperation = (MTLStencilOperation)info->back_stencil.stencil_fail_op;
    desc.backFaceStencil.stencilCompareFunction = (MTLCompareFunction)info->back_stencil.stencil_compare_function;
    desc.backFaceStencil.writeMask = info->back_stencil.write_mask;
    desc.backFaceStencil.readMask = info->back_stencil.read_mask;
  }

  params->ret = (obj_handle_t)[device newDepthStencilStateWithDescriptor:desc];
  [desc release];
  return STATUS_SUCCESS;
}

MTLPixelFormat to_metal_pixel_format(enum WMTPixelFormat format) {
  return (MTLPixelFormat)ORIGINAL_FORMAT(format);
}

void
fill_texture_descriptor(MTLTextureDescriptor *desc, struct WMTTextureInfo *info) {
  desc.textureType = (MTLTextureType)info->type;
  desc.pixelFormat = to_metal_pixel_format(info->pixel_format);
  desc.width = info->width;
  desc.height = info->height;
  desc.depth = info->depth;
  desc.arrayLength = info->array_length;
  desc.mipmapLevelCount = info->mipmap_level_count;
  desc.sampleCount = info->sample_count;
  desc.usage = (MTLTextureUsage)info->usage;
  desc.resourceOptions = (MTLResourceOptions)info->options;
  if (@available(macOS 26.0, *))
    desc.placementSparsePageSize = (MTLSparsePageSize)info->placement_sparse_page_size;
};

void
extract_texture_descriptor(id<MTLTexture> desc, struct WMTTextureInfo *info) {
  info->type = desc.textureType;
  info->pixel_format = desc.pixelFormat;
  info->width = desc.width;
  info->height = desc.height;
  info->depth = desc.depth;
  info->array_length = desc.arrayLength;
  info->mipmap_level_count = desc.mipmapLevelCount;
  info->sample_count = desc.sampleCount;
  info->usage = desc.usage;
  info->options = (enum WMTResourceOptions)desc.resourceOptions;
  info->placement_sparse_page_size = 0;
};

static NTSTATUS
CREATES(_MTLDevice_newTexture)(void *obj) {
  struct unixcall_mtldevice_newtexture *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  struct WMTTextureInfo *info = params->info.ptr;
  MTLTextureDescriptor *desc = [[MTLTextureDescriptor alloc] init];
  fill_texture_descriptor(desc, info);

  id<MTLTexture> ret = [device newTextureWithDescriptor:desc];
  params->ret = (obj_handle_t)ret;
  info->gpu_resource_id = [ret gpuResourceID]._impl;
  info->mach_port = 0;

  [desc release];
  return STATUS_SUCCESS;
}

static NTSTATUS
CREATES(_MTLBuffer_newTexture)(void *obj) {
  struct unixcall_mtlbuffer_newtexture *params = obj;
  id<MTLBuffer> buffer = (id<MTLBuffer>)params->buffer;
  struct WMTTextureInfo *info = params->info.ptr;
  MTLTextureDescriptor *desc = [[MTLTextureDescriptor alloc] init];
  fill_texture_descriptor(desc, info);

  id<MTLTexture> ret = [buffer newTextureWithDescriptor:desc offset:params->offset bytesPerRow:params->bytes_per_row];
  params->ret = (obj_handle_t)ret;
  info->gpu_resource_id = [ret gpuResourceID]._impl;
  info->mach_port = 0;

  [desc release];
  return STATUS_SUCCESS;
}

static inline MTLTextureSwizzleChannels
to_metal_swizzle(struct WMTTextureSwizzleChannels swizzle, enum WMTPixelFormat format) {
  return MTLTextureSwizzleChannelsMake(
      (MTLTextureSwizzle)DECODE_FORMAT_COMPONENT_SWIZZLE(format, 0, swizzle.r),
      (MTLTextureSwizzle)DECODE_FORMAT_COMPONENT_SWIZZLE(format, 1, swizzle.g),
      (MTLTextureSwizzle)DECODE_FORMAT_COMPONENT_SWIZZLE(format, 2, swizzle.b),
      (MTLTextureSwizzle)DECODE_FORMAT_COMPONENT_SWIZZLE(format, 3, swizzle.a)
  );
}

static NTSTATUS
CREATES(_MTLTexture_newTextureView)(void *obj) {
  struct unixcall_mtltexture_newtextureview *params = obj;
  id<MTLTexture> texture = (id<MTLTexture>)params->texture;

  id<MTLTexture> ret = [texture
      newTextureViewWithPixelFormat:to_metal_pixel_format(params->format)
                        textureType:(MTLTextureType)params->texture_type
                             levels:NSMakeRange(params->level_start, params->level_count)
                             slices:NSMakeRange(params->slice_start, params->slice_count)
                            swizzle:to_metal_swizzle(params->swizzle, params->format)];
  params->ret = (obj_handle_t)ret;
  params->gpu_resource_id = [ret gpuResourceID]._impl;
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_minimumLinearTextureAlignmentForPixelFormat(void *obj) {
  struct unixcall_generic_obj_uint64_uint64_ret *params = obj;
  params->ret = [(id<MTLDevice>)params->handle minimumLinearTextureAlignmentForPixelFormat:to_metal_pixel_format(params->arg)];
  return STATUS_SUCCESS;
}

static NTSTATUS
CREATES(_MTLDevice_newLibrary)(void *obj) {
  struct unixcall_mtldevice_newlibrary *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  NSError *err = NULL;
  params->ret_library = (obj_handle_t)[device newLibraryWithData:(dispatch_data_t)params->data error:&err];
  params->ret_error = (obj_handle_t)[err retain];
  return STATUS_SUCCESS;
}

static NTSTATUS
CREATES(_MTLLibrary_newFunction)(void *obj) {
  struct unixcall_generic_obj_uint64_obj_ret *params = obj;
  id<MTLLibrary> library = (id<MTLLibrary>)params->handle;
  NSString *name = [[NSString alloc] initWithCString:(char *)params->arg encoding:NSUTF8StringEncoding];
  params->ret = (obj_handle_t)[library newFunctionWithName:name];
  [name release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_NSString_lengthOfBytesUsingEncoding(void *obj) {
  struct unixcall_generic_obj_uint64_uint64_ret *params = obj;
  params->ret = (uint64_t)[(NSString *)params->handle lengthOfBytesUsingEncoding:(NSStringEncoding)params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
_NSObject_description(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(NSObject *)params->handle description];
  return STATUS_SUCCESS;
}

static NTSTATUS
CREATES(_MTLDevice_newComputePipelineState)(void *obj) {
  struct unixcall_mtldevice_newcomputepso *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  const struct WMTComputePipelineInfo *info = params->info.ptr;
  MTLComputePipelineDescriptor *descriptor = [[MTLComputePipelineDescriptor alloc] init];
  NSError *err = NULL;
  descriptor.computeFunction = (id<MTLFunction>)info->compute_function;
  descriptor.threadGroupSizeIsMultipleOfThreadExecutionWidth = info->tgsize_is_multiple_of_sgwidth;
  descriptor.supportIndirectCommandBuffers = info->support_indirect_command_buffers;
  for (unsigned i = 0; i < 31; i++) {
    if (info->immutable_buffers & (1 << i))
      descriptor.buffers[i].mutability = MTLMutabilityImmutable;
  }
  if (info->num_binary_archives_for_lookup && info->binary_archives_for_lookup.ptr)
    descriptor.binaryArchives = [NSArray arrayWithObjects:(id<MTLBinaryArchive> *)info->binary_archives_for_lookup.ptr
                                                    count:info->num_binary_archives_for_lookup];
  MTLPipelineOption options =
      info->fail_on_binary_archive_miss ? MTLPipelineOptionFailOnBinaryArchiveMiss : MTLPipelineOptionNone;
  params->ret_pso =
      (obj_handle_t)[device newComputePipelineStateWithDescriptor:descriptor options:options reflection:nil error:&err];
  params->ret_error = (obj_handle_t)[err retain];
  if (!err && info->binary_archive_for_serialization) {
    [(id<MTLBinaryArchive>)info->binary_archive_for_serialization addComputePipelineFunctionsWithDescriptor:descriptor
                                                                                                      error:&err];
  }
  [descriptor release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_blitCommandEncoder(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(id<MTLCommandBuffer>)params->handle blitCommandEncoder];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_computeCommandEncoder(void *obj) {
  struct unixcall_generic_obj_uint64_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(id<MTLCommandBuffer>)params->handle
      computeCommandEncoderWithDispatchType:params->arg ? MTLDispatchTypeConcurrent : MTLDispatchTypeSerial];
  return STATUS_SUCCESS;
}

// a render pass's own sample positions (WMTRenderPassInfo). one sample lies at the center already, and Metal takes
// custom positions only for more, from the pixel's top left corner
static void
sample_positions(id descriptor, const struct WMTRenderPassInfo *info) {
  unsigned count = info->programmed_samples;
  if (count <= 1)
    return;
  MTLSamplePosition positions[count];
  for (unsigned i = 0; i < count; i++)
    positions[i] = MTLSamplePositionMake(
        0.5f + info->sample_positions[i].x / 16.0f, 0.5f + info->sample_positions[i].y / 16.0f
    );
  [descriptor setSamplePositions:positions count:count];
}

static NTSTATUS
renderCommandEncoder(void *obj, const struct WMTSampleBufferAttachmentInfo *sample) {
  struct unixcall_generic_obj_uint64_obj_ret *params = obj;
  struct WMTRenderPassInfo *info = (struct WMTRenderPassInfo *)params->arg;
  MTLRenderPassDescriptor *descriptor = [[MTLRenderPassDescriptor alloc] init];
  for (unsigned i = 0; i < 8; i++) {
    descriptor.colorAttachments[i].clearColor = MTLClearColorMake(
        info->colors[i].clear_color.r, info->colors[i].clear_color.g, info->colors[i].clear_color.b,
        info->colors[i].clear_color.a
    );
    descriptor.colorAttachments[i].level = info->colors[i].level;
    descriptor.colorAttachments[i].slice = info->colors[i].slice;
    descriptor.colorAttachments[i].depthPlane = info->colors[i].depth_plane;
    descriptor.colorAttachments[i].texture = (id<MTLTexture>)info->colors[i].texture;
    descriptor.colorAttachments[i].loadAction = (MTLLoadAction)info->colors[i].load_action;
    descriptor.colorAttachments[i].storeAction = (MTLStoreAction)info->colors[i].store_action;
    descriptor.colorAttachments[i].resolveTexture = (id<MTLTexture>)info->colors[i].resolve_texture;
    descriptor.colorAttachments[i].resolveLevel = info->colors[i].resolve_level;
    descriptor.colorAttachments[i].resolveSlice = info->colors[i].resolve_slice;
    descriptor.colorAttachments[i].resolveDepthPlane = info->colors[i].resolve_depth_plane;
  }

  if (info->depth.texture) {
    descriptor.depthAttachment.clearDepth = info->depth.clear_depth;
    descriptor.depthAttachment.depthPlane = info->depth.depth_plane;
    descriptor.depthAttachment.level = info->depth.level;
    descriptor.depthAttachment.slice = info->depth.slice;
    descriptor.depthAttachment.texture = (id<MTLTexture>)info->depth.texture;
    descriptor.depthAttachment.loadAction = (MTLLoadAction)info->depth.load_action;
    descriptor.depthAttachment.storeAction = (MTLStoreAction)info->depth.store_action;
  }

  if (info->stencil.texture) {
    descriptor.stencilAttachment.clearStencil = info->stencil.clear_stencil;
    descriptor.stencilAttachment.depthPlane = info->stencil.depth_plane;
    descriptor.stencilAttachment.level = info->stencil.level;
    descriptor.stencilAttachment.slice = info->stencil.slice;
    descriptor.stencilAttachment.texture = (id<MTLTexture>)info->stencil.texture;
    descriptor.stencilAttachment.loadAction = (MTLLoadAction)info->stencil.load_action;
    descriptor.stencilAttachment.storeAction = (MTLStoreAction)info->stencil.store_action;
  }

  descriptor.defaultRasterSampleCount = info->default_raster_sample_count;
  descriptor.renderTargetArrayLength = info->render_target_array_length;
  descriptor.renderTargetHeight = info->render_target_height;
  descriptor.renderTargetWidth = info->render_target_width;
  descriptor.visibilityResultBuffer = (id<MTLBuffer>)info->visibility_buffer;

  sample_positions(descriptor, info);

  if (info->tile_height && info->tile_width) {
    descriptor.tileWidth = info->tile_width;
    descriptor.tileHeight = info->tile_height;
  }

  if (sample) {
    MTLRenderPassSampleBufferAttachmentDescriptor *samples = descriptor.sampleBufferAttachments[0];
    bool vertex = sample->start_of_encoder_sample_index != MTLCounterDontSample &&
        sample->end_of_encoder_sample_index - sample->start_of_encoder_sample_index + 1 == WMTRenderTimestampSamples;
    samples.sampleBuffer = (id<MTLCounterSampleBuffer>)sample->sample_buffer;
    samples.startOfVertexSampleIndex = vertex ? sample->start_of_encoder_sample_index : MTLCounterDontSample;
    samples.endOfVertexSampleIndex = vertex ? sample->start_of_encoder_sample_index + 1 : MTLCounterDontSample;
    samples.startOfFragmentSampleIndex = vertex ? sample->start_of_encoder_sample_index + 2
                                               : sample->start_of_encoder_sample_index;
    samples.endOfFragmentSampleIndex = sample->end_of_encoder_sample_index;
  }
  params->ret = (obj_handle_t)[(id<MTLCommandBuffer>)params->handle renderCommandEncoderWithDescriptor:descriptor];

  [descriptor release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_renderCommandEncoder(void *obj) {
  return renderCommandEncoder(obj, NULL);
}

static NTSTATUS
_MTLCommandEncoder_endEncoding(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  [(id<MTLCommandEncoder>)params->handle endEncoding];
  return STATUS_SUCCESS;
}

#ifndef MULLION_NO_PRIVATE_API

typedef NS_ENUM(NSUInteger, MTLLogicOperation) {
  MTLLogicOperationClear,
  MTLLogicOperationSet,
  MTLLogicOperationCopy,
  MTLLogicOperationCopyInverted,
  MTLLogicOperationNoop,
  MTLLogicOperationInvert,
  MTLLogicOperationAnd,
  MTLLogicOperationNand,
  MTLLogicOperationOr,
  MTLLogicOperationNor,
  MTLLogicOperationXor,
  MTLLogicOperationEquivalence,
  MTLLogicOperationAndReverse,
  MTLLogicOperationAndInverted,
  MTLLogicOperationOrReverse,
  MTLLogicOperationOrInverted,
};

@interface
MTLRenderPipelineDescriptor ()

- (void)setLogicOperationEnabled:(BOOL)enable;
- (void)setLogicOperation:(MTLLogicOperation)op;

@end

@interface
MTLMeshRenderPipelineDescriptor ()

- (void)setLogicOperationEnabled:(BOOL)enable;
- (void)setLogicOperation:(MTLLogicOperation)op;

@end

#endif

static NTSTATUS
CREATES(_MTLDevice_newRenderPipelineState)(void *obj) {
  struct unixcall_mtldevice_newrenderpso *params = obj;
  const struct WMTRenderPipelineInfo *info = params->info.ptr;
  MTLRenderPipelineDescriptor *descriptor = [[MTLRenderPipelineDescriptor alloc] init];

  for (unsigned i = 0; i < 8; i++) {
    descriptor.colorAttachments[i].pixelFormat = to_metal_pixel_format(info->colors[i].pixel_format);
    descriptor.colorAttachments[i].blendingEnabled = info->colors[i].blending_enabled;
    descriptor.colorAttachments[i].writeMask = (MTLColorWriteMask)info->colors[i].write_mask;

    descriptor.colorAttachments[i].alphaBlendOperation = (MTLBlendOperation)info->colors[i].alpha_blend_operation;
    descriptor.colorAttachments[i].rgbBlendOperation = (MTLBlendOperation)info->colors[i].rgb_blend_operation;

    descriptor.colorAttachments[i].sourceRGBBlendFactor = (MTLBlendFactor)info->colors[i].src_rgb_blend_factor;
    descriptor.colorAttachments[i].sourceAlphaBlendFactor = (MTLBlendFactor)info->colors[i].src_alpha_blend_factor;
    descriptor.colorAttachments[i].destinationRGBBlendFactor = (MTLBlendFactor)info->colors[i].dst_rgb_blend_factor;
    descriptor.colorAttachments[i].destinationAlphaBlendFactor = (MTLBlendFactor)info->colors[i].dst_alpha_blend_factor;
  }

  for (unsigned i = 0; i < 31; i++) {
    if (info->immutable_fragment_buffers & (1 << i))
      descriptor.fragmentBuffers[i].mutability = MTLMutabilityImmutable;
    if (info->immutable_vertex_buffers & (1 << i))
      descriptor.vertexBuffers[i].mutability = MTLMutabilityImmutable;
  }

#ifndef MULLION_NO_PRIVATE_API
  [descriptor setLogicOperationEnabled:info->logic_operation_enabled];
  [descriptor setLogicOperation:(MTLLogicOperation)info->logic_operation];
#endif
  descriptor.depthAttachmentPixelFormat = to_metal_pixel_format(info->depth_pixel_format);
  descriptor.stencilAttachmentPixelFormat = to_metal_pixel_format(info->stencil_pixel_format);
  descriptor.alphaToCoverageEnabled = info->alpha_to_coverage_enabled;
  descriptor.rasterizationEnabled = info->rasterization_enabled;
  descriptor.rasterSampleCount = info->raster_sample_count;
  descriptor.inputPrimitiveTopology = (MTLPrimitiveTopologyClass)info->input_primitive_topology;
  descriptor.tessellationPartitionMode = (MTLTessellationPartitionMode)info->tessellation_partition_mode;
  descriptor.tessellationFactorStepFunction = (MTLTessellationFactorStepFunction)info->tessellation_factor_step;
  descriptor.tessellationOutputWindingOrder = (MTLWinding)info->tessellation_output_winding_order;
  descriptor.maxTessellationFactor = info->max_tessellation_factor;

  descriptor.vertexFunction = (id<MTLFunction>)info->vertex_function;
  descriptor.fragmentFunction = (id<MTLFunction>)info->fragment_function;
  descriptor.supportIndirectCommandBuffers = info->support_indirect_command_buffers;

  if (info->num_binary_archives_for_lookup && info->binary_archives_for_lookup.ptr)
    descriptor.binaryArchives = [NSArray arrayWithObjects:(id<MTLBinaryArchive> *)info->binary_archives_for_lookup.ptr
                                                    count:info->num_binary_archives_for_lookup];
  NSError *err = NULL;
  MTLPipelineOption options =
      info->fail_on_binary_archive_miss ? MTLPipelineOptionFailOnBinaryArchiveMiss : MTLPipelineOptionNone;
  params->ret_pso = (obj_handle_t)[(id<MTLDevice>)params->device newRenderPipelineStateWithDescriptor:descriptor
                                                                                              options:options
                                                                                           reflection:nil
                                                                                                error:&err];
  params->ret_error = (obj_handle_t)[err retain];
  if (!err && info->binary_archive_for_serialization) {
    [(id<MTLBinaryArchive>)info->binary_archive_for_serialization addRenderPipelineFunctionsWithDescriptor:descriptor
                                                                                                     error:&err];
  }
  [descriptor release];
  return STATUS_SUCCESS;
}

// the device's fragment function that writes nothing, made once
static id<MTLFunction>
empty_fragment_function(id<MTLDevice> device) {
  static char key;
  @synchronized(device) {
    id<MTLFunction> function = objc_getAssociatedObject(device, &key);
    if (!function) {
      id<MTLLibrary> library = [device newLibraryWithSource:@"[[fragment]] void empty() {}" options:nil error:nil];
      function = [library newFunctionWithName:@"empty"];
      objc_setAssociatedObject(device, &key, function, OBJC_ASSOCIATION_RETAIN);
      [function release];
      [library release];
    }
    return function;
  }
}

static unsigned emulated_family(void);

static NTSTATUS
CREATES(_MTLDevice_newMeshRenderPipelineState)(void *obj) {
  struct unixcall_mtldevice_newmeshrenderpso *params = obj;
  const struct WMTMeshRenderPipelineInfo *info = params->info.ptr;
  // family emulation must also reject requests that the host GPU accepts (Metal Feature Set Tables, mesh draws in ICBs)
  unsigned family = emulated_family();
  if (info->support_indirect_command_buffers && family && family < MTLGPUFamilyApple9 - MTLGPUFamilyApple1 + 1) {
    params->ret_pso = 0;
    params->ret_error = (obj_handle_t)[[NSError
        errorWithDomain:NSCocoaErrorDomain
                   code:NSFeatureUnsupportedError
               userInfo:@{NSLocalizedDescriptionKey : @"indirect mesh command buffers require Apple9"}] retain];
    return STATUS_SUCCESS;
  }
  MTLMeshRenderPipelineDescriptor *descriptor = [[MTLMeshRenderPipelineDescriptor alloc] init];

  for (unsigned i = 0; i < 8; i++) {
    descriptor.colorAttachments[i].pixelFormat = to_metal_pixel_format(info->colors[i].pixel_format);
    descriptor.colorAttachments[i].blendingEnabled = info->colors[i].blending_enabled;
    descriptor.colorAttachments[i].writeMask = (MTLColorWriteMask)info->colors[i].write_mask;

    descriptor.colorAttachments[i].alphaBlendOperation = (MTLBlendOperation)info->colors[i].alpha_blend_operation;
    descriptor.colorAttachments[i].rgbBlendOperation = (MTLBlendOperation)info->colors[i].rgb_blend_operation;

    descriptor.colorAttachments[i].sourceRGBBlendFactor = (MTLBlendFactor)info->colors[i].src_rgb_blend_factor;
    descriptor.colorAttachments[i].sourceAlphaBlendFactor = (MTLBlendFactor)info->colors[i].src_alpha_blend_factor;
    descriptor.colorAttachments[i].destinationRGBBlendFactor = (MTLBlendFactor)info->colors[i].dst_rgb_blend_factor;
    descriptor.colorAttachments[i].destinationAlphaBlendFactor = (MTLBlendFactor)info->colors[i].dst_alpha_blend_factor;
  }

  for (unsigned i = 0; i < 31; i++) {
    if (info->immutable_fragment_buffers & (1 << i))
      descriptor.fragmentBuffers[i].mutability = MTLMutabilityImmutable;
    if (info->immutable_mesh_buffers & (1 << i))
      descriptor.meshBuffers[i].mutability = MTLMutabilityImmutable;
    if (info->immutable_object_buffers & (1 << i))
      descriptor.objectBuffers[i].mutability = MTLMutabilityImmutable;
  }

#ifndef MULLION_NO_PRIVATE_API
  [descriptor setLogicOperationEnabled:info->logic_operation_enabled];
  [descriptor setLogicOperation:(MTLLogicOperation)info->logic_operation];
#endif
  descriptor.depthAttachmentPixelFormat = to_metal_pixel_format(info->depth_pixel_format);
  descriptor.stencilAttachmentPixelFormat = to_metal_pixel_format(info->stencil_pixel_format);
  descriptor.alphaToCoverageEnabled = info->alpha_to_coverage_enabled;
  descriptor.rasterizationEnabled = info->rasterization_enabled;
  descriptor.rasterSampleCount = info->raster_sample_count;

  descriptor.objectFunction = (id<MTLFunction>)info->object_function;
  descriptor.meshFunction = (id<MTLFunction>)info->mesh_function;
  descriptor.fragmentFunction = (id<MTLFunction>)info->fragment_function;
  // Metal aborts on a mesh pipeline that rasterizes to a color attachment without a fragment function, which an
  // ordinary pipeline may: one that does nothing stands in, and depth is written as without
  if (info->rasterization_enabled && !info->fragment_function)
    descriptor.fragmentFunction = empty_fragment_function((id<MTLDevice>)params->device);
  descriptor.payloadMemoryLength = info->payload_memory_length;

  descriptor.meshThreadgroupSizeIsMultipleOfThreadExecutionWidth = info->mesh_tgsize_is_multiple_of_sgwidth;
  descriptor.objectThreadgroupSizeIsMultipleOfThreadExecutionWidth = info->object_tgsize_is_multiple_of_sgwidth;
  descriptor.supportIndirectCommandBuffers = info->support_indirect_command_buffers;

  MTLPipelineOption options = MTLPipelineOptionNone;
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
  if (@available(macOS 15, *)) {
    if (info->num_binary_archives_for_lookup && info->binary_archives_for_lookup.ptr)
      descriptor.binaryArchives = [NSArray arrayWithObjects:(id<MTLBinaryArchive> *)info->binary_archives_for_lookup.ptr
                                                      count:info->num_binary_archives_for_lookup];
    options = info->fail_on_binary_archive_miss ? MTLPipelineOptionFailOnBinaryArchiveMiss : MTLPipelineOptionNone;
  }
#endif
  NSError *err = NULL;
  params->ret_pso = (obj_handle_t)[(id<MTLDevice>)params->device newRenderPipelineStateWithMeshDescriptor:descriptor
                                                                                                  options:options
                                                                                               reflection:nil
                                                                                                    error:&err];
  params->ret_error = (obj_handle_t)[err retain];
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
  if (@available(macOS 15, *)) {
    if (!err && info->binary_archive_for_serialization) {
      [(id<MTLBinaryArchive>)info->binary_archive_for_serialization
          addMeshRenderPipelineFunctionsWithDescriptor:descriptor
                                                 error:&err];
    }
  }
#endif
  [descriptor release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLBlitCommandEncoder_encodeCommands(void *obj) {
  struct unixcall_generic_obj_cmd_noret *params = obj;
  const struct wmtcmd_base *next = params->cmd_head.ptr;
  id<MTLBlitCommandEncoder> encoder = (id<MTLBlitCommandEncoder>)params->encoder;
  while (next) {
    switch ((enum WMTBlitCommandType)next->type) {
    default:
      assert(!next->type && "unhandled blit command type");
      break;
    case WMTBlitCommandCopyFromBufferToBuffer: {
      struct wmtcmd_blit_copy_from_buffer_to_buffer *body = (struct wmtcmd_blit_copy_from_buffer_to_buffer *)next;
      [encoder copyFromBuffer:(id<MTLBuffer>)body->src
                 sourceOffset:body->src_offset
                     toBuffer:(id<MTLBuffer>)body->dst
            destinationOffset:body->dst_offset
                         size:body->copy_length];
      break;
    }
    case WMTBlitCommandCopyFromBufferToTexture: {
      struct wmtcmd_blit_copy_from_buffer_to_texture *body = (struct wmtcmd_blit_copy_from_buffer_to_texture *)next;
      [encoder copyFromBuffer:(id<MTLBuffer>)body->src
                 sourceOffset:body->src_offset
            sourceBytesPerRow:body->bytes_per_row
          sourceBytesPerImage:body->bytes_per_image
                   sourceSize:MTLSizeMake(body->size.width, body->size.height, body->size.depth)
                    toTexture:(id<MTLTexture>)body->dst
             destinationSlice:body->slice
             destinationLevel:body->level
            destinationOrigin:MTLOriginMake(body->origin.x, body->origin.y, body->origin.z)];
      break;
    }
    case WMTBlitCommandCopyFromBufferToTextureWithBlitOption: {
      struct wmtcmd_blit_copy_from_buffer_to_texture_withblitoption *body =
          (struct wmtcmd_blit_copy_from_buffer_to_texture_withblitoption *)next;
      [encoder copyFromBuffer:(id<MTLBuffer>)body->src
                 sourceOffset:body->src_offset
            sourceBytesPerRow:body->bytes_per_row
          sourceBytesPerImage:body->bytes_per_image
                   sourceSize:MTLSizeMake(body->size.width, body->size.height, body->size.depth)
                    toTexture:(id<MTLTexture>)body->dst
             destinationSlice:body->slice
             destinationLevel:body->level
            destinationOrigin:MTLOriginMake(body->origin.x, body->origin.y, body->origin.z)
                      options:(MTLBlitOption)body->options];
      break;
    }
    case WMTBlitCommandCopyFromTextureToBuffer: {
      struct wmtcmd_blit_copy_from_texture_to_buffer *body = (struct wmtcmd_blit_copy_from_texture_to_buffer *)next;
      [encoder copyFromTexture:(id<MTLTexture>)body->src
                       sourceSlice:body->slice
                       sourceLevel:body->level
                      sourceOrigin:MTLOriginMake(body->origin.x, body->origin.y, body->origin.z)
                        sourceSize:MTLSizeMake(body->size.width, body->size.height, body->size.depth)
                          toBuffer:(id<MTLBuffer>)body->dst
                 destinationOffset:body->offset
            destinationBytesPerRow:body->bytes_per_row
          destinationBytesPerImage:body->bytes_per_image];
      break;
    }
    case WMTBlitCommandCopyFromTextureToBufferWithBlitOption: {
      struct wmtcmd_blit_copy_from_texture_to_buffer_withblitoption *body =
          (struct wmtcmd_blit_copy_from_texture_to_buffer_withblitoption *)next;
      [encoder copyFromTexture:(id<MTLTexture>)body->src
                       sourceSlice:body->slice
                       sourceLevel:body->level
                      sourceOrigin:MTLOriginMake(body->origin.x, body->origin.y, body->origin.z)
                        sourceSize:MTLSizeMake(body->size.width, body->size.height, body->size.depth)
                          toBuffer:(id<MTLBuffer>)body->dst
                 destinationOffset:body->offset
            destinationBytesPerRow:body->bytes_per_row
          destinationBytesPerImage:body->bytes_per_image
                           options:(MTLBlitOption)body->options];
      break;
    }
    case WMTBlitCommandCopyFromTextureToTexture: {
      struct wmtcmd_blit_copy_from_texture_to_texture *body = (struct wmtcmd_blit_copy_from_texture_to_texture *)next;
      [encoder copyFromTexture:(id<MTLTexture>)body->src
                   sourceSlice:body->src_slice
                   sourceLevel:body->src_level
                  sourceOrigin:MTLOriginMake(body->src_origin.x, body->src_origin.y, body->src_origin.z)
                    sourceSize:MTLSizeMake(body->src_size.width, body->src_size.height, body->src_size.depth)
                     toTexture:(id<MTLTexture>)body->dst
              destinationSlice:body->dst_slice
              destinationLevel:body->dst_level
             destinationOrigin:MTLOriginMake(body->dst_origin.x, body->dst_origin.y, body->dst_origin.z)];
      break;
    }
    case WMTBlitCommandGenerateMipmaps: {
      struct wmtcmd_blit_generate_mipmaps *body = (struct wmtcmd_blit_generate_mipmaps *)next;
      [encoder generateMipmapsForTexture:(id<MTLTexture>)body->texture];
      break;
    }
    case WMTBlitCommandUpdateFence: {
      struct wmtcmd_blit_fence_op *body = (struct wmtcmd_blit_fence_op *)next;
      [encoder updateFence:(id<MTLFence>)body->fence];
      break;
    }
    case WMTBlitCommandWaitForFence: {
      struct wmtcmd_blit_fence_op *body = (struct wmtcmd_blit_fence_op *)next;
      [encoder waitForFence:(id<MTLFence>)body->fence];
      break;
    }
    case WMTBlitCommandFillBuffer: {
      struct wmtcmd_blit_fillbuffer *body = (struct wmtcmd_blit_fillbuffer *)next;
      [encoder fillBuffer:(id<MTLBuffer>)body->buffer range:NSMakeRange(body->offset, body->length) value:body->value];
      break;
    }
    case WMTBlitCommandResolveCounters: {
      struct wmtcmd_blit_resolvecounters *body = (struct wmtcmd_blit_resolvecounters *)next;
      [encoder resolveCounters:(id<MTLCounterSampleBuffer>)body->sample_buffer
                       inRange:NSMakeRange(body->start, body->len)
             destinationBuffer:(id<MTLBuffer>)body->dst_buffer
             destinationOffset:body->dst_offset];
      break;
    }
    case WMTBlitCommandResetCommandsInBuffer: {
      struct wmtcmd_blit_resetcommands *body = (struct wmtcmd_blit_resetcommands *)next;
      [encoder resetCommandsInBuffer:(id<MTLIndirectCommandBuffer>)body->indirect_command_buffer
                           withRange:NSMakeRange(body->location, body->length)];
      break;
    }
    case WMTBlitCommandCopyTexture: {
      struct wmtcmd_blit_copy_texture *body = (struct wmtcmd_blit_copy_texture *)next;
      [encoder copyFromTexture:(id<MTLTexture>)body->src toTexture:(id<MTLTexture>)body->dst];
      break;
    }
    }

    next = next->next.ptr;
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLComputeCommandEncoder_encodeCommands(void *obj) {
  struct unixcall_generic_obj_cmd_noret *params = obj;
  const struct wmtcmd_base *next = params->cmd_head.ptr;
  id<MTLComputeCommandEncoder> encoder = (id<MTLComputeCommandEncoder>)params->encoder;
  MTLSize threadgroup_size = {0, 0, 0};
  while (next) {
    switch ((enum WMTComputeCommandType)next->type) {
    default:
      assert(!next->type && "unhandled compute command type");
      break;
    case WMTComputeCommandDispatch: {
      struct wmtcmd_compute_dispatch *body = (struct wmtcmd_compute_dispatch *)next;
      // a grid with no groups runs nothing in Direct3D (D3D11.3 18.6.2), and Metal rejects it
      if (!body->size.width || !body->size.height || !body->size.depth)
        break;
      [encoder dispatchThreadgroups:MTLSizeMake(body->size.width, body->size.height, body->size.depth)
              threadsPerThreadgroup:threadgroup_size];
      break;
    }
    case WMTComputeCommandDispatchPart: {
      struct wmtcmd_compute_dispatch_part *body = (struct wmtcmd_compute_dispatch_part *)next;
      if (!body->size.width || !body->size.height || !body->size.depth)
        break;
      MTLSize size = MTLSizeMake(body->size.width, body->size.height, body->size.depth);
      MTLSize group = MTLSizeMake(body->threadgroup_size.width, body->threadgroup_size.height,
                                  body->threadgroup_size.depth);
      [encoder setStageInRegion:MTLRegionMake3D(
                                    body->origin.x, body->origin.y, body->origin.z, size.width, size.height, size.depth
                                )];
      [encoder dispatchThreadgroups:size threadsPerThreadgroup:group];
      // the origin is the encoder's: a dispatch that is not a part starts at none
      [encoder setStageInRegion:MTLRegionMake3D(0, 0, 0, size.width, size.height, size.depth)];
      break;
    }
    case WMTComputeCommandDispatchThreads: {
      struct wmtcmd_compute_dispatch *body = (struct wmtcmd_compute_dispatch *)next;
      [encoder dispatchThreads:MTLSizeMake(body->size.width, body->size.height, body->size.depth)
          threadsPerThreadgroup:threadgroup_size];
      break;
    }
    case WMTComputeCommandDispatchIndirect: {
      struct wmtcmd_compute_dispatch_indirect *body = (struct wmtcmd_compute_dispatch_indirect *)next;
      [encoder dispatchThreadgroupsWithIndirectBuffer:(id<MTLBuffer>)body->indirect_args_buffer
                                 indirectBufferOffset:body->indirect_args_offset
                                threadsPerThreadgroup:threadgroup_size];
      break;
    }
    case WMTComputeCommandSetPSO: {
      struct wmtcmd_compute_setpso *body = (struct wmtcmd_compute_setpso *)next;
      [encoder setComputePipelineState:(id<MTLComputePipelineState>)body->pso];
      threadgroup_size.width = body->threadgroup_size.width;
      threadgroup_size.height = body->threadgroup_size.height;
      threadgroup_size.depth = body->threadgroup_size.depth;
      break;
    }
    case WMTComputeCommandSetBuffer: {
      struct wmtcmd_compute_setbuffer *body = (struct wmtcmd_compute_setbuffer *)next;
      [encoder setBuffer:(id<MTLBuffer>)body->buffer offset:body->offset atIndex:body->index];
      break;
    }
    case WMTComputeCommandSetBufferOffset: {
      struct wmtcmd_compute_setbufferoffset *body = (struct wmtcmd_compute_setbufferoffset *)next;
      [encoder setBufferOffset:body->offset atIndex:body->index];
      break;
    }
    case WMTComputeCommandUseResource: {
      struct wmtcmd_compute_useresource *body = (struct wmtcmd_compute_useresource *)next;
      [encoder useResource:(id<MTLResource>)body->resource usage:(MTLResourceUsage)body->usage];
      break;
    }
    case WMTComputeCommandSetBytes: {
      struct wmtcmd_compute_setbytes *body = (struct wmtcmd_compute_setbytes *)next;
      [encoder setBytes:body->bytes.ptr length:body->length atIndex:body->index];
      break;
    }
    case WMTComputeCommandSetTexture: {
      struct wmtcmd_compute_settexture *body = (struct wmtcmd_compute_settexture *)next;
      [encoder setTexture:(id<MTLTexture>)body->texture atIndex:body->index];
      break;
    }
    case WMTComputeCommandUpdateFence: {
      struct wmtcmd_compute_fence_op *body = (struct wmtcmd_compute_fence_op *)next;
      [encoder updateFence:(id<MTLFence>)body->fence];
      break;
    }
    case WMTComputeCommandSetVisibleFunctionTable: {
      struct wmtcmd_compute_setvisiblefunctiontable *body = (struct wmtcmd_compute_setvisiblefunctiontable *)next;
      [encoder setVisibleFunctionTable:(id<MTLVisibleFunctionTable>)body->table atBufferIndex:body->index];
      break;
    }
    case WMTComputeCommandWaitForFence: {
      struct wmtcmd_compute_fence_op *body = (struct wmtcmd_compute_fence_op *)next;
      [encoder waitForFence:(id<MTLFence>)body->fence];
      break;
    }
    case WMTComputeCommandMemoryBarrier: {
      struct wmtcmd_compute_memory_barrier *body = (struct wmtcmd_compute_memory_barrier *)next;
      [encoder memoryBarrierWithScope:(MTLBarrierScope)body->scope];
      break;
    }    
    case WMTComputeCommandExecuteCommandsInBuffer: {
      struct wmtcmd_compute_executecommands *body = (struct wmtcmd_compute_executecommands *)next;
      [encoder executeCommandsInBuffer:(id<MTLIndirectCommandBuffer>)body->indirect_command_buffer
                             withRange:NSMakeRange(body->location, body->length)];
      break;
    }
    case WMTComputeCommandExecuteCommandsInBufferIndirect: {
      struct wmtcmd_compute_executecommands_indirect *body = (struct wmtcmd_compute_executecommands_indirect *)next;
      [encoder executeCommandsInBuffer:(id<MTLIndirectCommandBuffer>)body->indirect_command_buffer
                        indirectBuffer:(id<MTLBuffer>)body->indirect_buffer
                  indirectBufferOffset:body->indirect_buffer_offset];
      break;
    }
    }

    next = next->next.ptr;
  }
  return STATUS_SUCCESS;
}

/* a geometry draw's `warps` x `instances` object threadgroups, in Metal draws of at most `max` of them (0: one draw),
 * each told its first object threadgroup and instance (SM50_BINDING_INDEX_DRAW_ORIGIN) */
static void
draw_geometry_parts(id<MTLRenderCommandEncoder> encoder, uint32_t warps, uint32_t instances, uint32_t threads, uint32_t max) {
  uint32_t origin[2] = {0, 0};
  MTLSize object = MTLSizeMake(threads, 1, 1), mesh = MTLSizeMake(1, 1, 1);
  if (!max || (uint64_t)warps * instances <= max) {
    [encoder setObjectBytes:origin length:sizeof(origin) atIndex:SM50_BINDING_INDEX_DRAW_ORIGIN];
    [encoder drawMeshThreadgroups:MTLSizeMake(warps, instances, 1)
        threadsPerObjectThreadgroup:object
          threadsPerMeshThreadgroup:mesh];
    return;
  }
  /* whole instances at a time where one fits, otherwise parts of one instance */
  uint32_t width = warps <= max ? warps : max, height = warps <= max ? max / warps : 1;
  for (uint64_t instance = 0; instance < instances; instance += height)
    for (uint64_t warp = 0; warp < warps; warp += width) {
      origin[0] = warp;
      origin[1] = instance;
      [encoder setObjectBytes:origin length:sizeof(origin) atIndex:SM50_BINDING_INDEX_DRAW_ORIGIN];
      [encoder drawMeshThreadgroups:MTLSizeMake(MIN(width, warps - origin[0]), MIN(height, instances - origin[1]), 1)
          threadsPerObjectThreadgroup:object
            threadsPerMeshThreadgroup:mesh];
    }
}

/* an indirect geometry draw's `parts` Metal draws, whose grids and origins the GS dispatch marshal wrote in `args`:
 * grids from `grids` on, 12 bytes apart, origins (SM50_BINDING_INDEX_DRAW_ORIGIN) from `origins` on, 8 bytes apart */
static void
draw_geometry_indirect_parts(id<MTLRenderCommandEncoder> encoder, id<MTLBuffer> args, uint64_t grids, uint64_t origins,
                             uint32_t parts, uint32_t threads) {
  for (uint32_t i = 0; i < parts; i++) {
    [encoder setObjectBuffer:args offset:origins + 8 * i atIndex:SM50_BINDING_INDEX_DRAW_ORIGIN];
    [encoder drawMeshThreadgroupsWithIndirectBuffer:args
                               indirectBufferOffset:grids + 12 * i
                        threadsPerObjectThreadgroup:MTLSizeMake(threads, 1, 1)
                          threadsPerMeshThreadgroup:MTLSizeMake(1, 1, 1)];
  }
}

static NTSTATUS
_MTLRenderCommandEncoder_encodeCommands(void *obj) {
  struct unixcall_generic_obj_cmd_noret *params = obj;
  const struct wmtcmd_base *next = params->cmd_head.ptr;
  id<MTLRenderCommandEncoder> encoder = (id<MTLRenderCommandEncoder>)params->encoder;
  // a pipeline that could not be built is set as none: the draws after it are left out until another is set
  bool pipeline = true;
  while (next) {
    enum WMTRenderCommandType type = next->type;
    if (!pipeline && ((type >= WMTRenderCommandDraw && type <= WMTRenderCommandDrawMeshThreadgroupsIndirect) ||
                      (type >= WMTRenderCommandMullionGeometryDraw && type <= WMTRenderCommandMullionGeometryDrawIndexedIndirect) ||
                      (type >= WMTRenderCommandMullionTessellationMeshDraw &&
                       type <= WMTRenderCommandExecuteCommandsInBuffer) ||
                      type == WMTRenderCommandExecuteCommandsInBufferIndirect)) {
      next = next->next.ptr;
      continue;
    }
    switch (type) {
    default:
      assert(!next->type && "unhandled render command type");
      break;
    case WMTRenderCommandNop:
      break;
    case WMTRenderCommandUseResource: {
      struct wmtcmd_render_useresource *body = (struct wmtcmd_render_useresource *)next;
      [encoder useResource:(id<MTLResource>)body->resource
                     usage:(MTLResourceUsage)body->usage
                    stages:(MTLRenderStages)body->stages];
      break;
    }
    case WMTRenderCommandSetVertexBuffer: {
      struct wmtcmd_render_setbuffer *body = (struct wmtcmd_render_setbuffer *)next;
      [encoder setVertexBuffer:(id<MTLBuffer>)body->buffer offset:body->offset atIndex:body->index];
      break;
    }
    case WMTRenderCommandSetVertexBufferOffset: {
      struct wmtcmd_render_setbufferoffset *body = (struct wmtcmd_render_setbufferoffset *)next;
      [encoder setVertexBufferOffset:body->offset atIndex:body->index];
      break;
    }
    case WMTRenderCommandSetFragmentBuffer: {
      struct wmtcmd_render_setbuffer *body = (struct wmtcmd_render_setbuffer *)next;
      [encoder setFragmentBuffer:(id<MTLBuffer>)body->buffer offset:body->offset atIndex:body->index];
      break;
    }
    case WMTRenderCommandSetFragmentBufferOffset: {
      struct wmtcmd_render_setbufferoffset *body = (struct wmtcmd_render_setbufferoffset *)next;
      [encoder setFragmentBufferOffset:body->offset atIndex:body->index];
      break;
    }
    case WMTRenderCommandSetMeshBuffer: {
      struct wmtcmd_render_setbuffer *body = (struct wmtcmd_render_setbuffer *)next;
      [encoder setMeshBuffer:(id<MTLBuffer>)body->buffer offset:body->offset atIndex:body->index];
      break;
    }
    case WMTRenderCommandSetMeshBufferOffset: {
      struct wmtcmd_render_setbufferoffset *body = (struct wmtcmd_render_setbufferoffset *)next;
      [encoder setMeshBufferOffset:body->offset atIndex:body->index];
      break;
    }
    case WMTRenderCommandSetObjectBuffer: {
      struct wmtcmd_render_setbuffer *body = (struct wmtcmd_render_setbuffer *)next;
      [encoder setObjectBuffer:(id<MTLBuffer>)body->buffer offset:body->offset atIndex:body->index];
      break;
    }
    case WMTRenderCommandSetObjectBufferOffset: {
      struct wmtcmd_render_setbufferoffset *body = (struct wmtcmd_render_setbufferoffset *)next;
      [encoder setObjectBufferOffset:body->offset atIndex:body->index];
      break;
    }
    case WMTRenderCommandSetFragmentBytes: {
      struct wmtcmd_render_setbytes *body = (struct wmtcmd_render_setbytes *)next;
      [encoder setFragmentBytes:body->bytes.ptr length:body->length atIndex:body->index];
      break;
    }
    case WMTRenderCommandSetVertexBytes: {
      struct wmtcmd_render_setbytes *body = (struct wmtcmd_render_setbytes *)next;
      [encoder setVertexBytes:body->bytes.ptr length:body->length atIndex:body->index];
      break;
    }
    case WMTRenderCommandSetMeshBytes: {
      struct wmtcmd_render_setbytes *body = (struct wmtcmd_render_setbytes *)next;
      [encoder setMeshBytes:body->bytes.ptr length:body->length atIndex:body->index];
      break;
    }
    case WMTRenderCommandSetFragmentTexture: {
      struct wmtcmd_render_settexture *body = (struct wmtcmd_render_settexture *)next;
      [encoder setFragmentTexture:(id<MTLTexture>)body->texture atIndex:body->index];
      break;
    }
    case WMTRenderCommandSetRasterizerState: {
      struct wmtcmd_render_setrasterizerstate *body = (struct wmtcmd_render_setrasterizerstate *)next;
      [encoder setTriangleFillMode:(MTLTriangleFillMode)body->fill_mode];
      [encoder setCullMode:(MTLCullMode)body->cull_mode];
      [encoder setDepthClipMode:(MTLDepthClipMode)body->depth_clip_mode];
      [encoder setDepthBias:body->depth_bias slopeScale:body->scole_scale clamp:body->depth_bias_clamp];
      [encoder setFrontFacingWinding:(MTLWinding)body->winding];
      break;
    }
    case WMTRenderCommandSetViewports: {
      struct wmtcmd_render_setviewports *body = (struct wmtcmd_render_setviewports *)next;
      [encoder setViewports:(const MTLViewport *)body->viewports.ptr count:body->viewport_count];
      break;
    }
    case WMTRenderCommandSetScissorRects: {
      struct wmtcmd_render_setscissorrects *body = (struct wmtcmd_render_setscissorrects *)next;
      [encoder setScissorRects:(const MTLScissorRect *)body->scissor_rects.ptr count:body->rect_count];
      break;
    }
    case WMTRenderCommandSetPSO: {
      struct wmtcmd_render_setpso *body = (struct wmtcmd_render_setpso *)next;
      if ((pipeline = body->pso))
        [encoder setRenderPipelineState:(id<MTLRenderPipelineState>)body->pso];
      break;
    }
    case WMTRenderCommandSetDSSO: {
      struct wmtcmd_render_setdsso *body = (struct wmtcmd_render_setdsso *)next;
      [encoder setDepthStencilState:(id<MTLDepthStencilState>)body->dsso];
      [encoder setStencilFrontReferenceValue:body->stencil_ref backReferenceValue:body->back_stencil_ref];
      break;
    }
    case WMTRenderCommandSetDepthStencilState: {
      struct wmtcmd_render_setdepthstencilstate *body = (struct wmtcmd_render_setdepthstencilstate *)next;
      [encoder setDepthStencilState:(id<MTLDepthStencilState>)body->depth_stencil_state];
      break;
    }
    case WMTRenderCommandSetBlendFactorAndStencilRef: {
      struct wmtcmd_render_setblendcolor *body = (struct wmtcmd_render_setblendcolor *)next;
      [encoder setBlendColorRed:body->red green:body->green blue:body->blue alpha:body->alpha];
      [encoder setStencilFrontReferenceValue:body->stencil_ref backReferenceValue:body->back_stencil_ref];
      break;
    }
    case WMTRenderCommandSetBlendFactor: {
      struct wmtcmd_render_setblendcolor *body = (struct wmtcmd_render_setblendcolor *)next;
      [encoder setBlendColorRed:body->red green:body->green blue:body->blue alpha:body->alpha];
      break;
    }
    case WMTRenderCommandSetStencilRef: {
      struct wmtcmd_render_setstencilref *body = (struct wmtcmd_render_setstencilref *)next;
      [encoder setStencilFrontReferenceValue:body->stencil_ref backReferenceValue:body->back_stencil_ref];
      break;
    }
    case WMTRenderCommandSetDepthTestBounds: {
      struct wmtcmd_render_setdepthtestbounds *body = (struct wmtcmd_render_setdepthtestbounds *)next;
      if (@available(macOS 26.0, *))
        [encoder setDepthTestMinBound:body->min maxBound:body->max];
      break;
    }
    case WMTRenderCommandSetVisibilityMode: {
      struct wmtcmd_render_setvisibilitymode *body = (struct wmtcmd_render_setvisibilitymode *)next;
      [encoder setVisibilityResultMode:(MTLVisibilityResultMode)body->mode offset:body->offset];
      break;
    }
    case WMTRenderCommandDraw: {
      struct wmtcmd_render_draw *body = (struct wmtcmd_render_draw *)next;
      [encoder drawPrimitives:(MTLPrimitiveType)body->primitive_type
                  vertexStart:body->vertex_start
                  vertexCount:body->vertex_count
                instanceCount:body->instance_count
                 baseInstance:body->base_instance];
      break;
    }
    case WMTRenderCommandDrawIndexed: {
      struct wmtcmd_render_draw_indexed *body = (struct wmtcmd_render_draw_indexed *)next;
      [encoder drawIndexedPrimitives:(MTLPrimitiveType)body->primitive_type
                          indexCount:body->index_count
                           indexType:(MTLIndexType)body->index_type
                         indexBuffer:(id<MTLBuffer>)body->index_buffer
                   indexBufferOffset:body->index_buffer_offset
                       instanceCount:body->instance_count
                          baseVertex:body->base_vertex
                        baseInstance:body->base_instance];
      break;
    }
    case WMTRenderCommandDrawIndirect: {
      struct wmtcmd_render_draw_indirect *body = (struct wmtcmd_render_draw_indirect *)next;
      [encoder drawPrimitives:(MTLPrimitiveType)body->primitive_type
                indirectBuffer:(id<MTLBuffer>)body->indirect_args_buffer
          indirectBufferOffset:body->indirect_args_offset];
      break;
    }
    case WMTRenderCommandDrawIndexedIndirect: {
      struct wmtcmd_render_draw_indexed_indirect *body = (struct wmtcmd_render_draw_indexed_indirect *)next;
      [encoder drawIndexedPrimitives:(MTLPrimitiveType)body->primitive_type
                           indexType:(MTLIndexType)body->index_type
                         indexBuffer:(id<MTLBuffer>)body->index_buffer
                   indexBufferOffset:body->index_buffer_offset
                      indirectBuffer:(id<MTLBuffer>)body->indirect_args_buffer
                indirectBufferOffset:body->indirect_args_offset];
      break;
    }
    case WMTRenderCommandDrawMeshThreadgroups: {
      struct wmtcmd_render_draw_meshthreadgroups *body = (struct wmtcmd_render_draw_meshthreadgroups *)next;
      [encoder drawMeshThreadgroups:MTLSizeMake(
                                        body->threadgroup_per_grid.width, body->threadgroup_per_grid.height,
                                        body->threadgroup_per_grid.depth
                                    )
          threadsPerObjectThreadgroup:MTLSizeMake(
                                          body->object_threadgroup_size.width, body->object_threadgroup_size.height,
                                          body->object_threadgroup_size.depth
                                      )
            threadsPerMeshThreadgroup:MTLSizeMake(
                                          body->mesh_threadgroup_size.width, body->mesh_threadgroup_size.height,
                                          body->mesh_threadgroup_size.depth
                                      )];
      break;
    }
    case WMTRenderCommandDrawMeshThreadgroupsIndirect: {
      struct wmtcmd_render_draw_meshthreadgroups_indirect *body =
          (struct wmtcmd_render_draw_meshthreadgroups_indirect *)next;
      [encoder drawMeshThreadgroupsWithIndirectBuffer:(id<MTLBuffer>)body->indirect_args_buffer
                                 indirectBufferOffset:body->indirect_args_offset
                          threadsPerObjectThreadgroup:MTLSizeMake(
                                                          body->object_threadgroup_size.width,
                                                          body->object_threadgroup_size.height,
                                                          body->object_threadgroup_size.depth
                                                      )
                            threadsPerMeshThreadgroup:MTLSizeMake(
                                                          body->mesh_threadgroup_size.width,
                                                          body->mesh_threadgroup_size.height,
                                                          body->mesh_threadgroup_size.depth
                                                      )];
      break;
    }
    case WMTRenderCommandMemoryBarrier: {
      struct wmtcmd_render_memory_barrier *body = (struct wmtcmd_render_memory_barrier *)next;
      [encoder memoryBarrierWithScope:(MTLBarrierScope)body->scope
                          afterStages:(MTLRenderStages)body->stages_after
                         beforeStages:(MTLRenderStages)body->stages_before];
      break;
    }
    case WMTRenderCommandMullionGeometryDraw: {
      struct wmtcmd_render_mullion_geometry_draw *body = (struct wmtcmd_render_mullion_geometry_draw *)next;
      [encoder setObjectBufferOffset:body->draw_arguments_offset atIndex:SM50_BINDING_INDEX_DRAW_ARGUMENTS];
      draw_geometry_parts(encoder, body->warp_count, body->instance_count, body->vertex_per_warp,
                          body->max_object_threadgroups);
      break;
    }
    case WMTRenderCommandMullionGeometryDrawIndexed: {
      struct wmtcmd_render_mullion_geometry_draw_indexed *body = (struct wmtcmd_render_mullion_geometry_draw_indexed *)next;
      [encoder setObjectBuffer:(id<MTLBuffer>)body->index_buffer
                        offset:body->index_buffer_offset
                       atIndex:SM50_BINDING_INDEX_INDEX_BUFFER];
      [encoder setObjectBufferOffset:body->draw_arguments_offset atIndex:SM50_BINDING_INDEX_DRAW_ARGUMENTS];
      draw_geometry_parts(encoder, body->warp_count, body->instance_count, body->vertex_per_warp,
                          body->max_object_threadgroups);
      break;
    }
    case WMTRenderCommandMullionGeometryDrawIndirect: {
      struct wmtcmd_render_mullion_geometry_draw_indirect *body = (struct wmtcmd_render_mullion_geometry_draw_indirect *)next;
      [encoder setObjectBuffer:(id<MTLBuffer>)body->indirect_args_buffer
                        offset:body->indirect_args_offset
                       atIndex:SM50_BINDING_INDEX_INDIRECT_ARGUMENTS];
      draw_geometry_indirect_parts(encoder, (id<MTLBuffer>)body->dispatch_args_buffer, body->dispatch_args_offset,
                                   body->origins_offset, body->parts, body->vertex_per_warp);
      [encoder setObjectBuffer:(id<MTLBuffer>)body->imm_draw_arguments
                        offset:0
                       atIndex:SM50_BINDING_INDEX_DRAW_ARGUMENTS];
      break;
    }
    case WMTRenderCommandMullionGeometryDrawIndexedIndirect: {
      struct wmtcmd_render_mullion_geometry_draw_indexed_indirect *body =
          (struct wmtcmd_render_mullion_geometry_draw_indexed_indirect *)next;
      [encoder setObjectBuffer:(id<MTLBuffer>)body->index_buffer
                        offset:body->index_buffer_offset
                       atIndex:SM50_BINDING_INDEX_INDEX_BUFFER];
      [encoder setObjectBuffer:(id<MTLBuffer>)body->indirect_args_buffer
                        offset:body->indirect_args_offset
                       atIndex:SM50_BINDING_INDEX_INDIRECT_ARGUMENTS];
      draw_geometry_indirect_parts(encoder, (id<MTLBuffer>)body->dispatch_args_buffer, body->dispatch_args_offset,
                                   body->origins_offset, body->parts, body->vertex_per_warp);
      [encoder setObjectBuffer:(id<MTLBuffer>)body->imm_draw_arguments
                        offset:0
                       atIndex:SM50_BINDING_INDEX_DRAW_ARGUMENTS];
      break;
    }
    case WMTRenderCommandMullionTessellationMeshDraw: {
      struct wmtcmd_render_mullion_tessellation_mesh_draw *body = (struct wmtcmd_render_mullion_tessellation_mesh_draw *)next;
      [encoder setObjectBufferOffset:body->draw_arguments_offset atIndex:SM50_BINDING_INDEX_DRAW_ARGUMENTS];
      [encoder drawMeshThreadgroups:MTLSizeMake(body->patch_per_mesh_instance, body->instance_count, body->parts)
          threadsPerObjectThreadgroup:MTLSizeMake(body->threads_per_patch, body->patch_per_group, 1)
            threadsPerMeshThreadgroup:MTLSizeMake(32, 1, 1)];
      break;
    }
    case WMTRenderCommandMullionTessellationMeshDrawIndexed: {
      struct wmtcmd_render_mullion_tessellation_mesh_draw_indexed *body =
          (struct wmtcmd_render_mullion_tessellation_mesh_draw_indexed *)next;
      [encoder setObjectBuffer:(id<MTLBuffer>)body->index_buffer
                        offset:body->index_buffer_offset
                       atIndex:SM50_BINDING_INDEX_INDEX_BUFFER];
      [encoder setObjectBufferOffset:body->draw_arguments_offset atIndex:SM50_BINDING_INDEX_DRAW_ARGUMENTS];
      [encoder drawMeshThreadgroups:MTLSizeMake(body->patch_per_mesh_instance, body->instance_count, body->parts)
          threadsPerObjectThreadgroup:MTLSizeMake(body->threads_per_patch, body->patch_per_group, 1)
            threadsPerMeshThreadgroup:MTLSizeMake(32, 1, 1)];
      break;
    }

    case WMTRenderCommandMullionTessellationMeshDrawIndirect: {
      struct wmtcmd_render_mullion_tessellation_mesh_draw_indirect *body = (struct wmtcmd_render_mullion_tessellation_mesh_draw_indirect *)next;
      [encoder setObjectBuffer:(id<MTLBuffer>)body->indirect_args_buffer
                        offset:body->indirect_args_offset
                       atIndex:SM50_BINDING_INDEX_INDIRECT_ARGUMENTS];
      [encoder drawMeshThreadgroupsWithIndirectBuffer:(id<MTLBuffer>)body->dispatch_args_buffer
                                 indirectBufferOffset:body->dispatch_args_offset
                          threadsPerObjectThreadgroup:MTLSizeMake(body->threads_per_patch, body->patch_per_group, 1)
                            threadsPerMeshThreadgroup:MTLSizeMake(32, 1, 1)];
      [encoder setObjectBuffer:(id<MTLBuffer>)body->imm_draw_arguments
                        offset:0
                       atIndex:SM50_BINDING_INDEX_DRAW_ARGUMENTS];
      break;
    }
    case WMTRenderCommandMullionTessellationMeshDrawIndexedIndirect: {
      struct wmtcmd_render_mullion_tessellation_mesh_draw_indexed_indirect *body =
          (struct wmtcmd_render_mullion_tessellation_mesh_draw_indexed_indirect *)next;
      [encoder setObjectBuffer:(id<MTLBuffer>)body->index_buffer
                        offset:body->index_buffer_offset
                       atIndex:SM50_BINDING_INDEX_INDEX_BUFFER];
      [encoder setObjectBuffer:(id<MTLBuffer>)body->indirect_args_buffer
                        offset:body->indirect_args_offset
                       atIndex:SM50_BINDING_INDEX_INDIRECT_ARGUMENTS];
      [encoder drawMeshThreadgroupsWithIndirectBuffer:(id<MTLBuffer>)body->dispatch_args_buffer
                                 indirectBufferOffset:body->dispatch_args_offset
                          threadsPerObjectThreadgroup:MTLSizeMake(body->threads_per_patch, body->patch_per_group, 1)
                            threadsPerMeshThreadgroup:MTLSizeMake(32, 1, 1)];
      [encoder setObjectBuffer:(id<MTLBuffer>)body->imm_draw_arguments
                        offset:0
                       atIndex:SM50_BINDING_INDEX_DRAW_ARGUMENTS];
      break;
    }
    case WMTRenderCommandUpdateFence: {
      struct wmtcmd_render_fence_op *body = (struct wmtcmd_render_fence_op *)next;
      [encoder updateFence:(id<MTLFence>)body->fence afterStages:(MTLRenderStages)body->stages];
      break;
    }
    case WMTRenderCommandWaitForFence: {
      struct wmtcmd_render_fence_op *body = (struct wmtcmd_render_fence_op *)next;
      [encoder waitForFence:(id<MTLFence>)body->fence beforeStages:(MTLRenderStages)body->stages];
      break;
    }
    case WMTRenderCommandSetViewport: {
      struct wmtcmd_render_setviewport *body = (struct wmtcmd_render_setviewport *)next;
      union {
        struct WMTViewport src;
        MTLViewport dst;
      } u = {.src = body->viewport};
      [encoder setViewport:u.dst];
      break;
    }
    case WMTRenderCommandSetScissorRect: {
      struct wmtcmd_render_setscissorrect *body = (struct wmtcmd_render_setscissorrect *)next;
      union {
        struct WMTScissorRect src;
        MTLScissorRect dst;
      } u = {.src = body->scissor_rect};
      [encoder setScissorRect:u.dst];
      break;
    }
    case WMTRenderCommandDispatchThreadsPerTile: {
      struct wmtcmd_render_dispatch_threads_per_tile *body = (struct wmtcmd_render_dispatch_threads_per_tile *)next;
      [encoder dispatchThreadsPerTile:MTLSizeMake(body->width, body->height, 1)];
      break;
    }
    case WMTRenderCommandExecuteCommandsInBuffer: {
      struct wmtcmd_render_executecommands *body = (struct wmtcmd_render_executecommands *)next;
      [encoder executeCommandsInBuffer:(id<MTLIndirectCommandBuffer>)body->indirect_command_buffer
                             withRange:NSMakeRange(body->location, body->length)];
      break;
    }
    case WMTRenderCommandExecuteCommandsInBufferIndirect: {
      struct wmtcmd_render_executecommands_indirect *body = (struct wmtcmd_render_executecommands_indirect *)next;
      [encoder executeCommandsInBuffer:(id<MTLIndirectCommandBuffer>)body->indirect_command_buffer
                        indirectBuffer:(id<MTLBuffer>)body->indirect_buffer
                  indirectBufferOffset:body->indirect_buffer_offset];
      break;
    }
    }
    next = next->next.ptr;
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLTexture_pixelFormat(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLTexture>)params->handle pixelFormat];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLTexture_width(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLTexture>)params->handle width];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLTexture_height(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLTexture>)params->handle height];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLTexture_depth(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLTexture>)params->handle depth];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLTexture_arrayLength(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLTexture>)params->handle arrayLength];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLTexture_mipmapLevelCount(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLTexture>)params->handle mipmapLevelCount];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLTexture_replaceRegion(void *obj) {
  struct unixcall_mtltexture_replaceregion *params = obj;
  [(id<MTLTexture>)params->texture replaceRegion:MTLRegionMake3D(
                                                     params->origin.x, params->origin.y, params->origin.z,
                                                     params->size.width, params->size.height, params->size.depth
                                                 )
                                     mipmapLevel:params->level
                                           slice:params->slice
                                       withBytes:params->data.ptr
                                     bytesPerRow:params->bytes_per_row
                                   bytesPerImage:params->bytes_per_image];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLBuffer_didModifyRange(void *obj) {
  struct unixcall_generic_obj_uint64_uint64_ret *params = obj;
  [(id<MTLBuffer>)params->handle didModifyRange:NSMakeRange(params->arg, params->ret)];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_presentDrawable(void *obj) {
  struct unixcall_generic_obj_obj_noret *params = obj;
  [(id<MTLCommandBuffer>)params->handle presentDrawable:(id<MTLDrawable>)params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_presentDrawableAfterMinimumDuration(void *obj) {
  struct unixcall_generic_obj_obj_double_noret *params = obj;
  [(id<MTLCommandBuffer>)params->handle presentDrawable:(id<MTLDrawable>)params->arg0
                                   afterMinimumDuration:params->arg1];
  return STATUS_SUCCESS;
}

// the Apple GPU family the bridge answers as, to test on a newer GPU what an older one takes: MULLION_GPU_FAMILY=7 is
// Apple7. 0: the GPU's own
static unsigned
emulated_family(void) {
  static int family = -1;
  if (family < 0) {
    const char *value = (getenv("MULLION_GPU_FAMILY") ?: getenv("DXMT_GPU_FAMILY"));
    family = value ? atoi(value) : 0;
  }
  return family;
}

static NTSTATUS
_MTLDevice_supportsFamily(void *obj) {
  struct unixcall_generic_obj_uint64_uint64_ret *params = obj;
  MTLGPUFamily family = (MTLGPUFamily)params->arg;
  // the Apple families are numbered from Apple1, before the Mac ones
  bool newer = emulated_family() && family < MTLGPUFamilyMac2 && family - MTLGPUFamilyApple1 >= emulated_family();
  params->ret = !newer && [(id<MTLDevice>)params->handle supportsFamily:family];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_supportsBCTextureCompression(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLDevice>)params->handle supportsBCTextureCompression];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_supports32BitFloatFiltering(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLDevice>)params->handle supports32BitFloatFiltering];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_supports32BitMSAA(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLDevice>)params->handle supports32BitMSAA];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_supportsShaderBarycentricCoordinates(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLDevice>)params->handle supportsShaderBarycentricCoordinates];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_maxArgumentBufferSamplerCount(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLDevice>)params->handle maxArgumentBufferSamplerCount];
  // an emulated Apple7 or Apple8 holds what they do: 1024 samplers a stage reaches from an argument buffer (Metal
  // feature set tables), which an M1 Pro reports here too
  if (emulated_family() && emulated_family() < MTLGPUFamilyApple9 - MTLGPUFamilyApple1 + 1)
    params->ret = MIN(params->ret, 1024);
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_supportsTextureSampleCount(void *obj) {
  struct unixcall_generic_obj_uint64_uint64_ret *params = obj;
  params->ret = [(id<MTLDevice>)params->handle supportsTextureSampleCount:params->arg];
  // render passes take 8 samples from Apple10 on, 4 before (Metal feature set tables)
  if (emulated_family() && emulated_family() < MTLGPUFamilyApple10 - MTLGPUFamilyApple1 + 1)
    params->ret &= params->arg <= 4;
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_hasUnifiedMemory(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLDevice>)params->handle hasUnifiedMemory];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCaptureManager_sharedCaptureManager(void *obj) {
  struct unixcall_generic_obj_ret *params = obj;
  params->ret = (obj_handle_t)[MTLCaptureManager sharedCaptureManager];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCaptureManager_startCapture(void *obj) {
  struct unixcall_mtlcapturemanager_startcapture *params = obj;
  MTLCaptureDescriptor *desc = [[MTLCaptureDescriptor alloc] init];
  const struct WMTCaptureInfo *info = params->info.ptr;
  desc.destination = (MTLCaptureDestination)info->destination;
  desc.captureObject = (id)info->capture_object;
  NSString *path_str = [[NSString alloc] initWithCString:info->output_url.ptr encoding:NSUTF8StringEncoding];
  NSURL *url = [[NSURL alloc] initFileURLWithPath:path_str];
  desc.outputURL = url;
  [(MTLCaptureManager *)params->capture_manager startCaptureWithDescriptor:desc error:nil];
  [url release];
  [path_str release];
  [desc release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCaptureManager_stopCapture(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  [(MTLCaptureManager *)params->handle stopCapture];
  return STATUS_SUCCESS;
}

#include <signal.h>

void
temp_handler(int signum) {
  fprintf(stderr, "received signal %d in temp_handler(), and it may cause problem!\n", signum);
}

static const int SIGNALS[] = {
    SIGHUP,
    SIGINT,
    SIGTERM,
    SIGUSR2,
    SIGILL,
    SIGTRAP,
    SIGABRT,
    SIGFPE,
    SIGBUS,
    SIGSEGV,
    SIGQUIT
#ifdef SIGSYS
    ,
    SIGSYS
#endif
#ifdef SIGXCPU
    ,
    SIGXCPU
#endif
#ifdef SIGXFSZ
    ,
    SIGXFSZ
#endif
#ifdef SIGEMT
    ,
    SIGEMT
#endif
    ,
    SIGUSR1
#ifdef SIGINFO
    ,
    SIGINFO
#endif
};

static NTSTATUS
CREATES(_MTLDevice_newTemporalScaler)(void *obj) {
  struct unixcall_mtldevice_newfxtemporalscaler *params = obj;
  MTLFXTemporalScalerDescriptor *desc = [[MTLFXTemporalScalerDescriptor alloc] init];
  const struct WMTFXTemporalScalerInfo *info = params->info.ptr;
  desc.colorTextureFormat = to_metal_pixel_format(info->color_format);
  desc.outputTextureFormat = to_metal_pixel_format(info->output_format);
  desc.depthTextureFormat = to_metal_pixel_format(info->depth_format);
  desc.motionTextureFormat = to_metal_pixel_format(info->motion_format);
  desc.inputWidth = info->input_width;
  desc.inputHeight = info->input_height;
  desc.outputWidth = info->output_width;
  desc.outputHeight = info->output_height;
  desc.inputContentMaxScale = info->input_content_max_scale;
  desc.inputContentMinScale = info->input_content_min_scale;
  desc.inputContentPropertiesEnabled = info->input_content_properties_enabled;
  #if __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
  if (@available(macOS 15, *)) {
    desc.requiresSynchronousInitialization = info->requires_synchronous_initialization;
  }
  #endif
  desc.autoExposureEnabled = info->auto_exposure;

  struct sigaction old_action[sizeof(SIGNALS) / sizeof(int)], new_action;
  if (@available(macOS 16, *)) {} else {
    new_action.sa_handler = temp_handler;
    sigemptyset(&new_action.sa_mask);
    new_action.sa_flags = 0;
    for (unsigned int i = 0; i < sizeof(SIGNALS) / sizeof(int); i++)
      sigaction(SIGNALS[i], &new_action, &old_action[i]);
  }

  params->ret = (obj_handle_t)[desc newTemporalScalerWithDevice:(id<MTLDevice>)params->device];

  if (@available(macOS 16, *)) {} else {
    for (unsigned int i = 0; i < sizeof(SIGNALS) / sizeof(int); i++)
      sigaction(SIGNALS[i], &old_action[i], NULL);
  }

  [desc release];
  return STATUS_SUCCESS;
}

static NTSTATUS
CREATES(_MTLDevice_newSpatialScaler)(void *obj) {
  struct unixcall_mtldevice_newfxspatialscaler *params = obj;
  MTLFXSpatialScalerDescriptor *desc = [[MTLFXSpatialScalerDescriptor alloc] init];
  const struct WMTFXSpatialScalerInfo *info = params->info.ptr;
  desc.colorTextureFormat = to_metal_pixel_format(info->color_format);
  desc.outputTextureFormat = to_metal_pixel_format(info->output_format);
  desc.inputWidth = info->input_width;
  desc.inputHeight = info->input_height;
  desc.outputWidth = info->output_width;
  desc.outputHeight = info->output_height;
  params->ret = (obj_handle_t)[desc newSpatialScalerWithDevice:(id<MTLDevice>)params->device];
  [desc release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_encodeTemporalScale(void *obj) {
  struct unixcall_mtlcommandbuffer_temporal_scale *params = obj;
  id<MTLCommandBuffer> cmdbuf = (id<MTLCommandBuffer>)params->cmdbuf;
  id<MTLFXTemporalScaler> scaler = (id<MTLFXTemporalScaler>)params->scaler;
  scaler.colorTexture = (id<MTLTexture>)params->color;
  scaler.outputTexture = (id<MTLTexture>)params->output;
  scaler.depthTexture = (id<MTLTexture>)params->depth;
  scaler.motionTexture = (id<MTLTexture>)params->motion;
  scaler.exposureTexture = (id<MTLTexture>)params->exposure;
  scaler.fence = (id<MTLFence>)params->fence;
  const struct WMTFXTemporalScalerProps *props = params->props.ptr;
  scaler.inputContentWidth = props->input_content_width;
  scaler.inputContentHeight = props->input_content_height;
  scaler.reset = props->reset;
  scaler.depthReversed = props->depth_reversed;
  scaler.motionVectorScaleX = props->motion_vector_scale_x;
  scaler.motionVectorScaleY = props->motion_vector_scale_y;
  scaler.jitterOffsetX = props->jitter_offset_x;
  scaler.jitterOffsetY = props->jitter_offset_y;
  scaler.preExposure = props->pre_exposure;
  [scaler encodeToCommandBuffer:cmdbuf];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_encodeSpatialScale(void *obj) {
  struct unixcall_mtlcommandbuffer_spatial_scale *params = obj;
  id<MTLCommandBuffer> cmdbuf = (id<MTLCommandBuffer>)params->cmdbuf;
  id<MTLFXSpatialScaler> scaler = (id<MTLFXSpatialScaler>)params->scaler;
  scaler.colorTexture = (id<MTLTexture>)params->color;
  scaler.outputTexture = (id<MTLTexture>)params->output;
  scaler.fence = (id<MTLFence>)params->fence;
  [scaler encodeToCommandBuffer:cmdbuf];
  return STATUS_SUCCESS;
}

static NTSTATUS
_NSString_string(void *obj) {
  struct unixcall_nsstring_string *params = obj;
  NSString *str = [NSString stringWithCString:params->buffer_ptr.ptr encoding:(NSStringEncoding)params->encoding];
  params->ret = (obj_handle_t)str;
  return STATUS_SUCCESS;
}

static NTSTATUS
_NSString_alloc_init(void *obj) {
  struct unixcall_nsstring_string *params = obj;
  NSString *str = [[NSString alloc] initWithCString:params->buffer_ptr.ptr encoding:(NSStringEncoding)params->encoding];
  params->ret = (obj_handle_t)str;
  return STATUS_SUCCESS;
}

static bool
developer_hud_uses_metric_service(void) {
  NSOperatingSystemVersion version = [NSProcessInfo processInfo].operatingSystemVersion;
  return version.majorVersion >= 27;
}

static NTSTATUS
_DeveloperHUDProperties_instance(void *obj) {
  struct unixcall_generic_obj_ret *params = obj;
  Class cls = objc_lookUpClass(
      developer_hud_uses_metric_service()
          ? "MTLHUDService"
          : "_CADeveloperHUDProperties"
  );
  params->ret =
      (obj_handle_t)((id(*)(id, SEL))objc_msgSend)((id)cls, @selector(instance));
  return STATUS_SUCCESS;
}

static NTSTATUS
_DeveloperHUDProperties_addLabel(void *obj) {
  struct unixcall_generic_obj_obj_obj_uint64_ret *params = obj;
  if (developer_hud_uses_metric_service()) {
    params->ret =
        ((bool (*)(id, SEL, id, id, id, id, uint32_t, uint32_t, uint32_t, uint64_t))objc_msgSend)(
            (id)params->handle,
            @selector(insertMetric:after:name:unit:nameColor:valueColor:visualType:options:),
            (id)params->arg0,
            (id)params->arg1,
            @"",
            @"",
            UINT32_MAX,
            UINT32_MAX,
            1u,
            0u
        );
  } else {
    params->ret =
        ((bool (*)(id, SEL, id, id))objc_msgSend)(
            (id)params->handle,
            @selector(addLabel:after:),
            (id)params->arg0,
            (id)params->arg1
        );
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_DeveloperHUDProperties_updateLabel(void *obj) {
  struct unixcall_generic_obj_obj_obj_noret *params = obj;
  ((void (*)(id, SEL, id, id)
  )objc_msgSend)((id)params->handle, @selector(updateLabel:value:), (id)params->arg0, (id)params->arg1);
  return STATUS_SUCCESS;
}

static NTSTATUS
_DeveloperHUDProperties_remove(void *obj) {
  struct unixcall_generic_obj_obj_noret *params = obj;
  ((void (*)(id, SEL, id))objc_msgSend)((id)params->handle, @selector(remove:), (id)params->arg);
  return STATUS_SUCCESS;
}

static NTSTATUS
_MetalDrawable_texture(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(id<CAMetalDrawable>)params->handle texture];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MetalLayer_nextDrawable(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(CAMetalLayer *)params->handle nextDrawable];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_supportsFXSpatialScaler(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [MTLFXSpatialScalerDescriptor supportsDevice:(id<MTLDevice>)params->handle];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_supportsFXTemporalScaler(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [MTLFXTemporalScalerDescriptor supportsDevice:(id<MTLDevice>)params->handle];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MetalLayer_setProps(void *obj) {
  struct unixcall_generic_obj_constptr_noret *params = obj;
  CAMetalLayer *layer = (CAMetalLayer *)params->handle;
  const struct WMTLayerProps *props = params->arg.ptr;
  execute_on_main(^{
    layer.device = (id<MTLDevice>)props->device;
    layer.opaque = props->opaque;
    layer.framebufferOnly = props->framebuffer_only;
    layer.contentsScale = props->contents_scale;
    layer.displaySyncEnabled = props->display_sync_enabled;
    layer.drawableSize = CGSizeMake(props->drawable_width, props->drawable_height);
    layer.pixelFormat = to_metal_pixel_format(props->pixel_format);
  });
  return STATUS_SUCCESS;
}

static NTSTATUS
_MetalLayer_getProps(void *obj) {
  struct unixcall_generic_obj_ptr_noret *params = obj;
  CAMetalLayer *layer = (CAMetalLayer *)params->handle;
  struct WMTLayerProps *props = params->arg.ptr;
  props->device = (obj_handle_t)layer.device;
  props->opaque = layer.opaque;
  props->framebuffer_only = layer.framebufferOnly;
  props->contents_scale = layer.contentsScale;
  props->display_sync_enabled = layer.displaySyncEnabled;
  props->drawable_height = layer.drawableSize.height;
  props->drawable_width = layer.drawableSize.width;
  props->pixel_format = layer.pixelFormat;
  return STATUS_SUCCESS;
}

typedef struct macdrv_opaque_metal_device *macdrv_metal_device;
typedef struct macdrv_opaque_metal_view *macdrv_metal_view;
typedef struct macdrv_opaque_metal_layer *macdrv_metal_layer;
typedef struct macdrv_opaque_view *macdrv_view;
typedef struct macdrv_opaque_window *macdrv_window;
typedef struct macdrv_opaque_window_data *macdrv_window_data;
typedef struct opaque_window_surface *window_surface;
typedef struct opaque_HWND *HWND;
struct macdrv_win_data {
  HWND hwnd; /* hwnd that this private data belongs to */
  macdrv_window cocoa_window;
  macdrv_view cocoa_view;
  macdrv_view client_cocoa_view;
};

struct macdrv_functions_t {
  void (*macdrv_init_display_devices)(BOOL);
  struct macdrv_win_data *(*get_win_data)(HWND hwnd);
  void (*release_win_data)(struct macdrv_win_data *data);
  macdrv_window (*macdrv_get_cocoa_window)(HWND hwnd, BOOL require_on_screen);
  macdrv_metal_device (*macdrv_create_metal_device)(void);
  void (*macdrv_release_metal_device)(macdrv_metal_device d);
  macdrv_metal_view (*macdrv_view_create_metal_view)(macdrv_view v, macdrv_metal_device d);
  macdrv_metal_layer (*macdrv_view_get_metal_layer)(macdrv_metal_view v);
  void (*macdrv_view_release_metal_view)(macdrv_metal_view v);
  void (*on_main_thread)(dispatch_block_t b);
};

// a Metal layer over a window's client area, and the view that shows it once the main thread has made it. mouse
// events go to Wine's views under it
@interface MullionMetalView : NSView
@end

@implementation MullionMetalView
- (NSView *)hitTest:(NSPoint)point {
  return nil;
}
@end

@interface MullionMetalHost : NSObject {
@public
  CAMetalLayer *layer;
  NSView *view;
}
@end

@implementation MullionMetalHost
@end

// puts a host's view over its window's client area, making it when it is not there yet. on the main thread
static void
MullionPlace(MullionMetalHost *host, struct WMTWindowPlacement placement) {
  for (NSWindow *window in [NSApp windows]) {
    // a 32-bit program's handle is the low half of Wine's
    if (![window respondsToSelector:@selector(hwnd)] ||
        (uint32_t)(uintptr_t)((void *(*)(id, SEL))objc_msgSend)(window, @selector(hwnd)) != (uint32_t)placement.top_level)
      continue;
    // the content view is the top-level window less what Cocoa draws around it. what Wine draws there itself
    // is a border as wide below as beside the client area, and the rest (caption, menu) above
    NSView *content = window.contentView;
    CGFloat scale = placement.screen_width / NSScreen.screens.firstObject.frame.size.width;
    CGFloat border = (content.bounds.size.width - placement.top_level_width / scale) / 2;
    CGFloat above = content.bounds.size.height - placement.top_level_height / scale - border;
    NSRect area = NSMakeRect(
        border + placement.x / scale, above + placement.y / scale, placement.width / scale, placement.height / scale
    );
    if (!content.isFlipped)
      area.origin.y = content.bounds.size.height - NSMaxY(area);
    if (!host->view) {
      host->view = [[MullionMetalView alloc] initWithFrame:area];
      host->view.layer = host->layer;
      host->view.wantsLayer = YES;
    }
    // a window given another parent takes its view along
    if (host->view.superview != content)
      [content addSubview:host->view positioned:NSWindowAbove relativeTo:nil];
    host->view.frame = area;
    return;
  }
  NSLog(@"winemetal: no window to present %#llx in", placement.top_level);
}

static NTSTATUS
_CreateMetalViewFromHWND(void *obj) {
  struct unixcall_create_metal_view_from_hwnd *params = obj;

  struct macdrv_win_data *(*pfn_get_win_data)(HWND hwnd) = NULL;
  void (*pfn_release_win_data)(struct macdrv_win_data *data) = NULL;
  macdrv_metal_view (*pfn_macdrv_view_create_metal_view)(macdrv_view v, macdrv_metal_device d) = NULL;
  macdrv_metal_layer (*pfn_macdrv_view_get_metal_layer)(macdrv_metal_view v) = NULL;

  struct macdrv_functions_t *macdrv_functions;
  if ((macdrv_functions = dlsym(RTLD_DEFAULT, "macdrv_functions"))) {
    pfn_get_win_data = macdrv_functions->get_win_data;
    pfn_release_win_data = macdrv_functions->release_win_data;
    pfn_macdrv_view_create_metal_view = macdrv_functions->macdrv_view_create_metal_view;
    pfn_macdrv_view_get_metal_layer = macdrv_functions->macdrv_view_get_metal_layer;
  } else {
    pfn_get_win_data = dlsym(RTLD_DEFAULT, "get_win_data");
    pfn_release_win_data = dlsym(RTLD_DEFAULT, "release_win_data");
    pfn_macdrv_view_create_metal_view = dlsym(RTLD_DEFAULT, "macdrv_view_create_metal_view");
    pfn_macdrv_view_get_metal_layer = dlsym(RTLD_DEFAULT, "macdrv_view_get_metal_layer");
  }

  if (pfn_get_win_data && pfn_release_win_data && pfn_macdrv_view_create_metal_view &&
      pfn_macdrv_view_get_metal_layer) {
    struct macdrv_win_data *win_data = pfn_get_win_data((HWND)params->hwnd);
    macdrv_metal_view view =
        pfn_macdrv_view_create_metal_view(win_data->client_cocoa_view, (macdrv_metal_device)params->device);
    params->ret_view = (obj_handle_t)view;
    if (view) {
      params->ret_layer = (obj_handle_t)pfn_macdrv_view_get_metal_layer(view);
    }
    pfn_release_win_data(win_data);
  } else if (objc_getClass("WineWindow")) {
    // a Wine whose winemac.drv exports nothing (WineHQ's) still has its Cocoa window in the process. the layer is
    // made here and shown later, by the main thread, which this one must not wait for: it may be waiting on us
    MullionMetalHost *host = [[MullionMetalHost alloc] init];
    // a layer made outside a committed transaction never shows, and this thread has no run loop to commit one
    [CATransaction begin];
    host->layer = [[CAMetalLayer alloc] init];
    host->layer.device = (id<MTLDevice>)params->device;
    [CATransaction commit];
    [CATransaction flush];
    params->ret_view = (obj_handle_t)host;
    params->ret_layer = (obj_handle_t)host->layer;
    struct WMTWindowPlacement placement = params->placement;
    dispatch_async(dispatch_get_main_queue(), ^{
      MullionPlace(host, placement);
    });
  }

  return STATUS_SUCCESS;
}

// a host's view does not follow its window: whoever presents says where the window is now. winemac.drv's own views
// follow theirs
static NTSTATUS
_MetalView_setPlacement(void *obj) {
  struct unixcall_metal_view_set_placement *params = obj;
  if (!(params->ret = [(id)params->view isKindOfClass:[MullionMetalHost class]]))
    return STATUS_SUCCESS;
  MullionMetalHost *host = (MullionMetalHost *)params->view;
  struct WMTWindowPlacement placement = params->placement;
  // the release of the host waits in the same queue, behind this
  dispatch_async(dispatch_get_main_queue(), ^{
    MullionPlace(host, placement);
  });
  return STATUS_SUCCESS;
}

static NTSTATUS
_ReleaseMetalView(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;

  void (*pfn_macdrv_view_release_metal_view)(macdrv_metal_view v) = NULL;

  struct macdrv_functions_t *macdrv_functions;
  if ((macdrv_functions = dlsym(RTLD_DEFAULT, "macdrv_functions"))) {
    pfn_macdrv_view_release_metal_view = macdrv_functions->macdrv_view_release_metal_view;
  } else {
    pfn_macdrv_view_release_metal_view = dlsym(RTLD_DEFAULT, "macdrv_view_release_metal_view");
  }

  if ([(id)params->handle isKindOfClass:[MullionMetalHost class]]) {
    MullionMetalHost *host = (MullionMetalHost *)params->handle;
    dispatch_async(dispatch_get_main_queue(), ^{
      [host->view removeFromSuperview];
      [host->view release];
      [host->layer release];
      [host release];
    });
  } else if (pfn_macdrv_view_release_metal_view)
    pfn_macdrv_view_release_metal_view((macdrv_metal_view)params->handle);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50Initialize(void *args) {
  struct sm50_initialize_params *params = args;

  params->ret =
      SM50Initialize(params->bytecode, params->bytecode_size, params->shader, params->reflection, params->error);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50Destroy(void *args) {
  struct sm50_destroy_params *params = args;

  SM50Destroy(params->shader);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50Compile(void *args) {
  struct sm50_compile_params *params = args;

  params->ret = SM50Compile(params->shader, params->args, params->func_name, params->bitcode, params->error);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50GetCompiledBitcode(void *args) {
  struct sm50_get_compiled_bitcode_params *params = args;

  SM50GetCompiledBitcode(params->bitcode, params->data_out);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50DestroyBitcode(void *args) {
  struct sm50_destroy_bitcode_params *params = args;

  SM50DestroyBitcode(params->bitcode);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50GetErrorMessage(void *args) {
  struct sm50_get_error_message_params *params = args;

  params->ret_size = SM50GetErrorMessage(params->error, params->buffer, params->buffer_size);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50FreeError(void *args) {
  struct sm50_free_error_params *params = args;

  SM50FreeError(params->error);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50CompileTessellationPipelineHull(void *args) {
  struct sm50_compile_tessellation_pipeline_hull_params *params = args;

  params->ret = SM50CompileTessellationPipelineHull(
      params->vertex, params->hull, params->hull_args, params->func_name, params->bitcode, params->error
  );

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50CompileTessellationPipelineDomain(void *args) {
  struct sm50_compile_tessellation_pipeline_domain_params *params = args;

  params->ret = SM50CompileTessellationPipelineDomain(
      params->hull, params->domain, params->domain_args, params->func_name, params->bitcode, params->error
  );

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50CompileGeometryPipelineVertex(void *args) {
  struct sm50_compile_geometry_pipeline_vertex_params *params = args;

  params->ret = SM50CompileGeometryPipelineVertex(
      params->vertex, params->geometry, params->vertex_args, params->func_name, params->bitcode, params->error
  );

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50CompileGeometryPipelineGeometry(void *args) {
  struct sm50_compile_geometry_pipeline_geometry_params *params = args;

  params->ret = SM50CompileGeometryPipelineGeometry(
      params->vertex, params->geometry, params->geometry_args, params->func_name, params->bitcode, params->error
  );

  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandEncoder_setLabel(void *args) {
  struct unixcall_generic_obj_obj_noret *params = args;
  [(id<MTLCommandEncoder>)params->handle setLabel:(NSString *)params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_setShouldMaximizeConcurrentCompilation(void *args) {
  struct unixcall_generic_obj_uint64_noret *params = args;
  [(id<MTLDevice>)params->handle setShouldMaximizeConcurrentCompilation:(BOOL)params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50GetArgumentsInfo(void *args) {
  struct sm50_get_arguments_info_params *params = args;
  SM50GetArgumentsInfo(params->shader, params->constant_buffers, params->arguments);
  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50GetRayShader(void *args) {
  struct sm50_get_ray_shader_params *params = args;
  params->ret = SM50GetRayShader(params->shader, params->index, params->name, params->name_size, params->info);
  return STATUS_SUCCESS;
}

static inline void *
UInt32ToPtr(uint32_t v) {
  return (void *)(uint64_t)v;
}

#ifndef MULLION_NATIVE

static NTSTATUS
thunk32_SM50Initialize(void *args) {
  struct sm50_initialize_params32 *params = args;

  params->ret = SM50Initialize(
      UInt32ToPtr(params->bytecode), params->bytecode_size, UInt32ToPtr(params->shader),
      UInt32ToPtr(params->reflection), UInt32ToPtr(params->error)
  );

  return STATUS_SUCCESS;
}

struct SM50_SHADER_COMMON_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  enum SM50_SHADER_METAL_VERSION metal_version;
  enum SM50_SHADER_FLAG flag;
  uint32_t simd_width;
};

struct SM50_SHADER_COMPILATION_ARGUMENT_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
};

struct SM50_SHADER_IA_INPUT_LAYOUT_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  enum SM50_INDEX_BUFFER_FORMAT index_buffer_format;
  uint32_t slot_mask;
  uint32_t num_elements;
  uint32_t elements;
};

struct SM50_SHADER_PSO_PIXEL_SHADER_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t sample_mask;
  bool dual_source_blending;
  bool disable_depth_output;
  uint32_t unorm_output_reg_mask;
  /** MTLPixelFormat */
  uint32_t pixel_formats[8];
  bool pipeline_statistics;
  bool alpha_to_coverage;
  bool centered_samples;
  bool single_raster_sample;
  uint32_t clip_distances;
  struct SM50_SHADER_BLEND blend[8];
  bool depth_bounds;
  bool late_tests;
  bool conservative;
  bool coarse_shading;
  uint32_t shading_rate_varying;
  bool quad_lines;
};

struct SM50_SHADER_GS_PASS_THROUGH_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  union {
    struct MTL_GEOMETRY_SHADER_PASS_THROUGH Data;
    uint32_t DataEncoded;
  };
  bool RasterizationDisabled;
  bool ClipDistanceVaryings;
};

struct SM50_SHADER_PSO_GEOMETRY_SHADER_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  bool strip_topology;
  bool strip_cut;
  uint32_t strip_cut_index;
  uint32_t input_primitive;
  bool pass_through;
  bool pipeline_statistics;
  bool clip_distance_varyings;
  bool quad_lines;
  bool barycentrics;
  bool conservative;
  uint32_t flat_inputs;
  uint32_t noperspective_inputs;
};

struct SM50_SHADER_STREAM_OUTPUT_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t num_elements;
  uint32_t elements;
  uint32_t strides[4];
  uint32_t rasterized_stream;
  bool counting;
};

struct SM50_SHADER_PSO_TESSELLATOR_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t max_potential_tess_factor;
  uint32_t mesh_vertex_size;
  uint32_t max_mesh_threadgroups;
  sm50_shader_t geometry;
  bool conservative;
  uint32_t flat_inputs;
  uint32_t noperspective_inputs;
  bool barycentrics;
};

struct SM50_SHADER_ROOT_SIGNATURE_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t bytecode;
  uint32_t bytecode_length;
  uint64_t static_samplers;
};

void
sm50_compilation_argument32_convert(
    struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *first_arg, struct SM50_SHADER_COMPILATION_ARGUMENT_DATA32 *args32
) {
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *last_arg = first_arg;

  first_arg->type = SM50_SHADER_ARGUMENT_TYPE_MAX;
  first_arg->next = NULL;

  while (args32) {
    switch (args32->type) {
    case SM50_SHADER_COMMON: {
      struct SM50_SHADER_COMMON_DATA32 *src = (void *)args32;
      struct SM50_SHADER_COMMON_DATA *data = malloc(sizeof(struct SM50_SHADER_COMMON_DATA));
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = src->type;
      data->metal_version = src->metal_version;
      data->flags = src->flag;
      data->simd_width = src->simd_width;
      break;
    }
    case SM50_SHADER_PSO_PIXEL_SHADER: {
      struct SM50_SHADER_PSO_PIXEL_SHADER_DATA32 *src = (void *)args32;
      struct SM50_SHADER_PSO_PIXEL_SHADER_DATA *data = malloc(sizeof(struct SM50_SHADER_PSO_PIXEL_SHADER_DATA));
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = src->type;
      data->unorm_output_reg_mask = src->unorm_output_reg_mask;
      data->disable_depth_output = src->disable_depth_output;
      data->sample_mask = src->sample_mask;
      data->dual_source_blending = src->dual_source_blending;
      memcpy(data->pixel_formats, src->pixel_formats, sizeof(data->pixel_formats));
      data->pipeline_statistics = src->pipeline_statistics;
      data->alpha_to_coverage = src->alpha_to_coverage;
      data->centered_samples = src->centered_samples;
      data->single_raster_sample = src->single_raster_sample;
      data->clip_distances = src->clip_distances;
      memcpy(data->blend, src->blend, sizeof(data->blend));
      data->depth_bounds = src->depth_bounds;
      data->late_tests = src->late_tests;
      data->conservative = src->conservative;
      data->coarse_shading = src->coarse_shading;
      data->shading_rate_varying = src->shading_rate_varying;
      data->quad_lines = src->quad_lines;
      break;
    }
    case SM50_SHADER_IA_INPUT_LAYOUT: {
      struct SM50_SHADER_IA_INPUT_LAYOUT_DATA32 *src = (void *)args32;
      struct SM50_SHADER_IA_INPUT_LAYOUT_DATA *data = malloc(sizeof(struct SM50_SHADER_IA_INPUT_LAYOUT_DATA));
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = src->type;
      data->slot_mask = src->slot_mask;
      data->index_buffer_format = src->index_buffer_format;
      data->num_elements = src->num_elements;
      data->elements = UInt32ToPtr(src->elements);
      break;
    }
    case SM50_SHADER_GS_PASS_THROUGH: {
      struct SM50_SHADER_GS_PASS_THROUGH_DATA32 *src = (void *)args32;
      struct SM50_SHADER_GS_PASS_THROUGH_DATA *data = malloc(sizeof(struct SM50_SHADER_GS_PASS_THROUGH_DATA));
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = src->type;
      data->Data = src->Data;
      data->RasterizationDisabled = src->RasterizationDisabled;
      data->ClipDistanceVaryings = src->ClipDistanceVaryings;
      break;
    }
    case SM50_SHADER_PSO_GEOMETRY_SHADER: {
      struct SM50_SHADER_PSO_GEOMETRY_SHADER_DATA32 *src = (void *)args32;
      struct SM50_SHADER_PSO_GEOMETRY_SHADER_DATA *data = malloc(sizeof(struct SM50_SHADER_PSO_GEOMETRY_SHADER_DATA));
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = src->type;
      data->strip_topology = src->strip_topology;
      data->strip_cut = src->strip_cut;
      data->strip_cut_index = src->strip_cut_index;
      data->input_primitive = src->input_primitive;
      data->pass_through = src->pass_through;
      data->pipeline_statistics = src->pipeline_statistics;
      data->clip_distance_varyings = src->clip_distance_varyings;
      data->quad_lines = src->quad_lines;
      data->barycentrics = src->barycentrics;
      data->conservative = src->conservative;
      data->flat_inputs = src->flat_inputs;
      data->noperspective_inputs = src->noperspective_inputs;
      break;
    }
    case SM50_SHADER_MESH_SHADER: {
      // next, type, then the pixel shader's bytecode
      uint32_t *src = (void *)args32;
      struct SM50_SHADER_MESH_SHADER_DATA *data = calloc(1, sizeof(struct SM50_SHADER_MESH_SHADER_DATA));
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = args32->type;
      data->pixel_shader_bytecode = UInt32ToPtr(src[2]);
      break;
    }
    case SM50_SHADER_RAY_SHADER: {
      // next, type, then the shader's name
      uint32_t *src = (void *)args32;
      struct SM50_SHADER_RAY_SHADER_DATA *data = malloc(sizeof(struct SM50_SHADER_RAY_SHADER_DATA));
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = args32->type;
      data->name = UInt32ToPtr(src[2]);
      break;
    }
    case SM50_SHADER_STREAM_OUTPUT: {
      struct SM50_SHADER_STREAM_OUTPUT_DATA32 *src = (void *)args32;
      struct SM50_SHADER_STREAM_OUTPUT_DATA *data = malloc(sizeof(struct SM50_SHADER_STREAM_OUTPUT_DATA));
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = src->type;
      data->num_elements = src->num_elements;
      data->elements = UInt32ToPtr(src->elements);
      memcpy(data->strides, src->strides, sizeof(data->strides));
      data->rasterized_stream = src->rasterized_stream;
      data->counting = src->counting;
      break;
    }
    case SM50_SHADER_PSO_TESSELLATOR: {
      struct SM50_SHADER_PSO_TESSELLATOR_DATA32 *src = (void *)args32;
      struct SM50_SHADER_PSO_TESSELLATOR_DATA *data = malloc(sizeof(struct SM50_SHADER_PSO_TESSELLATOR_DATA));
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = src->type;
      data->max_potential_tess_factor = src->max_potential_tess_factor;
      data->mesh_vertex_size = src->mesh_vertex_size;
      data->max_mesh_threadgroups = src->max_mesh_threadgroups;
      data->geometry = src->geometry;
      data->conservative = src->conservative;
      data->flat_inputs = src->flat_inputs;
      data->noperspective_inputs = src->noperspective_inputs;
      data->barycentrics = src->barycentrics;
      break;
    }
    case SM50_SHADER_ROOT_SIGNATURE:
    case SM50_SHADER_ROOT_SIGNATURE2: {
      struct SM50_SHADER_ROOT_SIGNATURE_DATA32 *src = (void *)args32;
      struct SM50_SHADER_ROOT_SIGNATURE_DATA *data = malloc(sizeof(struct SM50_SHADER_ROOT_SIGNATURE_DATA));
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = src->type;
      data->bytecode = UInt32ToPtr(src->bytecode);
      data->bytecode_length = src->bytecode_length;
      data->static_samplers = src->static_samplers;
      break;
    }
    case SM50_SHADER_ARGUMENT_TYPE_MAX:
      break;
    }
    args32 = UInt32ToPtr(args32->next);
  }
}

void
sm50_compilation_argument32_free(struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *first_arg) {
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *arg = first_arg->next;

  while (arg) {
    struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *next = arg->next;
    free(arg);
    arg = next;
  }
}

static NTSTATUS
thunk32_SM50Compile(void *args) {
  struct sm50_compile_params32 *params = args;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA first_arg;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA32 *args32 = UInt32ToPtr(params->args);
  sm50_compilation_argument32_convert(&first_arg, args32);

  params->ret = SM50Compile(
      params->shader, &first_arg, UInt32ToPtr(params->func_name), UInt32ToPtr(params->bitcode),
      UInt32ToPtr(params->error)
  );

  sm50_compilation_argument32_free(&first_arg);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_SM50GetCompiledBitcode(void *args) {
  struct sm50_get_compiled_bitcode_params32 *params = args;

  SM50GetCompiledBitcode(params->bitcode, UInt32ToPtr(params->data_out));

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_SM50GetErrorMessage(void *args) {
  struct sm50_get_error_message_params32 *params = args;

  params->ret_size = SM50GetErrorMessage(params->error, UInt32ToPtr(params->buffer), params->buffer_size);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_SM50CompileTessellationPipelineHull(void *args) {
  struct sm50_compile_tessellation_pipeline_hull_params32 *params = args;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA first_arg;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA32 *args32 = UInt32ToPtr(params->hull_args);
  sm50_compilation_argument32_convert(&first_arg, args32);

  params->ret = SM50CompileTessellationPipelineHull(
      params->vertex, params->hull, &first_arg, UInt32ToPtr(params->func_name), UInt32ToPtr(params->bitcode),
      UInt32ToPtr(params->error)
  );

  sm50_compilation_argument32_free(&first_arg);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_SM50CompileTessellationPipelineDomain(void *args) {
  struct sm50_compile_tessellation_pipeline_domain_params32 *params = args;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA first_arg;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA32 *args32 = UInt32ToPtr(params->domain_args);
  sm50_compilation_argument32_convert(&first_arg, args32);

  params->ret = SM50CompileTessellationPipelineDomain(
      params->hull, params->domain, &first_arg, UInt32ToPtr(params->func_name), UInt32ToPtr(params->bitcode),
      UInt32ToPtr(params->error)
  );

  sm50_compilation_argument32_free(&first_arg);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_SM50CompileGeometryPipelineVertex(void *args) {
  struct sm50_compile_geometry_pipeline_vertex_params32 *params = args;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA first_arg;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA32 *args32 = UInt32ToPtr(params->vertex_args);
  sm50_compilation_argument32_convert(&first_arg, args32);

  params->ret = SM50CompileGeometryPipelineVertex(
      params->vertex, params->geometry, &first_arg, UInt32ToPtr(params->func_name), UInt32ToPtr(params->bitcode),
      UInt32ToPtr(params->error)
  );

  sm50_compilation_argument32_free(&first_arg);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_SM50CompileGeometryPipelineGeometry(void *args) {
  struct sm50_compile_geometry_pipeline_geometry_params32 *params = args;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA first_arg;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA32 *args32 = UInt32ToPtr(params->geometry_args);
  sm50_compilation_argument32_convert(&first_arg, args32);

  params->ret = SM50CompileGeometryPipelineGeometry(
      params->vertex, params->geometry, &first_arg, UInt32ToPtr(params->func_name), UInt32ToPtr(params->bitcode),
      UInt32ToPtr(params->error)
  );

  sm50_compilation_argument32_free(&first_arg);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_SM50GetRayShader(void *args) {
  struct sm50_get_ray_shader_params32 *params = args;
  params->ret = SM50GetRayShader(
      params->shader, params->index, UInt32ToPtr(params->name), params->name_size, UInt32ToPtr(params->info)
  );
  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_SM50GetArgumentsInfo(void *args) {
  struct sm50_get_arguments_info_params32 *params = args;

  SM50GetArgumentsInfo(params->shader, UInt32ToPtr(params->constant_buffers), UInt32ToPtr(params->arguments));

  return STATUS_SUCCESS;
}
#endif /* MULLION_NATIVE */

static NTSTATUS
_MTLCommandBuffer_error(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(id<MTLCommandBuffer>)params->handle error];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_logs(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(id<MTLCommandBuffer>)params->handle logs];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLLogContainer_enumerate(void *obj) {
  struct unixcall_enumerate *params = obj;
  uint64_t count = 0;
  uint64_t read = 0;
  id *buffer = params->buffer.ptr;
  for (id _ in (id<MTLLogContainer>)params->enumerable) {
    if (count >= params->start) {
      if (count < params->start + params->buffer_size) {
        buffer[count - params->start] = _;
        read++;
      } else {
        break;
      }
    }
    count++;
  }
  params->ret_read = read;
  return STATUS_SUCCESS;
}

CFStringRef
GetColorSpaceName(enum WMTColorSpace colorspace) {
  switch (colorspace) {
  case WMTColorSpaceSRGB:
    return kCGColorSpaceSRGB;
  case WMTColorSpaceSRGBLinear:
  case WMTColorSpaceHDR_scRGB:
    return kCGColorSpaceExtendedLinearSRGB;
  case WMTColorSpaceBT2020:
    return kCGColorSpaceITUR_2020_sRGBGamma;
  case WMTColorSpaceHDR_PQ:
    return kCGColorSpaceITUR_2100_PQ;
  default:
    return nil;
  }
}

static NTSTATUS
_CGColorSpace_checkColorSpaceSupported(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = false;
  CFStringRef name = GetColorSpaceName((enum WMTColorSpace)params->handle);
  if (!name)
    return STATUS_SUCCESS;
  CGColorSpaceRef ref = CGColorSpaceCreateWithName(name);
  if (!ref)
    return STATUS_SUCCESS;
  CGColorSpaceRelease(ref);
  params->ret = true;
  return STATUS_SUCCESS;
}

static NTSTATUS
_MetalLayer_setColorSpace(void *obj) {
  struct unixcall_generic_obj_uint64_uint64_ret *params = obj;
  CAMetalLayer *layer = (CAMetalLayer *)params->handle;
  enum WMTColorSpace colorspace = params->arg;
  CFStringRef name = GetColorSpaceName(colorspace);
  params->ret = false;
  if (!name)
    return STATUS_SUCCESS;
  CGColorSpaceRef ref = CGColorSpaceCreateWithName(name);
  if (!ref)
    return STATUS_SUCCESS;
  execute_on_main(^{
    layer.colorspace = ref;
    layer.wantsExtendedDynamicRangeContent = WMT_COLORSPACE_IS_HDR(colorspace);
    CGColorSpaceRelease(ref);
  });
  params->ret = true;
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTGetPrimaryDisplayId(void *obj) {
  struct unixcall_generic_obj_ret *params = obj;
  params->ret = CGMainDisplayID();
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTGetSecondaryDisplayId(void *obj) {
  struct unixcall_generic_obj_ret *params = obj;
  params->ret = kCGNullDirectDisplay;

  uint32_t count = 0;
  CGGetOnlineDisplayList(0, NULL, &count);

  if (count == 0)
    return STATUS_SUCCESS;

  CGDirectDisplayID main_display = CGMainDisplayID();
  CGDirectDisplayID displays[count];
  CGGetOnlineDisplayList(count, displays, &count);

  for (uint32_t i = 0; i < count; i++) {
    CGDirectDisplayID id = displays[i];
    if (id == main_display)
      continue;
    if (CGDisplayMirrorsDisplay(id) != kCGNullDirectDisplay)
      continue;
    params->ret = id;
    break;
  }

  return STATUS_SUCCESS;
}

typedef struct icc_XYZ_t {
  uint32_t sig;      // 0x205a5958
  uint32_t reserved; // 0
  int32_t x;
  int32_t y;
  int32_t z;
} icc_XYZ_t;

bool
GetChromaticity_xy(ColorSyncProfileRef profile, CFStringRef tag, float *out_x, float *out_y) {
  CFDataRef tag_data = ColorSyncProfileCopyTag(profile, tag);
  if (!tag_data)
    return false;
  if (CFDataGetLength(tag_data) != sizeof(icc_XYZ_t))
    return false;
  icc_XYZ_t *data = (icc_XYZ_t *)CFDataGetBytePtr(tag_data);
  if (data->sig != 0x205a5958)
    return false;
  double X = (int32_t)__builtin_bswap32(data->x) / 65536.0;
  double Y = (int32_t)__builtin_bswap32(data->y) / 65536.0;
  double Z = (int32_t)__builtin_bswap32(data->z) / 65536.0;
  *out_x = X / (X + Y + Z);
  *out_y = Y / (X + Y + Z);
  return true;
}

bool
GetDisplayColorGamut(ColorSyncProfileRef profile, struct WMTDisplayDescription *desc_out) {
  return GetChromaticity_xy(
             profile, kColorSyncSigMediaWhitePointTag, &desc_out->white_points[0], &desc_out->white_points[1]
         ) &&
         GetChromaticity_xy(
             profile, kColorSyncSigRedColorantTag, &desc_out->red_primaries[0], &desc_out->red_primaries[1]
         ) &&
         GetChromaticity_xy(
             profile, kColorSyncSigGreenColorantTag, &desc_out->green_primaries[0], &desc_out->green_primaries[1]
         ) &&
         GetChromaticity_xy(
             profile, kColorSyncSigBlueColorantTag, &desc_out->blue_primaries[0], &desc_out->blue_primaries[1]
         );
}

NSScreen *
GetNSScreenForDisplayID(CGDirectDisplayID display_id) {
  for (NSScreen *screen in [NSScreen screens]) {
    CGDirectDisplayID id = [[[screen deviceDescription] objectForKey:@"NSScreenNumber"] unsignedIntValue];
    if (id == display_id) {
      return screen;
    }
  }
  return nil;
}

static NTSTATUS
_WMTGetDisplayDescription(void *obj) {
  struct unixcall_generic_obj_ptr_noret *params = obj;
  CGDirectDisplayID display_id = params->handle;
  struct WMTDisplayDescription *desc_out = params->arg.ptr;
  ColorSyncProfileRef profile = ColorSyncProfileCreateWithDisplayID(display_id);
  if (!profile || !GetDisplayColorGamut(profile, desc_out))
    GetDisplayColorGamut(ColorSyncProfileCreateWithName(kColorSyncGenericRGBProfile), desc_out);
  NSScreen *screen = GetNSScreenForDisplayID(display_id);
  if (screen) {
    desc_out->maximum_edr_color_component_value = [screen maximumExtendedDynamicRangeColorComponentValue];
    desc_out->maximum_reference_edr_color_component_value =
        [screen maximumReferenceExtendedDynamicRangeColorComponentValue];
    desc_out->maximum_potential_edr_color_component_value =
        [screen maximumPotentialExtendedDynamicRangeColorComponentValue];
  } else {
    desc_out->maximum_edr_color_component_value = 1.0;
    desc_out->maximum_reference_edr_color_component_value = 0.0;
    desc_out->maximum_potential_edr_color_component_value = 1.0;
  }
  return STATUS_SUCCESS;
}

struct DisplaySetting {
  uint64_t version;
  enum WMTColorSpace colorspace;
  struct WMTHDRMetadata hdr_metadata;
};

struct DisplaySetting g_display_settings[2] = {{0, 0, {}}, {0, 0, {}}};

static NTSTATUS
_MetalLayer_getEDRValue(void *obj) {
  struct unixcall_generic_obj_ptr_noret *params = obj;
  CAMetalLayer *layer = (CAMetalLayer *)params->handle;
  struct WMTEDRValue *value = params->arg.ptr;
  value->maximum_edr_color_component_value = 1.0;
  value->maximum_potential_edr_color_component_value = 1.0;

  if (![layer.delegate isKindOfClass:NSView.class])
    return STATUS_SUCCESS;

  NSView *view = (NSView *)layer.delegate;
  if (!view.window)
    return STATUS_SUCCESS;

  if (!view.window.screen)
    return STATUS_SUCCESS;

  NSScreen *screen = view.window.screen;

  value->maximum_edr_color_component_value =
      layer.wantsExtendedDynamicRangeContent ? screen.maximumExtendedDynamicRangeColorComponentValue : 1.0;
  value->maximum_potential_edr_color_component_value = screen.maximumPotentialExtendedDynamicRangeColorComponentValue;

  return STATUS_SUCCESS;
}

static NTSTATUS
CREATES(_MTLLibrary_newFunctionWithConstants)(void *obj) {
  struct unixcall_mtllibrary_newfunction_with_constants *params = obj;
  id<MTLLibrary> library = (id<MTLLibrary>)params->library;
  NSString *name = [[NSString alloc] initWithCString:(char *)params->name.ptr encoding:NSUTF8StringEncoding];
  struct WMTFunctionConstant *constants = (struct WMTFunctionConstant *)params->constants.ptr;
  NSError *err = NULL;
  MTLFunctionConstantValues *values = [[MTLFunctionConstantValues alloc] init];
  for (uint64_t i = 0; i < params->num_constants; i++)
    [values setConstantValue:constants[i].data.ptr type:(MTLDataType)constants[i].type atIndex:constants[i].index];

  params->ret = (obj_handle_t)[library newFunctionWithName:name constantValues:values error:&err];
  params->ret_error = (obj_handle_t)[err retain];
  [name release];
  [values release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTQueryDisplaySetting(void *obj) {
  struct unixcall_query_display_setting *params = obj;
  CGDirectDisplayID display_id = params->display_id;
  struct WMTHDRMetadata *value = params->hdr_metadata.ptr;
  params->ret = false;
  struct DisplaySetting *setting = &g_display_settings[display_id == CGMainDisplayID()];
  if (setting->version) {
    *value = setting->hdr_metadata;
    params->colorspace = setting->colorspace;
    params->ret = true;
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTUpdateDisplaySetting(void *obj) {
  struct unixcall_update_display_setting *params = obj;
  CGDirectDisplayID display_id = params->display_id;
  const struct WMTHDRMetadata *value = params->hdr_metadata.ptr;
  struct DisplaySetting *setting = &g_display_settings[display_id == CGMainDisplayID()];
  if (value) {
    setting->hdr_metadata = *value;
    setting->colorspace = params->colorspace;
    setting->version++;
  } else {
    setting->version = 0;
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTQueryDisplaySettingForLayer(void *obj) {
  struct unixcall_query_display_setting_for_layer *params = obj;
  CAMetalLayer *layer = (CAMetalLayer *)params->layer;
  struct WMTHDRMetadata *hdr_metadata_out = params->hdr_metadata.ptr;

  params->version = 0;
  if (![layer.delegate isKindOfClass:NSView.class])
    return STATUS_SUCCESS;

  NSView *view = (NSView *)layer.delegate;
  if (!view.window)
    return STATUS_SUCCESS;

  if (!view.window.screen)
    return STATUS_SUCCESS;

  NSScreen *screen = view.window.screen;
  CGDirectDisplayID id = [[[screen deviceDescription] objectForKey:@"NSScreenNumber"] unsignedIntValue];

  struct DisplaySetting *setting = &g_display_settings[id == CGMainDisplayID()];
  *hdr_metadata_out = setting->hdr_metadata;
  params->version = setting->version;
  params->colorspace = setting->colorspace;
  params->edr_value.maximum_edr_color_component_value =
      layer.wantsExtendedDynamicRangeContent ? screen.maximumExtendedDynamicRangeColorComponentValue : 1.0;
  params->edr_value.maximum_potential_edr_color_component_value =
      screen.maximumPotentialExtendedDynamicRangeColorComponentValue;

  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_encodeWaitForEvent(void *obj) {
  struct unixcall_generic_obj_obj_uint64_noret *params = obj;
  [(id<MTLCommandBuffer>)params->handle encodeWaitForEvent:(id<MTLSharedEvent>)params->arg0 value:params->arg1];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLSharedEvent_signalValue(void *obj) {
  struct unixcall_generic_obj_uint64_noret *params = obj;
  [(id<MTLSharedEvent>)params->handle setSignaledValue:params->arg];
  return STATUS_SUCCESS;
}

#ifndef MULLION_NATIVE

typedef struct {
  _Atomic(CFRunLoopRef) runloop_ref;
  MTLSharedEventListener *shared_listener;
  pthread_mutex_t mutex;
  bool stopped;
} *shared_event_listener_t;

extern NTSTATUS NtSetEvent(void *handle, void *prev_state);

static NTSTATUS
_MTLSharedEvent_setWin32EventAtValue(void *obj) {
  struct unixcall_mtlsharedevent_setevent *params = obj;
  void *nt_event_handle = (shared_event_listener_t)params->event_handle;
  shared_event_listener_t q = (shared_event_listener_t)params->shared_event_listener;
  [(id<MTLSharedEvent>)params->shared_event
      notifyListener:q->shared_listener
             atValue:params->value
               block:^(id<MTLSharedEvent> _e, uint64_t _v) {
                 // NOTE: must ensure no more notification comes after listener been destroyed.
                 while (!atomic_load_explicit(&q->runloop_ref, memory_order_acquire)) {
#if defined(__x86_64__)
                   _mm_pause();
#elif defined(__aarch64__)
          __asm__ __volatile__("yield");
#endif
                 }
                 CFRunLoopPerformBlock(q->runloop_ref, kCFRunLoopCommonModes, ^{
                   NtSetEvent(nt_event_handle, NULL);
                 });
                 CFRunLoopWakeUp(q->runloop_ref);
               }];
  return STATUS_SUCCESS;
}

static NTSTATUS
_SharedEventListener_start(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  shared_event_listener_t q = (shared_event_listener_t)params->handle;
  if (q) {
    CFRunLoopRef runloop = CFRunLoopGetCurrent();
    /* Add a dummy source so the runloop stays running */
    CFRunLoopSourceContext source_context = {0};
    CFRunLoopSourceRef source = CFRunLoopSourceCreate(NULL, 0, &source_context);
    CFRunLoopAddSource(runloop, source, kCFRunLoopCommonModes);
    pthread_mutex_lock(&q->mutex);
    atomic_store_explicit(&q->runloop_ref, runloop, memory_order_release);
    bool stopped = q->stopped;
    pthread_mutex_unlock(&q->mutex);
    if (!stopped)
      CFRunLoopRun();
    pthread_mutex_lock(&q->mutex);
    [q->shared_listener release];
    pthread_mutex_unlock(&q->mutex);
    CFRunLoopRemoveSource(runloop, source, kCFRunLoopCommonModes);
    CFRelease(source);
    pthread_mutex_destroy(&q->mutex);
    free(q);
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_SharedEventListener_create(void *obj) {
  struct unixcall_generic_obj_ret *params = obj;
  shared_event_listener_t q = malloc(sizeof(*q));
  if (q) {
    q->runloop_ref = NULL;
    q->shared_listener = [[MTLSharedEventListener alloc] init];
    q->mutex = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
    q->stopped = false;
  }
  params->ret = (obj_handle_t)q;
  return STATUS_SUCCESS;
}

static NTSTATUS
_SharedEventListener_destroy(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  shared_event_listener_t q = (shared_event_listener_t)params->handle;
  if (q) {
    pthread_mutex_lock(&q->mutex);
    q->stopped = true;
    CFRunLoopRef runloop = atomic_load_explicit(&q->runloop_ref, memory_order_acquire);
    if (runloop) {
      CFRunLoopPerformBlock(runloop, kCFRunLoopCommonModes, ^{
        CFRunLoopStop(runloop);
      });
      CFRunLoopWakeUp(runloop);
    }
    pthread_mutex_unlock(&q->mutex);
  }
  return STATUS_SUCCESS;
}

#else
static NTSTATUS
_MTLSharedEvent_setWin32EventAtValue(void *obj) {
  // nop
  return STATUS_SUCCESS;
}

static NTSTATUS
_SharedEventListener_start(void *obj) {
  return STATUS_SUCCESS;
}

static NTSTATUS
_SharedEventListener_create(void *obj) {
  return STATUS_SUCCESS;
}

static NTSTATUS
_SharedEventListener_destroy(void *obj) {
  return STATUS_SUCCESS;
}

#endif

static NTSTATUS
CREATES(_MTLDevice_newFence)(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(id<MTLDevice>)params->handle newFence];
  return STATUS_SUCCESS;
}

static NTSTATUS
CREATES(_MTLDevice_newEvent)(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(id<MTLDevice>)params->handle newEvent];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLBuffer_updateContents(void *obj) {
  struct unixcall_mtlbuffer_updatecontents *params = obj;
  memcpy((void *)((char *)[(id<MTLBuffer>)params->buffer contents] + params->offset), params->data.ptr, params->length);
  if ([(id<MTLBuffer>)params->buffer storageMode] == MTLStorageModeManaged)
    [(id<MTLBuffer>)params->buffer didModifyRange:NSMakeRange(params->offset, params->length)];
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTGetOSVersion(void *obj) {
  struct unixcall_get_os_version *params = obj;
  NSOperatingSystemVersion version = [NSProcessInfo processInfo].operatingSystemVersion;
  params->ret_major = version.majorVersion;
  params->ret_minor = version.minorVersion;
  params->ret_patch = version.patchVersion;
  return STATUS_SUCCESS;
}

static NTSTATUS
CREATES(_MTLDevice_newBinaryArchive)(void *obj) {
  struct unixcall_mtldevice_newbinaryarchive *params = obj;
  NSString *path_str = NULL;
  NSURL *url = NULL;
  MTLBinaryArchiveDescriptor *desc = [[MTLBinaryArchiveDescriptor alloc] init];
  if (params->url.ptr != NULL) {
    path_str = [[NSString alloc] initWithCString:params->url.ptr encoding:NSUTF8StringEncoding];
    url = [[NSURL alloc] initFileURLWithPath:path_str];
    desc.url = url;
  }
  NSError *err = NULL;
  params->ret_archive = (obj_handle_t)[(id<MTLDevice>)params->device newBinaryArchiveWithDescriptor:desc error:&err];
  params->ret_error = (obj_handle_t)[err retain];
  [desc release];
  if (url)
    [url release];
  if (path_str)
    [path_str release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLBinaryArchive_serialize(void *obj) {
  struct unixcall_mtlbinaryarchive_serialize *params = obj;
  NSString *path_str = [[NSString alloc] initWithCString:params->url.ptr encoding:NSUTF8StringEncoding];
  NSURL *url = [[NSURL alloc] initFileURLWithPath:path_str];
  NSError *err = NULL;
  [(id<MTLBinaryArchive>)params->archive serializeToURL:url error:&err];
  params->ret_error = (obj_handle_t)[err retain];
  [url release];
  [path_str release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_DispatchData_alloc_init(void *obj) {
  struct unixcall_generic_obj_uint64_obj_ret *params = obj;
  params->ret = (obj_handle_t)dispatch_data_create((void *)params->handle, params->arg, NULL, NULL);
  return STATUS_SUCCESS;
}

@interface MTLSharedTextureHandle ()

- (MTLSharedTextureHandle *)initWithMachPort:(mach_port_t)port;
- (mach_port_t)createMachPort;

@end

static NTSTATUS
CREATES(_MTLDevice_newSharedTexture)(void *obj) {
  struct unixcall_mtldevice_newtexture *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  struct WMTTextureInfo *info = params->info.ptr;

  if (info->mach_port) {
    MTLSharedTextureHandle *handle = [[MTLSharedTextureHandle alloc] initWithMachPort:info->mach_port];
    id<MTLTexture> ret = [device newSharedTextureWithHandle:handle];
    extract_texture_descriptor(ret, info);
    params->ret = (obj_handle_t)ret;
    info->gpu_resource_id = [ret gpuResourceID]._impl;
    [handle release];
  } else {
    MTLTextureDescriptor *desc = [[MTLTextureDescriptor alloc] init];
    fill_texture_descriptor(desc, info);
    id<MTLTexture> ret = [device newSharedTextureWithDescriptor:desc];
    MTLSharedTextureHandle *handle = [ret newSharedTextureHandle];
    params->ret = (obj_handle_t)ret;
    info->gpu_resource_id = [ret gpuResourceID]._impl;
    info->mach_port = [handle createMachPort]; // implicitly add ref to underlying IOSurface
    [handle release];
    [desc release];
  }

  return STATUS_SUCCESS;
}

/* Private API to register a mach port with the bootstrap server */
extern kern_return_t bootstrap_register2(mach_port_t bp, name_t service_name, mach_port_t sp, int flags);

/* A shared object's name. launchd keeps a name until the receive right of the port registered under it dies, and a
 * process cannot remove its own name (bootstrap_register2 of MACH_PORT_NULL fails), so the object's port is not
 * registered: its receive right lives in the GPU driver and never dies. The name holds a receive right of this
 * process's own instead, which answers each request sent to it with a copy of the object's port, and is destroyed
 * when the last object of this process made from that port goes; launchd then drops the name. An open after that, or
 * after this process exits, fails, where Windows keeps the object while any NT handle to it is open. */
struct shared_name {
  struct shared_name *next;
  char name[128];
  mach_port_t receive; /* registered under the name */
  mach_port_t port;    /* a send right to the object's port, handed out */
  uint32_t objects;    /* objects of this process that keep the name, under shared_names_lock */
  dispatch_source_t source;
};

/* the request an opener sends to the name's port, a bare header, and the answer it gets on its reply port */
enum { shared_name_request = 0x4d4c534e, shared_name_reply_id = shared_name_request + 100 };
struct shared_name_reply {
  mach_msg_header_t header;
  mach_msg_body_t body;
  mach_msg_port_descriptor_t port;
};

static pthread_mutex_t shared_names_lock = PTHREAD_MUTEX_INITIALIZER;
static struct shared_name *shared_names;

static struct shared_name *
shared_name_find(const char *name) {
  for (struct shared_name *entry = shared_names; entry; entry = entry->next)
    if (!strcmp(entry->name, name))
      return entry;
  return NULL;
}

static void
shared_name_release(struct shared_name *entry) {
  pthread_mutex_lock(&shared_names_lock);
  bool last = !--entry->objects;
  if (last)
    for (struct shared_name **link = &shared_names; *link; link = &(*link)->next)
      if (*link == entry) {
        *link = entry->next;
        break;
      }
  pthread_mutex_unlock(&shared_names_lock);
  if (!last)
    return;
  /* the cancel handler runs after the last request handler, and destroys the receive right */
  dispatch_source_cancel(entry->source);
  dispatch_release(entry->source);
}

/* attached to each object that keeps a name: the name goes with the last of them */
@interface WMTSharedNameKeeper : NSObject {
@public
  struct shared_name *entry;
}
@end

@implementation WMTSharedNameKeeper
- (void)dealloc {
  shared_name_release(entry);
  [super dealloc];
}
@end

static void
shared_name_keep(struct shared_name *entry, id object) {
  WMTSharedNameKeeper *keeper = [[WMTSharedNameKeeper alloc] init];
  keeper->entry = entry;
  objc_setAssociatedObject(object, keeper, keeper, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
  [keeper release];
}

static void
shared_name_answer(struct shared_name *entry) {
  union {
    mach_msg_header_t header;
    char buffer[sizeof(mach_msg_header_t) + MAX_TRAILER_SIZE];
  } request;
  for (;;) {
    kern_return_t kr = mach_msg(
        &request.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(request), entry->receive, 0, MACH_PORT_NULL
    );
    if (kr == MACH_RCV_TOO_LARGE)
      continue;
    if (kr != KERN_SUCCESS)
      return;
    if (request.header.msgh_id != shared_name_request || request.header.msgh_size != sizeof(mach_msg_header_t) ||
        (request.header.msgh_bits & MACH_MSGH_BITS_COMPLEX) ||
        MACH_MSGH_BITS_REMOTE(request.header.msgh_bits) != MACH_MSG_TYPE_PORT_SEND_ONCE) {
      mach_msg_destroy(&request.header);
      continue;
    }
    struct shared_name_reply reply = {};
    reply.header.msgh_bits = MACH_MSGH_BITS_COMPLEX | MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
    reply.header.msgh_size = sizeof(reply);
    reply.header.msgh_remote_port = request.header.msgh_remote_port;
    reply.header.msgh_id = shared_name_reply_id;
    reply.body.msgh_descriptor_count = 1;
    reply.port.name = entry->port;
    reply.port.disposition = MACH_MSG_TYPE_COPY_SEND;
    reply.port.type = MACH_MSG_PORT_DESCRIPTOR;
    /* an opener that stopped waiting left a dead name, which the failed send did not consume */
    if (mach_msg(&reply.header, MACH_SEND_MSG, sizeof(reply), 0, MACH_PORT_NULL, 0, MACH_PORT_NULL) ==
        MACH_SEND_INVALID_DEST)
      mach_port_deallocate(mach_task_self(), request.header.msgh_remote_port);
  }
}

static dispatch_queue_t
shared_name_queue(void) {
  static dispatch_once_t once;
  static dispatch_queue_t queue;
  dispatch_once(&once, ^{ queue = dispatch_queue_create("mullion-shared-names", DISPATCH_QUEUE_SERIAL); });
  return queue;
}

/* registers the name of `object`, whose port is `mach_port`; the name lasts while object does */
static NTSTATUS
_WMTBootstrapRegister(void *obj) {
  struct unixcall_bootstrap *params = obj;
  mach_port_t bp;

  if (!params->object || task_get_bootstrap_port(mach_task_self(), &bp) != KERN_SUCCESS)
    return STATUS_UNSUCCESSFUL;
  struct shared_name *entry = calloc(1, sizeof(*entry));
  if (!entry || mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &entry->receive) != KERN_SUCCESS) {
    free(entry);
    mach_port_deallocate(mach_task_self(), bp);
    return STATUS_UNSUCCESSFUL;
  }
  memcpy(entry->name, params->name, sizeof(entry->name));
  entry->name[sizeof(entry->name) - 1] = 0;
  /* the name hands out a send right of its own to the object's port, which must be one */
  entry->port = params->mach_port;
  kern_return_t kr = entry->port ? mach_port_mod_refs(mach_task_self(), entry->port, MACH_PORT_RIGHT_SEND, 1)
                                 : KERN_INVALID_NAME;
  /* launchd copies the send right it is given */
  if (kr == KERN_SUCCESS) {
    kr = mach_port_insert_right(mach_task_self(), entry->receive, entry->receive, MACH_MSG_TYPE_MAKE_SEND);
    if (kr == KERN_SUCCESS) {
      kr = bootstrap_register2(bp, entry->name, entry->receive, 0);
      mach_port_deallocate(mach_task_self(), entry->receive);
    }
    if (kr != KERN_SUCCESS)
      mach_port_deallocate(mach_task_self(), entry->port);
  }
  mach_port_deallocate(mach_task_self(), bp);
  if (kr != KERN_SUCCESS) {
    mach_port_mod_refs(mach_task_self(), entry->receive, MACH_PORT_RIGHT_RECEIVE, -1);
    free(entry);
    return STATUS_UNSUCCESSFUL;
  }
  entry->objects = 1;
  entry->source = dispatch_source_create(DISPATCH_SOURCE_TYPE_MACH_RECV, entry->receive, 0, shared_name_queue());
  dispatch_source_set_event_handler(entry->source, ^{ shared_name_answer(entry); });
  dispatch_source_set_cancel_handler(entry->source, ^{
    mach_port_mod_refs(mach_task_self(), entry->receive, MACH_PORT_RIGHT_RECEIVE, -1);
    mach_port_deallocate(mach_task_self(), entry->port);
    free(entry);
  });
  pthread_mutex_lock(&shared_names_lock);
  entry->next = shared_names;
  shared_names = entry;
  pthread_mutex_unlock(&shared_names_lock);
  shared_name_keep(entry, (id)params->object);
  dispatch_resume(entry->source);
  return STATUS_SUCCESS;
}

/* a send right to the port of the object registered under the name: this process's own, or the answer of the
 * process that registered it. a name of this process's own is held until WMTBootstrapKeep passes the hold to the
 * object made from the port, or drops it, so the name cannot go in between */
static NTSTATUS
_WMTBootstrapLookUp(void *obj) {
  struct unixcall_bootstrap *params = obj;
  mach_port_t bp, name_port = 0, reply_port;

  params->mach_port = 0;
  params->name[sizeof(params->name) - 1] = 0;
  pthread_mutex_lock(&shared_names_lock);
  struct shared_name *entry = shared_name_find(params->name);
  if (entry && mach_port_mod_refs(mach_task_self(), entry->port, MACH_PORT_RIGHT_SEND, 1) == KERN_SUCCESS) {
    entry->objects++;
    params->mach_port = entry->port;
  }
  pthread_mutex_unlock(&shared_names_lock);
  if (entry)
    return params->mach_port ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;

  if (task_get_bootstrap_port(mach_task_self(), &bp) != KERN_SUCCESS)
    return STATUS_UNSUCCESSFUL;
  kern_return_t kr = bootstrap_look_up(bp, params->name, &name_port);
  mach_port_deallocate(mach_task_self(), bp);
  if (kr != KERN_SUCCESS)
    return STATUS_UNSUCCESSFUL;
  if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &reply_port) != KERN_SUCCESS) {
    mach_port_deallocate(mach_task_self(), name_port);
    return STATUS_UNSUCCESSFUL;
  }
  union {
    mach_msg_header_t request;
    struct shared_name_reply reply;
    char buffer[sizeof(struct shared_name_reply) + MAX_TRAILER_SIZE];
  } msg = {};
  msg.request.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE);
  msg.request.msgh_size = sizeof(msg.request);
  msg.request.msgh_remote_port = name_port;
  msg.request.msgh_local_port = reply_port;
  msg.request.msgh_id = shared_name_request;
  /* a registering process that has gone answers with a send-once notification, not a port */
  kr = mach_msg(
      &msg.request, MACH_SEND_MSG | MACH_RCV_MSG | MACH_SEND_TIMEOUT | MACH_RCV_TIMEOUT, sizeof(msg.request),
      sizeof(msg), reply_port, 5000, MACH_PORT_NULL
  );
  /* a request that was not sent comes back with its rights */
  if (kr == MACH_SEND_TIMED_OUT || kr == MACH_SEND_INTERRUPTED)
    mach_msg_destroy(&msg.request);
  if (kr == KERN_SUCCESS) {
    if (msg.reply.header.msgh_id == shared_name_reply_id && (msg.reply.header.msgh_bits & MACH_MSGH_BITS_COMPLEX) &&
        msg.reply.body.msgh_descriptor_count == 1 && msg.reply.port.type == MACH_MSG_PORT_DESCRIPTOR &&
        msg.reply.port.disposition == MACH_MSG_TYPE_PORT_SEND)
      params->mach_port = msg.reply.port.name;
    else
      mach_msg_destroy(&msg.reply.header);
  }
  mach_port_mod_refs(mach_task_self(), reply_port, MACH_PORT_RIGHT_RECEIVE, -1);
  mach_port_deallocate(mach_task_self(), name_port);
  return params->mach_port ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

/* passes the hold WMTBootstrapLookUp took on a name of this process to `object`, made from the port, which then
 * keeps the name as its registrant does; without an object the hold is dropped. a name another process registered
 * is kept by that process's objects alone */
static NTSTATUS
_WMTBootstrapKeep(void *obj) {
  struct unixcall_bootstrap *params = obj;
  params->name[sizeof(params->name) - 1] = 0;
  pthread_mutex_lock(&shared_names_lock);
  struct shared_name *entry = shared_name_find(params->name);
  pthread_mutex_unlock(&shared_names_lock);
  if (entry && params->object)
    shared_name_keep(entry, (id)params->object);
  else if (entry)
    shared_name_release(entry);
  return STATUS_SUCCESS;
}

@protocol MTLDeviceSPI <MTLDevice>

- (id<MTLSharedEvent>)newSharedEventWithMachPort:(mach_port_t)machPort;

@end

@interface MTLSharedEventHandle ()

- (mach_port_t)eventPort;

@end

static NTSTATUS
CREATES(_MTLSharedEvent_createMachPort)(void *obj) {
  struct unixcall_mtlsharedevent_createmachport *params = obj;
  id<MTLSharedEvent> event = (id<MTLSharedEvent>)params->event;
  MTLSharedEventHandle *handle = [event newSharedEventHandle];
  mach_port_t port = [handle eventPort];
  
  // The eventPort method returns a send right that's owned by the handle.
  // We need to add our own send right since we're keeping the port but releasing the handle.
  // This increments the send right count so the port remains valid.
  mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_SEND, 1);
  
  params->ret_mach_port = port;
  [handle release];
  return STATUS_SUCCESS;
}

static NTSTATUS
CREATES(_MTLDevice_newSharedEventWithMachPort)(void *obj) {
  struct unixcall_mtldevice_newsharedeventwithmachport *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  id<MTLDeviceSPI> deviceSPI = (id<MTLDeviceSPI>)device;
  params->ret_event = (obj_handle_t)[deviceSPI newSharedEventWithMachPort:params->mach_port];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_registryID(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLDevice>)params->handle registryID];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLComputePipelineState_threadExecutionWidth(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLComputePipelineState>)params->handle threadExecutionWidth];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLComputePipelineState_maxTotalThreadsPerThreadgroup(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLComputePipelineState>)params->handle maxTotalThreadsPerThreadgroup];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLRenderPipelineState_maxTotalThreadgroupsPerMeshGrid(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLRenderPipelineState>)params->handle maxTotalThreadgroupsPerMeshGrid];
  // Metal feature set tables, "Maximum threadgroups per mesh shader grid", of the families that have less than later ones
  static const uint64_t limits[] = {[7] = 1024, [8] = 1024, [9] = 1048575};
  unsigned family = emulated_family();
  uint64_t limit = family < sizeof(limits) / sizeof(*limits) ? limits[family] : 0;
  // or a limit to test with, which no family has: MULLION_MESH_GRID_LIMIT
  const char *given = (getenv("MULLION_MESH_GRID_LIMIT") ?: getenv("DXMT_MESH_GRID_LIMIT"));
  if (given)
    limit = strtoull(given, NULL, 10);
  if (limit && limit < params->ret)
    params->ret = limit;
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_supportsPlacementSparse(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->handle;
  params->ret = 0;
  if (@available(macOS 26.4, *))
    params->ret = [device supportsPlacementSparse];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_sparseTileSizeInBytes(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLDevice>)params->handle sparseTileSizeInBytesForSparsePageSize:(MTLSparsePageSize)params->ret];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_sparseTileSize(void *obj) {
  struct unixcall_sparse_tile_size *params = obj;
  MTLSize size = [(id<MTLDevice>)params->device sparseTileSizeWithTextureType:(MTLTextureType)params->type
                                                                   pixelFormat:(MTLPixelFormat)params->format
                                                                   sampleCount:params->sample_count
                                                                sparsePageSize:(MTLSparsePageSize)params->page_size];
  params->ret = (struct WMTSize){size.width, size.height, size.depth};
  return STATUS_SUCCESS;
}

static NTSTATUS
CREATES(_MTLDevice_newPlacementSparseBuffer)(void *obj) {
  struct unixcall_new_placement_sparse_buffer *params = obj;
  id<MTLBuffer> buffer = nil;
  if (@available(macOS 26.0, *))
    buffer = [(id<MTLDevice>)params->device newBufferWithLength:params->length
                                                        options:(MTLResourceOptions)params->options
                                        placementSparsePageSize:(MTLSparsePageSize)params->page_size];
  params->ret = (obj_handle_t)buffer;
  params->gpu_address = [buffer gpuAddress];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLTexture_sparseTail(void *obj) {
  struct unixcall_sparse_tail *params = obj;
  id<MTLTexture> texture = (id<MTLTexture>)params->texture;
  params->first_mip = [texture firstMipmapInTail];
  params->bytes = [texture tailSizeInBytes];
  return STATUS_SUCCESS;
}

static NTSTATUS
CREATES(_MTLDevice_newMTL4CommandQueue)(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = 0;
  if (@available(macOS 26.0, *))
    params->ret = (obj_handle_t)[(id<MTLDevice>)params->handle newMTL4CommandQueue];
  return STATUS_SUCCESS;
}

/* the WMTSparse* operations are Metal's MTL4 sparse mapping operations, field for field */
_Static_assert(sizeof(struct WMTSparseBufferMapping) == sizeof(MTL4UpdateSparseBufferMappingOperation), "");
_Static_assert(sizeof(struct WMTSparseTextureMapping) == sizeof(MTL4UpdateSparseTextureMappingOperation), "");
_Static_assert(sizeof(struct WMTSparseBufferMappingCopy) == sizeof(MTL4CopySparseBufferMappingOperation), "");
_Static_assert(sizeof(struct WMTSparseTextureMappingCopy) == sizeof(MTL4CopySparseTextureMappingOperation), "");

static NTSTATUS
_MTL4CommandQueue_updateMappings(void *obj) {
  struct unixcall_mtl4_mappings *params = obj;
  if (@available(macOS 26.0, *)) {
    id<MTL4CommandQueue> queue = (id<MTL4CommandQueue>)params->queue;
    if (params->texture)
      [queue updateTextureMappings:(id<MTLTexture>)params->resource
                              heap:(id<MTLHeap>)params->other
                        operations:params->operations.ptr
                             count:params->count];
    else
      [queue updateBufferMappings:(id<MTLBuffer>)params->resource
                             heap:(id<MTLHeap>)params->other
                       operations:params->operations.ptr
                            count:params->count];
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTL4CommandQueue_copyMappings(void *obj) {
  struct unixcall_mtl4_mappings *params = obj;
  if (@available(macOS 26.0, *)) {
    id<MTL4CommandQueue> queue = (id<MTL4CommandQueue>)params->queue;
    if (params->texture)
      [queue copyTextureMappingsFromTexture:(id<MTLTexture>)params->resource
                                  toTexture:(id<MTLTexture>)params->other
                                 operations:params->operations.ptr
                                      count:params->count];
    else
      [queue copyBufferMappingsFromBuffer:(id<MTLBuffer>)params->resource
                                 toBuffer:(id<MTLBuffer>)params->other
                               operations:params->operations.ptr
                                    count:params->count];
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTL4CommandQueue_event(void *obj) {
  struct unixcall_mtl4_event *params = obj;
  if (@available(macOS 26.0, *)) {
    id<MTL4CommandQueue> queue = (id<MTL4CommandQueue>)params->queue;
    if (params->wait)
      [queue waitForEvent:(id<MTLEvent>)params->event value:params->value];
    else
      [queue signalEvent:(id<MTLEvent>)params->event value:params->value];
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_gpuCoreCount(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  // the device's own IORegistry entry says how many GPU cores it has; Metal does not
  io_service_t entry = IOServiceGetMatchingService(
      kIOMainPortDefault, IORegistryEntryIDMatching([(id<MTLDevice>)params->handle registryID])
  );
  CFTypeRef count = entry ? IORegistryEntryCreateCFProperty(entry, CFSTR("gpu-core-count"), kCFAllocatorDefault, 0) : NULL;
  params->ret = 0;
  if (count && CFGetTypeID(count) == CFNumberGetTypeID())
    CFNumberGetValue(count, kCFNumberSInt64Type, &params->ret);
  if (count)
    CFRelease(count);
  if (entry)
    IOObjectRelease(entry);
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_sampleTimestamps(void *obj) {
  struct unixcall_mtldevice_sampletimestamps *params = obj;
  MTLTimestamp cpu, gpu;
  [(id<MTLDevice>)params->device sampleTimestamps:&cpu gpuTimestamp:&gpu];
  params->cpu_timestamp = cpu;
  params->gpu_timestamp = gpu;
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLSharedEvent_waitUntilSignaledValue(void *obj) {
  struct unixcall_mtlsharedevent_waituntilsignaledvalue *params = obj;
  bool timeout = [(id<MTLSharedEvent>)params->event waitUntilSignaledValue:params->value timeoutMS:params->timeout_ms];
  params->ret_timeout = timeout;
  return STATUS_SUCCESS;
}

static NTSTATUS
CREATES(_MTLCounterSampleBuffer_newTimestampBuffer)(void *obj) {
  struct unixcall_mtlcountersamplebuffer_newtimestampbuffer *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;

  MTLCounterSampleBufferDescriptor *desc = [[MTLCounterSampleBufferDescriptor alloc] init];
  NSArray<id<MTLCounterSet>> *counter_sets = [device counterSets];
  id<MTLCounterSet> timestamp_counter_set = nil;
  for (id<MTLCounterSet> counterSet in counter_sets) {
    if ([counterSet.name isEqualToString:MTLCommonCounterSetTimestamp]) {
      timestamp_counter_set = counterSet;
      break;
    }
  }
  desc.counterSet = timestamp_counter_set;
  desc.sampleCount = params->sample_count;
  desc.storageMode = params->shared ? MTLStorageModeShared : MTLStorageModePrivate;

  NSError *error = nil;
  params->ret = (obj_handle_t)[device newCounterSampleBufferWithDescriptor:desc error:&error];

  [desc release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCounterSampleBuffer_resolveCounterRange(void *obj) {
  struct unixcall_mtlcountersamplebuffer_resolvecounterrange *params = obj;
  id<MTLCounterSampleBuffer> sample_buffer = (id<MTLCounterSampleBuffer>)params->sample_buffer;

  @autoreleasepool {
    NSData *data = [sample_buffer resolveCounterRange:NSMakeRange(params->start, params->len)];
    if (data && params->data_out.ptr) {
      [data getBytes:params->data_out.ptr length:params->data_length];
    }
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_blitCommandEncoderWithSampleBuffers(void *obj) {
  struct unixcall_mtlcommandbuffer_blitcommandencoderwithsamplebuffers *params = obj;
  id<MTLCommandBuffer> cmdbuf = (id<MTLCommandBuffer>)params->cmdbuf;
  struct WMTSampleBufferAttachmentInfo *attachments = params->attachments.ptr;

  MTLBlitPassDescriptor *blit_desc = [[MTLBlitPassDescriptor alloc] init];
  for (uint64_t i = 0; i < params->num_attachments; i++) {
    MTLBlitPassSampleBufferAttachmentDescriptor *desc = blit_desc.sampleBufferAttachments[i];
    desc.sampleBuffer = (id<MTLCounterSampleBuffer>)attachments[i].sample_buffer;
    desc.startOfEncoderSampleIndex = attachments[i].start_of_encoder_sample_index;
    desc.endOfEncoderSampleIndex = attachments[i].end_of_encoder_sample_index;
  }

  params->ret = (obj_handle_t)[cmdbuf blitCommandEncoderWithDescriptor:blit_desc];

  [blit_desc release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_property(void *obj) {
  struct unixcall_generic_obj_uint64_uint64_ret *params = obj;
  id<MTLCommandBuffer> cmdbuf = (id<MTLCommandBuffer>)params->handle;
  double ns = 1000000000;
  switch (params->arg) {
  case WMTCommandBufferPropertyKernelStartTime:
    params->ret = (uint64_t)([cmdbuf kernelStartTime] * ns);
    break;
  case WMTCommandBufferPropertyKernelEndTime:
    params->ret = (uint64_t)([cmdbuf kernelEndTime] * ns);
    break;
  case WMTCommandBufferPropertyGPUStartTime:
    params->ret = (uint64_t)([cmdbuf GPUStartTime] * ns);
    break;
  case WMTCommandBufferPropertyGPUEndTime:
    params->ret = (uint64_t)([cmdbuf GPUEndTime] * ns);
    break;
  default:
    params->ret = 0;
    break;
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
CREATES(_MTLDevice_newTileRenderPipelineState)(void *obj) {
  struct unixcall_mtldevice_newrenderpso *params = obj;
  const struct WMTTileRenderPipelineInfo *info = params->info.ptr;
  MTLTileRenderPipelineDescriptor *descriptor = [[MTLTileRenderPipelineDescriptor alloc] init];

  for (unsigned i = 0; i < 8; i++) {
    descriptor.colorAttachments[i].pixelFormat = to_metal_pixel_format(info->color_formats[i]);
  }

  for (unsigned i = 0; i < 31; i++) {
    if (info->immutable_tile_buffers & (1 << i))
      descriptor.tileBuffers[i].mutability = MTLMutabilityImmutable;
  }

  descriptor.rasterSampleCount = info->raster_sample_count;
  descriptor.threadgroupSizeMatchesTileSize = info->tgsize_matches_tile_size;


  descriptor.tileFunction = (id<MTLFunction>)info->tile_function;

  MTLPipelineOption options = MTLPipelineOptionNone;
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
  if (@available(macOS 15, *)) {
    if (info->num_binary_archives_for_lookup && info->binary_archives_for_lookup.ptr)
      descriptor.binaryArchives = [NSArray arrayWithObjects:(id<MTLBinaryArchive> *)info->binary_archives_for_lookup.ptr
                                                      count:info->num_binary_archives_for_lookup];
    options = info->fail_on_binary_archive_miss ? MTLPipelineOptionFailOnBinaryArchiveMiss : MTLPipelineOptionNone;
  }
#endif
  NSError *err = NULL;
  params->ret_pso = (obj_handle_t)[(id<MTLDevice>)params->device newRenderPipelineStateWithTileDescriptor:descriptor
                                                                                                  options:options
                                                                                               reflection:nil
                                                                                                    error:&err];
  params->ret_error = (obj_handle_t)[err retain];
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
  if (@available(macOS 15, *)) {
    if (!err && info->binary_archive_for_serialization) {
      [(id<MTLBinaryArchive>)info->binary_archive_for_serialization
          addTileRenderPipelineFunctionsWithDescriptor:descriptor
                                                 error:&err];
    }
  }
#endif
  [descriptor release];
  return STATUS_SUCCESS;
}

static NTSTATUS
CREATES(_MTLDevice_newResidencySet)(void *obj) {
  struct unixcall_mtldevice_newresidencyset *params = obj;
  NSError *err = NULL;
  MTLResidencySetDescriptor *descriptor = [[MTLResidencySetDescriptor alloc] init];
  descriptor.initialCapacity = params->init_capacity;
  params->ret_set = (obj_handle_t)[(id<MTLDevice>)params->device newResidencySetWithDescriptor:descriptor error:&err];
  params->ret_error = (obj_handle_t)[err retain];
  [descriptor release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLResidencySet_addAllocations(void *obj) {
  struct unixcall_mtlresidencyset_addallocations *params = obj;
  id<MTLResidencySet> set = (id<MTLResidencySet>)params->set;
  [set addAllocations:(id<MTLAllocation> const *)params->allocations.ptr count:params->count];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLResidencySet_removeAllocations(void *obj) {
  struct unixcall_mtlresidencyset_addallocations *params = obj;
  id<MTLResidencySet> set = (id<MTLResidencySet>)params->set;
  [set removeAllocations:(id<MTLAllocation> const *)params->allocations.ptr count:params->count];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLResidencySet_removeAllAllocations(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  id<MTLResidencySet> set = (id<MTLResidencySet>)params->handle;
  [set removeAllAllocations];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLResidencySet_commit(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  id<MTLResidencySet> set = (id<MTLResidencySet>)params->handle;
  [set commit];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandQueue_addResidencySet(void *obj) {
  struct unixcall_generic_obj_obj_noret *params = obj;
  id<MTLCommandQueue> queue = (id<MTLCommandQueue>)params->handle;
  id<MTLResidencySet> set = (id<MTLResidencySet>)params->arg;
  [queue addResidencySet:set];
  return STATUS_SUCCESS;
}

static NTSTATUS
CREATES(_MTLDevice_newHeap)(void *obj) {
  struct unixcall_mtldevice_newheap *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  struct WMTHeapInfo const *info = params->info.ptr;
  MTLHeapDescriptor *desc = [[MTLHeapDescriptor alloc] init];
  desc.resourceOptions = (MTLResourceOptions)info->options;
  desc.type = (MTLHeapType)info->type;
  if (desc.type == MTLHeapTypeSparse)
    desc.sparsePageSize = (MTLSparsePageSize)info->sparse_page_size;
  else if (@available(macOS 26.0, *))
    desc.maxCompatiblePlacementSparsePageSize = (MTLSparsePageSize)info->sparse_page_size;
  desc.size = info->size;

  params->ret = (obj_handle_t)[device newHeapWithDescriptor:desc];

  [desc release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_heapBufferSizeAndAlign(void *obj) {
  struct unixcall_mtldevice_heapbuffersizeandalign *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  MTLSizeAndAlign ret =
      [device heapBufferSizeAndAlignWithLength:params->length options:(MTLResourceOptions)params->options];
  params->ret_size = ret.size;
  params->ret_align = ret.align;
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_heapTextureSizeAndAlign(void *obj) {
  struct unixcall_mtldevice_heaptexturesizeandalign *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  struct WMTTextureInfo *info = params->info.ptr;
  MTLTextureDescriptor *desc = [[MTLTextureDescriptor alloc] init];
  fill_texture_descriptor(desc, info);
  MTLSizeAndAlign ret = [device heapTextureSizeAndAlignWithDescriptor:desc];
  params->ret_size = ret.size;
  params->ret_align = ret.align;
  [desc release];
  return STATUS_SUCCESS;
}

static NTSTATUS
CREATES(_MTLHeap_newBuffer)(void *obj) {
  struct unixcall_mtlheap_newbuffer *params = obj;
  id<MTLHeap> heap = (id<MTLHeap>)params->heap;
  struct WMTBufferInfo *info = params->info.ptr;
  id<MTLBuffer> buffer;
  if (~params->offset == 0)
    buffer = [heap newBufferWithLength:info->length options:(enum MTLResourceOptions)info->options];
  else
    buffer =
        [heap newBufferWithLength:info->length options:(enum MTLResourceOptions)info->options offset:params->offset];
  info->memory.ptr = [heap storageMode] == MTLStorageModePrivate ? NULL : [buffer contents];

  params->ret = (obj_handle_t)buffer;
  info->gpu_address = [buffer gpuAddress];
  return STATUS_SUCCESS;
}

static NTSTATUS
CREATES(_MTLHeap_newTexture)(void *obj) {
  struct unixcall_mtlheap_newtexture *params = obj;
  id<MTLHeap> heap = (id<MTLHeap>)params->heap;
  struct WMTTextureInfo *info = params->info.ptr;
  MTLTextureDescriptor *desc = [[MTLTextureDescriptor alloc] init];
  fill_texture_descriptor(desc, info);

  id<MTLTexture> ret;
  if (~params->offset == 0)
    ret = [heap newTextureWithDescriptor:desc];
  else
    ret = [heap newTextureWithDescriptor:desc offset:params->offset];
  params->ret = (obj_handle_t)ret;
  info->gpu_resource_id = [ret gpuResourceID]._impl;
  info->mach_port = 0;

  [desc release];
  return STATUS_SUCCESS;
}

static NTSTATUS
CREATES(_MTLDevice_newIndirectCommandBuffer)(void *obj) {
  struct unixcall_mtldevice_newicb *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;

  struct WMTIndirectCommandBufferInfo *info = params->info.ptr;

  MTLIndirectCommandBufferDescriptor *desc = [[MTLIndirectCommandBufferDescriptor alloc] init];

  desc.commandTypes = (MTLIndirectCommandType)info->type;

  desc.maxFragmentBufferBindCount = info->max_fragment_buffer_binding;
  desc.maxKernelBufferBindCount = info->max_kernel_buffer_binding;
  desc.maxKernelThreadgroupMemoryBindCount = info->max_kernel_threadgroup_memory_binding;
  desc.maxMeshBufferBindCount = info->max_mesh_buffer_binding;
  desc.maxObjectBufferBindCount = info->max_object_buffer_binding;
  desc.maxObjectThreadgroupMemoryBindCount = info->max_object_threadgroup_memory_binding;
  desc.maxVertexBufferBindCount = info->max_vertex_buffer_binding;

  desc.inheritBuffers = info->inherit_buffers;
  desc.inheritPipelineState = info->inherit_pso;
  desc.supportDynamicAttributeStride = info->support_dynamic_attribute_stride;
  desc.supportRayTracing = info->support_ray_tracing;

#if 0 // needs macOS 26 SDK, and we don't really use it at the moment
  desc.inheritCullMode = info->inherit_cull_mode;
  desc.inheritDepthBias = info->inherit_depth_bias;
  desc.inheritDepthClipMode = info->inherit_depth_clip_mode;
  desc.inheritDepthStencilState = info->inherit_depth_stencil_state;
  desc.inheritFrontFacingWinding = info->inherit_front_facing;
  desc.inheritTriangleFillMode = info->inherit_fill_mode;
  desc.supportColorAttachmentMapping = info->support_color_attachment_mapping;
#endif

  id<MTLIndirectCommandBuffer> icb =
      [device newIndirectCommandBufferWithDescriptor:desc
                                     maxCommandCount:params->max_count
                                             options:(MTLResourceOptions)params->options];
  params->ret = (obj_handle_t)icb;
  info->gpu_resource_id = [icb gpuResourceID]._impl;
  [desc release];
  return STATUS_SUCCESS;
}

static NTSTATUS
CREATES(_MTLDevice_newLibraryWithSource)(void *obj) {
  struct unixcall_mtldevice_newlibrarywithsource *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  NSString *source =
      [[NSString alloc] initWithBytes:params->source.ptr length:params->length encoding:NSASCIIStringEncoding];
  NSError *err = NULL;
  // without options Metal compiles for the language version of the SDK the host program was linked against, which
  // may predate what the source uses (a Wine loader linked against an old SDK gets MSL 2.4, without mesh shaders):
  // ask for the newest this macOS has
  MTLCompileOptions *options = [[MTLCompileOptions alloc] init];
  if (@available(macOS 26.0, *))
    options.languageVersion = MTLLanguageVersion4_0;
  else
    options.languageVersion = MTLLanguageVersion3_2;
  params->ret_library = (obj_handle_t)[device newLibraryWithSource:source options:options error:&err];
  params->ret_error = (obj_handle_t)[err retain];
  [options release];
  [source release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLTexture_getBytes(void *obj) {
  struct unixcall_mtltexture_replaceregion *params = obj;
  [(id<MTLTexture>)params->texture getBytes:params->data.ptr
                                bytesPerRow:params->bytes_per_row
                              bytesPerImage:params->bytes_per_image
                                 fromRegion:MTLRegionMake3D(
                                                params->origin.x, params->origin.y, params->origin.z,
                                                params->size.width, params->size.height, params->size.depth
                                            )
                                mipmapLevel:params->level
                                      slice:params->slice];
  return STATUS_SUCCESS;
}

/*
 * Definition from cache.c
 */

NTSTATUS _CacheReader_alloc_init(void *obj);
NTSTATUS _CacheReader_get(void *obj);
NTSTATUS _CacheWriter_alloc_init(void *obj);
NTSTATUS _CacheWriter_set(void *obj);
NTSTATUS _WMTSetMetalShaderCachePath(void *obj);

// the Metal descriptor of a WMTAccelerationStructureInfo, autoreleased
static MTLAccelerationStructureDescriptor *
acceleration_structure_descriptor(const struct WMTAccelerationStructureInfo *info) {
  if (info->instances) {
    MTLIndirectInstanceAccelerationStructureDescriptor *desc =
        [MTLIndirectInstanceAccelerationStructureDescriptor descriptor];
    desc.usage = (MTLAccelerationStructureUsage)info->usage;
    desc.instanceDescriptorType = MTLAccelerationStructureInstanceDescriptorTypeIndirect;
    desc.instanceDescriptorBuffer = (id<MTLBuffer>)info->instance_buffer;
    desc.instanceDescriptorBufferOffset = info->instance_offset;
    desc.instanceDescriptorStride = sizeof(MTLIndirectAccelerationStructureInstanceDescriptor);
    desc.maxInstanceCount = info->max_instance_count;
    desc.instanceCountBuffer = (id<MTLBuffer>)info->instance_count_buffer;
    desc.instanceCountBufferOffset = info->instance_count_offset;
    desc.instanceTransformationMatrixLayout = MTLMatrixLayoutRowMajor;
    return desc;
  }
  const struct WMTAccelerationStructureGeometry *geometries = info->geometries.ptr;
  NSMutableArray *array = [NSMutableArray arrayWithCapacity:info->geometry_count];
  for (uint32_t i = 0; i < info->geometry_count; i++) {
    const struct WMTAccelerationStructureGeometry *g = &geometries[i];
    MTLAccelerationStructureGeometryDescriptor *geometry;
    if (g->vertex_format == WMTAttributeFormatInvalid) {
      MTLAccelerationStructureBoundingBoxGeometryDescriptor *boxes =
          [MTLAccelerationStructureBoundingBoxGeometryDescriptor descriptor];
      boxes.boundingBoxBuffer = (id<MTLBuffer>)g->buffer;
      boxes.boundingBoxBufferOffset = g->offset;
      boxes.boundingBoxStride = g->stride;
      boxes.boundingBoxCount = g->count;
      geometry = boxes;
    } else {
      MTLAccelerationStructureTriangleGeometryDescriptor *triangles =
          [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
      triangles.vertexBuffer = (id<MTLBuffer>)g->buffer;
      triangles.vertexBufferOffset = g->offset;
      triangles.vertexStride = g->stride;
      triangles.vertexFormat = (MTLAttributeFormat)g->vertex_format;
      triangles.indexBuffer = (id<MTLBuffer>)g->index_buffer;
      triangles.indexBufferOffset = g->index_offset;
      triangles.indexType = (MTLIndexType)g->index_type;
      triangles.triangleCount = g->count;
      triangles.transformationMatrixBuffer = (id<MTLBuffer>)g->transform_buffer;
      triangles.transformationMatrixBufferOffset = g->transform_offset;
      triangles.transformationMatrixLayout = MTLMatrixLayoutRowMajor;
      geometry = triangles;
    }
    geometry.opaque = g->opaque;
    geometry.allowDuplicateIntersectionFunctionInvocation = !g->no_duplicate_any_hit;
    [array addObject:geometry];
  }
  MTLPrimitiveAccelerationStructureDescriptor *desc = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
  desc.usage = (MTLAccelerationStructureUsage)info->usage;
  desc.geometryDescriptors = array;
  return desc;
}

static NTSTATUS
_MTLDevice_accelerationStructureSizes(void *obj) {
  struct unixcall_mtldevice_accelerationstructuresizes *params = obj;
  struct WMTAccelerationStructureInfo *info = params->info.ptr;
  @autoreleasepool {
    MTLAccelerationStructureSizes sizes = [(id<MTLDevice>)params->device
        accelerationStructureSizesWithDescriptor:acceleration_structure_descriptor(info)];
    info->size = sizes.accelerationStructureSize;
    info->build_scratch_size = sizes.buildScratchBufferSize;
    info->refit_scratch_size = sizes.refitScratchBufferSize;
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
CREATES(_MTLDevice_newAccelerationStructure)(void *obj) {
  struct unixcall_mtldevice_newaccelerationstructure *params = obj;
  id<MTLAccelerationStructure> structure =
      [(id<MTLDevice>)params->device newAccelerationStructureWithSize:params->size];
  params->ret = (obj_handle_t)structure;
  params->ret_gpu_resource_id = structure.gpuResourceID._impl;
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_accelerationStructureCommandEncoder(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(id<MTLCommandBuffer>)params->handle accelerationStructureCommandEncoder];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLAccelerationStructureCommandEncoder_encodeCommands(void *obj) {
  struct unixcall_generic_obj_cmd_noret *params = obj;
  const struct wmtcmd_base *next = params->cmd_head.ptr;
  id<MTLAccelerationStructureCommandEncoder> encoder = (id<MTLAccelerationStructureCommandEncoder>)params->encoder;
  while (next) {
    @autoreleasepool {
      switch ((enum WMTAccelerationStructureCommandType)next->type) {
      default:
        assert(!next->type && "unhandled acceleration structure command type");
        break;
      case WMTAccelerationStructureCommandBuild: {
        struct wmtcmd_accelerationstructure_build *body = (struct wmtcmd_accelerationstructure_build *)next;
        MTLAccelerationStructureDescriptor *desc = acceleration_structure_descriptor(&body->info);
        if (body->src)
          [encoder refitAccelerationStructure:(id<MTLAccelerationStructure>)body->src
                                   descriptor:desc
                                  destination:(id<MTLAccelerationStructure>)body->dst
                                scratchBuffer:(id<MTLBuffer>)body->scratch
                          scratchBufferOffset:body->scratch_offset];
        else
          [encoder buildAccelerationStructure:(id<MTLAccelerationStructure>)body->dst
                                   descriptor:desc
                                scratchBuffer:(id<MTLBuffer>)body->scratch
                          scratchBufferOffset:body->scratch_offset];
        break;
      }
      case WMTAccelerationStructureCommandCopy: {
        struct wmtcmd_accelerationstructure_copy *body = (struct wmtcmd_accelerationstructure_copy *)next;
        [encoder copyAccelerationStructure:(id<MTLAccelerationStructure>)body->src
                   toAccelerationStructure:(id<MTLAccelerationStructure>)body->dst];
        break;
      }
      case WMTAccelerationStructureCommandCopyAndCompact: {
        struct wmtcmd_accelerationstructure_copy *body = (struct wmtcmd_accelerationstructure_copy *)next;
        [encoder copyAndCompactAccelerationStructure:(id<MTLAccelerationStructure>)body->src
                             toAccelerationStructure:(id<MTLAccelerationStructure>)body->dst];
        break;
      }
      case WMTAccelerationStructureCommandWriteCompactedSize: {
        struct wmtcmd_accelerationstructure_write_compacted_size *body =
            (struct wmtcmd_accelerationstructure_write_compacted_size *)next;
        [encoder writeCompactedAccelerationStructureSize:(id<MTLAccelerationStructure>)body->structure
                                                toBuffer:(id<MTLBuffer>)body->buffer
                                                  offset:body->offset
                                            sizeDataType:MTLDataTypeULong];
        break;
      }
      case WMTAccelerationStructureCommandWaitForFence: {
        struct wmtcmd_accelerationstructure_fence_op *body = (struct wmtcmd_accelerationstructure_fence_op *)next;
        [encoder waitForFence:(id<MTLFence>)body->fence];
        break;
      }
      case WMTAccelerationStructureCommandUpdateFence: {
        struct wmtcmd_accelerationstructure_fence_op *body = (struct wmtcmd_accelerationstructure_fence_op *)next;
        [encoder updateFence:(id<MTLFence>)body->fence];
        break;
      }
      }
    }
    next = next->next.ptr;
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newLinkedComputePipelineState(void *obj) {
  struct unixcall_mtldevice_newlinkedcomputepso *params = obj;
  const struct WMTLinkedComputePipelineInfo *info = params->info.ptr;
  @autoreleasepool {
    MTLComputePipelineDescriptor *descriptor = [[MTLComputePipelineDescriptor alloc] init];
    NSError *err = NULL;
    MTLLinkedFunctions *linked = [MTLLinkedFunctions linkedFunctions];
    linked.functions = [NSArray arrayWithObjects:(id<MTLFunction> *)info->functions.ptr count:info->function_count];
    descriptor.computeFunction = (id<MTLFunction>)info->compute_function;
    descriptor.linkedFunctions = linked;
    descriptor.maxCallStackDepth = info->max_call_stack_depth;
    params->ret_pso = (obj_handle_t)[(id<MTLDevice>)params->device newComputePipelineStateWithDescriptor:descriptor
                                                                                               options:MTLPipelineOptionNone
                                                                                            reflection:nil
                                                                                                 error:&err];
    params->ret_error = (obj_handle_t)[err retain];
    [descriptor release];
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLComputePipelineState_newVisibleFunctionTable(void *obj) {
  struct unixcall_mtlcomputepso_newvisiblefunctiontable *params = obj;
  id<MTLComputePipelineState> pso = (id<MTLComputePipelineState>)params->pso;
  const obj_handle_t *functions = params->functions.ptr;
  @autoreleasepool {
    MTLVisibleFunctionTableDescriptor *descriptor = [MTLVisibleFunctionTableDescriptor visibleFunctionTableDescriptor];
    descriptor.functionCount = params->function_count;
    id<MTLVisibleFunctionTable> table = [pso newVisibleFunctionTableWithDescriptor:descriptor];
    for (uint64_t i = 0; i < params->function_count; i++)
      if (functions[i])
        [table setFunction:[pso functionHandleWithFunction:(id<MTLFunction>)functions[i]] atIndex:i];
    params->ret = (obj_handle_t)table;
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_sampledCommandEncoder(void *obj) {
  struct unixcall_mtlcommandbuffer_sampledcommandencoder *params = obj;
  id<MTLCommandBuffer> cmdbuf = (id<MTLCommandBuffer>)params->cmdbuf;
  const struct WMTSampleBufferAttachmentInfo *samples = params->samples.ptr;
  if (params->type == WMTSampledRender) {
    struct unixcall_generic_obj_uint64_obj_ret render = {params->cmdbuf, (uint64_t)params->render_info.ptr, 0};
    NTSTATUS status = renderCommandEncoder(&render, samples);
    params->ret = render.ret;
    return status;
  }
  if (params->type == WMTSampledAccelerationStructure) {
    MTLAccelerationStructurePassDescriptor *desc = [[MTLAccelerationStructurePassDescriptor alloc] init];
    desc.sampleBufferAttachments[0].sampleBuffer = (id<MTLCounterSampleBuffer>)samples->sample_buffer;
    desc.sampleBufferAttachments[0].startOfEncoderSampleIndex = samples->start_of_encoder_sample_index;
    desc.sampleBufferAttachments[0].endOfEncoderSampleIndex = samples->end_of_encoder_sample_index;
    params->ret = (obj_handle_t)[cmdbuf accelerationStructureCommandEncoderWithDescriptor:desc];
    [desc release];
  } else {
    MTLComputePassDescriptor *desc = [[MTLComputePassDescriptor alloc] init];
    desc.dispatchType = params->type == WMTSampledConcurrentCompute ? MTLDispatchTypeConcurrent : MTLDispatchTypeSerial;
    desc.sampleBufferAttachments[0].sampleBuffer = (id<MTLCounterSampleBuffer>)samples->sample_buffer;
    desc.sampleBufferAttachments[0].startOfEncoderSampleIndex = samples->start_of_encoder_sample_index;
    desc.sampleBufferAttachments[0].endOfEncoderSampleIndex = samples->end_of_encoder_sample_index;
    params->ret = (obj_handle_t)[cmdbuf computeCommandEncoderWithDescriptor:desc];
    [desc release];
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTDiagWrite(void *obj) {
  struct unixcall_wmtdiag_write *params = obj;
  static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
  static FILE *file;
  static bool opened;
  params->ret = getpid();
  if (!params->lines.ptr)
    return STATUS_SUCCESS;
  pthread_mutex_lock(&lock);
  if (!opened) {
    opened = true;
    NSString *path =
        [NSString stringWithFormat:@"%s/mullion-%u.diag", (const char *)params->directory.ptr, params->ret];
    file = fopen(path.fileSystemRepresentation, "w");
    if (!file)
      perror("mullion-diag: open record");
  }
  if (!file || fwrite(params->lines.ptr, 1, params->length, file) != params->length || fflush(file)) {
    params->ret = 0;
    perror("mullion-diag: write record");
  }
  pthread_mutex_unlock(&lock);
  return STATUS_SUCCESS;
}

// MULLION_TRACE's log: one file for the process, whose DLLs each write their categories' lines through here, each line
// after its time in ms since the log was opened
static pthread_mutex_t trace_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *trace_file;
static uint64_t trace_started;

static double
trace_ms(void) {
  static mach_timebase_info_data_t timebase;
  if (!timebase.denom)
    mach_timebase_info(&timebase);
  return (double)(mach_absolute_time() - trace_started) * timebase.numer / timebase.denom / 1e6;
}

// the lines go to the file as they come and reach the disk within a second (trace_flusher); a GPU line, a warning or an
// error reaches it at once, so a process that ends or hangs right after has them
static void
trace_lines(const char *lines, uint64_t length) {
  double ms = trace_ms();
  bool now = false;
  for (const char *line = lines, *end = lines + length; line < end;) {
    const char *newline = memchr(line, '\n', end - line);
    const char *next = newline ? newline + 1 : end;
    int size = (int)((newline ? newline : end) - line);
    fprintf(trace_file, "%10.3f %.*s\n", ms, size, line);
    now |= !strncmp(line, "[gpu]", 5) || !strncmp(line, "err:", 4) || !strncmp(line, "warn:", 5);
    line = next;
  }
  if (now)
    fflush(trace_file);
}

static void *
trace_flusher(void *unused) {
  (void)unused;
  pthread_setname_np("mullion-trace-flush");
  for (;;) {
    sleep(1);
    pthread_mutex_lock(&trace_lock);
    fflush(trace_file);
    pthread_mutex_unlock(&trace_lock);
  }
  return NULL;
}

// `memory`: every 5 s, the process's memory footprint and the system's swap and paging, as a game that runs out of
// memory or starts paging shows it
static void *
trace_memory_sampler(void *unused) {
  (void)unused;
  pthread_setname_np("mullion-trace-memory");
  uint64_t pageouts = 0, swapouts = 0;
  for (;;) {
    task_vm_info_data_t task;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    vm_statistics64_data_t vm;
    mach_msg_type_number_t vm_count = HOST_VM_INFO64_COUNT;
    struct xsw_usage swap = {0};
    size_t swap_size = sizeof(swap);
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&task, &count) == KERN_SUCCESS &&
        host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vm, &vm_count) == KERN_SUCCESS) {
      sysctlbyname("vm.swapusage", &swap, &swap_size, NULL, 0);
      uint64_t page = vm_kernel_page_size;
      char line[256];
      int length = snprintf(line, sizeof(line),
                            "[memory] sampler: process footprint %llu MB (peak %llu MB); system: swap used %llu MB, "
                            "compressed %llu MB, free %llu MB, %llu pageouts and %llu swapouts in the last 5 s",
                            task.phys_footprint >> 20, task.ledger_phys_footprint_peak >> 20, swap.xsu_used >> 20,
                            (vm.compressor_page_count * page) >> 20, (vm.free_count * page) >> 20,
                            pageouts ? vm.pageouts - pageouts : 0, swapouts ? vm.swapouts - swapouts : 0);
      pageouts = vm.pageouts;
      swapouts = vm.swapouts;
      pthread_mutex_lock(&trace_lock);
      trace_lines(line, length);
      fflush(trace_file);
      pthread_mutex_unlock(&trace_lock);
    }
    sleep(5);
  }
  return NULL;
}

static NTSTATUS
_WMTTraceWrite(void *obj) {
  @autoreleasepool {
    struct unixcall_wmttrace_write *params = obj;
    static bool opened;
    pthread_mutex_lock(&trace_lock);
    if (!opened) {
      opened = true;
      trace_started = mach_absolute_time();
      NSString *directory = [NSString stringWithUTF8String:params->directory.ptr];
      NSFileManager *files = [NSFileManager defaultManager];
      [files createDirectoryAtPath:directory withIntermediateDirectories:YES attributes:nil error:nil];
      NSString *path = [directory stringByAppendingPathComponent:@"mullion.log"];
      NSString *previous = [directory stringByAppendingPathComponent:@"mullion.prev.log"];
      [files removeItemAtPath:previous error:nil];
      [files moveItemAtPath:path toPath:previous error:nil];
      trace_file = directory.length ? fopen(path.fileSystemRepresentation, "w") : NULL;
      if (!trace_file)
        fprintf(stderr, "mullion: trace log in '%s': %s\n", directory.UTF8String, strerror(errno));
      else {
        NSString *date = [NSDateFormatter localizedStringFromDate:[NSDate date]
                                                        dateStyle:NSDateFormatterShortStyle
                                                        timeStyle:NSDateFormatterMediumStyle];
        fprintf(trace_file, "Mullion trace %s, process %d\n", date.UTF8String, getpid());
        fflush(trace_file);
        pthread_t flusher;
        if (!pthread_create(&flusher, NULL, trace_flusher, NULL))
          pthread_detach(flusher);
        if (params->memory_samples) {
          pthread_t sampler;
          if (!pthread_create(&sampler, NULL, trace_memory_sampler, NULL))
            pthread_detach(sampler);
        }
      }
    }
    if (params->encoder_errors)
      __atomic_store_n(&encoder_errors, true, __ATOMIC_RELAXED);
    if (trace_file && params->lines.ptr)
      trace_lines(params->lines.ptr, params->length);
    params->ret = trace_file != NULL;
    pthread_mutex_unlock(&trace_lock);
  }
  return STATUS_SUCCESS;
}

static int
flight_order(const void *a, const void *b) {
  uint64_t x = ((const struct WMTFlightEntry *)a)->time, y = ((const struct WMTFlightEntry *)b)->time;
  return (x > y) - (x < y);
}

static NTSTATUS
_WMTFlightWrite(void *obj) {
  @autoreleasepool {
    struct unixcall_wmtflight_write *params = obj;
    static bool written;
    struct WMTFlightEntry *entries = (struct WMTFlightEntry *)params->records.ptr;
    const char *given = params->directory.ptr;
    NSString *directory = *given ? [NSString stringWithUTF8String:given]
                                 : [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Logs/Mullion"];
    [[NSFileManager defaultManager] createDirectoryAtPath:directory withIntermediateDirectories:YES attributes:nil
                                                    error:nil];
    NSString *path = [directory
        stringByAppendingPathComponent:[NSString stringWithFormat:@"%s.flight", (const char *)params->executable.ptr]];
    FILE *file = fopen(path.fileSystemRepresentation, written ? "a" : "w");
    if (!file) {
      perror("mullion: flight record");
      return STATUS_SUCCESS;
    }
    written = true;
    // stable: records a counter tick apart keep their thread's order, and the order the rings were read in otherwise
    if (mergesort(entries, params->count, sizeof(*entries), flight_order))
      perror("mullion: flight record order");
    // a dump at the recorder's start has no time to scale its counter by: its records show in ticks
    double frequency = params->frequency ? params->frequency : 1000;
    fprintf(file, "== %s, process %d, %llu records (%llu overwritten, %llu lost), times in ms before the dump\n",
            (const char *)params->reason.ptr, getpid(), params->count, params->overwritten, params->lost);
    for (uint64_t i = 0; i < params->count; i++) {
      struct WMTFlightEntry *entry = &entries[i];
      fprintf(file, "%12.3f %6u ", ((double)params->now - (double)entry->time) * 1000 / frequency, entry->thread);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
      fprintf(file, (const char *)entry->site.ptr, entry->args[0], entry->args[1], entry->args[2], entry->args[3]);
#pragma clang diagnostic pop
      fputc('\n', file);
    }
    fclose(file);
    fprintf(stderr, "mullion: flight record of %s in %s\n", (const char *)params->reason.ptr,
            path.fileSystemRepresentation);
    return STATUS_SUCCESS;
  }
}

static NTSTATUS
_WMTGPUStatistics(void *obj) {
  struct unixcall_wmtgpu_statistics *params = obj;
  io_service_t entry = IOServiceGetMatchingService(
      kIOMainPortDefault, IORegistryEntryIDMatching([(id<MTLDevice>)params->device registryID])
  );
  NSDictionary *statistics =
      entry ? (NSDictionary *)IORegistryEntryCreateCFProperty(entry, CFSTR("PerformanceStatistics"), kCFAllocatorDefault, 0)
            : nil;
  NSMutableString *text = [[NSMutableString alloc] init];
  for (NSString *name in statistics)
    if ([statistics[name] isKindOfClass:[NSNumber class]])
      [text appendFormat:@"\t%@=%lld", name, [statistics[name] longLongValue]];
  params->ret = strlen(text.UTF8String);
  if (params->buffer.ptr && params->size >= params->ret)
    memcpy(params->buffer.ptr, text.UTF8String, params->ret);
  [text release];
  [statistics release];
  if (entry)
    IOObjectRelease(entry);
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_pushDebugGroup(void *args) {
  struct unixcall_generic_obj_obj_noret *params = args;
  [(id<MTLCommandBuffer>)params->handle pushDebugGroup:(NSString *)params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_popDebugGroup(void *args) {
  struct unixcall_generic_obj_noret *params = args;
  [(id<MTLCommandBuffer>)params->handle popDebugGroup];
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTPresentStatistics_new(void *obj) {
  struct unixcall_generic_obj_ret *params = obj;
  struct WMTPresentStatistics statistics = {.since = CACurrentMediaTime()};
  params->ret = (obj_handle_t)[[NSMutableData alloc] initWithBytes:&statistics length:sizeof(statistics)];
  return STATUS_SUCCESS;
}

static void
count_present(NSMutableData *data, uint64_t number, double time) {
  @synchronized(data) {
    struct WMTPresentStatistics *statistics = data.mutableBytes;
    if (number > statistics->shown) {
      statistics->shown = number;
      statistics->shown_time = time;
    }
  }
}

static NTSTATUS
_MetalDrawable_countPresent(void *obj) {
  struct unixcall_generic_obj_obj_uint64_noret *params = obj;
  NSMutableData *data = (NSMutableData *)params->arg0;
  uint64_t number = params->arg1;
  if (!params->handle) {
    count_present(data, number, CACurrentMediaTime());
    return STATUS_SUCCESS;
  }
  // the copied handler keeps `data` until it runs; a drawable that was never shown has a presentedTime of 0
  [(id<MTLDrawable>)params->handle addPresentedHandler:^(id<MTLDrawable> drawable) {
    if (drawable.presentedTime)
      count_present(data, number, drawable.presentedTime);
  }];
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTPresentStatistics_get(void *obj) {
  struct unixcall_generic_obj_ptr_noret *params = obj;
  NSMutableData *data = (NSMutableData *)params->handle;
  struct WMTPresentStatistics *out = params->arg.ptr;
  @synchronized(data) {
    *out = *(const struct WMTPresentStatistics *)data.bytes;
  }
  out->now = CACurrentMediaTime();
  return STATUS_SUCCESS;
}

static NTSTATUS
_DispatchData_getBytes(void *obj) {
  struct unixcall_dispatchdata_bytes *params = obj;
  dispatch_data_t data = (dispatch_data_t)params->data;
  params->ret = dispatch_data_get_size(data);
  if (params->buffer.ptr && params->size >= params->ret) {
    const void *bytes;
    size_t length;
    dispatch_data_t flat = dispatch_data_create_map(data, &bytes, &length);
    memcpy(params->buffer.ptr, bytes, length);
    dispatch_release(flat);
  }
  return STATUS_SUCCESS;
}

enum {
  Metal4VertexBinding,
  Metal4FragmentBinding,
  Metal4ObjectBinding,
  Metal4MeshBinding,
  Metal4ComputeBinding,
  Metal4BindingStageCount
};

API_AVAILABLE(macos(26.0)) @interface MullionMetal4Buffer : NSObject {
@public
  id<MTLDevice> device;
  id<MTL4CommandQueue> queue;
  id<MTL4CommandBuffer> buffer;
  id<MTL4CommandAllocator> allocator;
  id<MTLSharedEvent> event;
  id<MTLFence> fence;
  id<MTLResidencySet> residency;
  NSMutableSet *allocations;
  NSMutableSet *objects;
  id<MTLBuffer> bytes;
  uint64_t bytes_used;
  id<MTLIndirectCommandBuffer> parts;
  NSUInteger parts_used;
  id compute;
  uint64_t value;
  struct WMTMetal4BindingCounts bindings;
  struct WMTMetal4Feedback feedback;
  bool finished;
  dispatch_group_t feedback_ready;
  id<MTLDrawable> drawable;
  id<MTLEvent> wait_event;
  uint64_t wait_value;
  // the next encoder begins with a barrier against all the queue's work before it that makes aliased memory
  // consistent (MTL4CommandBuffer_orderAliases)
  bool order_aliases;
  double after;
  // the sample the open compute encoder's last pass ended with
  obj_handle_t stamp_heap;
  uint64_t stamp_index;
  // the buffer's encoders share its tables, whose bindings Metal takes at each draw and dispatch (MTL4ArgumentTable);
  // a render encoder clears the slots below those counts as it ends, so the next starts as a new table does
  id<MTL4ArgumentTable> tables[Metal4BindingStageCount];
  unsigned buffers_set[Metal4BindingStageCount], textures_set[Metal4BindingStageCount];
}
- (void)closeCompute;
- (void)fail:(NSError *)error;
@end

API_AVAILABLE(macos(26.0)) @interface MullionMetal4Encoder : NSObject {
@public
  MullionMetal4Buffer *owner;
  id<MTL4CommandEncoder> encoder;
  bool tables_set[Metal4BindingStageCount];
  MTLGPUAddress *bases;
  MTLSize threadgroup;
  struct WMTSampleBufferAttachmentInfo samples;
  bool render;
  // a compute encoder that updates a fence of its own overlaps other passes: its end leaves the buffer's fence
  bool overlaps;
}
@end

@implementation MullionMetal4Encoder
- (void)setLabel:(NSString *)label {
  [encoder setLabel:label];
}
- (void)dealloc {
  free(bases);
  [encoder release];
  [super dealloc];
}
@end

// the queue's allocators that no work holds: a new one costs its command buffer tens of microseconds to make, and its
// commit as long again before the GPU starts it, where a reset one costs neither
static NSMutableArray *
API_AVAILABLE(macos(26.0)) metal4_allocators(id<MTL4CommandQueue> queue) {
  static char key;
  @synchronized(queue) {
    NSMutableArray *pool = objc_getAssociatedObject(queue, &key);
    if (!pool) {
      pool = [NSMutableArray array];
      objc_setAssociatedObject(queue, &key, pool, OBJC_ASSOCIATION_RETAIN);
    }
    return pool;
  }
}

@implementation MullionMetal4Buffer
// a debug group is the command buffer's, so none of its encoders is open
- (void)pushDebugGroup:(NSString *)name {
  [self closeCompute];
  [buffer pushDebugGroup:name];
}
- (void)popDebugGroup {
  [self closeCompute];
  [buffer popDebugGroup];
}
- (void)fail:(NSError *)error {
  if (!error)
    error = [NSError errorWithDomain:MTL4CommandQueueErrorDomain code:MTL4CommandQueueErrorOutOfMemory userInfo:nil];
  @synchronized(self) {
    if (!feedback.error)
      feedback.error = (obj_handle_t)[error retain];
  }
}
- (void)closeCompute {
  MullionMetal4Encoder *state = compute;
  if (!state)
    return;
  if (!state->overlaps)
    [state->encoder updateFence:fence
             afterEncoderStages:MTLStageDispatch | MTLStageBlit | MTLStageAccelerationStructure];
  [state->encoder endEncoding];
  compute = nil;
  stamp_heap = 0;
}
- (void)dealloc {
  dispatch_release(feedback_ready);
  for (unsigned i = 0; i < Metal4BindingStageCount; i++)
    [tables[i] release];
  [objects release];
  [allocations release];
  [residency release];
  [buffer release];
  // the GPU is done with the commands once the buffer's feedback came, and never had those of one not committed
  if (allocator && (finished || feedback.status < WMTCommandBufferStatusCommitted)) {
    [allocator reset];
    NSMutableArray *pool = metal4_allocators(queue);
    @synchronized(pool) {
      [pool addObject:allocator];
    }
  }
  [allocator release];
  [event release];
  [fence release];
  [queue release];
  [drawable release];
  [wait_event release];
  [(NSError *)feedback.error release];
  [super dealloc];
}
@end

static void
API_AVAILABLE(macos(26.0)) metal4_resident(MullionMetal4Buffer *state, id<MTLAllocation> resource) {
  if (resource && ![state->allocations containsObject:resource]) {
    [state->allocations addObject:resource];
    [state->residency addAllocation:resource];
  }
}

static MTLGPUAddress
API_AVAILABLE(macos(26.0)) metal4_address(MullionMetal4Buffer *state, obj_handle_t buffer) {
  metal4_resident(state, (id<MTLBuffer>)buffer);
  return [(id<MTLBuffer>)buffer gpuAddress];
}

static MTL4BufferRange
API_AVAILABLE(macos(26.0)) metal4_range(MullionMetal4Buffer *state, obj_handle_t buffer, uint64_t offset) {
  return MTL4BufferRangeMake(
      buffer ? metal4_address(state, buffer) + offset : 0, buffer ? [(id<MTLBuffer>)buffer length] - offset : 0
  );
}

static id<MTL4ArgumentTable>
API_AVAILABLE(macos(26.0)) metal4_table(MullionMetal4Encoder *state, unsigned stage) {
  MullionMetal4Buffer *owner = state->owner;
  if (!owner->tables[stage]) {
    MTL4ArgumentTableDescriptor *desc = [[MTL4ArgumentTableDescriptor alloc] init];
    desc.maxBufferBindCount = owner->bindings.buffers;
    desc.maxTextureBindCount = owner->bindings.textures;
    desc.maxSamplerStateBindCount = owner->bindings.samplers;
    desc.initializeBindings = YES;
    NSError *error = nil;
    owner->tables[stage] = [owner->device newArgumentTableWithDescriptor:desc error:&error];
    [desc release];
    if (!owner->tables[stage]) {
      [owner fail:error];
      return nil;
    }
  }
  if (!state->tables_set[stage]) {
    state->tables_set[stage] = true;
    if (state->render) {
      const MTLRenderStages stages[] = {
          MTLRenderStageVertex, MTLRenderStageFragment, MTLRenderStageObject, MTLRenderStageMesh
      };
      [(id<MTL4RenderCommandEncoder>)state->encoder setArgumentTable:owner->tables[stage] atStages:stages[stage]];
    } else {
      [(id<MTL4ComputeCommandEncoder>)state->encoder setArgumentTable:owner->tables[stage]];
    }
  }
  return owner->tables[stage];
}

static void
API_AVAILABLE(macos(26.0)) metal4_buffer(
    MullionMetal4Encoder *state, unsigned stage, obj_handle_t buffer, uint64_t offset, unsigned index
) {
  MTLGPUAddress base = metal4_address(state->owner, buffer);
  state->bases[stage * state->owner->bindings.buffers + index] = base;
  [metal4_table(state, stage) setAddress:base ? base + offset : 0 atIndex:index];
  state->owner->buffers_set[stage] = MAX(state->owner->buffers_set[stage], index + 1);
}

static void
API_AVAILABLE(macos(26.0)) metal4_offset(MullionMetal4Encoder *state, unsigned stage, uint64_t offset, unsigned index) {
  MTLGPUAddress base = state->bases[stage * state->owner->bindings.buffers + index];
  [metal4_table(state, stage) setAddress:base ? base + offset : 0 atIndex:index];
}

static void
API_AVAILABLE(macos(26.0)) metal4_bytes(
    MullionMetal4Encoder *state, unsigned stage, const void *bytes, uint64_t length, unsigned index
) {
  // a command buffer's bytes share buffers it keeps until it completes, each twice the last, at offsets that suit
  // any type: a device argument's offset is aligned "to the data type's size", which is at most 16 bytes
  // (MTLComputeCommandEncoder setBuffer:offset:atIndex:)
  MullionMetal4Buffer *owner = state->owner;
  uint64_t at = (owner->bytes_used + 15) & ~15ull;
  if (!owner->bytes || at + length > [owner->bytes length]) {
    id<MTLBuffer> buffer = [owner->device newBufferWithLength:MAX(MAX(length, 2 * [owner->bytes length]), vm_page_size)
                                                      options:MTLResourceStorageModeShared];
    if (!buffer) {
      [owner fail:nil];
      return;
    }
    [owner->objects addObject:buffer];
    [buffer release];
    owner->bytes = buffer;
    at = 0;
  }
  memcpy((char *)[owner->bytes contents] + at, bytes, length);
  owner->bytes_used = at + length;
  metal4_buffer(state, stage, (obj_handle_t)owner->bytes, at, index);
}

static void
API_AVAILABLE(macos(26.0)) metal4_texture(
    MullionMetal4Encoder *state, unsigned stage, obj_handle_t texture, unsigned index
) {
  metal4_resident(state->owner, (id<MTLTexture>)texture);
  [metal4_table(state, stage) setTexture:[(id<MTLTexture>)texture gpuResourceID] atIndex:index];
  state->owner->textures_set[stage] = MAX(state->owner->textures_set[stage], index + 1);
}

static NTSTATUS
_MTL4CommandBuffer_new(void *obj) {
  struct unixcall_metal4_new *p = obj;
  if (@available(macOS 26.0, *)) {
    MullionMetal4Buffer *state = [[MullionMetal4Buffer alloc] init];
    if (!state)
      return STATUS_SUCCESS;
    state->queue = [(id<MTL4CommandQueue>)p->queue retain];
    state->device = [state->queue device];
    state->buffer = [state->device newCommandBuffer];
    NSMutableArray *pool = metal4_allocators(state->queue);
    @synchronized(pool) {
      state->allocator = [[pool lastObject] retain];
      if (state->allocator)
        [pool removeLastObject];
    }
    if (!state->allocator)
      state->allocator = [state->device newCommandAllocator];
    state->fence = [(id<MTLFence>)p->fence retain];
    state->event = [(id<MTLSharedEvent>)p->event retain];
    state->value = p->value;
    state->bindings = p->bindings;
    state->allocations = [[NSMutableSet alloc] init];
    state->objects = [[NSMutableSet alloc] init];
    state->feedback_ready = dispatch_group_create();
    state->residency =
        [state->device newResidencySetWithDescriptor:[[[MTLResidencySetDescriptor alloc] init] autorelease] error:nil];
    if (!state->buffer || !state->allocator || !state->residency || !state->objects || !state->allocations) {
      [state fail:nil];
      p->ret = (obj_handle_t)state;
      return STATUS_SUCCESS;
    }
    [state->buffer beginCommandBufferWithAllocator:state->allocator];
    [state->buffer useResidencySet:(id<MTLResidencySet>)p->residency];
    [state->buffer useResidencySet:state->residency];
    p->ret = (obj_handle_t)state;
  }
  return STATUS_SUCCESS;
}

static void
API_AVAILABLE(macos(26.0)) metal4_begin_samples(
    MullionMetal4Encoder *state, const struct WMTSampleBufferAttachmentInfo *sample
) {
  state->samples = (struct WMTSampleBufferAttachmentInfo){0};
  if (sample)
    state->samples = *(const struct WMTSampleBufferAttachmentInfo *)sample;
  if (state->samples.sample_buffer && state->samples.start_of_encoder_sample_index != MTLCounterDontSample) {
    uint64_t first = state->samples.start_of_encoder_sample_index;
    if (state->render) {
      bool vertex = state->samples.end_of_encoder_sample_index - first + 1 == WMTRenderTimestampSamples;
      if (vertex)
        [(id<MTL4RenderCommandEncoder>)state->encoder
            writeTimestampWithGranularity:MTL4TimestampGranularityPrecise
                               afterStage:MTLRenderStageVertex
                                 intoHeap:(id<MTL4CounterHeap>)state->samples.sample_buffer
                                  atIndex:first];
      [(id<MTL4RenderCommandEncoder>)state->encoder
          writeTimestampWithGranularity:MTL4TimestampGranularityPrecise
                             afterStage:MTLRenderStageFragment
                               intoHeap:(id<MTL4CounterHeap>)state->samples.sample_buffer
                                atIndex:vertex ? first + 2 : first];
    } else {
      [(id<MTL4ComputeCommandEncoder>)state->encoder
          writeTimestampWithGranularity:MTL4TimestampGranularityPrecise
                               intoHeap:(id<MTL4CounterHeap>)state->samples.sample_buffer
                                atIndex:first];
    }
  }
}

static NTSTATUS
_MTL4CommandBuffer_encoder(void *obj) {
  struct unixcall_metal4_encoder *p = obj;
  if (@available(macOS 26.0, *)) {
    MullionMetal4Buffer *owner = (MullionMetal4Buffer *)p->buffer;
    // a barrier against the queue's earlier work takes an encoder of its own: the open compute encoder ends first
    if (owner->order_aliases)
      [owner closeCompute];
    if (p->kind != WMTMetal4Render && owner->compute) {
      MullionMetal4Encoder *state = owner->compute;
      [(id<MTL4ComputeCommandEncoder>)state->encoder
          barrierAfterEncoderStages:[(id<MTL4ComputeCommandEncoder>)state->encoder stages]
                beforeEncoderStages:MTLStageDispatch | MTLStageBlit | MTLStageAccelerationStructure
                  visibilityOptions:MTL4VisibilityOptionDevice];
      // the barrier starts this pass where that sample ended the last, and Metal drops a timestamp that follows
      // another with no work between
      struct WMTSampleBufferAttachmentInfo *sample = p->samples.ptr, start = {0};
      if (sample)
        start = *sample;
      if (sample && sample->sample_buffer && sample->sample_buffer == owner->stamp_heap &&
          sample->start_of_encoder_sample_index != MTLCounterDontSample) {
        start.start_of_encoder_sample_index = MTLCounterDontSample;
        sample->start_of_encoder_sample_index = owner->stamp_index;
      }
      owner->stamp_heap = 0;
      metal4_begin_samples(state, sample ? &start : NULL);
      p->ret = (obj_handle_t)state;
      return STATUS_SUCCESS;
    }
    MullionMetal4Encoder *state = [[[MullionMetal4Encoder alloc] init] autorelease];
    if (!state) {
      [owner fail:nil];
      return STATUS_SUCCESS;
    }
    state->owner = owner;
    state->render = p->kind == WMTMetal4Render;
    [owner->objects addObject:state];
    p->ret = (obj_handle_t)state;
    if (owner->feedback.error)
      return STATUS_SUCCESS;
    state->bases =
        calloc(owner->bindings.buffers * Metal4BindingStageCount, sizeof(MTLGPUAddress));
    if (!state->bases) {
      [owner fail:nil];
      return STATUS_SUCCESS;
    }
    if (state->render) {
      [owner closeCompute];
      const struct WMTRenderPassInfo *info = p->render.ptr;
      MTL4RenderPassDescriptor *descriptor = [[MTL4RenderPassDescriptor alloc] init];
      for (unsigned i = 0; i < sizeof(info->colors) / sizeof(info->colors[0]); i++) {
        descriptor.colorAttachments[i].clearColor = MTLClearColorMake(
            info->colors[i].clear_color.r, info->colors[i].clear_color.g, info->colors[i].clear_color.b,
            info->colors[i].clear_color.a
        );
        descriptor.colorAttachments[i].level = info->colors[i].level;
        descriptor.colorAttachments[i].slice = info->colors[i].slice;
        descriptor.colorAttachments[i].depthPlane = info->colors[i].depth_plane;
        descriptor.colorAttachments[i].texture = (id<MTLTexture>)info->colors[i].texture;
        descriptor.colorAttachments[i].loadAction = (MTLLoadAction)info->colors[i].load_action;
        descriptor.colorAttachments[i].storeAction = (MTLStoreAction)info->colors[i].store_action;
        descriptor.colorAttachments[i].resolveTexture = (id<MTLTexture>)info->colors[i].resolve_texture;
        descriptor.colorAttachments[i].resolveLevel = info->colors[i].resolve_level;
        descriptor.colorAttachments[i].resolveSlice = info->colors[i].resolve_slice;
        descriptor.colorAttachments[i].resolveDepthPlane = info->colors[i].resolve_depth_plane;
      }

      if (info->depth.texture) {
        descriptor.depthAttachment.clearDepth = info->depth.clear_depth;
        descriptor.depthAttachment.depthPlane = info->depth.depth_plane;
        descriptor.depthAttachment.level = info->depth.level;
        descriptor.depthAttachment.slice = info->depth.slice;
        descriptor.depthAttachment.texture = (id<MTLTexture>)info->depth.texture;
        descriptor.depthAttachment.loadAction = (MTLLoadAction)info->depth.load_action;
        descriptor.depthAttachment.storeAction = (MTLStoreAction)info->depth.store_action;
      }

      if (info->stencil.texture) {
        descriptor.stencilAttachment.clearStencil = info->stencil.clear_stencil;
        descriptor.stencilAttachment.depthPlane = info->stencil.depth_plane;
        descriptor.stencilAttachment.level = info->stencil.level;
        descriptor.stencilAttachment.slice = info->stencil.slice;
        descriptor.stencilAttachment.texture = (id<MTLTexture>)info->stencil.texture;
        descriptor.stencilAttachment.loadAction = (MTLLoadAction)info->stencil.load_action;
        descriptor.stencilAttachment.storeAction = (MTLStoreAction)info->stencil.store_action;
      }

      descriptor.defaultRasterSampleCount = info->default_raster_sample_count;
      descriptor.renderTargetArrayLength = info->render_target_array_length;
      descriptor.renderTargetHeight = info->render_target_height;
      descriptor.renderTargetWidth = info->render_target_width;
      descriptor.visibilityResultBuffer = (id<MTLBuffer>)info->visibility_buffer;
      sample_positions(descriptor, info);

      if (info->tile_height && info->tile_width) {
        descriptor.tileWidth = info->tile_width;
        descriptor.tileHeight = info->tile_height;
      }

      for (unsigned i = 0; i < sizeof(info->colors) / sizeof(info->colors[0]); i++) {
        metal4_resident(owner, (id<MTLTexture>)info->colors[i].texture);
        metal4_resident(owner, (id<MTLTexture>)info->colors[i].resolve_texture);
      }
      metal4_resident(owner, (id<MTLTexture>)info->depth.texture);
      metal4_resident(owner, (id<MTLTexture>)info->stencil.texture);
      metal4_resident(owner, (id<MTLBuffer>)info->visibility_buffer);
      state->encoder = [[owner->buffer renderCommandEncoderWithDescriptor:descriptor] retain];
      [descriptor release];
    } else {
      state->encoder = [[owner->buffer computeCommandEncoder] retain];
      [state->encoder waitForFence:owner->fence
               beforeEncoderStages:MTLStageDispatch | MTLStageBlit | MTLStageAccelerationStructure];
      owner->compute = state;
    }
    if (!state->encoder) {
      [owner fail:[NSError errorWithDomain:MTL4CommandQueueErrorDomain
                                      code:MTL4CommandQueueErrorOutOfMemory
                                  userInfo:nil]];
      return STATUS_SUCCESS;
    }
    if (state->render)
      owner->feedback.render_passes++;
    else
      owner->feedback.compute_passes++;
    if (owner->order_aliases) {
      // memory one resource writes and another reads through an aliased address is consistent only after a barrier
      // with resource-alias visibility: a fence orders execution alone, and device visibility leaves the aliased
      // addresses' caches (MTL4VisibilityOptionResourceAlias; d3d12_sparse_remap)
      owner->order_aliases = false;
      [(id<MTL4CommandEncoder>)state->encoder barrierAfterQueueStages:MTLStageAll
                                                       beforeStages:MTLStageAll
                                                  visibilityOptions:MTL4VisibilityOptionDevice |
                                                                    MTL4VisibilityOptionResourceAlias];
    }
    metal4_begin_samples(state, p->samples.ptr);
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTL4CommandBuffer_orderAliases(void *obj) {
  struct unixcall_generic_obj_noret *p = obj;
  if (@available(macOS 26.0, *))
    ((MullionMetal4Buffer *)p->handle)->order_aliases = true;
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTL4CommandEncoder_endEncoding(void *obj) {
  struct unixcall_generic_obj_uint64_noret *p = obj;
  if (@available(macOS 26.0, *)) {
    MullionMetal4Encoder *state = (MullionMetal4Encoder *)p->handle;
    if (!state)
      return STATUS_SUCCESS;
    if (state->samples.sample_buffer) {
      if (state->render) {
        if (state->samples.end_of_encoder_sample_index - state->samples.start_of_encoder_sample_index + 1 ==
            WMTRenderTimestampSamples)
          [(id<MTL4RenderCommandEncoder>)state->encoder
              writeTimestampWithGranularity:MTL4TimestampGranularityPrecise
                                 afterStage:MTLRenderStageVertex
                                   intoHeap:(id<MTL4CounterHeap>)state->samples.sample_buffer
                                    atIndex:state->samples.start_of_encoder_sample_index + 1];
        [(id<MTL4RenderCommandEncoder>)state->encoder
            writeTimestampWithGranularity:MTL4TimestampGranularityPrecise
                               afterStage:MTLRenderStageFragment
                                 intoHeap:(id<MTL4CounterHeap>)state->samples.sample_buffer
                                  atIndex:state->samples.end_of_encoder_sample_index];
      } else {
        [(id<MTL4ComputeCommandEncoder>)state->encoder
            writeTimestampWithGranularity:MTL4TimestampGranularityPrecise
                                 intoHeap:(id<MTL4CounterHeap>)state->samples.sample_buffer
                                  atIndex:state->samples.end_of_encoder_sample_index];
        state->owner->stamp_heap = state->samples.sample_buffer;
        state->owner->stamp_index = state->samples.end_of_encoder_sample_index;
      }
      state->samples.sample_buffer = 0;
    }
    if (state->render) {
      MullionMetal4Buffer *owner = state->owner;
      for (unsigned stage = 0; stage < Metal4ComputeBinding; stage++) {
        for (unsigned i = 0; i < owner->buffers_set[stage]; i++)
          [owner->tables[stage] setAddress:0 atIndex:i];
        for (unsigned i = 0; i < owner->textures_set[stage]; i++)
          [owner->tables[stage] setTexture:(MTLResourceID){0} atIndex:i];
        owner->buffers_set[stage] = owner->textures_set[stage] = 0;
      }
      [state->encoder endEncoding];
    } else if (!p->arg)
      [state->owner closeCompute];
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTL4CommandBuffer_commit(void *obj) {
  if (@available(macOS 26.0, *)) {
    MullionMetal4Buffer *state = (MullionMetal4Buffer *)*(obj_handle_t *)obj;
    // a buffer that failed to begin is never ended (MTL4CommandBuffer: begin before end)
    @synchronized(state) {
      if (state->feedback.error) {
        state->feedback.status = WMTCommandBufferStatusError;
        state->finished = true;
        return STATUS_SUCCESS;
      }
      state->feedback.status = WMTCommandBufferStatusCommitted;
    }
    [state closeCompute];
    [state->residency commit];
    [state->buffer endCommandBuffer];
    dispatch_group_enter(state->feedback_ready);
    MTL4CommitOptions *options = [[MTL4CommitOptions alloc] init];
    if (!options) {
      [state fail:nil];
      state->feedback.status = WMTCommandBufferStatusError;
      state->finished = true;
      dispatch_group_leave(state->feedback_ready);
      return STATUS_SUCCESS;
    }
    [options addFeedbackHandler:^(id<MTL4CommitFeedback> feedback) {
      @synchronized(state) {
        state->feedback.error = (obj_handle_t)[feedback.error retain];
        state->feedback.gpu_start = feedback.GPUStartTime * NSEC_PER_SEC;
        state->feedback.gpu_end = feedback.GPUEndTime * NSEC_PER_SEC;
        state->feedback.status = feedback.error ? WMTCommandBufferStatusError : WMTCommandBufferStatusCompleted;
        state->finished = true;
      }
      dispatch_group_leave(state->feedback_ready);
    }];
    void (^commit)(void) = ^{
      if (state->wait_event)
        [state->queue waitForEvent:state->wait_event value:state->wait_value];
      if (state->drawable)
        [state->queue waitForDrawable:state->drawable];
      [state->queue commit:&state->buffer count:1 options:options];
      [state->queue signalEvent:state->event value:state->value];
      if (state->drawable) {
        [state->queue signalDrawable:state->drawable];
        state->after > 0 ? [state->drawable presentAfterMinimumDuration:state->after] : [state->drawable present];
      }
    };
    if (test_late_ns) {
      dispatch_time_t deadline = dispatch_time(DISPATCH_TIME_NOW, test_late_ns);
      pthread_mutex_lock(&test_late_mutex);
      dispatch_async(test_late_commits, ^{
        dispatch_semaphore_wait(test_late_timer, deadline);
        @autoreleasepool {
          commit();
        }
      });
      pthread_mutex_unlock(&test_late_mutex);
    } else {
      commit();
    }
    [options release];
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTL4CommandBuffer_feedback(void *obj) {
  struct unixcall_metal4_feedback *p = obj;
  if (@available(macOS 26.0, *)) {
    MullionMetal4Buffer *state = (MullionMetal4Buffer *)p->buffer;
    if (state)
      @synchronized(state) {
        p->ret = state->feedback;
      }
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTL4CommandBuffer_waitUntilCompleted(void *obj) {
  if (@available(macOS 26.0, *)) {
    MullionMetal4Buffer *state = (MullionMetal4Buffer *)*(obj_handle_t *)obj;
    for (;;) {
      @synchronized(state) {
        if (state->finished && state->feedback.error)
          break;
      }
      // an error's feedback also ends the wait if the GPU cannot signal its event
      if ([state->event waitUntilSignaledValue:state->value timeoutMS:NSEC_PER_SEC / NSEC_PER_MSEC]) {
        dispatch_group_wait(state->feedback_ready, DISPATCH_TIME_FOREVER);
        break;
      }
    }
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTL4CounterHeap_new(void *obj) {
  struct unixcall_generic_obj_uint64_obj_ret *p = obj;
  if (@available(macOS 26.0, *)) {
    _Static_assert(sizeof(MTL4TimestampHeapEntry) == sizeof(uint64_t), "timestamp query layout");
    MTL4CounterHeapDescriptor *desc = [[MTL4CounterHeapDescriptor alloc] init];
    desc.type = MTL4CounterHeapTypeTimestamp;
    desc.count = p->arg;
    // the driver writes the error of a heap past its size without checking that one was asked for
    NSError *error = nil;
    p->ret = (obj_handle_t)[(id<MTLDevice>)p->handle newCounterHeapWithDescriptor:desc error:&error];
    [desc release];
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_queryTimestampFrequency(void *obj) {
  struct unixcall_generic_obj_uint64_ret *p = obj;
  if (@available(macOS 26.0, *))
    p->ret = [(id<MTLDevice>)p->handle queryTimestampFrequency];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTL4CommandBuffer_waitForEvent(void *obj) {
  struct unixcall_mtl4_event *p = obj;
  if (@available(macOS 26.0, *)) {
    MullionMetal4Buffer *state = (MullionMetal4Buffer *)p->queue;
    [state->wait_event release];
    state->wait_event = [(id<MTLEvent>)p->event retain];
    state->wait_value = p->value;
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTL4CommandBuffer_present(void *obj) {
  struct unixcall_generic_obj_obj_double_noret *p = obj;
  if (@available(macOS 26.0, *)) {
    MullionMetal4Buffer *state = (MullionMetal4Buffer *)p->handle;
    state->drawable = [(id<MTLDrawable>)p->arg0 retain];
    state->after = p->arg1;
  }
  return STATUS_SUCCESS;
}

static void
API_AVAILABLE(macos(26.0)) metal4_draw_geometry_parts(
    MullionMetal4Encoder *state, uint32_t warps, uint32_t instances, uint32_t threads, uint32_t max
) {
  id<MTL4RenderCommandEncoder> encoder = (id<MTL4RenderCommandEncoder>)state->encoder;
  uint32_t origin[2] = {0, 0};
  MTLSize object = MTLSizeMake(threads, 1, 1), mesh = MTLSizeMake(1, 1, 1);
  if (!max || (uint64_t)warps * instances <= max) {
    metal4_bytes(state, Metal4ObjectBinding, origin, sizeof(origin), SM50_BINDING_INDEX_DRAW_ORIGIN);
    [encoder drawMeshThreadgroups:MTLSizeMake(warps, instances, 1)
        threadsPerObjectThreadgroup:object
          threadsPerMeshThreadgroup:mesh];
    return;
  }
  /* whole instances at a time where one fits, otherwise parts of one instance */
  uint32_t width = warps <= max ? warps : max, height = warps <= max ? max / warps : 1;
  for (uint64_t instance = 0; instance < instances; instance += height)
    for (uint64_t warp = 0; warp < warps; warp += width) {
      origin[0] = warp;
      origin[1] = instance;
      metal4_bytes(state, Metal4ObjectBinding, origin, sizeof(origin), SM50_BINDING_INDEX_DRAW_ORIGIN);
      [encoder drawMeshThreadgroups:MTLSizeMake(MIN(width, warps - origin[0]), MIN(height, instances - origin[1]), 1)
          threadsPerObjectThreadgroup:object
            threadsPerMeshThreadgroup:mesh];
    }
}

/* an indirect geometry draw's `parts` Metal draws, whose grids and origins the GS dispatch marshal wrote in `args`:
 * grids from `grids` on, 12 bytes apart, origins (SM50_BINDING_INDEX_DRAW_ORIGIN) from `origins` on, 8 bytes apart */
static void
API_AVAILABLE(macos(26.0)) metal4_draw_geometry_indirect_parts(
    MullionMetal4Encoder *state, id<MTLBuffer> args, uint64_t grids, uint64_t origins, uint32_t parts, uint32_t threads
) {
  id<MTL4RenderCommandEncoder> encoder = (id<MTL4RenderCommandEncoder>)state->encoder;
  for (uint32_t i = 0; i < parts; i++) {
    metal4_buffer(state, Metal4ObjectBinding, (obj_handle_t)args, origins + 8 * i, SM50_BINDING_INDEX_DRAW_ORIGIN);
    [encoder drawMeshThreadgroupsWithIndirectBuffer:metal4_address(state->owner, (obj_handle_t)args) + grids + 12 * i
                        threadsPerObjectThreadgroup:MTLSizeMake(threads, 1, 1)
                          threadsPerMeshThreadgroup:MTLSizeMake(1, 1, 1)];
  }
}

static void
API_AVAILABLE(macos(26.0)) metal4_render(MullionMetal4Encoder *state, const struct wmtcmd_base *next) {
  id<MTL4RenderCommandEncoder> encoder = (id<MTL4RenderCommandEncoder>)state->encoder;
  // a pipeline that could not be built is set as none: the draws after it are left out until another is set
  bool pipeline = true;
  while (next && !state->owner->feedback.error) {
    enum WMTRenderCommandType type = next->type;
    if (!pipeline &&
        ((type >= WMTRenderCommandDraw && type <= WMTRenderCommandDrawMeshThreadgroupsIndirect) ||
         (type >= WMTRenderCommandMullionGeometryDraw && type <= WMTRenderCommandMullionGeometryDrawIndexedIndirect) ||
         (type >= WMTRenderCommandMullionTessellationMeshDraw && type <= WMTRenderCommandExecuteCommandsInBuffer) ||
         type == WMTRenderCommandExecuteCommandsInBufferIndirect)) {
      next = next->next.ptr;
      continue;
    }
    switch (type) {
    default:
      assert(!next->type && "unhandled render command type");
      break;
    case WMTRenderCommandNop:
      break;
    case WMTRenderCommandUseResource: {
      const struct wmtcmd_render_useresource *body = (const struct wmtcmd_render_useresource *)next;
      metal4_resident(state->owner, (id<MTLResource>)body->resource);
      break;
    }
    case WMTRenderCommandSetVertexBuffer: {
      struct wmtcmd_render_setbuffer *body = (struct wmtcmd_render_setbuffer *)next;
      metal4_buffer(state, Metal4VertexBinding, body->buffer, body->offset, body->index);
      break;
    }
    case WMTRenderCommandSetVertexBufferOffset: {
      struct wmtcmd_render_setbufferoffset *body = (struct wmtcmd_render_setbufferoffset *)next;
      metal4_offset(state, Metal4VertexBinding, body->offset, body->index);
      break;
    }
    case WMTRenderCommandSetFragmentBuffer: {
      struct wmtcmd_render_setbuffer *body = (struct wmtcmd_render_setbuffer *)next;
      metal4_buffer(state, Metal4FragmentBinding, body->buffer, body->offset, body->index);
      break;
    }
    case WMTRenderCommandSetFragmentBufferOffset: {
      struct wmtcmd_render_setbufferoffset *body = (struct wmtcmd_render_setbufferoffset *)next;
      metal4_offset(state, Metal4FragmentBinding, body->offset, body->index);
      break;
    }
    case WMTRenderCommandSetMeshBuffer: {
      struct wmtcmd_render_setbuffer *body = (struct wmtcmd_render_setbuffer *)next;
      metal4_buffer(state, Metal4MeshBinding, body->buffer, body->offset, body->index);
      break;
    }
    case WMTRenderCommandSetMeshBufferOffset: {
      struct wmtcmd_render_setbufferoffset *body = (struct wmtcmd_render_setbufferoffset *)next;
      metal4_offset(state, Metal4MeshBinding, body->offset, body->index);
      break;
    }
    case WMTRenderCommandSetObjectBuffer: {
      struct wmtcmd_render_setbuffer *body = (struct wmtcmd_render_setbuffer *)next;
      metal4_buffer(state, Metal4ObjectBinding, body->buffer, body->offset, body->index);
      break;
    }
    case WMTRenderCommandSetObjectBufferOffset: {
      struct wmtcmd_render_setbufferoffset *body = (struct wmtcmd_render_setbufferoffset *)next;
      metal4_offset(state, Metal4ObjectBinding, body->offset, body->index);
      break;
    }
    case WMTRenderCommandSetFragmentBytes: {
      struct wmtcmd_render_setbytes *body = (struct wmtcmd_render_setbytes *)next;
      metal4_bytes(state, Metal4FragmentBinding, body->bytes.ptr, body->length, body->index);
      break;
    }
    case WMTRenderCommandSetFragmentTexture: {
      struct wmtcmd_render_settexture *body = (struct wmtcmd_render_settexture *)next;
      metal4_texture(state, Metal4FragmentBinding, body->texture, body->index);
      break;
    }
    case WMTRenderCommandSetRasterizerState: {
      struct wmtcmd_render_setrasterizerstate *body = (struct wmtcmd_render_setrasterizerstate *)next;
      [encoder setTriangleFillMode:(MTLTriangleFillMode)body->fill_mode];
      [encoder setCullMode:(MTLCullMode)body->cull_mode];
      [encoder setDepthClipMode:(MTLDepthClipMode)body->depth_clip_mode];
      [encoder setDepthBias:body->depth_bias slopeScale:body->scole_scale clamp:body->depth_bias_clamp];
      [encoder setFrontFacingWinding:(MTLWinding)body->winding];
      break;
    }
    case WMTRenderCommandSetViewports: {
      struct wmtcmd_render_setviewports *body = (struct wmtcmd_render_setviewports *)next;
      [encoder setViewports:(const MTLViewport *)body->viewports.ptr count:body->viewport_count];
      break;
    }
    case WMTRenderCommandSetScissorRects: {
      struct wmtcmd_render_setscissorrects *body = (struct wmtcmd_render_setscissorrects *)next;
      [encoder setScissorRects:(const MTLScissorRect *)body->scissor_rects.ptr count:body->rect_count];
      break;
    }
    case WMTRenderCommandSetPSO: {
      struct wmtcmd_render_setpso *body = (struct wmtcmd_render_setpso *)next;
      metal4_resident(state->owner, (id<MTLRenderPipelineState>)body->pso);
      if ((pipeline = body->pso))
        [encoder setRenderPipelineState:(id<MTLRenderPipelineState>)body->pso];
      break;
    }
    case WMTRenderCommandSetDSSO: {
      struct wmtcmd_render_setdsso *body = (struct wmtcmd_render_setdsso *)next;
      [encoder setDepthStencilState:(id<MTLDepthStencilState>)body->dsso];
      [encoder setStencilFrontReferenceValue:body->stencil_ref backReferenceValue:body->back_stencil_ref];
      break;
    }
    case WMTRenderCommandSetDepthStencilState: {
      struct wmtcmd_render_setdepthstencilstate *body = (struct wmtcmd_render_setdepthstencilstate *)next;
      [encoder setDepthStencilState:(id<MTLDepthStencilState>)body->depth_stencil_state];
      break;
    }
    case WMTRenderCommandSetBlendFactorAndStencilRef: {
      struct wmtcmd_render_setblendcolor *body = (struct wmtcmd_render_setblendcolor *)next;
      [encoder setBlendColorRed:body->red green:body->green blue:body->blue alpha:body->alpha];
      [encoder setStencilFrontReferenceValue:body->stencil_ref backReferenceValue:body->back_stencil_ref];
      break;
    }
    case WMTRenderCommandSetBlendFactor: {
      struct wmtcmd_render_setblendcolor *body = (struct wmtcmd_render_setblendcolor *)next;
      [encoder setBlendColorRed:body->red green:body->green blue:body->blue alpha:body->alpha];
      break;
    }
    case WMTRenderCommandSetStencilRef: {
      struct wmtcmd_render_setstencilref *body = (struct wmtcmd_render_setstencilref *)next;
      [encoder setStencilFrontReferenceValue:body->stencil_ref backReferenceValue:body->back_stencil_ref];
      break;
    }
    case WMTRenderCommandSetDepthTestBounds: {
      struct wmtcmd_render_setdepthtestbounds *body = (struct wmtcmd_render_setdepthtestbounds *)next;
      if (@available(macOS 26.0, *))
        [encoder setDepthTestMinBound:body->min maxBound:body->max];
      break;
    }
    case WMTRenderCommandSetVisibilityMode: {
      struct wmtcmd_render_setvisibilitymode *body = (struct wmtcmd_render_setvisibilitymode *)next;
      [encoder setVisibilityResultMode:(MTLVisibilityResultMode)body->mode offset:body->offset];
      break;
    }
    case WMTRenderCommandDraw: {
      struct wmtcmd_render_draw *body = (struct wmtcmd_render_draw *)next;
      [encoder drawPrimitives:(MTLPrimitiveType)body->primitive_type
                  vertexStart:body->vertex_start
                  vertexCount:body->vertex_count
                instanceCount:body->instance_count
                 baseInstance:body->base_instance];
      break;
    }
    case WMTRenderCommandDrawIndexed: {
      struct wmtcmd_render_draw_indexed *body = (struct wmtcmd_render_draw_indexed *)next;
      metal4_resident(state->owner, (id<MTLAllocation>)body->index_buffer);
      [encoder drawIndexedPrimitives:(MTLPrimitiveType)body->primitive_type
                          indexCount:body->index_count
                           indexType:(MTLIndexType)body->index_type
                         indexBuffer:metal4_address(state->owner, body->index_buffer) + body->index_buffer_offset
                   indexBufferLength:[(id<MTLBuffer>)body->index_buffer length] - body->index_buffer_offset
                       instanceCount:body->instance_count
                          baseVertex:body->base_vertex
                        baseInstance:body->base_instance];
      break;
    }
    case WMTRenderCommandDrawIndirect: {
      struct wmtcmd_render_draw_indirect *body = (struct wmtcmd_render_draw_indirect *)next;
      [encoder drawPrimitives:(MTLPrimitiveType)body->primitive_type
               indirectBuffer:metal4_address(state->owner, body->indirect_args_buffer) + body->indirect_args_offset];
      break;
    }
    case WMTRenderCommandDrawIndexedIndirect: {
      struct wmtcmd_render_draw_indexed_indirect *body = (struct wmtcmd_render_draw_indexed_indirect *)next;
      metal4_resident(state->owner, (id<MTLAllocation>)body->index_buffer);
      [encoder
          drawIndexedPrimitives:(MTLPrimitiveType)body->primitive_type
                      indexType:(MTLIndexType)body->index_type
                    indexBuffer:metal4_address(state->owner, body->index_buffer) + body->index_buffer_offset
              indexBufferLength:[(id<MTLBuffer>)body->index_buffer length] - body->index_buffer_offset
                 indirectBuffer:metal4_address(state->owner, body->indirect_args_buffer) + body->indirect_args_offset];
      break;
    }
    case WMTRenderCommandDrawMeshThreadgroups: {
      struct wmtcmd_render_draw_meshthreadgroups *body = (struct wmtcmd_render_draw_meshthreadgroups *)next;
      [encoder drawMeshThreadgroups:MTLSizeMake(
                                        body->threadgroup_per_grid.width, body->threadgroup_per_grid.height,
                                        body->threadgroup_per_grid.depth
                                    )
          threadsPerObjectThreadgroup:MTLSizeMake(
                                          body->object_threadgroup_size.width, body->object_threadgroup_size.height,
                                          body->object_threadgroup_size.depth
                                      )
            threadsPerMeshThreadgroup:MTLSizeMake(
                                          body->mesh_threadgroup_size.width, body->mesh_threadgroup_size.height,
                                          body->mesh_threadgroup_size.depth
                                      )];
      break;
    }
    case WMTRenderCommandDrawMeshThreadgroupsIndirect: {
      struct wmtcmd_render_draw_meshthreadgroups_indirect *body =
          (struct wmtcmd_render_draw_meshthreadgroups_indirect *)next;
      [encoder drawMeshThreadgroupsWithIndirectBuffer:metal4_address(state->owner, body->indirect_args_buffer) +
                                                      body->indirect_args_offset
                          threadsPerObjectThreadgroup:MTLSizeMake(
                                                          body->object_threadgroup_size.width,
                                                          body->object_threadgroup_size.height,
                                                          body->object_threadgroup_size.depth
                                                      )
                            threadsPerMeshThreadgroup:MTLSizeMake(
                                                          body->mesh_threadgroup_size.width,
                                                          body->mesh_threadgroup_size.height,
                                                          body->mesh_threadgroup_size.depth
                                                      )];
      break;
    }
    case WMTRenderCommandMemoryBarrier: {
      struct wmtcmd_render_memory_barrier *body = (struct wmtcmd_render_memory_barrier *)next;
      [encoder barrierAfterEncoderStages:(MTLStages)body->stages_after
                     beforeEncoderStages:(MTLStages)body->stages_before
                       visibilityOptions:MTL4VisibilityOptionDevice];
      break;
    }
    case WMTRenderCommandMullionGeometryDraw: {
      struct wmtcmd_render_mullion_geometry_draw *body = (struct wmtcmd_render_mullion_geometry_draw *)next;
      metal4_offset(state, Metal4ObjectBinding, body->draw_arguments_offset, SM50_BINDING_INDEX_DRAW_ARGUMENTS);
      metal4_draw_geometry_parts(
          state, body->warp_count, body->instance_count, body->vertex_per_warp, body->max_object_threadgroups
      );
      break;
    }
    case WMTRenderCommandMullionGeometryDrawIndexed: {
      struct wmtcmd_render_mullion_geometry_draw_indexed *body =
          (struct wmtcmd_render_mullion_geometry_draw_indexed *)next;
      metal4_buffer(
          state, Metal4ObjectBinding, body->index_buffer, body->index_buffer_offset, SM50_BINDING_INDEX_INDEX_BUFFER
      );
      metal4_offset(state, Metal4ObjectBinding, body->draw_arguments_offset, SM50_BINDING_INDEX_DRAW_ARGUMENTS);
      metal4_draw_geometry_parts(
          state, body->warp_count, body->instance_count, body->vertex_per_warp, body->max_object_threadgroups
      );
      break;
    }
    case WMTRenderCommandMullionGeometryDrawIndirect: {
      struct wmtcmd_render_mullion_geometry_draw_indirect *body =
          (struct wmtcmd_render_mullion_geometry_draw_indirect *)next;
      metal4_resident(state->owner, (id<MTLAllocation>)body->dispatch_args_buffer);
      metal4_buffer(
          state, Metal4ObjectBinding, body->indirect_args_buffer, body->indirect_args_offset,
          SM50_BINDING_INDEX_INDIRECT_ARGUMENTS
      );
      metal4_draw_geometry_indirect_parts(
          state, (id<MTLBuffer>)body->dispatch_args_buffer, body->dispatch_args_offset, body->origins_offset,
          body->parts, body->vertex_per_warp
      );
      metal4_buffer(state, Metal4ObjectBinding, body->imm_draw_arguments, 0, SM50_BINDING_INDEX_DRAW_ARGUMENTS);
      break;
    }
    case WMTRenderCommandMullionGeometryDrawIndexedIndirect: {
      struct wmtcmd_render_mullion_geometry_draw_indexed_indirect *body =
          (struct wmtcmd_render_mullion_geometry_draw_indexed_indirect *)next;
      metal4_buffer(
          state, Metal4ObjectBinding, body->index_buffer, body->index_buffer_offset, SM50_BINDING_INDEX_INDEX_BUFFER
      );
      metal4_buffer(
          state, Metal4ObjectBinding, body->indirect_args_buffer, body->indirect_args_offset,
          SM50_BINDING_INDEX_INDIRECT_ARGUMENTS
      );
      metal4_draw_geometry_indirect_parts(
          state, (id<MTLBuffer>)body->dispatch_args_buffer, body->dispatch_args_offset, body->origins_offset,
          body->parts, body->vertex_per_warp
      );
      metal4_buffer(state, Metal4ObjectBinding, body->imm_draw_arguments, 0, SM50_BINDING_INDEX_DRAW_ARGUMENTS);
      break;
    }
    case WMTRenderCommandMullionTessellationMeshDraw: {
      struct wmtcmd_render_mullion_tessellation_mesh_draw *body =
          (struct wmtcmd_render_mullion_tessellation_mesh_draw *)next;
      metal4_offset(state, Metal4ObjectBinding, body->draw_arguments_offset, SM50_BINDING_INDEX_DRAW_ARGUMENTS);
      [encoder drawMeshThreadgroups:MTLSizeMake(body->patch_per_mesh_instance, body->instance_count, body->parts)
          threadsPerObjectThreadgroup:MTLSizeMake(body->threads_per_patch, body->patch_per_group, 1)
            threadsPerMeshThreadgroup:MTLSizeMake(32, 1, 1)];
      break;
    }
    case WMTRenderCommandMullionTessellationMeshDrawIndexed: {
      struct wmtcmd_render_mullion_tessellation_mesh_draw_indexed *body =
          (struct wmtcmd_render_mullion_tessellation_mesh_draw_indexed *)next;
      metal4_buffer(
          state, Metal4ObjectBinding, body->index_buffer, body->index_buffer_offset, SM50_BINDING_INDEX_INDEX_BUFFER
      );
      metal4_offset(state, Metal4ObjectBinding, body->draw_arguments_offset, SM50_BINDING_INDEX_DRAW_ARGUMENTS);
      [encoder drawMeshThreadgroups:MTLSizeMake(body->patch_per_mesh_instance, body->instance_count, body->parts)
          threadsPerObjectThreadgroup:MTLSizeMake(body->threads_per_patch, body->patch_per_group, 1)
            threadsPerMeshThreadgroup:MTLSizeMake(32, 1, 1)];
      break;
    }

    case WMTRenderCommandMullionTessellationMeshDrawIndirect: {
      struct wmtcmd_render_mullion_tessellation_mesh_draw_indirect *body =
          (struct wmtcmd_render_mullion_tessellation_mesh_draw_indirect *)next;
      metal4_buffer(
          state, Metal4ObjectBinding, body->indirect_args_buffer, body->indirect_args_offset,
          SM50_BINDING_INDEX_INDIRECT_ARGUMENTS
      );
      [encoder drawMeshThreadgroupsWithIndirectBuffer:metal4_address(state->owner, body->dispatch_args_buffer) +
                                                      body->dispatch_args_offset
                          threadsPerObjectThreadgroup:MTLSizeMake(body->threads_per_patch, body->patch_per_group, 1)
                            threadsPerMeshThreadgroup:MTLSizeMake(32, 1, 1)];
      metal4_buffer(state, Metal4ObjectBinding, body->imm_draw_arguments, 0, SM50_BINDING_INDEX_DRAW_ARGUMENTS);
      break;
    }
    case WMTRenderCommandMullionTessellationMeshDrawIndexedIndirect: {
      struct wmtcmd_render_mullion_tessellation_mesh_draw_indexed_indirect *body =
          (struct wmtcmd_render_mullion_tessellation_mesh_draw_indexed_indirect *)next;
      metal4_buffer(
          state, Metal4ObjectBinding, body->index_buffer, body->index_buffer_offset, SM50_BINDING_INDEX_INDEX_BUFFER
      );
      metal4_buffer(
          state, Metal4ObjectBinding, body->indirect_args_buffer, body->indirect_args_offset,
          SM50_BINDING_INDEX_INDIRECT_ARGUMENTS
      );
      [encoder drawMeshThreadgroupsWithIndirectBuffer:metal4_address(state->owner, body->dispatch_args_buffer) +
                                                      body->dispatch_args_offset
                          threadsPerObjectThreadgroup:MTLSizeMake(body->threads_per_patch, body->patch_per_group, 1)
                            threadsPerMeshThreadgroup:MTLSizeMake(32, 1, 1)];
      metal4_buffer(state, Metal4ObjectBinding, body->imm_draw_arguments, 0, SM50_BINDING_INDEX_DRAW_ARGUMENTS);
      break;
    }
    case WMTRenderCommandUpdateFence: {
      struct wmtcmd_render_fence_op *body = (struct wmtcmd_render_fence_op *)next;
      [encoder updateFence:(id<MTLFence>)body->fence afterEncoderStages:(MTLStages)body->stages];
      break;
    }
    case WMTRenderCommandWaitForFence: {
      struct wmtcmd_render_fence_op *body = (struct wmtcmd_render_fence_op *)next;
      [encoder waitForFence:(id<MTLFence>)body->fence beforeEncoderStages:(MTLStages)body->stages];
      break;
    }
    case WMTRenderCommandSetViewport: {
      struct wmtcmd_render_setviewport *body = (struct wmtcmd_render_setviewport *)next;
      union {
        struct WMTViewport src;
        MTLViewport dst;
      } u = {.src = body->viewport};
      [encoder setViewport:u.dst];
      break;
    }
    case WMTRenderCommandSetScissorRect: {
      struct wmtcmd_render_setscissorrect *body = (struct wmtcmd_render_setscissorrect *)next;
      union {
        struct WMTScissorRect src;
        MTLScissorRect dst;
      } u = {.src = body->scissor_rect};
      [encoder setScissorRect:u.dst];
      break;
    }
    case WMTRenderCommandDispatchThreadsPerTile: {
      struct wmtcmd_render_dispatch_threads_per_tile *body = (struct wmtcmd_render_dispatch_threads_per_tile *)next;
      [encoder dispatchThreadsPerTile:MTLSizeMake(body->width, body->height, 1)];
      break;
    }
    case WMTRenderCommandExecuteCommandsInBuffer: {
      struct wmtcmd_render_executecommands *body = (struct wmtcmd_render_executecommands *)next;
      metal4_resident(state->owner, (id<MTLAllocation>)body->indirect_command_buffer);
      [encoder executeCommandsInBuffer:(id<MTLIndirectCommandBuffer>)body->indirect_command_buffer
                             withRange:NSMakeRange(body->location, body->length)];
      break;
    }
    case WMTRenderCommandExecuteCommandsInBufferIndirect: {
      struct wmtcmd_render_executecommands_indirect *body = (struct wmtcmd_render_executecommands_indirect *)next;
      metal4_resident(state->owner, (id<MTLAllocation>)body->indirect_command_buffer);
      [encoder
          executeCommandsInBuffer:(id<MTLIndirectCommandBuffer>)body->indirect_command_buffer
                   indirectBuffer:metal4_address(state->owner, body->indirect_buffer) + body->indirect_buffer_offset];
      break;
    }
    }
    next = next->next.ptr;
  }
}
static void
API_AVAILABLE(macos(26.0)) metal4_compute(MullionMetal4Encoder *state, const struct wmtcmd_base *next) {
  id<MTL4ComputeCommandEncoder> encoder = (id<MTL4ComputeCommandEncoder>)state->encoder;
  MTLSize threadgroup_size = state->threadgroup;
  while (next && !state->owner->feedback.error) {
    switch ((enum WMTComputeCommandType)next->type) {
    default:
      assert(!next->type && "unhandled compute command type");
      break;
    case WMTComputeCommandDispatch: {
      struct wmtcmd_compute_dispatch *body = (struct wmtcmd_compute_dispatch *)next;
      // a grid with no groups runs nothing in Direct3D (D3D11.3 18.6.2), and Metal rejects it
      if (!body->size.width || !body->size.height || !body->size.depth)
        break;
      [encoder dispatchThreadgroups:MTLSizeMake(body->size.width, body->size.height, body->size.depth)
              threadsPerThreadgroup:threadgroup_size];
      break;
    }
    case WMTComputeCommandDispatchPart: {
      struct wmtcmd_compute_dispatch_part *body = (struct wmtcmd_compute_dispatch_part *)next;
      if (!body->size.width || !body->size.height || !body->size.depth)
        break;
      MTLSize size = MTLSizeMake(body->size.width, body->size.height, body->size.depth);
      MTLSize group =
          MTLSizeMake(body->threadgroup_size.width, body->threadgroup_size.height, body->threadgroup_size.depth);
      // MTL4ComputeCommandEncoder has no stage-in region; MTLIndirectComputeCommand retains this API. the parts of
      // a command buffer take the commands of indirect command buffers it keeps, each twice the last
      MullionMetal4Buffer *owner = state->owner;
      if (!owner->parts || owner->parts_used == owner->parts.size) {
        MTLIndirectCommandBufferDescriptor *desc = [[[MTLIndirectCommandBufferDescriptor alloc] init] autorelease];
        desc.commandTypes = MTLIndirectCommandTypeConcurrentDispatch;
        desc.inheritBuffers = YES;
        desc.inheritPipelineState = YES;
        id<MTLIndirectCommandBuffer> commands =
            [owner->device newIndirectCommandBufferWithDescriptor:desc
                                                  maxCommandCount:MAX(2 * owner->parts.size, 1)
                                                          options:MTLResourceStorageModeShared];
        if (!commands) {
          [owner fail:nil];
          break;
        }
        metal4_resident(owner, commands);
        [commands release];
        owner->parts = commands;
        owner->parts_used = 0;
      }
      id<MTLIndirectComputeCommand> command = [owner->parts indirectComputeCommandAtIndex:owner->parts_used];
      [command setStageInRegion:MTLRegionMake3D(
                                    body->origin.x, body->origin.y, body->origin.z, size.width, size.height, size.depth
                                )];
      [command concurrentDispatchThreadgroups:size threadsPerThreadgroup:group];
      [encoder executeCommandsInBuffer:owner->parts withRange:NSMakeRange(owner->parts_used++, 1)];
      break;
    }
    case WMTComputeCommandDispatchThreads: {
      struct wmtcmd_compute_dispatch *body = (struct wmtcmd_compute_dispatch *)next;
      [encoder dispatchThreads:MTLSizeMake(body->size.width, body->size.height, body->size.depth)
          threadsPerThreadgroup:threadgroup_size];
      break;
    }
    case WMTComputeCommandDispatchIndirect: {
      struct wmtcmd_compute_dispatch_indirect *body = (struct wmtcmd_compute_dispatch_indirect *)next;
      [encoder dispatchThreadgroupsWithIndirectBuffer:metal4_address(state->owner, body->indirect_args_buffer) +
                                                      body->indirect_args_offset
                                threadsPerThreadgroup:threadgroup_size];
      break;
    }
    case WMTComputeCommandSetPSO: {
      struct wmtcmd_compute_setpso *body = (struct wmtcmd_compute_setpso *)next;
      [encoder setComputePipelineState:(id<MTLComputePipelineState>)body->pso];
      threadgroup_size.width = body->threadgroup_size.width;
      threadgroup_size.height = body->threadgroup_size.height;
      threadgroup_size.depth = body->threadgroup_size.depth;
      state->threadgroup = threadgroup_size;
      metal4_resident(state->owner, (id<MTLComputePipelineState>)body->pso);
      break;
    }
    case WMTComputeCommandSetBuffer: {
      struct wmtcmd_compute_setbuffer *body = (struct wmtcmd_compute_setbuffer *)next;
      metal4_buffer(state, Metal4ComputeBinding, body->buffer, body->offset, body->index);
      break;
    }
    case WMTComputeCommandSetBufferOffset: {
      struct wmtcmd_compute_setbufferoffset *body = (struct wmtcmd_compute_setbufferoffset *)next;
      metal4_offset(state, Metal4ComputeBinding, body->offset, body->index);
      break;
    }
    case WMTComputeCommandUseResource: {
      const struct wmtcmd_compute_useresource *body = (const struct wmtcmd_compute_useresource *)next;
      metal4_resident(state->owner, (id<MTLResource>)body->resource);
      break;
    }
    case WMTComputeCommandSetBytes: {
      struct wmtcmd_compute_setbytes *body = (struct wmtcmd_compute_setbytes *)next;
      metal4_bytes(state, Metal4ComputeBinding, body->bytes.ptr, body->length, body->index);
      break;
    }
    case WMTComputeCommandSetTexture: {
      struct wmtcmd_compute_settexture *body = (struct wmtcmd_compute_settexture *)next;
      metal4_texture(state, Metal4ComputeBinding, body->texture, body->index);
      break;
    }
    case WMTComputeCommandUpdateFence: {
      struct wmtcmd_compute_fence_op *body = (struct wmtcmd_compute_fence_op *)next;
      [encoder updateFence:(id<MTLFence>)body->fence
          afterEncoderStages:MTLStageDispatch | MTLStageBlit | MTLStageAccelerationStructure];
      state->overlaps = true;
      break;
    }
    case WMTComputeCommandSetVisibleFunctionTable: {
      struct wmtcmd_compute_setvisiblefunctiontable *body = (struct wmtcmd_compute_setvisiblefunctiontable *)next;
      metal4_resident(state->owner, (id<MTLVisibleFunctionTable>)body->table);
      [metal4_table(state, Metal4ComputeBinding) setResource:[(id<MTLVisibleFunctionTable>)body->table gpuResourceID]
                                               atBufferIndex:body->index];
      break;
    }
    case WMTComputeCommandWaitForFence: {
      struct wmtcmd_compute_fence_op *body = (struct wmtcmd_compute_fence_op *)next;
      // the encoder runs blits and acceleration structure builds as well (Metal4CommandBuffer)
      [encoder waitForFence:(id<MTLFence>)body->fence
          beforeEncoderStages:MTLStageDispatch | MTLStageBlit | MTLStageAccelerationStructure];
      break;
    }
    case WMTComputeCommandMemoryBarrier: {
      [encoder barrierAfterEncoderStages:MTLStageDispatch
                     beforeEncoderStages:MTLStageDispatch
                       visibilityOptions:MTL4VisibilityOptionDevice];
      break;
    }
    case WMTComputeCommandExecuteCommandsInBuffer: {
      struct wmtcmd_compute_executecommands *body = (struct wmtcmd_compute_executecommands *)next;
      metal4_resident(state->owner, (id<MTLAllocation>)body->indirect_command_buffer);
      [encoder executeCommandsInBuffer:(id<MTLIndirectCommandBuffer>)body->indirect_command_buffer
                             withRange:NSMakeRange(body->location, body->length)];
      break;
    }
    case WMTComputeCommandExecuteCommandsInBufferIndirect: {
      struct wmtcmd_compute_executecommands_indirect *body = (struct wmtcmd_compute_executecommands_indirect *)next;
      metal4_resident(state->owner, (id<MTLAllocation>)body->indirect_command_buffer);
      [encoder
          executeCommandsInBuffer:(id<MTLIndirectCommandBuffer>)body->indirect_command_buffer
                   indirectBuffer:metal4_address(state->owner, body->indirect_buffer) + body->indirect_buffer_offset];
      break;
    }
    }

    next = next->next.ptr;
  }
}
static void
API_AVAILABLE(macos(26.0)) metal4_blit(MullionMetal4Encoder *state, const struct wmtcmd_base *next) {
  id<MTL4ComputeCommandEncoder> encoder = (id<MTL4ComputeCommandEncoder>)state->encoder;
  while (next && !state->owner->feedback.error) {
    switch ((enum WMTBlitCommandType)next->type) {
    default:
      assert(!next->type && "unhandled blit command type");
      break;
    case WMTBlitCommandCopyFromBufferToBuffer: {
      struct wmtcmd_blit_copy_from_buffer_to_buffer *body = (struct wmtcmd_blit_copy_from_buffer_to_buffer *)next;
      metal4_resident(state->owner, (id<MTLAllocation>)body->dst);
      metal4_resident(state->owner, (id<MTLAllocation>)body->src);
      [encoder copyFromBuffer:(id<MTLBuffer>)body->src
                 sourceOffset:body->src_offset
                     toBuffer:(id<MTLBuffer>)body->dst
            destinationOffset:body->dst_offset
                         size:body->copy_length];
      break;
    }
    case WMTBlitCommandCopyFromBufferToTexture: {
      struct wmtcmd_blit_copy_from_buffer_to_texture *body = (struct wmtcmd_blit_copy_from_buffer_to_texture *)next;
      metal4_resident(state->owner, (id<MTLAllocation>)body->dst);
      metal4_resident(state->owner, (id<MTLAllocation>)body->src);
      [encoder copyFromBuffer:(id<MTLBuffer>)body->src
                 sourceOffset:body->src_offset
            sourceBytesPerRow:body->bytes_per_row
          sourceBytesPerImage:body->bytes_per_image
                   sourceSize:MTLSizeMake(body->size.width, body->size.height, body->size.depth)
                    toTexture:(id<MTLTexture>)body->dst
             destinationSlice:body->slice
             destinationLevel:body->level
            destinationOrigin:MTLOriginMake(body->origin.x, body->origin.y, body->origin.z)];
      break;
    }
    case WMTBlitCommandCopyFromBufferToTextureWithBlitOption: {
      struct wmtcmd_blit_copy_from_buffer_to_texture_withblitoption *body =
          (struct wmtcmd_blit_copy_from_buffer_to_texture_withblitoption *)next;
      metal4_resident(state->owner, (id<MTLAllocation>)body->dst);
      metal4_resident(state->owner, (id<MTLAllocation>)body->src);
      [encoder copyFromBuffer:(id<MTLBuffer>)body->src
                 sourceOffset:body->src_offset
            sourceBytesPerRow:body->bytes_per_row
          sourceBytesPerImage:body->bytes_per_image
                   sourceSize:MTLSizeMake(body->size.width, body->size.height, body->size.depth)
                    toTexture:(id<MTLTexture>)body->dst
             destinationSlice:body->slice
             destinationLevel:body->level
            destinationOrigin:MTLOriginMake(body->origin.x, body->origin.y, body->origin.z)
                      options:(MTLBlitOption)body->options];
      break;
    }
    case WMTBlitCommandCopyFromTextureToBuffer: {
      struct wmtcmd_blit_copy_from_texture_to_buffer *body = (struct wmtcmd_blit_copy_from_texture_to_buffer *)next;
      metal4_resident(state->owner, (id<MTLAllocation>)body->dst);
      metal4_resident(state->owner, (id<MTLAllocation>)body->src);
      [encoder copyFromTexture:(id<MTLTexture>)body->src
                       sourceSlice:body->slice
                       sourceLevel:body->level
                      sourceOrigin:MTLOriginMake(body->origin.x, body->origin.y, body->origin.z)
                        sourceSize:MTLSizeMake(body->size.width, body->size.height, body->size.depth)
                          toBuffer:(id<MTLBuffer>)body->dst
                 destinationOffset:body->offset
            destinationBytesPerRow:body->bytes_per_row
          destinationBytesPerImage:body->bytes_per_image];
      break;
    }
    case WMTBlitCommandCopyFromTextureToBufferWithBlitOption: {
      struct wmtcmd_blit_copy_from_texture_to_buffer_withblitoption *body =
          (struct wmtcmd_blit_copy_from_texture_to_buffer_withblitoption *)next;
      metal4_resident(state->owner, (id<MTLAllocation>)body->dst);
      metal4_resident(state->owner, (id<MTLAllocation>)body->src);
      [encoder copyFromTexture:(id<MTLTexture>)body->src
                       sourceSlice:body->slice
                       sourceLevel:body->level
                      sourceOrigin:MTLOriginMake(body->origin.x, body->origin.y, body->origin.z)
                        sourceSize:MTLSizeMake(body->size.width, body->size.height, body->size.depth)
                          toBuffer:(id<MTLBuffer>)body->dst
                 destinationOffset:body->offset
            destinationBytesPerRow:body->bytes_per_row
          destinationBytesPerImage:body->bytes_per_image
                           options:(MTLBlitOption)body->options];
      break;
    }
    case WMTBlitCommandCopyFromTextureToTexture: {
      struct wmtcmd_blit_copy_from_texture_to_texture *body = (struct wmtcmd_blit_copy_from_texture_to_texture *)next;
      metal4_resident(state->owner, (id<MTLAllocation>)body->dst);
      metal4_resident(state->owner, (id<MTLAllocation>)body->src);
      [encoder copyFromTexture:(id<MTLTexture>)body->src
                   sourceSlice:body->src_slice
                   sourceLevel:body->src_level
                  sourceOrigin:MTLOriginMake(body->src_origin.x, body->src_origin.y, body->src_origin.z)
                    sourceSize:MTLSizeMake(body->src_size.width, body->src_size.height, body->src_size.depth)
                     toTexture:(id<MTLTexture>)body->dst
              destinationSlice:body->dst_slice
              destinationLevel:body->dst_level
             destinationOrigin:MTLOriginMake(body->dst_origin.x, body->dst_origin.y, body->dst_origin.z)];
      break;
    }
    case WMTBlitCommandGenerateMipmaps: {
      struct wmtcmd_blit_generate_mipmaps *body = (struct wmtcmd_blit_generate_mipmaps *)next;
      metal4_resident(state->owner, (id<MTLAllocation>)body->texture);
      [encoder generateMipmapsForTexture:(id<MTLTexture>)body->texture];
      break;
    }
    case WMTBlitCommandUpdateFence: {
      struct wmtcmd_blit_fence_op *body = (struct wmtcmd_blit_fence_op *)next;
      [encoder updateFence:(id<MTLFence>)body->fence afterEncoderStages:MTLStageBlit];
      break;
    }
    case WMTBlitCommandWaitForFence: {
      struct wmtcmd_blit_fence_op *body = (struct wmtcmd_blit_fence_op *)next;
      [encoder waitForFence:(id<MTLFence>)body->fence beforeEncoderStages:MTLStageBlit];
      break;
    }
    case WMTBlitCommandFillBuffer: {
      struct wmtcmd_blit_fillbuffer *body = (struct wmtcmd_blit_fillbuffer *)next;
      metal4_resident(state->owner, (id<MTLAllocation>)body->buffer);
      [encoder fillBuffer:(id<MTLBuffer>)body->buffer range:NSMakeRange(body->offset, body->length) value:body->value];
      break;
    }
    case WMTBlitCommandResetCommandsInBuffer: {
      struct wmtcmd_blit_resetcommands *body = (struct wmtcmd_blit_resetcommands *)next;
      metal4_resident(state->owner, (id<MTLAllocation>)body->indirect_command_buffer);
      [encoder resetCommandsInBuffer:(id<MTLIndirectCommandBuffer>)body->indirect_command_buffer
                           withRange:NSMakeRange(body->location, body->length)];
      break;
    }
    case WMTBlitCommandCopyTexture: {
      struct wmtcmd_blit_copy_texture *body = (struct wmtcmd_blit_copy_texture *)next;
      metal4_resident(state->owner, (id<MTLAllocation>)body->dst);
      metal4_resident(state->owner, (id<MTLAllocation>)body->src);
      [encoder copyFromTexture:(id<MTLTexture>)body->src toTexture:(id<MTLTexture>)body->dst];
      break;
    }
    }

    [encoder barrierAfterEncoderStages:MTLStageBlit
                   beforeEncoderStages:MTLStageBlit
                     visibilityOptions:MTL4VisibilityOptionDevice];
    next = next->next.ptr;
  }
}
static MTL4AccelerationStructureDescriptor *
API_AVAILABLE(macos(26.0)) metal4_structure_descriptor(
    MullionMetal4Buffer *state, const struct WMTAccelerationStructureInfo *info
) {
  if (info->instances) {
    MTL4IndirectInstanceAccelerationStructureDescriptor *desc =
        [[[MTL4IndirectInstanceAccelerationStructureDescriptor alloc] init] autorelease];
    desc.usage = (MTLAccelerationStructureUsage)info->usage;
    desc.instanceDescriptorType = MTLAccelerationStructureInstanceDescriptorTypeIndirect;
    desc.instanceDescriptorBuffer = metal4_range(state, info->instance_buffer, info->instance_offset);
    desc.instanceDescriptorStride = sizeof(MTLIndirectAccelerationStructureInstanceDescriptor);
    desc.maxInstanceCount = info->max_instance_count;
    desc.instanceCountBuffer = metal4_range(state, info->instance_count_buffer, info->instance_count_offset);
    desc.instanceTransformationMatrixLayout = MTLMatrixLayoutRowMajor;
    return desc;
  }
  const struct WMTAccelerationStructureGeometry *geometries = info->geometries.ptr;
  NSMutableArray *array = [NSMutableArray arrayWithCapacity:info->geometry_count];
  for (uint32_t i = 0; i < info->geometry_count; i++) {
    const struct WMTAccelerationStructureGeometry *g = &geometries[i];
    MTL4AccelerationStructureGeometryDescriptor *geometry;
    if (g->vertex_format == WMTAttributeFormatInvalid) {
      MTL4AccelerationStructureBoundingBoxGeometryDescriptor *boxes =
          [[[MTL4AccelerationStructureBoundingBoxGeometryDescriptor alloc] init] autorelease];
      boxes.boundingBoxBuffer = metal4_range(state, g->buffer, g->offset);
      boxes.boundingBoxStride = g->stride;
      boxes.boundingBoxCount = g->count;
      geometry = boxes;
    } else {
      MTL4AccelerationStructureTriangleGeometryDescriptor *triangles =
          [[[MTL4AccelerationStructureTriangleGeometryDescriptor alloc] init] autorelease];
      triangles.vertexBuffer = metal4_range(state, g->buffer, g->offset);
      triangles.vertexStride = g->stride;
      triangles.vertexFormat = (MTLAttributeFormat)g->vertex_format;
      triangles.indexBuffer = metal4_range(state, g->index_buffer, g->index_offset);
      triangles.indexType = (MTLIndexType)g->index_type;
      triangles.triangleCount = g->count;
      triangles.transformationMatrixBuffer = metal4_range(state, g->transform_buffer, g->transform_offset);
      triangles.transformationMatrixLayout = MTLMatrixLayoutRowMajor;
      geometry = triangles;
    }
    geometry.opaque = g->opaque;
    geometry.allowDuplicateIntersectionFunctionInvocation = !g->no_duplicate_any_hit;
    [array addObject:geometry];
  }
  MTL4PrimitiveAccelerationStructureDescriptor *desc =
      [[[MTL4PrimitiveAccelerationStructureDescriptor alloc] init] autorelease];
  desc.usage = (MTLAccelerationStructureUsage)info->usage;
  desc.geometryDescriptors = array;
  return desc;
}

static void
API_AVAILABLE(macos(26.0)) metal4_acceleration(MullionMetal4Encoder *state, const struct wmtcmd_base *next) {
  id<MTL4ComputeCommandEncoder> encoder = (id<MTL4ComputeCommandEncoder>)state->encoder;
  while (next && !state->owner->feedback.error) {
    @autoreleasepool {
      switch ((enum WMTAccelerationStructureCommandType)next->type) {
      default:
        assert(!next->type && "unhandled acceleration structure command type");
        break;
      case WMTAccelerationStructureCommandBuild: {
        struct wmtcmd_accelerationstructure_build *body = (struct wmtcmd_accelerationstructure_build *)next;
        metal4_resident(state->owner, (id<MTLAllocation>)body->dst);
        metal4_resident(state->owner, (id<MTLAllocation>)body->src);
        MTL4AccelerationStructureDescriptor *desc = metal4_structure_descriptor(state->owner, &body->info);
        if (body->src)
          [encoder refitAccelerationStructure:(id<MTLAccelerationStructure>)body->src
                                   descriptor:desc
                                  destination:(id<MTLAccelerationStructure>)body->dst
                                scratchBuffer:metal4_range(state->owner, body->scratch, body->scratch_offset)];
        else
          [encoder buildAccelerationStructure:(id<MTLAccelerationStructure>)body->dst
                                   descriptor:desc
                                scratchBuffer:metal4_range(state->owner, body->scratch, body->scratch_offset)];
        break;
      }
      case WMTAccelerationStructureCommandCopy: {
        struct wmtcmd_accelerationstructure_copy *body = (struct wmtcmd_accelerationstructure_copy *)next;
        metal4_resident(state->owner, (id<MTLAllocation>)body->dst);
        metal4_resident(state->owner, (id<MTLAllocation>)body->src);
        [encoder copyAccelerationStructure:(id<MTLAccelerationStructure>)body->src
                   toAccelerationStructure:(id<MTLAccelerationStructure>)body->dst];
        break;
      }
      case WMTAccelerationStructureCommandCopyAndCompact: {
        struct wmtcmd_accelerationstructure_copy *body = (struct wmtcmd_accelerationstructure_copy *)next;
        metal4_resident(state->owner, (id<MTLAllocation>)body->dst);
        metal4_resident(state->owner, (id<MTLAllocation>)body->src);
        [encoder copyAndCompactAccelerationStructure:(id<MTLAccelerationStructure>)body->src
                             toAccelerationStructure:(id<MTLAccelerationStructure>)body->dst];
        break;
      }
      case WMTAccelerationStructureCommandWriteCompactedSize: {
        struct wmtcmd_accelerationstructure_write_compacted_size *body =
            (struct wmtcmd_accelerationstructure_write_compacted_size *)next;
        metal4_resident(state->owner, (id<MTLAllocation>)body->structure);
        [encoder writeCompactedAccelerationStructureSize:(id<MTLAccelerationStructure>)body->structure
                                                toBuffer:metal4_range(state->owner, body->buffer, body->offset)];
        break;
      }
      case WMTAccelerationStructureCommandWaitForFence: {
        struct wmtcmd_accelerationstructure_fence_op *body = (struct wmtcmd_accelerationstructure_fence_op *)next;
        [encoder waitForFence:(id<MTLFence>)body->fence beforeEncoderStages:MTLStageAccelerationStructure];
        break;
      }
      case WMTAccelerationStructureCommandUpdateFence: {
        struct wmtcmd_accelerationstructure_fence_op *body = (struct wmtcmd_accelerationstructure_fence_op *)next;
        [encoder updateFence:(id<MTLFence>)body->fence afterEncoderStages:MTLStageAccelerationStructure];
        break;
      }
      }
    }
    [encoder barrierAfterEncoderStages:MTLStageAccelerationStructure
                   beforeEncoderStages:MTLStageAccelerationStructure
                     visibilityOptions:MTL4VisibilityOptionDevice];
    next = next->next.ptr;
  }
}

static NTSTATUS
_MTL4CommandEncoder_encodeCommands(void *obj) {
  struct unixcall_metal4_commands *p = obj;
  if (@available(macOS 26.0, *)) {
    MullionMetal4Encoder *state = (MullionMetal4Encoder *)p->encoder;
    if (!state || state->owner->feedback.error)
      return STATUS_SUCCESS;
    switch (p->kind) {
    case WMTMetal4Render:
      metal4_render(state, p->head.ptr);
      break;
    case WMTMetal4Compute:
      metal4_compute(state, p->head.ptr);
      break;
    case WMTMetal4Blit:
      metal4_blit(state, p->head.ptr);
      break;
    case WMTMetal4AccelerationStructure:
      metal4_acceleration(state, p->head.ptr);
      break;
    default:
      return STATUS_UNSUCCESSFUL;
    }
  }
  return STATUS_SUCCESS;
}

const void *__wine_unix_call_funcs[] = {
    &_NSObject_retain,
    &_NSObject_release,
    &_NSArray_object,
    &_NSArray_count,
    &_MTLCopyAllDevices,
    &_MTLDevice_recommendedMaxWorkingSetSize,
    &_MTLDevice_currentAllocatedSize,
    &_MTLDevice_name,
    &_NSString_getCString,
    &_MTLDevice_newCommandQueue,
    &_NSAutoreleasePool_alloc_init,
    &_MTLCommandQueue_commandBuffer,
    &_MTLCommandBuffer_commit,
    &_MTLCommandBuffer_waitUntilCompleted,
    &_MTLCommandBuffer_status,
    &_MTLDevice_newSharedEvent,
    &_MTLSharedEvent_signaledValue,
    &_MTLCommandBuffer_encodeSignalEvent,
    &_MTLDevice_newBuffer,
    &_MTLDevice_newSamplerState,
    &_MTLDevice_newDepthStencilState,
    &_MTLDevice_newTexture,
    &_MTLBuffer_newTexture,
    &_MTLTexture_newTextureView,
    &_MTLDevice_minimumLinearTextureAlignmentForPixelFormat,
    &_MTLDevice_newLibrary,
    &_MTLLibrary_newFunction,
    &_NSString_lengthOfBytesUsingEncoding,
    &_NSObject_description,
    &_MTLDevice_newComputePipelineState,
    &_MTLCommandBuffer_blitCommandEncoder,
    &_MTLCommandBuffer_computeCommandEncoder,
    &_MTLCommandBuffer_renderCommandEncoder,
    &_MTLCommandEncoder_endEncoding,
    &_MTLDevice_newRenderPipelineState,
    &_MTLDevice_newMeshRenderPipelineState,
    &_MTLBlitCommandEncoder_encodeCommands,
    &_MTLComputeCommandEncoder_encodeCommands,
    &_MTLRenderCommandEncoder_encodeCommands,
    &_MTLTexture_pixelFormat,
    &_MTLTexture_width,
    &_MTLTexture_height,
    &_MTLTexture_depth,
    &_MTLTexture_arrayLength,
    &_MTLTexture_mipmapLevelCount,
    &_MTLTexture_replaceRegion,
    &_MTLBuffer_didModifyRange,
    &_MTLCommandBuffer_presentDrawable,
    &_MTLCommandBuffer_presentDrawableAfterMinimumDuration,
    &_MTLDevice_supportsFamily,
    &_MTLDevice_supportsBCTextureCompression,
    &_MTLDevice_supportsTextureSampleCount,
    &_MTLDevice_hasUnifiedMemory,
    &_MTLCaptureManager_sharedCaptureManager,
    &_MTLCaptureManager_startCapture,
    &_MTLCaptureManager_stopCapture,
    &_MTLDevice_newTemporalScaler,
    &_MTLDevice_newSpatialScaler,
    &_MTLCommandBuffer_encodeTemporalScale,
    &_MTLCommandBuffer_encodeSpatialScale,
    &_NSString_string,
    &_NSString_alloc_init,
    &_DeveloperHUDProperties_instance,
    &_DeveloperHUDProperties_addLabel,
    &_DeveloperHUDProperties_updateLabel,
    &_DeveloperHUDProperties_remove,
    &_MetalDrawable_texture,
    &_MetalLayer_nextDrawable,
    &_MTLDevice_supportsFXSpatialScaler,
    &_MTLDevice_supportsFXTemporalScaler,
    &_MetalLayer_setProps,
    &_MetalLayer_getProps,
    &_CreateMetalViewFromHWND,
    &_ReleaseMetalView,
    &thunk_SM50Initialize,
    &thunk_SM50Destroy,
    &thunk_SM50Compile,
    &thunk_SM50GetCompiledBitcode,
    &thunk_SM50DestroyBitcode,
    &thunk_SM50GetErrorMessage,
    &thunk_SM50FreeError,
    &thunk_SM50CompileGeometryPipelineVertex,
    &thunk_SM50CompileGeometryPipelineGeometry,
    NULL,
    &thunk_SM50CompileTessellationPipelineHull,
    &thunk_SM50CompileTessellationPipelineDomain,
    &_MTLCommandEncoder_setLabel,
    &_MTLDevice_setShouldMaximizeConcurrentCompilation,
    &thunk_SM50GetArgumentsInfo,
    &_MTLCommandBuffer_error,
    &_MTLCommandBuffer_logs,
    &_MTLLogContainer_enumerate,
    &_CGColorSpace_checkColorSpaceSupported,
    &_MetalLayer_setColorSpace,
    &_WMTGetPrimaryDisplayId,
    &_WMTGetSecondaryDisplayId,
    &_WMTGetDisplayDescription,
    &_MetalLayer_getEDRValue,
    &_MTLLibrary_newFunctionWithConstants,
    &_WMTQueryDisplaySetting,
    &_WMTUpdateDisplaySetting,
    &_WMTQueryDisplaySettingForLayer,
    &_MTLCommandBuffer_encodeWaitForEvent,
    &_MTLSharedEvent_signalValue,
    &_MTLSharedEvent_setWin32EventAtValue,
    &_MTLDevice_newFence,
    &_MTLDevice_newEvent,
    &_MTLBuffer_updateContents,
    &_SharedEventListener_create,
    &_SharedEventListener_start,
    &_SharedEventListener_destroy,
    &_WMTGetOSVersion,
    &_MTLDevice_newBinaryArchive,
    &_MTLBinaryArchive_serialize,
    &_DispatchData_alloc_init,
    &_CacheReader_alloc_init,
    &_CacheReader_get,
    &_CacheWriter_alloc_init,
    &_CacheWriter_set,
    &_WMTSetMetalShaderCachePath,
    &_MTLDevice_newSharedTexture,
    &_WMTBootstrapRegister,
    &_WMTBootstrapLookUp,
    &_MTLSharedEvent_createMachPort,
    &_MTLDevice_newSharedEventWithMachPort,
    &_MTLDevice_registryID,
    &_MTLSharedEvent_waitUntilSignaledValue,
    &_MTLCounterSampleBuffer_newTimestampBuffer,
    &_MTLCounterSampleBuffer_resolveCounterRange,
    &_MTLCommandBuffer_blitCommandEncoderWithSampleBuffers,
    &_MTLCommandBuffer_property,
    &_MTLDevice_newTileRenderPipelineState,
    &_MTLDevice_newResidencySet,
    &_MTLResidencySet_addAllocations,
    &_MTLResidencySet_removeAllocations,
    &_MTLResidencySet_removeAllAllocations,
    &_MTLResidencySet_commit,
    &_MTLCommandQueue_addResidencySet,
    &_MTLDevice_newHeap,
    &_MTLDevice_heapBufferSizeAndAlign,
    &_MTLDevice_heapTextureSizeAndAlign,
    &_MTLHeap_newBuffer,
    &_MTLHeap_newTexture,
    &_MTLDevice_newIndirectCommandBuffer,
    &_MTLDevice_newLibraryWithSource,
    &_MTLTexture_getBytes,
    &_MTLDevice_sampleTimestamps,
    &_MTLComputePipelineState_threadExecutionWidth,
    &_MTLDevice_gpuCoreCount,
    &_MTLDevice_supportsPlacementSparse,
    &_MTLDevice_sparseTileSizeInBytes,
    &_MTLDevice_sparseTileSize,
    &_MTLDevice_newPlacementSparseBuffer,
    &_MTLTexture_sparseTail,
    &_MTLDevice_newMTL4CommandQueue,
    &_MTL4CommandQueue_updateMappings,
    &_MTL4CommandQueue_copyMappings,
    &_MTL4CommandQueue_event,
    &_MTLDevice_accelerationStructureSizes,
    &_MTLDevice_newAccelerationStructure,
    &_MTLCommandBuffer_accelerationStructureCommandEncoder,
    &_MTLAccelerationStructureCommandEncoder_encodeCommands,
    &_MTLDevice_newLinkedComputePipelineState,
    &_MTLComputePipelineState_newVisibleFunctionTable,
    &thunk_SM50GetRayShader,
    &_MetalView_setPlacement,
    &_MTLRenderPipelineState_maxTotalThreadgroupsPerMeshGrid,
    &_MTLCommandBuffer_sampledCommandEncoder,
    &_WMTDiagWrite,
    &_DispatchData_getBytes,
    &_MTLComputePipelineState_maxTotalThreadsPerThreadgroup,
    &_MTLDevice_supports32BitFloatFiltering,
    &_MTLDevice_maxArgumentBufferSamplerCount,
    &_MTLDevice_supports32BitMSAA,
    &_WMTPresentStatistics_new,
    &_MetalDrawable_countPresent,
    &_WMTPresentStatistics_get,
    &_WMTGPUStatistics,
    &_MTLCommandBuffer_pushDebugGroup,
    &_MTLCommandBuffer_popDebugGroup,
    &_WMTFlightWrite,
    &_MTL4CommandBuffer_new,
    &_MTL4CommandBuffer_encoder,
    &_MTL4CommandEncoder_encodeCommands,
    &_MTL4CommandEncoder_endEncoding,
    &_MTL4CommandBuffer_commit,
    &_MTL4CommandBuffer_feedback,
    &_MTL4CommandBuffer_waitUntilCompleted,
    &_MTL4CounterHeap_new,
    &_MTLDevice_queryTimestampFrequency,
    &_MTL4CommandBuffer_present,
    &_MTL4CommandBuffer_waitForEvent,
    &_MTLDevice_supportsShaderBarycentricCoordinates,
    &_WMTTraceWrite,
    &_MTL4CommandBuffer_orderAliases,
    &_WMTBootstrapKeep,
};

#ifndef MULLION_NATIVE
const void *__wine_unix_call_wow64_funcs[] = {
    &_NSObject_retain,
    &_NSObject_release,
    &_NSArray_object,
    &_NSArray_count,
    &_MTLCopyAllDevices,
    &_MTLDevice_recommendedMaxWorkingSetSize,
    &_MTLDevice_currentAllocatedSize,
    &_MTLDevice_name,
    &_NSString_getCString,
    &_MTLDevice_newCommandQueue,
    &_NSAutoreleasePool_alloc_init,
    &_MTLCommandQueue_commandBuffer,
    &_MTLCommandBuffer_commit,
    &_MTLCommandBuffer_waitUntilCompleted,
    &_MTLCommandBuffer_status,
    &_MTLDevice_newSharedEvent,
    &_MTLSharedEvent_signaledValue,
    &_MTLCommandBuffer_encodeSignalEvent,
    &_MTLDevice_newBuffer,
    &_MTLDevice_newSamplerState,
    &_MTLDevice_newDepthStencilState,
    &_MTLDevice_newTexture,
    &_MTLBuffer_newTexture,
    &_MTLTexture_newTextureView,
    &_MTLDevice_minimumLinearTextureAlignmentForPixelFormat,
    &_MTLDevice_newLibrary,
    &_MTLLibrary_newFunction,
    &_NSString_lengthOfBytesUsingEncoding,
    &_NSObject_description,
    &_MTLDevice_newComputePipelineState,
    &_MTLCommandBuffer_blitCommandEncoder,
    &_MTLCommandBuffer_computeCommandEncoder,
    &_MTLCommandBuffer_renderCommandEncoder,
    &_MTLCommandEncoder_endEncoding,
    &_MTLDevice_newRenderPipelineState,
    &_MTLDevice_newMeshRenderPipelineState,
    &_MTLBlitCommandEncoder_encodeCommands,
    &_MTLComputeCommandEncoder_encodeCommands,
    &_MTLRenderCommandEncoder_encodeCommands,
    &_MTLTexture_pixelFormat,
    &_MTLTexture_width,
    &_MTLTexture_height,
    &_MTLTexture_depth,
    &_MTLTexture_arrayLength,
    &_MTLTexture_mipmapLevelCount,
    &_MTLTexture_replaceRegion,
    &_MTLBuffer_didModifyRange,
    &_MTLCommandBuffer_presentDrawable,
    &_MTLCommandBuffer_presentDrawableAfterMinimumDuration,
    &_MTLDevice_supportsFamily,
    &_MTLDevice_supportsBCTextureCompression,
    &_MTLDevice_supportsTextureSampleCount,
    &_MTLDevice_hasUnifiedMemory,
    &_MTLCaptureManager_sharedCaptureManager,
    &_MTLCaptureManager_startCapture,
    &_MTLCaptureManager_stopCapture,
    &_MTLDevice_newTemporalScaler,
    &_MTLDevice_newSpatialScaler,
    &_MTLCommandBuffer_encodeTemporalScale,
    &_MTLCommandBuffer_encodeSpatialScale,
    &_NSString_string,
    &_NSString_alloc_init,
    &_DeveloperHUDProperties_instance,
    &_DeveloperHUDProperties_addLabel,
    &_DeveloperHUDProperties_updateLabel,
    &_DeveloperHUDProperties_remove,
    &_MetalDrawable_texture,
    &_MetalLayer_nextDrawable,
    &_MTLDevice_supportsFXSpatialScaler,
    &_MTLDevice_supportsFXTemporalScaler,
    &_MetalLayer_setProps,
    &_MetalLayer_getProps,
    &_CreateMetalViewFromHWND,
    &_ReleaseMetalView,
    &thunk32_SM50Initialize,
    &thunk_SM50Destroy,
    &thunk32_SM50Compile,
    &thunk32_SM50GetCompiledBitcode,
    &thunk_SM50DestroyBitcode,
    &thunk32_SM50GetErrorMessage,
    &thunk_SM50FreeError,
    &thunk32_SM50CompileGeometryPipelineVertex,
    &thunk32_SM50CompileGeometryPipelineGeometry,
    NULL,
    &thunk32_SM50CompileTessellationPipelineHull,
    &thunk32_SM50CompileTessellationPipelineDomain,
    &_MTLCommandEncoder_setLabel,
    &_MTLDevice_setShouldMaximizeConcurrentCompilation,
    &thunk32_SM50GetArgumentsInfo,
    &_MTLCommandBuffer_error,
    &_MTLCommandBuffer_logs,
    &_MTLLogContainer_enumerate,
    &_CGColorSpace_checkColorSpaceSupported,
    &_MetalLayer_setColorSpace,
    &_WMTGetPrimaryDisplayId,
    &_WMTGetSecondaryDisplayId,
    &_WMTGetDisplayDescription,
    &_MetalLayer_getEDRValue,
    &_MTLLibrary_newFunctionWithConstants,
    &_WMTQueryDisplaySetting,
    &_WMTUpdateDisplaySetting,
    &_WMTQueryDisplaySettingForLayer,
    &_MTLCommandBuffer_encodeWaitForEvent,
    &_MTLSharedEvent_signalValue,
    &_MTLSharedEvent_setWin32EventAtValue,
    &_MTLDevice_newFence,
    &_MTLDevice_newEvent,
    &_MTLBuffer_updateContents,
    &_SharedEventListener_create,
    &_SharedEventListener_start,
    &_SharedEventListener_destroy,
    &_WMTGetOSVersion,
    &_MTLDevice_newBinaryArchive,
    &_MTLBinaryArchive_serialize,
    &_DispatchData_alloc_init,
    &_CacheReader_alloc_init,
    &_CacheReader_get,
    &_CacheWriter_alloc_init,
    &_CacheWriter_set,
    &_WMTSetMetalShaderCachePath,
    &_MTLDevice_newSharedTexture,
    &_WMTBootstrapRegister,
    &_WMTBootstrapLookUp,
    &_MTLSharedEvent_createMachPort,
    &_MTLDevice_newSharedEventWithMachPort,
    &_MTLDevice_registryID,
    &_MTLSharedEvent_waitUntilSignaledValue,
    &_MTLCounterSampleBuffer_newTimestampBuffer,
    &_MTLCounterSampleBuffer_resolveCounterRange,
    &_MTLCommandBuffer_blitCommandEncoderWithSampleBuffers,
    &_MTLCommandBuffer_property,
    &_MTLDevice_newTileRenderPipelineState,
    &_MTLDevice_newResidencySet,
    &_MTLResidencySet_addAllocations,
    &_MTLResidencySet_removeAllocations,
    &_MTLResidencySet_removeAllAllocations,
    &_MTLResidencySet_commit,
    &_MTLCommandQueue_addResidencySet,
    &_MTLDevice_newHeap,
    &_MTLDevice_heapBufferSizeAndAlign,
    &_MTLDevice_heapTextureSizeAndAlign,
    &_MTLHeap_newBuffer,
    &_MTLHeap_newTexture,
    &_MTLDevice_newIndirectCommandBuffer,
    &_MTLDevice_newLibraryWithSource,
    &_MTLTexture_getBytes,
    &_MTLDevice_sampleTimestamps,
    &_MTLComputePipelineState_threadExecutionWidth,
    &_MTLDevice_gpuCoreCount,
    &_MTLDevice_supportsPlacementSparse,
    &_MTLDevice_sparseTileSizeInBytes,
    &_MTLDevice_sparseTileSize,
    &_MTLDevice_newPlacementSparseBuffer,
    &_MTLTexture_sparseTail,
    &_MTLDevice_newMTL4CommandQueue,
    &_MTL4CommandQueue_updateMappings,
    &_MTL4CommandQueue_copyMappings,
    &_MTL4CommandQueue_event,
    &_MTLDevice_accelerationStructureSizes,
    &_MTLDevice_newAccelerationStructure,
    &_MTLCommandBuffer_accelerationStructureCommandEncoder,
    &_MTLAccelerationStructureCommandEncoder_encodeCommands,
    &_MTLDevice_newLinkedComputePipelineState,
    &_MTLComputePipelineState_newVisibleFunctionTable,
    &thunk32_SM50GetRayShader,
    &_MetalView_setPlacement,
    &_MTLRenderPipelineState_maxTotalThreadgroupsPerMeshGrid,
    &_MTLCommandBuffer_sampledCommandEncoder,
    &_WMTDiagWrite,
    &_DispatchData_getBytes,
    &_MTLComputePipelineState_maxTotalThreadsPerThreadgroup,
    &_MTLDevice_supports32BitFloatFiltering,
    &_MTLDevice_maxArgumentBufferSamplerCount,
    &_MTLDevice_supports32BitMSAA,
    &_WMTPresentStatistics_new,
    &_MetalDrawable_countPresent,
    &_WMTPresentStatistics_get,
    &_WMTGPUStatistics,
    &_MTLCommandBuffer_pushDebugGroup,
    &_MTLCommandBuffer_popDebugGroup,
    &_WMTFlightWrite,
    &_MTL4CommandBuffer_new,
    &_MTL4CommandBuffer_encoder,
    &_MTL4CommandEncoder_encodeCommands,
    &_MTL4CommandEncoder_endEncoding,
    &_MTL4CommandBuffer_commit,
    &_MTL4CommandBuffer_feedback,
    &_MTL4CommandBuffer_waitUntilCompleted,
    &_MTL4CounterHeap_new,
    &_MTLDevice_queryTimestampFrequency,
    &_MTL4CommandBuffer_present,
    &_MTL4CommandBuffer_waitForEvent,
    &_MTLDevice_supportsShaderBarycentricCoordinates,
    &_WMTTraceWrite,
    &_MTL4CommandBuffer_orderAliases,
    &_WMTBootstrapKeep,
};
_Static_assert(sizeof(__wine_unix_call_wow64_funcs) / sizeof(__wine_unix_call_wow64_funcs[0]) == WMTUnixCallCount,
               "32-bit bridge call count");
#endif

_Static_assert(sizeof(__wine_unix_call_funcs) / sizeof(__wine_unix_call_funcs[0]) == WMTUnixCallCount,
               "bridge call count");
