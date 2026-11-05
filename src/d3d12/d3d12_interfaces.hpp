#pragma once
#include "com/com_guid.hpp"
#include "d3d12.h"

// MetalFX upscaling recorded in a D3D12 command list (IMTLD3D12CommandListExt), for the NGX bridge: the same inputs
// as DLSS's, which the list's queue hands to MetalFX when it gets there

struct MTL_TEMPORAL_UPSCALE_D3D12_DESC {
  UINT InputContentWidth;  // 0: the color's width
  UINT InputContentHeight; // 0: the color's height
  BOOL AutoExposure;
  BOOL InReset;
  BOOL DepthReversed;
  BOOL MotionVectorInDisplayRes;
  ID3D12Resource *Color;
  ID3D12Resource *Depth;
  ID3D12Resource *MotionVector;
  ID3D12Resource *Output;
  ID3D12Resource *ExposureTexture; // optional
  FLOAT MotionVectorScaleX;
  FLOAT MotionVectorScaleY;
  FLOAT PreExposure;
  FLOAT JitterOffsetX;
  FLOAT JitterOffsetY;
};

// a ray-traced frame, upscaled and denoised from its G-buffer (MTLFXTemporalDenoisedScaler; DLSS Ray Reconstruction)
struct MTL_DENOISED_UPSCALE_D3D12_DESC {
  BOOL AutoExposure;
  BOOL InReset;
  BOOL DepthReversed;
  BOOL RoughnessInNormalAlpha; // roughness is the normals' alpha (DLSS.Roughness.Mode packed)
  ID3D12Resource *Color;
  ID3D12Resource *Depth;
  ID3D12Resource *MotionVector;
  ID3D12Resource *DiffuseAlbedo;
  ID3D12Resource *SpecularAlbedo;
  ID3D12Resource *Normals;
  ID3D12Resource *Roughness;           // unused when RoughnessInNormalAlpha
  ID3D12Resource *SpecularHitDistance; // optional
  ID3D12Resource *ExposureTexture;     // optional
  ID3D12Resource *Output;
  FLOAT MotionVectorScaleX;
  FLOAT MotionVectorScaleY;
  FLOAT PreExposure;
  FLOAT JitterOffsetX;
  FLOAT JitterOffsetY;
  // row-major, of row vectors (D3D's)
  FLOAT WorldToView[16];
  FLOAT ViewToClip[16];
};

// the frame between the last one interpolated and `Color` (MTLFXFrameInterpolator; DLSS Frame Generation). depth and
// motion are at the render size, color, UI and output at the swap chain's
struct MTL_FRAME_INTERPOLATE_D3D12_DESC {
  BOOL InReset;
  BOOL DepthReversed;
  BOOL UIComposited; // the UI is in Color too
  ID3D12Resource *Color;
  ID3D12Resource *Depth;
  ID3D12Resource *MotionVector;
  ID3D12Resource *UI; // optional: color and alpha
  ID3D12Resource *Output;
  // motion vectors times these are in pixels of the motion vectors' resource
  FLOAT MotionVectorScaleX;
  FLOAT MotionVectorScaleY;
  FLOAT JitterOffsetX;
  FLOAT JitterOffsetY;
  FLOAT DeltaTime; // seconds
  FLOAT Near;
  FLOAT Far;
  FLOAT FieldOfView; // vertical, in degrees
  FLOAT AspectRatio;
};

// a feature's MetalFX scaler, which its owner destroys when it is done with the feature
class IMTLD3D12Upscaler {
public:
  virtual void STDMETHODCALLTYPE Destroy() = 0;
};

DEFINE_COM_INTERFACE("987426c0-bca1-4bfb-8e8a-1da3257f8467", IMTLD3D12CommandListExt) : public IUnknown {
  virtual BOOL STDMETHODCALLTYPE SupportsTemporalUpscale() = 0;
  virtual BOOL STDMETHODCALLTYPE SupportsDenoisedUpscale() = 0;
  // *ppUpscaler is a feature's scaler, made at its first upscale and again when the inputs' sizes or formats change
  virtual HRESULT STDMETHODCALLTYPE
  TemporalUpscale(const MTL_TEMPORAL_UPSCALE_D3D12_DESC *pDesc, IMTLD3D12Upscaler **ppUpscaler) = 0;
  virtual HRESULT STDMETHODCALLTYPE
  DenoisedUpscale(const MTL_DENOISED_UPSCALE_D3D12_DESC *pDesc, IMTLD3D12Upscaler **ppUpscaler) = 0;
  virtual BOOL STDMETHODCALLTYPE SupportsFrameInterpolation() = 0;
  virtual HRESULT STDMETHODCALLTYPE
  FrameInterpolate(const MTL_FRAME_INTERPOLATE_D3D12_DESC *pDesc, IMTLD3D12Upscaler **ppUpscaler) = 0;
};
