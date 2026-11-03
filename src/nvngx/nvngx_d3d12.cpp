// NGX for D3D12: DLSS Super Resolution on MetalFX's temporal scaler, DLSS Ray Reconstruction on its temporal denoised
// scaler, and DLSS Frame Generation on its frame interpolator, recorded in the application's command list
// (IMTLD3D12CommandListExt)
#include "com/com_pointer.hpp"
#include "d3d12.h"
#include "nvngx.hpp"
#include "log/log.hpp"
#include "nvngx_feature.hpp"
#include "nvngx_parameter.hpp"
#include "../d3d12/d3d12_interfaces.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

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

// whether the device NGX started with has MetalFX's denoiser, which Ray Reconstruction is offered by: a command list
// of its own says
static std::atomic<bool> denoising = false;

static NVNGX_RESULT
Start(ID3D12Device *device) {
  Com<ID3D12CommandAllocator> allocator;
  Com<ID3D12GraphicsCommandList> list;
  if (!device || FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
      FAILED(device->CreateCommandList(
          0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.ptr(), nullptr, IID_PPV_ARGS(&list)
      )))
    return NVNGX_RESULT_INVALID_PARAMETER;
  auto ext = GetExt(list.ptr());
  denoising = ext && ext->SupportsDenoisedUpscale();
  list->Close();
  return NVNGX_RESULT_OK;
}

// NGX's NVSDK_NGX_FeatureCommonInfo: where the application keeps its feature libraries (nvsdk_ngx_defs.h)
struct NVNGX_PathListInfo {
  const wchar_t *const *Path;
  unsigned int Length;
};
struct NVNGX_FeatureCommonInfo {
  NVNGX_PathListInfo PathListInfo;
  void *InternalData;
};

// the folders an Init named for feature libraries
static std::mutex library_paths_mutex;
static std::vector<std::wstring> library_paths;

static void
RememberLibraryPaths(const void *feature_info) {
  auto info = static_cast<const NVNGX_FeatureCommonInfo *>(feature_info);
  if (!info || !info->PathListInfo.Path)
    return;
  std::lock_guard lock(library_paths_mutex);
  for (unsigned i = 0; i < info->PathListInfo.Length; i++)
    if (info->PathListInfo.Path[i])
      library_paths.emplace_back(info->PathListInfo.Path[i]);
}

// whether the application ships a feature's library (`name`): NVIDIA's NGX core offers a feature only when it finds
// its snippet (nvngx_dlss.dll for Super Resolution, nvngx_dlssd.dll for Ray Reconstruction) loaded, beside the
// executable, or in a folder Init or the discovery names. a game that ships none asks for the feature anyway where it
// is offered, and then has none of its inputs: Control, which has no nvngx_dlssd.dll, turns Ray Reconstruction on
// when the feature requirements say it is supported, and its renderer passes no Ray Reconstruction textures
static bool
HasFeatureLibrary(const wchar_t *name, const void *feature_info = nullptr) {
  if (GetModuleHandleW(name))
    return true;
  std::vector<std::wstring> folders;
  wchar_t exe[MAX_PATH];
  if (DWORD length = GetModuleFileNameW(nullptr, exe, MAX_PATH); length && length < MAX_PATH) {
    std::wstring path(exe, length);
    folders.push_back(path.substr(0, path.find_last_of(L"\\/")));
  }
  {
    std::lock_guard lock(library_paths_mutex);
    folders.insert(folders.end(), library_paths.begin(), library_paths.end());
  }
  if (auto info = static_cast<const NVNGX_FeatureCommonInfo *>(feature_info); info && info->PathListInfo.Path)
    for (unsigned i = 0; i < info->PathListInfo.Length; i++)
      if (info->PathListInfo.Path[i])
        folders.emplace_back(info->PathListInfo.Path[i]);
  for (auto &folder : folders) {
    auto path = folder;
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/')
      path += L'\\';
    if (GetFileAttributesW((path + name).c_str()) != INVALID_FILE_ATTRIBUTES)
      return true;
  }
  return false;
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
  RememberLibraryPaths(feature_info);
  return Start(device);
}

