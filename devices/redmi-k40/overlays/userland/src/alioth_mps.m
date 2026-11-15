/*
 * MetalPerformanceShaders' image kernels, the ones applications use, done by this port (2026-11-15).
 *
 * MPS cannot work on this device: its kernels are not in the 20F66 system image at all (on an iPhone the GPU's own
 * driver bundle supplies them), so alioth_metal.m answers MPSCore's -supportsFamily: ladder with NO and MPS gets no
 * MPSDevice -- and an application that makes one of its kernels anyway dies on MPS's assertion: Nagram (Telegram),
 * tapping a photo in its picker, "[MPSImageHistogram initWithDevice:] Failed to get MPSDevice" (MPSKernel.mm:95).
 *
 * So the five classes Telegram-iOS uses (its sources: MediaEditor's histogram, enhance and blur passes, the round
 * video processor's blur and Lanczos scaling, the camera's and the call screen's bilinear downscale and 3x3 sharpen)
 * are made ours once the device is registered: each one's +allocWithZone: hands out an instance of a class of this
 * file instead (HKMPSImage..., NSObject's), which never runs MPSKernel's init, and does the work with compute
 * kernels of its own -- MSL in this file, compiled through Metal's compiler service the first time (which works
 * here, 11-08). MPSSupportsMTLDevice() and the -supportsFamily: answer stay as they are: system frameworks keep
 * their roads without MPS; only an application's own `MPSImage...(device:)` comes here.
 *
 *   MPSImageHistogram         initWithDevice:histogramInfo:, zeroHistogram, clipRectSource, histogramInfo,
 *                             histogramSizeForSourceFormat:, encodeToCommandBuffer:sourceTexture:histogram:histogramOffset:
 *   MPSImageGaussianBlur      initWithDevice:sigma:, sigma           (separable, radius ceil(3 sigma), two passes)
 *   MPSImageConvolution       initWithDevice:kernelWidth:kernelHeight:weights:, bias, kernelWidth/Height
 *   MPSImageBilinearScale     initWithDevice:, scaleTransform         (the sampler's bilinear filter)
 *   MPSImageLanczosScale      initWithDevice:, scaleTransform         (Lanczos-3, widened when shrinking; two passes)
 *   and of every one: encodeToCommandBuffer:sourceTexture:destinationTexture:, edgeMode (zero, clamp), offset,
 *   clipRect, label, device, options, copyWithZone:. Anything else sent to one is said once and answered with 0.
 *
 * Manual retain/release, as the rest of libhakios.metal.dylib. Tested by System Check's mps.* (each class against the
 * same arithmetic on the processor).
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalPerformanceShaders/MetalPerformanceShaders.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <math.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void mps_say(const char *format, ...) __printflike(1, 2);

/* (an application's standard error goes nowhere: the console, as libhakios.ui.dylib's lines) */
static void
mps_say(const char *format, ...)
{
	static int console = -1;
	char line[512];
	va_list ap;
	int n = snprintf(line, sizeof(line), "aliothmps[%s %d]: ", getprogname(), getpid());

	va_start(ap, format);
	n += vsnprintf(line + n, sizeof(line) - (size_t)n, format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 2) {
		n = (int)sizeof(line) - 2;
	}
	line[n++] = '\n';
	if (console < 0) {
		console = open("/dev/console", O_WRONLY);
	}
	write(console >= 0 ? console : 2, line, (size_t)n);
}

/* ---- the kernels ----------------------------------------------------------------------------------------------- */

