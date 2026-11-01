// NGX for D3D12: DLSS Super Resolution on MetalFX's temporal scaler, and DLSS Ray Reconstruction on its temporal
// denoised scaler, recorded in the application's command list (IMTLD3D12CommandListExt)
#include "com/com_pointer.hpp"
#include "d3d12.h"
#include "nvngx.hpp"
#include "log/log.hpp"
#include "nvngx_feature.hpp"
#include "nvngx_parameter.hpp"
#include "../d3d12/d3d12_interfaces.hpp"
#include <cstring>
#include <memory>

namespace mullion {

NVNGX_API NVNGX_RESULT NVSDK_NGX_D3D11_GetCapabilityParameters(NVNGXParameter **out_params);

#define NVNGX_Parameter_DLSS_Roughness_Mode "DLSS.Roughness.Mode"
#define NVNGX_Parameter_Use_HW_Depth "DLSS.Use.HW.Depth"
#define NVNGX_Parameter_DiffuseAlbedo "DLSS.Input.DiffuseAlbedo"
#define NVNGX_Parameter_SpecularAlbedo "DLSS.Input.SpecularAlbedo"
#define NVNGX_Parameter_GBuffer_Albedo "GBuffer.Albedo"
#define NVNGX_Parameter_GBuffer_Specular "GBuffer.Specular"
#define NVNGX_Parameter_GBuffer_Normals "GBuffer.Normals"
#define NVNGX_Parameter_GBuffer_Roughness "GBuffer.Roughness"
#define NVNGX_Parameter_DLSSD_SpecularHitDistance "DLSSD.SpecularHitDistance"
#define NVNGX_Parameter_WorldToViewMatrix "WorldToViewMatrix"
#define NVNGX_Parameter_ViewToClipMatrix "ViewToClipMatrix"

static Com<IMTLD3D12CommandListExt>
GetExt(ID3D12GraphicsCommandList *list) {
  Com<IMTLD3D12CommandListExt> ext;
  if (!list || FAILED(list->QueryInterface(IID_PPV_ARGS(&ext))))
    return nullptr;
  return ext;
}

static ID3D12Resource *
GetResource(const ParametersImpl *params, const char *name, const char *alternative = nullptr) {
  ID3D12Resource *resource = nullptr;
  if (NVNGX_FAILED(params->Get(name, &resource)) && alternative)
    params->Get(alternative, &resource);
  return resource;
}

template <typename T>
static T
GetOr(const ParametersImpl *params, const char *name, T fallback) {
  T value;
  return NVNGX_FAILED(params->Get(name, &value)) ? fallback : value;
}

static NVNGX_RESULT
ReadCreate(const ParametersImpl *params, DLSSFeature &dlss) {
  if (NVNGX_FAILED(params->Get(NVNGX_Parameter_Width, &dlss.width)) ||
      NVNGX_FAILED(params->Get(NVNGX_Parameter_Height, &dlss.height)) ||
      NVNGX_FAILED(params->Get(NVNGX_Parameter_OutWidth, &dlss.target_width)) ||
      NVNGX_FAILED(params->Get(NVNGX_Parameter_OutHeight, &dlss.target_height)))
    return NVNGX_RESULT_INVALID_PARAMETER;
  dlss.quality = GetOr(params, NVNGX_Parameter_PerfQualityValue, 0);
  dlss.flag = GetOr(params, NVNGX_Parameter_DLSS_Feature_Create_Flags, 0);
  dlss.enable_output_subrects = GetOr(params, "DLSS.Enable.Output.Subrects", 0);
  return NVNGX_RESULT_OK;
}

NVNGX_API NVNGX_RESULT
NVSDK_NGX_D3D12_Init(
    unsigned long long id, const wchar_t *path, ID3D12Device *device, const void *feature_info, unsigned int sdk_version
) {
  Logger::debug("NGX: NVSDK_NGX_D3D12_Init");
  return NVNGX_RESULT_OK;
}

NVNGX_API NVNGX_RESULT
NVSDK_NGX_D3D12_Init_Ext(
    unsigned long long id, const wchar_t *path, ID3D12Device *device, unsigned int sdk_version, const void *parameters
) {
  Logger::debug("NGX: NVSDK_NGX_D3D12_Init_Ext");
  return NVNGX_RESULT_OK;
}

NVNGX_API NVNGX_RESULT
NVSDK_NGX_D3D12_Init_ProjectID(
    const char *project, unsigned int engine_type, const char *engine_version, const wchar_t *path,
    ID3D12Device *device, unsigned int sdk_version, const void *feature_info
) {
  Logger::debug("NGX: NVSDK_NGX_D3D12_Init_ProjectID");
  return NVNGX_RESULT_OK;
}

NVNGX_API NVNGX_RESULT
NVSDK_NGX_D3D12_Init_with_ProjectID(
    const char *project, unsigned int engine_type, const char *engine_version, const wchar_t *path,
    ID3D12Device *device, const void *feature_info, unsigned int sdk_version
) {
  Logger::debug("NGX: NVSDK_NGX_D3D12_Init_with_ProjectID");
  return NVNGX_RESULT_OK;
}

NVNGX_API NVNGX_RESULT
NVSDK_NGX_D3D12_Shutdown() {
  Logger::debug("NGX: NVSDK_NGX_D3D12_Shutdown");
  return NVNGX_RESULT_OK;
}

NVNGX_API NVNGX_RESULT
NVSDK_NGX_D3D12_Shutdown1(ID3D12Device *device) {
  Logger::debug("NGX: NVSDK_NGX_D3D12_Shutdown1");
  return NVNGX_RESULT_OK;
}

NVNGX_API NVNGX_RESULT
NVSDK_NGX_D3D12_GetScratchBufferSize(unsigned int feature, const NVNGXParameter *params, size_t *out_size) {
  Logger::debug("NGX: NVSDK_NGX_D3D12_GetScratchBufferSize");
  if (out_size)
    *out_size = 0;
  return NVNGX_RESULT_OK;
}

NVNGX_API NVNGX_RESULT
NVSDK_NGX_D3D12_AllocateParameters(NVNGXParameter **out_params) {
  Logger::debug("NGX: NVSDK_NGX_D3D12_AllocateParameters");
  *out_params = new ParametersImpl();
  return NVNGX_RESULT_OK;
}

NVNGX_API NVNGX_RESULT
NVSDK_NGX_D3D12_DestroyParameters(NVNGXParameter *params) {
  Logger::debug("NGX: NVSDK_NGX_D3D12_DestroyParameters");
  delete static_cast<ParametersImpl *>(params);
  return NVNGX_RESULT_OK;
}

// D3D11's, with Ray Reconstruction
NVNGX_API NVNGX_RESULT
NVSDK_NGX_D3D12_GetCapabilityParameters(NVNGXParameter **out_params) {
  Logger::debug("NGX: NVSDK_NGX_D3D12_GetCapabilityParameters");
  NVNGX_RESULT result = NVSDK_NGX_D3D11_GetCapabilityParameters(out_params);
  if (NVNGX_FAILED(result))
    return result;
  auto params = *out_params;
  void *optimal = nullptr, *stats = nullptr;
  params->Get(NVNGX_Parameter_DLSSOptimalSettingsCallback, &optimal);
  params->Get("DLSSGetStatsCallback", &stats);
  params->Set("SuperSamplingDenoising.Available", 1u);
  params->Set("SuperSamplingDenoising.NeedsUpdatedDriver", 0);
  params->Set("SuperSamplingDenoising.MinDriverVersionMajor", 0);
  params->Set("SuperSamplingDenoising.MinDriverVersionMinor", 0);
  params->Set("SuperSamplingDenoising.FeatureInitResult", 1u);
  params->Set("DLSSDOptimalSettingsCallback", optimal);
  params->Set("DLSSDGetStatsCallback", stats);
  return NVNGX_RESULT_OK;
}

NVNGX_API NVNGX_RESULT
NVSDK_NGX_D3D12_GetParameters(NVNGXParameter **out_params) {
  Logger::debug("NGX: NVSDK_NGX_D3D12_GetParameters");
  return NVSDK_NGX_D3D12_GetCapabilityParameters(out_params);
}

NVNGX_API NVNGX_RESULT
NVSDK_NGX_D3D12_GetFeatureRequirements(
    IDXGIAdapter *adapter, const NVNGX_FeatureDiscoveryInfo *discovery_info, NVNGX_FeatureRequirement *requirement
) {
  Logger::debug("NGX: NVSDK_NGX_D3D12_GetFeatureRequirements");
  if (!requirement || !discovery_info)
    return NVNGX_RESULT_INVALID_PARAMETER;
  if (discovery_info->FeatureID == NVNGX_FEATURE_SUPERSAMPLING ||
      discovery_info->FeatureID == NVNGX_FEATURE_RAYRECONSTRUCTION) {
    requirement->FeatureSupported = NVNGX_FEATURE_SUPPORT_RESULT_SUPPORTED;
    requirement->MinHWArchitecture = 0;
    strcpy_s(requirement->MinOSVersion, "10.0.16299.0");
    return NVNGX_RESULT_OK;
  }
  WARN("NVSDK_NGX_D3D12_GetFeatureRequirements: unsupported feature ", discovery_info->FeatureID);
  requirement->FeatureSupported = NVNGX_FEATURE_SUPPORT_RESULT_UNSUPPORTED;
  return NVNGX_RESULT_FAIL;
}

NVNGX_API NVNGX_RESULT
NVSDK_NGX_D3D12_CreateFeature(
    ID3D12GraphicsCommandList *list, unsigned int feature, NVNGXParameter *params, unsigned int **out_handle
) {
  Logger::debug("NGX: NVSDK_NGX_D3D12_CreateFeature");
  auto parameters = static_cast<ParametersImpl *>(params);
  auto ext = GetExt(list);
  if (!ext || !out_handle)
    return NVNGX_RESULT_INVALID_PARAMETER;
  if (feature == NVNGX_FEATURE_SUPERSAMPLING) {
    if (!ext->SupportsTemporalUpscale())
      return NVNGX_RESULT_FEATURE_NOT_SUPPORTED;
    auto dlss = std::make_unique<DLSSFeature>();
    dlss->feature = NVNGX_FEATURE_SUPERSAMPLING;
    if (NVNGX_FAILED(ReadCreate(parameters, *dlss)))
      return NVNGX_RESULT_INVALID_PARAMETER;
    *out_handle = &dlss.release()->handle;
    return NVNGX_RESULT_OK;
  }
  if (feature == NVNGX_FEATURE_RAYRECONSTRUCTION) {
    if (!ext->SupportsDenoisedUpscale())
      return NVNGX_RESULT_FEATURE_NOT_SUPPORTED;
    auto dlssd = std::make_unique<DLSSDFeature>();
    dlssd->feature = NVNGX_FEATURE_RAYRECONSTRUCTION;
    if (NVNGX_FAILED(ReadCreate(parameters, *dlssd)))
      return NVNGX_RESULT_INVALID_PARAMETER;
    dlssd->roughness_mode = GetOr(parameters, NVNGX_Parameter_DLSS_Roughness_Mode, 0u);
    dlssd->hw_depth = GetOr(parameters, NVNGX_Parameter_Use_HW_Depth, 1u);
    if (!dlssd->hw_depth)
      WARN("NVSDK_NGX_D3D12_CreateFeature: Ray Reconstruction with linear depth, which MetalFX reads as device depth");
    Logger::info(str::format(
        "NGX: Ray Reconstruction ", dlssd->width, "x", dlssd->height, " to ", dlssd->target_width, "x",
        dlssd->target_height, " on MetalFX's denoised scaler"
    ));
    *out_handle = &dlssd.release()->handle;
    return NVNGX_RESULT_OK;
  }
  ERR("NVSDK_NGX_D3D12_CreateFeature: feature ", feature, " not handled");
  return NVNGX_RESULT_FAIL;
}

static void
ReadCommon(const ParametersImpl *params, const DLSSFeature &dlss, float &mv_x, float &mv_y, float &jitter_x,
           float &jitter_y, float &pre_exposure, BOOL &reset) {
  mv_x = GetOr(params, NVNGX_Parameter_MV_Scale_X, 1.0f);
  mv_y = GetOr(params, NVNGX_Parameter_MV_Scale_Y, 1.0f);
  jitter_x = GetOr(params, NVNGX_Parameter_Jitter_Offset_X, 0.0f);
  jitter_y = GetOr(params, NVNGX_Parameter_Jitter_Offset_Y, 0.0f);
  // DLSS takes 0 (or none) for no pre-exposure, which MetalFX, dividing by it, takes as 1
  pre_exposure = GetOr(params, NVNGX_Parameter_DLSS_Pre_Exposure, 0.0f);
  if (pre_exposure <= 0.0f)
    pre_exposure = 1.0f;
  reset = GetOr(params, NVNGX_Parameter_Reset, 0) != 0;
}

NVNGX_API NVNGX_RESULT
NVSDK_NGX_D3D12_EvaluateFeature(
    ID3D12GraphicsCommandList *list, const unsigned int *handle, NVNGXParameter *params, void *callback
) {
  Logger::debug("NGX: NVSDK_NGX_D3D12_EvaluateFeature");
  auto parameters = static_cast<ParametersImpl *>(params);
  auto ext = GetExt(list);
  if (!ext || !handle) {
    ERR("NVSDK_NGX_D3D12_EvaluateFeature: ", !ext ? "a command list without Mullion's extension" : "no feature");
    return NVNGX_RESULT_INVALID_PARAMETER;
  }
  auto common = static_cast<CommonFeature *>((void *)handle);
  switch (common->feature) {
  case NVNGX_FEATURE_SUPERSAMPLING: {
    auto dlss = static_cast<DLSSFeature *>(common);
    MTL_TEMPORAL_UPSCALE_D3D12_DESC desc = {};
    desc.Color = GetResource(parameters, NVNGX_Parameter_Color);
    desc.Output = GetResource(parameters, NVNGX_Parameter_Output);
    desc.Depth = GetResource(parameters, NVNGX_Parameter_Depth);
    desc.MotionVector = GetResource(parameters, NVNGX_Parameter_MotionVectors);
    desc.ExposureTexture = GetResource(parameters, NVNGX_Parameter_ExposureTexture);
    if (!desc.Color || !desc.Output || !desc.Depth || !desc.MotionVector) {
      ERR("NVSDK_NGX_D3D12_EvaluateFeature: DLSS without ", !desc.Color ? "color " : "", !desc.Output ? "output " : "",
          !desc.Depth ? "depth " : "", !desc.MotionVector ? "motion vectors" : "");
      return NVNGX_RESULT_INVALID_PARAMETER;
    }
    desc.InputContentWidth = GetOr(parameters, NVNGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, 0u);
    desc.InputContentHeight = GetOr(parameters, NVNGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, 0u);
    desc.DepthReversed = bool(dlss->flag & NVNGX_DLSS_FLAG_DEPTH_INVERTED);
    desc.AutoExposure = bool(dlss->flag & NVNGX_DLSS_FLAG_AUTO_EXPOSURE);
    desc.MotionVectorInDisplayRes = !bool(dlss->flag & NVNGX_DLSS_FLAG_MV_LOWRES);
    ReadCommon(
        parameters, *dlss, desc.MotionVectorScaleX, desc.MotionVectorScaleY, desc.JitterOffsetX, desc.JitterOffsetY,
        desc.PreExposure, desc.InReset
    );
    return FAILED(ext->TemporalUpscale(&desc, &dlss->upscaler)) ? NVNGX_RESULT_FAIL : NVNGX_RESULT_OK;
  }
  case NVNGX_FEATURE_RAYRECONSTRUCTION: {
    auto dlssd = static_cast<DLSSDFeature *>(common);
    MTL_DENOISED_UPSCALE_D3D12_DESC desc = {};
    desc.Color = GetResource(parameters, NVNGX_Parameter_Color);
    desc.Output = GetResource(parameters, NVNGX_Parameter_Output);
    desc.Depth = GetResource(parameters, NVNGX_Parameter_Depth);
    desc.MotionVector = GetResource(parameters, NVNGX_Parameter_MotionVectors);
    desc.DiffuseAlbedo = GetResource(parameters, NVNGX_Parameter_DiffuseAlbedo, NVNGX_Parameter_GBuffer_Albedo);
    desc.SpecularAlbedo = GetResource(parameters, NVNGX_Parameter_SpecularAlbedo, NVNGX_Parameter_GBuffer_Specular);
    desc.Normals = GetResource(parameters, NVNGX_Parameter_GBuffer_Normals);
    desc.Roughness = GetResource(parameters, NVNGX_Parameter_GBuffer_Roughness);
    desc.SpecularHitDistance = GetResource(parameters, NVNGX_Parameter_DLSSD_SpecularHitDistance);
    desc.ExposureTexture = GetResource(parameters, NVNGX_Parameter_ExposureTexture);
    desc.RoughnessInNormalAlpha = dlssd->roughness_mode == 1;
    if (!desc.Color || !desc.Output || !desc.Depth || !desc.MotionVector || !desc.DiffuseAlbedo ||
        !desc.SpecularAlbedo || !desc.Normals || (!desc.Roughness && !desc.RoughnessInNormalAlpha)) {
      ERR("NVSDK_NGX_D3D12_EvaluateFeature: Ray Reconstruction without all of its inputs");
      return NVNGX_RESULT_INVALID_PARAMETER;
    }
    float *world_to_view = nullptr, *view_to_clip = nullptr;
    parameters->Get(NVNGX_Parameter_WorldToViewMatrix, (void **)&world_to_view);
    parameters->Get(NVNGX_Parameter_ViewToClipMatrix, (void **)&view_to_clip);
    if (!world_to_view || !view_to_clip) {
      ERR("NVSDK_NGX_D3D12_EvaluateFeature: Ray Reconstruction without its view matrices");
      return NVNGX_RESULT_INVALID_PARAMETER;
    }
    memcpy(desc.WorldToView, world_to_view, sizeof(desc.WorldToView));
    memcpy(desc.ViewToClip, view_to_clip, sizeof(desc.ViewToClip));
    desc.DepthReversed = bool(dlssd->flag & NVNGX_DLSS_FLAG_DEPTH_INVERTED);
    desc.AutoExposure = bool(dlssd->flag & NVNGX_DLSS_FLAG_AUTO_EXPOSURE);
    ReadCommon(
        parameters, *dlssd, desc.MotionVectorScaleX, desc.MotionVectorScaleY, desc.JitterOffsetX, desc.JitterOffsetY,
        desc.PreExposure, desc.InReset
    );
    return FAILED(ext->DenoisedUpscale(&desc, &dlssd->upscaler)) ? NVNGX_RESULT_FAIL : NVNGX_RESULT_OK;
  }
  default:
    break;
  }
  return NVNGX_RESULT_FAIL;
}

NVNGX_API NVNGX_RESULT
NVSDK_NGX_D3D12_EvaluateFeature_C(
    ID3D12GraphicsCommandList *list, const unsigned int *handle, NVNGXParameter *params, void *callback
) {
  Logger::debug("NGX: NVSDK_NGX_D3D12_EvaluateFeature_C");
  return NVSDK_NGX_D3D12_EvaluateFeature(list, handle, params, callback);
}

NVNGX_API NVNGX_RESULT
NVSDK_NGX_D3D12_ReleaseFeature(unsigned int *handle) {
  Logger::debug("NGX: NVSDK_NGX_D3D12_ReleaseFeature");
  if (!handle)
    return NVNGX_RESULT_INVALID_PARAMETER;
  auto common = static_cast<CommonFeature *>((void *)handle);
  switch (common->feature) {
  case NVNGX_FEATURE_SUPERSAMPLING:
  case NVNGX_FEATURE_RAYRECONSTRUCTION: {
    auto dlss = static_cast<DLSSFeature *>(common);
    if (dlss->upscaler)
      dlss->upscaler->Destroy();
    if (common->feature == NVNGX_FEATURE_RAYRECONSTRUCTION)
      delete static_cast<DLSSDFeature *>(common);
    else
      delete dlss;
    break;
  }
  default:
    break;
  }
  return NVNGX_RESULT_OK;
}

} // namespace mullion