NVNGX_API NVNGX_RESULT
NVSDK_NGX_D3D12_Init_Ext(
    unsigned long long id, const wchar_t *path, ID3D12Device *device, unsigned int sdk_version, const void *parameters
) {
  Logger::debug("NGX: NVSDK_NGX_D3D12_Init_Ext");
  return Start(device);
}

NVNGX_API NVNGX_RESULT
NVSDK_NGX_D3D12_Init_ProjectID(
    const char *project, unsigned int engine_type, const char *engine_version, const wchar_t *path,
    ID3D12Device *device, unsigned int sdk_version, const void *feature_info
) {
  Logger::debug("NGX: NVSDK_NGX_D3D12_Init_ProjectID");
  RememberLibraryPaths(feature_info);
  return Start(device);
}

NVNGX_API NVNGX_RESULT
NVSDK_NGX_D3D12_Init_with_ProjectID(
    const char *project, unsigned int engine_type, const char *engine_version, const wchar_t *path,
    ID3D12Device *device, const void *feature_info, unsigned int sdk_version
) {
  Logger::debug("NGX: NVSDK_NGX_D3D12_Init_with_ProjectID");
  RememberLibraryPaths(feature_info);
  return Start(device);
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

// what Streamline asks of a Frame Generation feature at each frame: that it evaluates every frame (when it is off too,
// so the real frame is copied out), and that no capture is running
static NVNGX_RESULT
DLSSG_GetCurrentSettings(unsigned int *handle, NVNGXParameter *params) {
  if (!params)
    return NVNGX_RESULT_INVALID_PARAMETER;
  params->Set("DLSSG.MustCallEval", 1u);
  params->Set("DLSSG.BurstCaptureRunning", 0u);
  return NVNGX_RESULT_OK;
}

// the video memory Frame Generation needs at a size, which Streamline asks before turning it on: MetalFX's interpolator
// keeps a few frames at the output size
static NVNGX_RESULT
DLSSG_EstimateVRAM(
    unsigned int width, unsigned int height, unsigned int, unsigned int, unsigned int, unsigned int, unsigned int,
    unsigned int, unsigned int, size_t *estimated
) {
  if (estimated)
    *estimated = size_t(std::max(width, 1u)) * std::max(height, 1u) * 4 * 8 + (64 << 20);
  return NVNGX_RESULT_OK;
}

// D3D11's, with Ray Reconstruction and Frame Generation
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
  params->Set("SuperSamplingDenoising.Available", unsigned(denoising.load() && HasFeatureLibrary(L"nvngx_dlssd.dll")));
  params->Set("SuperSamplingDenoising.NeedsUpdatedDriver", 0);
  params->Set("SuperSamplingDenoising.MinDriverVersionMajor", 0);
  params->Set("SuperSamplingDenoising.MinDriverVersionMinor", 0);
  params->Set("SuperSamplingDenoising.FeatureInitResult", 1u);
  params->Set("DLSSDOptimalSettingsCallback", optimal);
  params->Set("DLSSDGetStatsCallback", stats);
  // Frame Generation, as the game's Streamline (sl.dlss_g) reads it: one frame between two, no Reflex frame warp
  unsigned frame_generation = HasFeatureLibrary(L"nvngx_dlssg.dll");
  params->Set("FrameGeneration.Available", frame_generation);
  params->Set("FrameInterpolation.Available", frame_generation);
  params->Set("FrameGeneration.NeedsUpdatedDriver", 0);
  params->Set("FrameGeneration.MinDriverVersionMajor", 0);
  params->Set("FrameGeneration.MinDriverVersionMinor", 0);
  params->Set("FrameGeneration.FeatureInitResult", 1u);
  params->Set("DLSSG.GetCurrentSettingsCallback", (void *)&DLSSG_GetCurrentSettings);
  params->Set("DLSSG.EstimateVRAMCallback", (void *)&DLSSG_EstimateVRAM);
  params->Set("DLSSG.MultiFrameCountMax", 1u);
  params->Set("DLSSG.ReflexWarp.Available", 0u);
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
      (discovery_info->FeatureID == NVNGX_FEATURE_RAYRECONSTRUCTION && denoising &&
       HasFeatureLibrary(L"nvngx_dlssd.dll", discovery_info->FeatureInfo)) ||
      (discovery_info->FeatureID == NVNGX_FEATURE_FRAMEGENERATION &&
       HasFeatureLibrary(L"nvngx_dlssg.dll", discovery_info->FeatureInfo))) {
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
    if (!ext->SupportsDenoisedUpscale() || !HasFeatureLibrary(L"nvngx_dlssd.dll"))
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
  if (feature == NVNGX_FEATURE_FRAMEGENERATION) {
    if (!ext->SupportsFrameInterpolation() || !HasFeatureLibrary(L"nvngx_dlssg.dll"))
      return NVNGX_RESULT_FEATURE_NOT_SUPPORTED;
    auto dlssg = std::make_unique<DLSSGFeature>();
    dlssg->feature = NVNGX_FEATURE_FRAMEGENERATION;
    dlssg->width = GetOr(parameters, NVNGX_Parameter_Width, 0u);
    dlssg->height = GetOr(parameters, NVNGX_Parameter_Height, 0u);
    parameters->Set("DLSSG.MustCallEval", 1u);
    Logger::info(str::format("NGX: Frame Generation at ", dlssg->width, "x", dlssg->height, " on MetalFX's frame interpolator"));
    *out_handle = &dlssg.release()->handle;
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

// the camera Frame Generation is given: near and far planes, and the vertical field of view in degrees. Streamline's
// is in radians, which some games give in degrees; without one it comes from the projection (row vectors: [1][1] is
// 1 / tan(fov / 2))
static void
ReadCamera(const ParametersImpl *params, MTL_FRAME_INTERPOLATE_D3D12_DESC &desc) {
  desc.Near = GetOr(params, "DLSSG.CameraNear", 0.0f);
  desc.Far = GetOr(params, "DLSSG.CameraFar", 0.0f);
  float fov = GetOr(params, "DLSSG.CameraFOV", 0.0f);
  float *view_to_clip = nullptr;
  params->Get("DLSSG.CameraViewToClip", (void **)&view_to_clip);
  if (!(fov > 0.0f) && view_to_clip && view_to_clip[5] > 0.0f)
    fov = 2.0f * std::atan(1.0f / view_to_clip[5]);
  if (!(fov > 0.0f))
    fov = 1.0f;
  desc.FieldOfView = fov > 10.0f ? fov : fov * 180.0f / 3.14159265f;
  // 0 is a far plane at infinity
  if (!(desc.Far > 0.0f) || !std::isfinite(desc.Far))
    desc.Far = std::max(desc.Near, 0.1f) * 1.0e5f;
  if (!(desc.Near > 0.0f))
    desc.Near = 0.1f;
}

static NVNGX_RESULT
EvaluateFrameGeneration(
    ID3D12GraphicsCommandList *list, IMTLD3D12CommandListExt *ext, DLSSGFeature *dlssg, ParametersImpl *params
) {
  params->Set("DLSSG.FlushRequired", 0u);
  // Streamline records the feature's work in a list it hands over closed unless it says it is recording
  bool recording = GetOr(params, "DLSSG.IsRecording", 0u) != 0;
  if (!recording) {
    ID3D12CommandAllocator *allocator = nullptr;
    params->Get("DLSSG.CmdAlloc", (void **)&allocator);
    if (!allocator || FAILED(list->Reset(allocator, nullptr))) {
      ERR("NVSDK_NGX_D3D12_EvaluateFeature: Frame Generation without a command allocator");
      return NVNGX_RESULT_INVALID_PARAMETER;
    }
  }
  auto backbuffer = GetResource(params, "DLSSG.Backbuffer");
  auto real = GetResource(params, "DLSSG.OutputReal");
  NVNGX_RESULT result = NVNGX_RESULT_OK;
  if (GetOr(params, "DLSSG.EnableInterp", 0u)) {
    MTL_FRAME_INTERPOLATE_D3D12_DESC desc = {};
    auto hudless = GetResource(params, "DLSSG.HUDLess");
    desc.UI = GetResource(params, "DLSSG.UI");
    // the frame without its UI, and the UI on top, where the game gives both; else the frame as presented, with the
    // UI it has in it where the game gives that
    desc.Color = hudless && desc.UI ? hudless : backbuffer;
    desc.UIComposited = desc.Color == backbuffer;
    desc.Depth = GetResource(params, "DLSSG.Depth");
    desc.MotionVector = GetResource(params, "DLSSG.MVecs");
    desc.Output = GetResource(params, "DLSSG.OutputInterpolated");
    if (!desc.Color || !desc.Depth || !desc.MotionVector || !desc.Output) {
      ERR("NVSDK_NGX_D3D12_EvaluateFeature: Frame Generation without ", !desc.Color ? "color " : "",
          !desc.Depth ? "depth " : "", !desc.MotionVector ? "motion vectors " : "",
          !desc.Output ? "an output" : "");
      result = NVNGX_RESULT_INVALID_PARAMETER;
    } else {
      // motion vectors Streamline's scale takes to the screen's [-1, 1]: in pixels of their own resource
      auto motion = desc.MotionVector->GetDesc();
      unsigned motion_width = GetOr(params, "DLSSG.MVecsSubrectWidth", 0u),
               motion_height = GetOr(params, "DLSSG.MVecsSubrectHeight", 0u);
      if (!motion_width || motion_width > motion.Width)
        motion_width = unsigned(motion.Width);
      if (!motion_height || motion_height > motion.Height)
        motion_height = motion.Height;
      desc.MotionVectorScaleX = GetOr(params, "DLSSG.MvecScaleX", 1.0f) * motion_width;
      desc.MotionVectorScaleY = GetOr(params, "DLSSG.MvecScaleY", 1.0f) * motion_height;
      desc.JitterOffsetX = GetOr(params, "DLSSG.JitterOffsetX", 0.0f);
      desc.JitterOffsetY = GetOr(params, "DLSSG.JitterOffsetY", 0.0f);
      desc.DepthReversed = GetOr(params, "DLSSG.DepthInverted", 0u) != 0;
      desc.InReset = GetOr(params, "DLSSG.Reset", 0u) != 0 || !dlssg->last_counter;
      ReadCamera(params, desc);
      auto output = desc.Output->GetDesc();
      desc.AspectRatio = GetOr(params, "DLSSG.CameraAspectRatio", 0.0f);
      if (!(desc.AspectRatio > 0.0f))
        desc.AspectRatio = float(output.Width) / float(std::max(output.Height, 1u));
      LARGE_INTEGER now, frequency;
      QueryPerformanceCounter(&now);
      QueryPerformanceFrequency(&frequency);
      desc.DeltaTime = dlssg->last_counter ? float(now.QuadPart - dlssg->last_counter) / float(frequency.QuadPart)
                                           : 1.0f / 60.0f;
      if (!(desc.DeltaTime > 0.0f) || desc.DeltaTime > 0.25f)
        desc.DeltaTime = 1.0f / 60.0f;
      dlssg->last_counter = now.QuadPart;
      if (FAILED(ext->FrameInterpolate(&desc, &dlssg->interpolator)))
        result = NVNGX_RESULT_FAIL;
    }
  }
  // the real frame, which Streamline presents after the interpolated one
  if (backbuffer && real && backbuffer != real)
    list->CopyResource(real, backbuffer);
  if (!recording)
    list->Close();
  return result;
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
  case NVNGX_FEATURE_FRAMEGENERATION:
    return EvaluateFrameGeneration(list, ext.ptr(), static_cast<DLSSGFeature *>(common), parameters);
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
  case NVNGX_FEATURE_FRAMEGENERATION: {
    auto dlssg = static_cast<DLSSGFeature *>(common);
    if (dlssg->interpolator)
      dlssg->interpolator->Destroy();
    delete dlssg;
    break;
  }
  default:
    break;
  }
  return NVNGX_RESULT_OK;
}

} // namespace mullion