static NSString *const hk_mps_source = @
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct hk_params {\n"
"    int2 dst_size; int2 src_size; int2 offset; int edge; int taps;\n"
"    float2 scale; float2 translate; float bias; float filter_scale; int2 dir;\n"
"};\n"
"struct hk_hist { uint2 origin; uint2 size; uint entries; uint channels; float4 lo; float4 per; };\n"
"static float4 hk_fetch(texture2d<float, access::read> t, int2 p, int2 size, int edge) {\n"
"    if (p.x < 0 || p.y < 0 || p.x >= size.x || p.y >= size.y) {\n"
"        if (edge == 0) return float4(0.0);\n"
"        p = clamp(p, int2(0), size - 1);\n"
"    }\n"
"    return t.read(uint2(p));\n"
"}\n"
/* one direction of a separable filter with weights w[0 .. taps-1], centred */
"kernel void hk_separable(texture2d<float, access::read> src [[texture(0)]], texture2d<float, access::write> dst [[texture(1)]],\n"
"    constant hk_params &k [[buffer(0)]], constant float *w [[buffer(1)]], uint2 gid [[thread_position_in_grid]]) {\n"
"    if (gid.x >= uint(k.dst_size.x) || gid.y >= uint(k.dst_size.y)) return;\n"
"    int2 p = int2(gid) + k.offset;\n"
"    int r = k.taps / 2;\n"
"    float4 s = float4(0.0);\n"
"    for (int i = 0; i < k.taps; i++) s += w[i] * hk_fetch(src, p + k.dir * (i - r), k.src_size, k.edge);\n"
"    dst.write(s, gid);\n"
"}\n"
/* a kernel_w x kernel_h convolution (taps = kernel_w, dir.y = kernel_h), weights row by row, plus the bias */
"kernel void hk_convolve(texture2d<float, access::read> src [[texture(0)]], texture2d<float, access::write> dst [[texture(1)]],\n"
"    constant hk_params &k [[buffer(0)]], constant float *w [[buffer(1)]], uint2 gid [[thread_position_in_grid]]) {\n"
"    if (gid.x >= uint(k.dst_size.x) || gid.y >= uint(k.dst_size.y)) return;\n"
"    int2 p = int2(gid) + k.offset;\n"
"    int kw = k.taps, kh = k.dir.y;\n"
"    float4 s = float4(k.bias);\n"
"    for (int j = 0; j < kh; j++)\n"
"        for (int i = 0; i < kw; i++) s += w[j * kw + i] * hk_fetch(src, p + int2(i - kw / 2, j - kh / 2), k.src_size, k.edge);\n"
"    dst.write(s, gid);\n"
"}\n"
/* bilinear: the destination's pixel centre back through the transform into the source, the sampler's filter */
"kernel void hk_bilinear(texture2d<float, access::sample> src [[texture(0)]], texture2d<float, access::write> dst [[texture(1)]],\n"
"    constant hk_params &k [[buffer(0)]], uint2 gid [[thread_position_in_grid]]) {\n"
"    if (gid.x >= uint(k.dst_size.x) || gid.y >= uint(k.dst_size.y)) return;\n"
"    constexpr sampler s(coord::normalized, address::clamp_to_edge, filter::linear);\n"
"    float2 at = (float2(gid) + 0.5 - k.translate) / k.scale;\n"
"    dst.write(src.sample(s, at / float2(k.src_size)), gid);\n"
"}\n"
/* Lanczos-3 along one direction (dir), the kernel widened by 1/filter_scale when shrinking */
"static float hk_lanczos(float t) {\n"
"    t = fabs(t);\n"
"    if (t < 1e-5) return 1.0;\n"
"    if (t >= 3.0) return 0.0;\n"
"    float a = M_PI_F * t;\n"
"    return 3.0 * sin(a) * sin(a / 3.0) / (a * a);\n"
"}\n"
"kernel void hk_lanczos_pass(texture2d<float, access::read> src [[texture(0)]], texture2d<float, access::write> dst [[texture(1)]],\n"
"    constant hk_params &k [[buffer(0)]], uint2 gid [[thread_position_in_grid]]) {\n"
"    if (gid.x >= uint(k.dst_size.x) || gid.y >= uint(k.dst_size.y)) return;\n"
"    bool across = k.dir.x != 0;\n"
"    float d = across ? float(gid.x) : float(gid.y);\n"
"    float sc = across ? k.scale.x : k.scale.y, tr = across ? k.translate.x : k.translate.y;\n"
"    float u = (d + 0.5 - tr) / sc - 0.5;\n"
"    float fs = k.filter_scale, support = 3.0 / fs;\n"
"    int i0 = int(floor(u - support)) + 1, i1 = int(floor(u + support));\n"
"    if (i1 - i0 > 63) { i0 = int(u) - 31; i1 = i0 + 63; }\n"
"    float4 s = float4(0.0); float ws = 0.0;\n"
"    for (int i = i0; i <= i1; i++) {\n"
"        float w = hk_lanczos((u - float(i)) * fs);\n"
"        int2 p = across ? int2(i, int(gid.y)) : int2(int(gid.x), i);\n"
"        s += w * hk_fetch(src, p, k.src_size, 1);\n"
"        ws += w;\n"
"    }\n"
"    dst.write(ws != 0.0 ? s / ws : s, gid);\n"
"}\n"
/* the histogram: every pixel of the rectangle adds one to its bin of each channel */
"kernel void hk_histogram(texture2d<float, access::read> src [[texture(0)]], device atomic_uint *h [[buffer(0)]],\n"
"    constant hk_hist &k [[buffer(1)]], uint2 gid [[thread_position_in_grid]]) {\n"
"    if (gid.x >= k.size.x || gid.y >= k.size.y) return;\n"
"    float4 v = src.read(gid + k.origin);\n"
"    for (uint c = 0; c < k.channels; c++) {\n"
"        int bin = clamp(int(floor((v[c] - k.lo[c]) * k.per[c])), 0, int(k.entries) - 1);\n"
"        atomic_fetch_add_explicit(&h[c * k.entries + uint(bin)], 1u, memory_order_relaxed);\n"
"    }\n"
"}\n";

/* the parameters, laid out as hk_params / hk_hist above (int2 and float2: 8-byte aligned; float4: 16) */
struct hk_params {
	int32_t dst_size[2], src_size[2], offset[2], edge, taps;
	float scale[2], translate[2], bias, filter_scale;
	int32_t dir[2];
};
struct hk_hist {
	uint32_t origin[2], size[2], entries, channels, pad[2];
	float lo[4], per[4];
};

/* one device's pipelines (an application has one device here) */
static id<MTLDevice> hk_device;
static id<MTLComputePipelineState> hk_pipe_separable, hk_pipe_convolve, hk_pipe_bilinear, hk_pipe_lanczos, hk_pipe_histogram;

static id<MTLComputePipelineState>
hk_pipeline(id<MTLLibrary> library, NSString *name)
{
	id<MTLFunction> function = [library newFunctionWithName:name];
	NSError *error = nil;
	id<MTLComputePipelineState> pipe = function ? [hk_device newComputePipelineStateWithFunction:function error:&error] : nil;

	if (pipe == nil) {
		mps_say("no pipeline for %s: %s", name.UTF8String, error ? error.localizedDescription.UTF8String : "no function");
	}
	[function release];
	return pipe;
}

static BOOL
hk_ready(id<MTLDevice> device)
{
	static dispatch_once_t once;

	dispatch_once(&once, ^{
		NSError *error = nil;
		id<MTLLibrary> library;

		hk_device = [device retain];
		library = [device newLibraryWithSource:hk_mps_source options:nil error:&error];
		if (library == nil) {
			mps_say("its kernels would not compile: %s", error ? error.localizedDescription.UTF8String : "?");
			return;
		}
		hk_pipe_separable = hk_pipeline(library, @"hk_separable");
		hk_pipe_convolve = hk_pipeline(library, @"hk_convolve");
		hk_pipe_bilinear = hk_pipeline(library, @"hk_bilinear");
		hk_pipe_lanczos = hk_pipeline(library, @"hk_lanczos_pass");
		hk_pipe_histogram = hk_pipeline(library, @"hk_histogram");
		[library release];
		mps_say("kernels compiled: MPS's image kernels are this port's in this process");
	});
	return hk_pipe_separable != nil && device == hk_device;
}

static void
hk_dispatch(id<MTLComputeCommandEncoder> e, id<MTLComputePipelineState> pipe, NSUInteger w, NSUInteger h)
{
	[e setComputePipelineState:pipe];
	[e dispatchThreadgroups:MTLSizeMake((w + 7) / 8, (h + 7) / 8, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
}

/* a scratch texture for the first of two passes (kept: the same size comes every frame) */
static id<MTLTexture>
hk_scratch(id<MTLTexture> *kept, NSUInteger w, NSUInteger h)
{
	if (*kept != nil && (*kept).width == w && (*kept).height == h) {
		return *kept;
	}
	[*kept release];
	MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:w
	                                                                             height:h mipmapped:NO];

	d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
	d.storageMode = MTLStorageModePrivate;
	*kept = [hk_device newTextureWithDescriptor:d];
	return *kept;
}

/* ---- the classes ----------------------------------------------------------------------------------------------- */

/* what every MPSUnaryImageKernel has */
@interface HKMPSKernel : NSObject <NSCopying> {
@public
	id<MTLDevice> _device;
	NSString *_label;
	NSUInteger _options;
	NSUInteger _edgeMode;
	MPSOffset _offset;
	MTLRegion _clipRect;
	Class _stands_for;      /* the MPS class it is in place of (for selectors it does not have) */
}
@end

@implementation HKMPSKernel
- (instancetype)initWithDevice:(id<MTLDevice>)device
{
	if ((self = [super init]) != nil) {
		_device = [device retain];
		_edgeMode = MPSImageEdgeModeZero;
		_clipRect = MTLRegionMake3D(0, 0, 0, NSUIntegerMax, NSUIntegerMax, NSUIntegerMax);  /* (MPSRectNoClip) */
		hk_ready(device);
	}
	return self;
}
- (void)dealloc
{
	[_device release];
	[_label release];
	[super dealloc];
}
- (id)copyWithZone:(NSZone *)zone
{
	(void)zone;
	return [self retain];   /* (nothing in it changes behind an encode) */
}
- (id<MTLDevice>)device { return _device; }
- (NSString *)label { return _label; }
- (void)setLabel:(NSString *)label { [_label release]; _label = [label copy]; }
- (NSUInteger)options { return _options; }
- (void)setOptions:(NSUInteger)options { _options = options; }
- (NSUInteger)edgeMode { return _edgeMode; }
- (void)setEdgeMode:(NSUInteger)edgeMode { _edgeMode = edgeMode; }
- (MPSOffset)offset { return _offset; }
- (void)setOffset:(MPSOffset)offset { _offset = offset; }
- (MTLRegion)clipRect { return _clipRect; }
- (void)setClipRect:(MTLRegion)clipRect { _clipRect = clipRect; }
- (BOOL)isKindOfClass:(Class)cls
{
	return [super isKindOfClass:cls] || (_stands_for != Nil && [_stands_for isSubclassOfClass:cls]);
}

/* the parameters every pass shares; the destination's size within clipRect */
- (struct hk_params)paramsFrom:(id<MTLTexture>)src to:(id<MTLTexture>)dst
{
	struct hk_params k;

	memset(&k, 0, sizeof(k));
	k.dst_size[0] = (int32_t)MIN(dst.width, _clipRect.origin.x + MIN(_clipRect.size.width, dst.width));
	k.dst_size[1] = (int32_t)MIN(dst.height, _clipRect.origin.y + MIN(_clipRect.size.height, dst.height));
	k.src_size[0] = (int32_t)src.width;
	k.src_size[1] = (int32_t)src.height;
	k.offset[0] = (int32_t)_offset.x;
	k.offset[1] = (int32_t)_offset.y;
	k.edge = _edgeMode == MPSImageEdgeModeClamp ? 1 : 0;
	k.scale[0] = k.scale[1] = 1;
	k.filter_scale = 1;
	return k;
}

/* selectors MPS's class has and this one does not: said once, answered with zeros */
- (NSMethodSignature *)methodSignatureForSelector:(SEL)sel
{
	NSMethodSignature *s = [super methodSignatureForSelector:sel];

	return s != nil ? s : _stands_for != Nil ? [_stands_for instanceMethodSignatureForSelector:sel] : nil;
}
- (void)forwardInvocation:(NSInvocation *)invocation
{
	static char said[64][96];
	const char *name = sel_getName(invocation.selector);
	int i;

	for (i = 0; i < 64 && said[i][0] != 0 && strcmp(said[i], name) != 0; i++) {
	}
	if (i < 64 && said[i][0] == 0) {
		strlcpy(said[i], name, sizeof(said[i]));
		mps_say("%s -%s is not done by this port: answered with 0", class_getName(_stands_for), name);
	}
	NSUInteger length = invocation.methodSignature.methodReturnLength;

	if (length > 0) {
		void *zero = calloc(1, length);

		[invocation setReturnValue:zero];
		free(zero);
	}
}
@end

@interface HKMPSImageGaussianBlur : HKMPSKernel {
	float _sigma;
	id<MTLTexture> _scratch;
}
@end

@implementation HKMPSImageGaussianBlur
- (instancetype)initWithDevice:(id<MTLDevice>)device sigma:(float)sigma
{
	if ((self = [super initWithDevice:device]) != nil) {
		_sigma = sigma;
	}
	return self;
}
- (void)dealloc
{
	[_scratch release];
	[super dealloc];
}
- (float)sigma { return _sigma; }
- (void)encodeToCommandBuffer:(id<MTLCommandBuffer>)commandBuffer sourceTexture:(id<MTLTexture>)src
           destinationTexture:(id<MTLTexture>)dst
{
	int radius = (int)ceilf(3 * _sigma);
	float w[193], sum = 0;

	if (!hk_ready(_device)) {
		return;
	}
	radius = radius < 1 ? 1 : radius > 96 ? 96 : radius;
	for (int i = -radius; i <= radius; i++) {
		w[i + radius] = _sigma > 0 ? expf(-(float)(i * i) / (2 * _sigma * _sigma)) : (i == 0);
		sum += w[i + radius];
	}
	for (int i = 0; i < 2 * radius + 1; i++) {
		w[i] /= sum;
	}
	struct hk_params k = [self paramsFrom:src to:dst];
	id<MTLTexture> mid = hk_scratch(&_scratch, src.width, src.height);
	id<MTLComputeCommandEncoder> e = [commandBuffer computeCommandEncoder];
	struct hk_params across = k;

	/* across, over the whole source (offset 0); then down, into the destination with the offset */
	across.dst_size[0] = k.src_size[0];
	across.dst_size[1] = k.src_size[1];
	across.offset[0] = across.offset[1] = 0;
	across.taps = 2 * radius + 1;
	across.dir[0] = 1;
	[e setTexture:src atIndex:0];
	[e setTexture:mid atIndex:1];
	[e setBytes:&across length:sizeof(across) atIndex:0];
	[e setBytes:w length:sizeof(float) * (size_t)(2 * radius + 1) atIndex:1];
	hk_dispatch(e, hk_pipe_separable, (NSUInteger)across.dst_size[0], (NSUInteger)across.dst_size[1]);
	/*
	 * (the second pass in an encoder of its own: in one encoder this driver did not make the first dispatch's
	 * writes to the scratch texture visible to the second -- System Check mps.blur read all zeros, 11-15)
	 */
	[e endEncoding];
	e = [commandBuffer computeCommandEncoder];
	k.taps = 2 * radius + 1;
	k.dir[1] = 1;
	[e setBytes:w length:sizeof(float) * (size_t)(2 * radius + 1) atIndex:1];
	[e setTexture:mid atIndex:0];
	[e setTexture:dst atIndex:1];
	[e setBytes:&k length:sizeof(k) atIndex:0];
	hk_dispatch(e, hk_pipe_separable, (NSUInteger)k.dst_size[0], (NSUInteger)k.dst_size[1]);
	[e endEncoding];
}
@end

@interface HKMPSImageConvolution : HKMPSKernel {
	NSUInteger _kw, _kh;
	float *_weights;
	float _bias;
}
@end

@implementation HKMPSImageConvolution
- (instancetype)initWithDevice:(id<MTLDevice>)device kernelWidth:(NSUInteger)kw kernelHeight:(NSUInteger)kh
                       weights:(const float *)weights
{
	if ((self = [super initWithDevice:device]) != nil) {
		_kw = kw < 1 ? 1 : kw > 31 ? 31 : kw;
		_kh = kh < 1 ? 1 : kh > 31 ? 31 : kh;
		_weights = calloc(_kw * _kh, sizeof(float));
		if (weights != NULL && _weights != NULL) {
			memcpy(_weights, weights, _kw * _kh * sizeof(float));
		}
	}
	return self;
}
- (void)dealloc
{
	free(_weights);
	[super dealloc];
}
- (NSUInteger)kernelWidth { return _kw; }
- (NSUInteger)kernelHeight { return _kh; }
- (float)bias { return _bias; }
- (void)setBias:(float)bias { _bias = bias; }
- (void)encodeToCommandBuffer:(id<MTLCommandBuffer>)commandBuffer sourceTexture:(id<MTLTexture>)src
           destinationTexture:(id<MTLTexture>)dst
{
	if (!hk_ready(_device) || _weights == NULL) {
		return;
	}
	struct hk_params k = [self paramsFrom:src to:dst];
	id<MTLComputeCommandEncoder> e = [commandBuffer computeCommandEncoder];

	k.taps = (int32_t)_kw;
	k.dir[1] = (int32_t)_kh;
	k.bias = _bias;
	[e setTexture:src atIndex:0];
	[e setTexture:dst atIndex:1];
	[e setBytes:&k length:sizeof(k) atIndex:0];
	[e setBytes:_weights length:_kw * _kh * sizeof(float) atIndex:1];
	hk_dispatch(e, hk_pipe_convolve, (NSUInteger)k.dst_size[0], (NSUInteger)k.dst_size[1]);
	[e endEncoding];
}
@end

/* the two scalers: scaleTransform (a pointer, NULL = the source stretched over the destination) */
@interface HKMPSImageScale : HKMPSKernel {
	MPSScaleTransform _transform;
	BOOL _has_transform;
	id<MTLTexture> _scratch;
}
@end

@implementation HKMPSImageScale
- (void)dealloc
{
	[_scratch release];
	[super dealloc];
}
- (const MPSScaleTransform *)scaleTransform { return _has_transform ? &_transform : NULL; }
- (void)setScaleTransform:(const MPSScaleTransform *)t
{
	_has_transform = t != NULL;
	if (t != NULL) {
		_transform = *t;
	}
}
- (struct hk_params)scaleParamsFrom:(id<MTLTexture>)src to:(id<MTLTexture>)dst
{
	struct hk_params k = [self paramsFrom:src to:dst];

	if (_has_transform) {
		k.scale[0] = (float)_transform.scaleX, k.scale[1] = (float)_transform.scaleY;
		k.translate[0] = (float)_transform.translateX, k.translate[1] = (float)_transform.translateY;
	} else {
		k.scale[0] = (float)dst.width / (float)src.width;
		k.scale[1] = (float)dst.height / (float)src.height;
	}
	return k;
}
@end

@interface HKMPSImageBilinearScale : HKMPSImageScale
@end

@implementation HKMPSImageBilinearScale
- (void)encodeToCommandBuffer:(id<MTLCommandBuffer>)commandBuffer sourceTexture:(id<MTLTexture>)src
           destinationTexture:(id<MTLTexture>)dst
{
	if (!hk_ready(_device)) {
		return;
	}
	struct hk_params k = [self scaleParamsFrom:src to:dst];
	id<MTLComputeCommandEncoder> e = [commandBuffer computeCommandEncoder];

	[e setTexture:src atIndex:0];
	[e setTexture:dst atIndex:1];
	[e setBytes:&k length:sizeof(k) atIndex:0];
	hk_dispatch(e, hk_pipe_bilinear, (NSUInteger)k.dst_size[0], (NSUInteger)k.dst_size[1]);
	[e endEncoding];
}
@end

@interface HKMPSImageLanczosScale : HKMPSImageScale
@end

@implementation HKMPSImageLanczosScale
- (void)encodeToCommandBuffer:(id<MTLCommandBuffer>)commandBuffer sourceTexture:(id<MTLTexture>)src
           destinationTexture:(id<MTLTexture>)dst
{
	if (!hk_ready(_device)) {
		return;
	}
	struct hk_params k = [self scaleParamsFrom:src to:dst], across = k;
	id<MTLTexture> mid = hk_scratch(&_scratch, (NSUInteger)k.dst_size[0], src.height);
	id<MTLComputeCommandEncoder> e = [commandBuffer computeCommandEncoder];

	/* across: the destination's width by the source's height; then down */
	across.dst_size[1] = k.src_size[1];
	across.dir[0] = 1;
	across.filter_scale = k.scale[0] < 1 ? k.scale[0] : 1;
	[e setTexture:src atIndex:0];
	[e setTexture:mid atIndex:1];
	[e setBytes:&across length:sizeof(across) atIndex:0];
	hk_dispatch(e, hk_pipe_lanczos, (NSUInteger)across.dst_size[0], (NSUInteger)across.dst_size[1]);
	[e endEncoding];        /* (the second pass in an encoder of its own: see the blur) */
	e = [commandBuffer computeCommandEncoder];
	k.src_size[0] = k.dst_size[0];
	k.dir[1] = 1;
	k.filter_scale = k.scale[1] < 1 ? k.scale[1] : 1;
	[e setTexture:mid atIndex:0];
	[e setTexture:dst atIndex:1];
	[e setBytes:&k length:sizeof(k) atIndex:0];
	hk_dispatch(e, hk_pipe_lanczos, (NSUInteger)k.dst_size[0], (NSUInteger)k.dst_size[1]);
	[e endEncoding];
}
@end

/* channels a histogram counts for a format: one, two, or red-green-blue (and alpha if asked) */
static NSUInteger
hk_channels(MTLPixelFormat format, BOOL alpha)
{
	switch (format) {
	case MTLPixelFormatA8Unorm: case MTLPixelFormatR8Unorm: case MTLPixelFormatR8Unorm_sRGB: case MTLPixelFormatR8Snorm:
	case MTLPixelFormatR8Uint: case MTLPixelFormatR8Sint: case MTLPixelFormatR16Unorm: case MTLPixelFormatR16Snorm:
	case MTLPixelFormatR16Uint: case MTLPixelFormatR16Sint: case MTLPixelFormatR16Float: case MTLPixelFormatR32Uint:
	case MTLPixelFormatR32Sint: case MTLPixelFormatR32Float:
		return 1;
	case MTLPixelFormatRG8Unorm: case MTLPixelFormatRG8Unorm_sRGB: case MTLPixelFormatRG8Snorm: case MTLPixelFormatRG16Unorm:
	case MTLPixelFormatRG16Snorm: case MTLPixelFormatRG16Float: case MTLPixelFormatRG32Float:
		return 2;
	default:
		return alpha ? 4 : 3;
	}
}

@interface HKMPSImageHistogram : HKMPSKernel {
	MPSImageHistogramInfo _info;
	BOOL _zero;
	MTLRegion _clipSource;
}
@end

@implementation HKMPSImageHistogram
- (instancetype)initWithDevice:(id<MTLDevice>)device histogramInfo:(const MPSImageHistogramInfo *)info
{
	if ((self = [super initWithDevice:device]) != nil) {
		if (info != NULL) {
			_info = *info;
		}
		if (_info.numberOfHistogramEntries < 1) {
			_info.numberOfHistogramEntries = 256;
		}
		_zero = YES;
		_clipSource = MTLRegionMake3D(0, 0, 0, NSUIntegerMax, NSUIntegerMax, 1);
	}
	return self;
}
- (MPSImageHistogramInfo)histogramInfo { return _info; }
- (BOOL)zeroHistogram { return _zero; }
- (void)setZeroHistogram:(BOOL)zero { _zero = zero; }
- (MTLRegion)clipRectSource { return _clipSource; }
- (void)setClipRectSource:(MTLRegion)r { _clipSource = r; }
- (size_t)histogramSizeForSourceFormat:(MTLPixelFormat)format
{
	return _info.numberOfHistogramEntries * sizeof(uint32_t) * hk_channels(format, _info.histogramForAlpha);
}
- (void)encodeToCommandBuffer:(id<MTLCommandBuffer>)commandBuffer sourceTexture:(id<MTLTexture>)src
                    histogram:(id<MTLBuffer>)histogram histogramOffset:(NSUInteger)offset
{
	if (!hk_ready(_device)) {
		return;
	}
	struct hk_hist k;
	size_t size = [self histogramSizeForSourceFormat:src.pixelFormat];
	NSUInteger x0 = MIN(_clipSource.origin.x, src.width), y0 = MIN(_clipSource.origin.y, src.height);

	memset(&k, 0, sizeof(k));
	k.origin[0] = (uint32_t)x0, k.origin[1] = (uint32_t)y0;
	k.size[0] = (uint32_t)MIN(_clipSource.size.width, src.width - x0);
	k.size[1] = (uint32_t)MIN(_clipSource.size.height, src.height - y0);
	k.entries = (uint32_t)_info.numberOfHistogramEntries;
	k.channels = (uint32_t)hk_channels(src.pixelFormat, _info.histogramForAlpha);
	for (int c = 0; c < 4; c++) {
		float lo = _info.minPixelValue[c], hi = _info.maxPixelValue[c];

		k.lo[c] = lo;
		k.per[c] = hi > lo ? (float)k.entries / (hi - lo) : 0;
	}
	if (_zero) {
		id<MTLBlitCommandEncoder> b = [commandBuffer blitCommandEncoder];

		[b fillBuffer:histogram range:NSMakeRange(offset, size) value:0];
		[b endEncoding];
	}
	if (k.size[0] == 0 || k.size[1] == 0) {
		return;
	}
	id<MTLComputeCommandEncoder> e = [commandBuffer computeCommandEncoder];

	[e setTexture:src atIndex:0];
	[e setBuffer:histogram offset:offset atIndex:0];
	[e setBytes:&k length:sizeof(k) atIndex:1];
	hk_dispatch(e, hk_pipe_histogram, k.size[0], k.size[1]);
	[e endEncoding];
}
@end

/* ---- in MPS's place ---------------------------------------------------------------------------------------------- */

static const struct { const char *mps, *ours; } hk_classes[] = {
	{ "MPSImageHistogram", "HKMPSImageHistogram" },
	{ "MPSImageGaussianBlur", "HKMPSImageGaussianBlur" },
	{ "MPSImageConvolution", "HKMPSImageConvolution" },
	{ "MPSImageBilinearScale", "HKMPSImageBilinearScale" },
	{ "MPSImageLanczosScale", "HKMPSImageLanczosScale" },
};
static bool hk_installed;

static id
hk_alloc_with_zone(Class cls, SEL cmd, NSZone *zone)
{
	(void)cmd;
	for (size_t i = 0; i < sizeof(hk_classes) / sizeof(hk_classes[0]); i++) {
		if (strcmp(class_getName(cls), hk_classes[i].mps) == 0) {
			HKMPSKernel *k = [objc_getClass(hk_classes[i].ours) allocWithZone:zone];

			k->_stands_for = cls;
			return k;
		}
	}
	/* (a subclass of one of them -- an application's own: MPS's way, which ends as before) */
	return class_createInstance(cls, 0);
}

static bool
hk_try_install(void)
{
	if (hk_installed) {
		return true;
	}
	if (objc_getClass("MPSImageHistogram") == Nil) {
		return false;   /* (MetalPerformanceShaders not loaded yet) */
	}
	for (size_t i = 0; i < sizeof(hk_classes) / sizeof(hk_classes[0]); i++) {
		Class mps = objc_getClass(hk_classes[i].mps);

		if (mps != Nil) {
			class_replaceMethod(object_getClass(mps), @selector(allocWithZone:), (IMP)hk_alloc_with_zone, "@@:^{_NSZone=}");
		}
	}
	hk_installed = true;
	mps_say("MPSImageHistogram, GaussianBlur, Convolution, BilinearScale and LanczosScale are this port's here");
	return true;
}

static void
hk_image_added(const struct mach_header *mh, intptr_t slide)
{
	(void)mh, (void)slide;
	hk_try_install();
}

/* called once the port's device is made (alioth_metal.m -initWithAcceleratorPort:) */
void
alioth_mps_install(void)
{
	static dispatch_once_t once;

	dispatch_once(&once, ^{
		if (getenv("HAKIOS_MPS_OFF") != NULL) {
			mps_say("HAKIOS_MPS_OFF: MPS left as it is");
			return;
		}
		/* (now, or as soon as MetalPerformanceShaders is loaded: the callback is also called for the images there are) */
		if (!hk_try_install()) {
			_dyld_register_func_for_add_image(hk_image_added);
		}
	});
}
