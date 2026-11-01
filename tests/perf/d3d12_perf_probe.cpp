// D3D12 performance probe: one small scene per CPU or GPU cost of a D3D12 implementation on Metal
// (Mullion's, or another's for comparison), for before/after comparisons. tests/perf/perf_probe.py
// runs every scene in its own process and compares runs. Not part of the functional tests.
//
//   d3d12_perf_probe.exe SCENE [frames] [warmup] [salt]
//   d3d12_perf_probe.exe list
//
// Each frame renders into the backbuffers of a 1920x1080 flip-model swap chain created like Unity's
// (frame latency waitable, maximum latency 2), three frames in flight. Only the present scene
// presents: presentation is paced by the window server (an unfocused window is composited at the
// display's refresh rate), which would hide every other cost. Prints, for the frames after the
// warm-up: cpu (time between frames), work (CPU time from the frame's start through its
// submission), and gpu (submit-to-completion time of serialized frames run afterwards): mean,
// median, 95th percentile and max; the first frame's CPU time, any scene-specific values
// ("extra name=value"), then D3D12_PERF_PROBE_OK SCENE.
// Shaders are DXBC (d3dcompiler_47), as most D3D12 games ship theirs, except the [dxil] scenes'; the salt
// makes compiles cold.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <dxcapi.h>
#include "../dxbc_hash.hpp"
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <algorithm>
#include <functional>
#include <thread>
#include <mutex>
#include <condition_variable>

#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { printf("error: %s:%d: %s = 0x%08lx\n", __FILE__, __LINE__, #x, (unsigned long)hr_); exit(3); } } while (0)

static const UINT W = 1920, H = 1080, FRAMES_IN_FLIGHT = 3, HEAP_SIZE = 16384;
static const DXGI_FORMAT BACKBUFFER_FORMAT = DXGI_FORMAT_R8G8B8A8_UNORM;
static const DXGI_FORMAT HDR_FORMAT = DXGI_FORMAT_R16G16B16A16_FLOAT;

static double qpc_ms()
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return 1000.0 * (double)t.QuadPart / (double)freq.QuadPart;
}

// ---- device, swap chain and per-frame state ----

struct Probe
{
    ID3D12Device *device = nullptr;
    ID3D12CommandQueue *queue = nullptr;
    IDXGISwapChain3 *swapchain = nullptr;
    HANDLE waitable = nullptr;
    ID3D12CommandAllocator *allocators[FRAMES_IN_FLIGHT] = {};
    ID3D12GraphicsCommandList *list = nullptr;
    ID3D12Fence *fence = nullptr;
    UINT64 fence_value = 0, frame_fence[FRAMES_IN_FLIGHT] = {};
    HANDLE event = nullptr;
    ID3D12Resource *backbuffers[FRAMES_IN_FLIGHT] = {};
    ID3D12DescriptorHeap *rtv_heap = nullptr, *dsv_heap = nullptr, *srv_heap = nullptr, *cpu_srv_heap = nullptr;
    UINT rtv_size = 0, srv_size = 0;
    ID3D12QueryHeap *timestamps = nullptr;
    ID3D12Resource *timestamp_readback = nullptr;
    UINT64 timestamp_frequency = 0;
    ID3D12RootSignature *root_signature = nullptr, *compute_root_signature = nullptr;
    // Shared resources most scenes use.
    ID3D12Resource *upload_ring = nullptr;
    uint8_t *upload_ptr = nullptr;
    UINT64 upload_size = 64ull << 20, upload_offset = 0;
    ID3D12Resource *hdr_targets[3] = {}, *depth = nullptr, *textures[8] = {};
    ID3D12Resource *uav_buffer = nullptr;
    unsigned salt = 0;
    UINT frame_index = 0;
};
static Probe p;
static UINT g_frames_total;   // warm-up + timed frames of this run (scenes that pre-build per-frame variants)

static D3D12_CPU_DESCRIPTOR_HANDLE rtv(UINT i) { auto h = p.rtv_heap->GetCPUDescriptorHandleForHeapStart(); h.ptr += (SIZE_T)i * p.rtv_size; return h; }
static D3D12_CPU_DESCRIPTOR_HANDLE dsv(UINT i = 0) { auto h = p.dsv_heap->GetCPUDescriptorHandleForHeapStart(); h.ptr += (SIZE_T)i * p.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV); return h; }
static D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu(UINT i) { auto h = p.srv_heap->GetCPUDescriptorHandleForHeapStart(); h.ptr += (SIZE_T)i * p.srv_size; return h; }
static D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu(UINT i) { auto h = p.srv_heap->GetGPUDescriptorHandleForHeapStart(); h.ptr += (UINT64)i * p.srv_size; return h; }
static D3D12_CPU_DESCRIPTOR_HANDLE staging_cpu(UINT i) { auto h = p.cpu_srv_heap->GetCPUDescriptorHandleForHeapStart(); h.ptr += (SIZE_T)i * p.srv_size; return h; }

static ID3D12Resource *create_buffer(UINT64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE,
                                     D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON)
{
    D3D12_HEAP_PROPERTIES hp = { heap };
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size;
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = flags;
    if (heap == D3D12_HEAP_TYPE_UPLOAD)
        state = D3D12_RESOURCE_STATE_GENERIC_READ;
    else if (heap == D3D12_HEAP_TYPE_READBACK)
        state = D3D12_RESOURCE_STATE_COPY_DEST;
    ID3D12Resource *r = nullptr;
    CHECK(p.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r)));
    return r;
}

static ID3D12Resource *create_texture(UINT w, UINT h, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state,
                                      UINT mips = 1, UINT samples = 1, const D3D12_CLEAR_VALUE *clear = nullptr)
{
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w;
    rd.Height = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = mips;
    rd.Format = format;
    rd.SampleDesc.Count = samples;
    rd.Flags = flags;
    ID3D12Resource *r = nullptr;
    CHECK(p.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, clear, IID_PPV_ARGS(&r)));
    return r;
}

// Bump allocation in the persistently mapped upload ring (wraps; the ring is far larger than a
// frame's use, and a frame is complete before its slot comes around again).
static D3D12_GPU_VIRTUAL_ADDRESS upload_alloc(UINT64 size, void **cpu)
{
    size = (size + 255) & ~255ull;
    if (p.upload_offset + size > p.upload_size)
        p.upload_offset = 0;
    *cpu = p.upload_ptr + p.upload_offset;
    D3D12_GPU_VIRTUAL_ADDRESS va = p.upload_ring->GetGPUVirtualAddress() + p.upload_offset;
    p.upload_offset += size;
    return va;
}

static void barrier(ID3D12GraphicsCommandList *l, ID3D12Resource *r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    l->ResourceBarrier(1, &b);
}

static void wait_fence(UINT64 value)
{
    if (p.fence->GetCompletedValue() >= value)
        return;
    CHECK(p.fence->SetEventOnCompletion(value, p.event));
    WaitForSingleObject(p.event, INFINITE);
}

static UINT64 signal_queue()
{
    CHECK(p.queue->Signal(p.fence, ++p.fence_value));
    return p.fence_value;
}

// ---- DXIL (Shader Model 6) ----
// A probe whose Wine has no dxcompiler.dll of its machine (an x86_64 probe in an arm64 Wine that
// emulates x86_64) takes DXIL from a cache (PROBE_DXIL_CACHE=<dir>) that a probe of the Wine's own
// machine fills: perf_probe.py runs it once per DXIL scene first (`prepare`), with DXCOMPILER_PATH
// set when given. Keyed by a hash of source, entry, target and arguments.

static std::string dxil_cache_path(const std::string &source, const char *entry, const char *target, const std::vector<std::wstring> &args)
{
    char dir[MAX_PATH];
    if (!GetEnvironmentVariableA("PROBE_DXIL_CACHE", dir, sizeof(dir)))
        return {};
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](const void *ptr, size_t n) { for (size_t i = 0; i < n; i++) h = (h ^ ((const unsigned char *)ptr)[i]) * 1099511628211ull; };
    mix(source.data(), source.size()); mix(entry, strlen(entry)); mix(target, strlen(target));
    for (auto &a : args) mix(a.data(), a.size() * sizeof(wchar_t));
    char name[64];
    snprintf(name, sizeof(name), "\\%016llx.dxil", (unsigned long long)h);
    return std::string(dir) + name;
}

static std::vector<char> compile_dxil(const std::string &source, const char *entry, const char *target, std::vector<std::wstring> args = {})
{
    std::string path = dxil_cache_path(source, entry, target, args);
    if (!path.empty())
        if (FILE *f = fopen(path.c_str(), "rb"))
        {
            std::vector<char> out;
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)))
                out.insert(out.end(), buf, buf + n);
            fclose(f);
            return out;
        }
    static DxcCreateInstanceProc create;
    if (!create)
    {
        char dll[MAX_PATH];
        HMODULE m = GetEnvironmentVariableA("DXCOMPILER_PATH", dll, sizeof(dll)) ? LoadLibraryA(dll) : LoadLibraryA("dxcompiler.dll");
        if (!m || !(create = (DxcCreateInstanceProc)(void *)GetProcAddress(m, "DxcCreateInstance")))
        {
            printf("error: DXIL shader %s %s not in the cache and dxcompiler.dll unavailable (perf_probe.py --prepare-exe fills the cache)\n", entry, target);
            exit(3);
        }
    }
    IDxcCompiler3 *compiler = nullptr;
    CHECK(create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)));
    wchar_t wentry[64], wtarget[32];
    MultiByteToWideChar(CP_UTF8, 0, entry, -1, wentry, 64);
    MultiByteToWideChar(CP_UTF8, 0, target, -1, wtarget, 32);
    std::vector<LPCWSTR> argv = { L"-T", wtarget, L"-HV", L"2021", L"-O3" };
    if (*entry)
        argv.insert(argv.begin(), { L"-E", wentry });
    for (auto &a : args)
        argv.push_back(a.c_str());
    DxcBuffer buffer = { source.data(), source.size(), DXC_CP_UTF8 };
    IDxcResult *result = nullptr;
    std::vector<char> out;
    CHECK(compiler->Compile(&buffer, argv.data(), (UINT32)argv.size(), nullptr, IID_PPV_ARGS(&result)));
    HRESULT status;
    result->GetStatus(&status);
    IDxcBlobUtf8 *errors = nullptr;
    result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
    if (FAILED(status))
    {
        printf("error: dxc %s %s: %s\n", entry, target, errors ? errors->GetStringPointer() : "?");
        exit(3);
    }
    if (errors)
        errors->Release();
    IDxcBlob *object = nullptr;
    CHECK(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr));
    out.assign((char *)object->GetBufferPointer(), (char *)object->GetBufferPointer() + object->GetBufferSize());
    // DXC signs with a library the probe does not carry and leaves the hash zero, which Direct3D takes only as a
    // preview's shader (INF-0004): the probe signs it, as the tests do
    dxbc::sign(out);
    object->Release();
    result->Release();
    compiler->Release();
    if (!path.empty())
        if (FILE *f = fopen(path.c_str(), "wb"))
        {
            fwrite(out.data(), 1, out.size(), f);
            fclose(f);
        }
    return out;
}

// One shader as bytecode: DXBC through d3dcompiler_47 (SM 5.x targets) or DXIL (SM 6.x targets).
static std::vector<char> shader_code(const std::string &source, const char *entry, const char *target, std::vector<std::wstring> args = {})
{
    if (target[3] == '6' || (target[0] == 'l' && target[4] == '6'))
        return compile_dxil(source, entry, target, args);
    ID3DBlob *code = nullptr, *errors = nullptr;
    if (FAILED(D3DCompile(source.data(), source.size(), nullptr, nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors)))
    {
        printf("error: compile %s %s: %s\n", entry, target, errors ? (const char *)errors->GetBufferPointer() : "?");
        exit(3);
    }
    std::vector<char> out((char *)code->GetBufferPointer(), (char *)code->GetBufferPointer() + code->GetBufferSize());
    code->Release();
    if (errors) errors->Release();
    return out;
}

// Objects the GPU may still use: released once their frame's slot comes around again.
static std::vector<std::pair<UINT64, IUnknown *>> g_deferred;
static void deferred_release(IUnknown *o) { g_deferred.emplace_back(p.fence_value + FRAMES_IN_FLIGHT + 1, o); }
static void release_completed()
{
    UINT64 done = p.fence->GetCompletedValue();
    auto it = std::remove_if(g_deferred.begin(), g_deferred.end(), [done](auto &e) { if (e.first > done) return false; e.second->Release(); return true; });
    g_deferred.erase(it, g_deferred.end());
}

// ---- shaders and pipelines ----

static ID3DBlob *compile(const std::string &src, const char *entry, const char *target)
{
    ID3DBlob *code = nullptr, *errors = nullptr;
    HRESULT hr = D3DCompile(src.data(), src.size(), nullptr, nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr))
    {
        printf("error: compile %s: %s\n", entry, errors ? (const char *)errors->GetBufferPointer() : "?");
        exit(3);
    }
    if (errors)
        errors->Release();
    return code;
}

// b0: 4 root constants (offset.xy, scale, value), b1: root CBV, t0-t7: descriptor table, s0: static sampler.
static const char *common_hlsl = R"(
#ifndef SALT_VALUE
#define SALT_VALUE 0.0
#endif
cbuffer RC : register(b0) { float4 rc; };
cbuffer CB : register(b1) { float4 cb[4]; };
Texture2D tex[8] : register(t0);
SamplerState smp : register(s0);
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs_quad(uint id : SV_VertexID)
{
    V o;
    float2 q = float2((id << 1) & 2, id & 2);
    o.uv = q;
    o.pos = float4((q * 2 - 1) * rc.z + rc.xy, 0.5, 1) + cb[0] * 0.0001 + SALT_VALUE * 1e-7;
    return o;
}
float4 ps_flat(V i) : SV_Target { return float4(rc.w, i.uv, 1) + cb[1] * 0.25 + SALT_VALUE * 1e-7; }
float4 ps_heavy(V i) : SV_Target
{
    float3 c = float3(i.uv, rc.w);
    [loop] for (int k = 0; k < 24; k++)
        c = frac(c * 1.37 + sin(c.zxy * 4.1 + k) * 0.5);
    return float4(c, 1) + SALT_VALUE * 1e-7;
}
float4 ps_tex(V i) : SV_Target
{
    float4 c = 0;
    [unroll] for (int k = 0; k < 8; k++)
        c += tex[k].Sample(smp, i.uv * (1 + k * 0.13));
    return c * 0.125 + SALT_VALUE * 1e-7;
}
struct VI { float3 pos : POSITION; float2 uv : TEXCOORD0; };
V vs_input(VI v) { V o; o.pos = float4(v.pos.xy * rc.z + rc.xy, 0.5, 1); o.uv = v.uv; return o; }
// Alpha test, as in Unity's cutout shaders (foliage, fences): discard defeats hidden-surface removal.
float4 ps_alpha(V i) : SV_Target
{
    float4 c = tex[0].Sample(smp, i.uv * 3);
    clip(c.g - 0.5);
    return c * rc.w;
}
// Post-processing style: a few taps of the previous pass.
float4 ps_post(V i) : SV_Target
{
    float2 d = float2(1.0 / 1920, 1.0 / 1080);
    return (tex[0].Sample(smp, i.uv * 0.5) + tex[0].Sample(smp, i.uv * 0.5 + d) +
            tex[0].Sample(smp, i.uv * 0.5 - d) + tex[0].Sample(smp, i.uv * 0.5 + d.yx)) * 0.25 + rc.w * 0.01;
}
float4 ps_sample1(V i) : SV_Target { return tex[0].Sample(smp, i.uv * 3) * rc.w; }
// UI: textured, blended.
float4 ps_ui(V i) : SV_Target { float4 c = tex[0].Sample(smp, i.uv); c.a = 0.5; return c; }
// Instances spread over the screen.
V vs_instanced(uint id : SV_VertexID, uint inst : SV_InstanceID)
{
    V o;
    float2 q = float2((id << 1) & 2, id & 2);
    float2 at = float2(inst % 400, inst / 400 % 400) / 200.0 - 1;
    o.uv = q;
    o.pos = float4(at + q * 0.004, 0.5, 1);
    return o;
}
// Dense geometry: a grid of tiny triangles from the vertex index alone (vertex-bound).
V vs_dense(uint id : SV_VertexID)
{
    V o;
    uint tri = id / 3, corner = id % 3;
    float2 at = float2(tri % 1024, tri / 1024 % 1024) / 512.0 - 1;
    float2 q = float2(corner == 1, corner == 2) * (1.0 / 512);
    o.uv = q;
    o.pos = float4(at + q + sin(at.yx * 7 + rc.w) * 0.001, 0.5, 1);
    return o;
}
)";

// SM 5.1 dynamic indexing into a large table (bindless-style materials).
static const char *bindless_hlsl = R"(
cbuffer RC : register(b0) { float4 rc; uint4 idx; };
Texture2D tex[1000] : register(t0, space1);
SamplerState smp : register(s0);
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; nointerpolation uint index : INDEX; };
V vs_quad(uint id : SV_VertexID)
{
    V o;
    float2 q = float2((id << 1) & 2, id & 2);
    o.uv = q;
    o.pos = float4((q * 2 - 1) * rc.z + rc.xy, 0.5, 1);
    o.index = idx.x;
    return o;
}
float4 ps_bindless(V i) : SV_Target { return tex[NonUniformResourceIndex(i.index)].Sample(smp, i.uv); }
)";

// Compute post-processing writing a 1080p texture.
static const char *compute_image_hlsl = R"(
cbuffer RC : register(b0) { float4 rc; };
RWTexture2D<float4> img : register(u0);
Texture2D<float4> src : register(t0);
[numthreads(8, 8, 1)] void cs_blur(uint3 t : SV_DispatchThreadID)
{
    float4 c = src.Load(int3(t.xy, 0)) + src.Load(int3(t.xy + uint2(1, 0), 0)) + src.Load(int3(t.xy + uint2(0, 1), 0));
    img[t.xy] = c * 0.333 + rc;
}
)";

static const char *compute_hlsl = R"(
cbuffer RC : register(b0) { uint4 rc; };
RWStructuredBuffer<uint> u : register(u0);
[numthreads(64, 1, 1)] void cs_add(uint t : SV_DispatchThreadID) { u[t & 4095] += rc.x + t; }
)";

static std::string salted(const char *src, unsigned salt)
{
    // A different constant per salt gives a different DXBC hash, so nothing below can reuse a compile.
    // The shaders add SALT_VALUE * 1e-7 to their output, so the constant survives compilation.
    char buf[96];
    snprintf(buf, sizeof(buf), "#define SALT_VALUE %u.0\n", salt);
    return std::string(buf) + src;
}

struct PsoDesc
{
    const char *vs = "vs_quad", *ps = "ps_flat";
    const char *hlsl = nullptr;              // shader source (default common_hlsl)
    const char *ps_target = "ps_5_0", *vs_target = "vs_5_0";
    DXGI_FORMAT rt = BACKBUFFER_FORMAT, dsv = DXGI_FORMAT_D32_FLOAT;
    UINT num_rt = 1;                         // 0: depth-only
    bool depth = false, depth_write = true, blend = false, input = false;
    bool stencil_write = false, stencil_test = false;
    UINT samples = 1;
    unsigned salt = 0;
    ID3D12RootSignature *rs = nullptr;      // default p.root_signature
    const wchar_t *dxc_arg = nullptr;       // extra DXC argument for SM 6 targets (e.g. -enable-16bit-types)
    const char *gs = nullptr, *hs = nullptr, *ds = nullptr;   // other stages, compiled with vs_target's model
    std::function<void(D3D12_GRAPHICS_PIPELINE_STATE_DESC &)> tweak;   // last adjustments before creation
};

static ID3D12PipelineState *create_pso(const PsoDesc &d, double *compile_ms = nullptr, double *create_ms = nullptr)
{
    double t0 = qpc_ms();
    const char *hlsl = d.hlsl ? d.hlsl : common_hlsl;
    std::string src = d.salt ? salted(hlsl, d.salt) : std::string(hlsl);
    std::vector<std::wstring> args;
    if (d.dxc_arg) args.push_back(d.dxc_arg);
    std::vector<char> vs = shader_code(src, d.vs, d.vs_target, args), ps = d.ps ? shader_code(src, d.ps, d.ps_target, args) : std::vector<char>();
    auto stage = [&](const char *entry, const char *prefix) {
        std::string target = std::string(prefix) + (d.vs_target + 2);
        return entry ? shader_code(src, entry, target.c_str(), args) : std::vector<char>();
    };
    std::vector<char> gs = stage(d.gs, "gs"), hs = stage(d.hs, "hs"), ds = stage(d.ds, "ds");
    double t1 = qpc_ms();
    D3D12_GRAPHICS_PIPELINE_STATE_DESC g = {};
    g.pRootSignature = d.rs ? d.rs : p.root_signature;
    g.VS = { vs.data(), vs.size() };
    if (!ps.empty())
        g.PS = { ps.data(), ps.size() };
    if (!gs.empty()) g.GS = { gs.data(), gs.size() };
    if (!hs.empty()) g.HS = { hs.data(), hs.size() };
    if (!ds.empty()) g.DS = { ds.data(), ds.size() };
    g.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    if (d.blend)
    {
        auto &b = g.BlendState.RenderTarget[0];
        b.BlendEnable = TRUE;
        b.SrcBlend = D3D12_BLEND_SRC_ALPHA; b.DestBlend = D3D12_BLEND_INV_SRC_ALPHA; b.BlendOp = D3D12_BLEND_OP_ADD;
        b.SrcBlendAlpha = D3D12_BLEND_ONE; b.DestBlendAlpha = D3D12_BLEND_ZERO; b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }
    g.SampleMask = UINT_MAX;
    g.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    g.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    g.RasterizerState.DepthClipEnable = TRUE;
    if (d.depth || d.stencil_write || d.stencil_test)
    {
        g.DepthStencilState.DepthEnable = d.depth;
        g.DepthStencilState.DepthWriteMask = d.depth_write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        g.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        g.DSVFormat = d.dsv;
        if (d.stencil_write || d.stencil_test)
        {
            auto &f = g.DepthStencilState.FrontFace;
            g.DepthStencilState.StencilEnable = TRUE;
            g.DepthStencilState.StencilReadMask = g.DepthStencilState.StencilWriteMask = 0xff;
            f.StencilFailOp = f.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
            f.StencilPassOp = d.stencil_write ? D3D12_STENCIL_OP_REPLACE : D3D12_STENCIL_OP_KEEP;
            f.StencilFunc = d.stencil_test ? D3D12_COMPARISON_FUNC_EQUAL : D3D12_COMPARISON_FUNC_ALWAYS;
            g.DepthStencilState.BackFace = f;
        }
    }
    static const D3D12_INPUT_ELEMENT_DESC elements[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
    if (d.input)
        g.InputLayout = { elements, 2 };
    g.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    g.NumRenderTargets = d.num_rt;
    if (d.num_rt)
        g.RTVFormats[0] = d.rt;
    g.SampleDesc.Count = d.samples;
    if (d.hs)
        g.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    if (d.tweak)
        d.tweak(g);
    ID3D12PipelineState *pso = nullptr;
    CHECK(p.device->CreateGraphicsPipelineState(&g, IID_PPV_ARGS(&pso)));
    double t2 = qpc_ms();
    if (compile_ms) *compile_ms += t1 - t0;
    if (create_ms) *create_ms += t2 - t1;
    return pso;
}

static ID3D12PipelineState *create_compute_pso(unsigned salt = 0)
{
    std::string src = salt ? salted(compute_hlsl, salt) : std::string(compute_hlsl);
    ID3DBlob *cs = compile(src, "cs_add", "cs_5_0");
    D3D12_COMPUTE_PIPELINE_STATE_DESC c = {};
    c.pRootSignature = p.compute_root_signature;
    c.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
    ID3D12PipelineState *pso = nullptr;
    CHECK(p.device->CreateComputePipelineState(&c, IID_PPV_ARGS(&pso)));
    cs->Release();
    return pso;
}

static ID3D12RootSignature *serialize_root_signature(const D3D12_ROOT_SIGNATURE_DESC &desc)
{
    ID3DBlob *blob = nullptr, *errors = nullptr;
    CHECK(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors));
    ID3D12RootSignature *rs = nullptr;
    CHECK(p.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs)));
    blob->Release();
    return rs;
}

// ---- setup ----

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

static void setup()
{
    WNDCLASSA wc = {};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "d3d12_perf_probe";
    RegisterClassA(&wc);
    RECT r = { 0, 0, (LONG)W, (LONG)H };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowA("d3d12_perf_probe", "d3d12_perf_probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0,
                              r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);

    IDXGIFactory4 *factory = nullptr;
    CHECK(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
    CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&p.device)));
    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    CHECK(p.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&p.queue)));

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = W;
    sd.Height = H;
    sd.Format = BACKBUFFER_FORMAT;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = FRAMES_IN_FLIGHT;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    IDXGISwapChain1 *sc1 = nullptr;
    CHECK(factory->CreateSwapChainForHwnd(p.queue, hwnd, &sd, nullptr, nullptr, &sc1));
    CHECK(sc1->QueryInterface(IID_PPV_ARGS(&p.swapchain)));
    sc1->Release();
    CHECK(p.swapchain->SetMaximumFrameLatency(2));
    p.waitable = p.swapchain->GetFrameLatencyWaitableObject();

    for (UINT i = 0; i < FRAMES_IN_FLIGHT; i++)
    {
        CHECK(p.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&p.allocators[i])));
        CHECK(p.swapchain->GetBuffer(i, IID_PPV_ARGS(&p.backbuffers[i])));
    }
    CHECK(p.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, p.allocators[0], nullptr, IID_PPV_ARGS(&p.list)));
    CHECK(p.list->Close());
    CHECK(p.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&p.fence)));
    p.event = CreateEventA(nullptr, FALSE, FALSE, nullptr);

    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = FRAMES_IN_FLIGHT + 16;
    CHECK(p.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&p.rtv_heap)));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    hd.NumDescriptors = 4;
    CHECK(p.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&p.dsv_heap)));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = HEAP_SIZE;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    CHECK(p.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&p.srv_heap)));
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    CHECK(p.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&p.cpu_srv_heap)));
    p.rtv_size = p.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    p.srv_size = p.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (UINT i = 0; i < FRAMES_IN_FLIGHT; i++)
        p.device->CreateRenderTargetView(p.backbuffers[i], nullptr, rtv(i));

    D3D12_QUERY_HEAP_DESC qh = {};
    qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qh.Count = 2 * FRAMES_IN_FLIGHT;
    CHECK(p.device->CreateQueryHeap(&qh, IID_PPV_ARGS(&p.timestamps)));
    p.timestamp_readback = create_buffer(16 * FRAMES_IN_FLIGHT, D3D12_HEAP_TYPE_READBACK);
    CHECK(p.queue->GetTimestampFrequency(&p.timestamp_frequency));

    // Graphics root signature: root constants b0, root CBV b1, SRV table t0-t7, static sampler s0.
    D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 8, 0, 0, 0 };
    D3D12_ROOT_PARAMETER rp[3] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants = { 0, 0, 4 };
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rp[1].Descriptor = { 1, 0 };
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[2].DescriptorTable = { 1, &range };
    D3D12_STATIC_SAMPLER_DESC ss = {};
    ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    ss.MaxLOD = D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 3, rp, 1, &ss, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
    p.root_signature = serialize_root_signature(rsd);
    // Compute: root constants b0, root UAV u0.
    D3D12_ROOT_PARAMETER cp[2] = {};
    cp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    cp[0].Constants = { 0, 0, 4 };
    cp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    cp[1].Descriptor = { 0, 0 };
    D3D12_ROOT_SIGNATURE_DESC crsd = { 2, cp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    p.compute_root_signature = serialize_root_signature(crsd);

    p.upload_ring = create_buffer(p.upload_size, D3D12_HEAP_TYPE_UPLOAD);
    CHECK(p.upload_ring->Map(0, nullptr, (void **)&p.upload_ptr));

    D3D12_CLEAR_VALUE cv = { HDR_FORMAT, { 0, 0, 0, 0 } };
    for (int i = 0; i < 3; i++)
    {
        p.hdr_targets[i] = create_texture(W, H, HDR_FORMAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, 1, 1, &cv);
        p.device->CreateRenderTargetView(p.hdr_targets[i], nullptr, rtv(FRAMES_IN_FLIGHT + i));
    }
    D3D12_CLEAR_VALUE dv = { DXGI_FORMAT_D32_FLOAT };
    dv.DepthStencil.Depth = 1.0f;
    p.depth = create_texture(W, H, DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                             D3D12_RESOURCE_STATE_DEPTH_WRITE, 1, 1, &dv);
    p.device->CreateDepthStencilView(p.depth, nullptr, dsv());

    // Eight 512x512 RGBA8 textures with mips, filled once, SRVs in heap slots 0-7.
    std::vector<uint32_t> texels(512 * 512);
    for (UINT i = 0; i < texels.size(); i++)
        texels[i] = 0xff000000u | (i * 2654435761u >> 8);
    ID3D12CommandAllocator *a = p.allocators[0];
    CHECK(a->Reset());
    CHECK(p.list->Reset(a, nullptr));
    for (int t = 0; t < 8; t++)
    {
        p.textures[t] = create_texture(512, 512, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, 1);
        void *cpu;
        D3D12_GPU_VIRTUAL_ADDRESS va = upload_alloc(512 * 512 * 4, &cpu);
        memcpy(cpu, texels.data(), 512 * 512 * 4);
        D3D12_TEXTURE_COPY_LOCATION dst = { p.textures[t], D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
        D3D12_TEXTURE_COPY_LOCATION src = { p.upload_ring, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
        src.PlacedFootprint.Offset = va - p.upload_ring->GetGPUVirtualAddress();
        src.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, 512, 512, 1, 512 * 4 };
        p.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        barrier(p.list, p.textures[t], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        p.device->CreateShaderResourceView(p.textures[t], nullptr, srv_cpu(t));
    }
    CHECK(p.list->Close());
    ID3D12CommandList *lists[] = { p.list };
    p.queue->ExecuteCommandLists(1, lists);
    wait_fence(signal_queue());
    p.uav_buffer = create_buffer(4096 * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    factory->Release();
}

// ---- scenes ----
// init() runs once before the first frame; frame() records into p.list between the frame's
// begin (backbuffer bound as render target, cleared, viewport set) and end, and may submit
// work of its own. Extras accumulate per-operation timings.

struct Extras
{
    std::vector<std::pair<std::string, double>> values;
    bool frozen = false;
    void add(const char *name, double v)
    {
        if (frozen) return;
        for (auto &e : values)
            if (e.first == name) { e.second += v; return; }
        values.emplace_back(name, v);
    }
};
static Extras extras;

struct Scene
{
    const char *name, *desc;
    std::function<void()> init;
    std::function<void(UINT frame)> frame;
    bool present = false;   // Only the present scenes pace by the swap chain; the rest run offscreen.
    UINT sync_interval = 0;
    bool dxil = false;      // Needs SM 6 shaders: the runner fills the DXIL cache with the aarch64 probe first.
};

static void set_quad_state(ID3D12GraphicsCommandList *l, ID3D12PipelineState *pso)
{
    l->SetGraphicsRootSignature(p.root_signature);
    l->SetPipelineState(pso);
    l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    void *cpu;
    D3D12_GPU_VIRTUAL_ADDRESS cb = upload_alloc(256, &cpu);
    memset(cpu, 0, 256);
    l->SetGraphicsRootConstantBufferView(1, cb);
    l->SetGraphicsRootDescriptorTable(2, srv_gpu(0));
}

static void viewport(ID3D12GraphicsCommandList *l, UINT w = W, UINT h = H)
{
    D3D12_VIEWPORT vp = { 0, 0, (float)w, (float)h, 0, 1 };
    D3D12_RECT sc = { 0, 0, (LONG)w, (LONG)h };
    l->RSSetViewports(1, &vp);
    l->RSSetScissorRects(1, &sc);
}

static void small_quad(ID3D12GraphicsCommandList *l, UINT i, float value)
{
    float rc[4] = { (float)(i % 64) / 32.0f - 1.0f, (float)(i / 64 % 64) / 32.0f - 1.0f, 0.01f, value };
    l->SetGraphicsRoot32BitConstants(0, 4, rc, 0);
    l->DrawInstanced(3, 1, 0, 0);
}

static void submit_and_wait(ID3D12GraphicsCommandList *l)
{
    CHECK(l->Close());
    ID3D12CommandList *lists[] = { l };
    p.queue->ExecuteCommandLists(1, lists);
    wait_fence(signal_queue());
}

static std::vector<Scene> base_scenes()
{
    static ID3D12PipelineState *flat, *heavy, *texd, *input, *flat_depth, *flat_hdr, *flat_msaa, *compute, *psos[16];
    static ID3D12Resource *vb, *ib, *buffers_default, *upload_texture_dst, *readback, *msaa_target, *resolve_target;
    static ID3D12Heap *placed_heap;
    static ID3D12CommandAllocator *extra_allocs[40 * FRAMES_IN_FLIGHT];
    static ID3D12GraphicsCommandList *extra_lists[64];
    static ID3D12QueryHeap *ts_heap, *occlusion_heap;
    static ID3D12CommandSignature *draw_signature;
    static ID3D12Resource *indirect_args;

    auto base = [] { flat = create_pso({}); };
    return {
        { "empty", "clear only: the fixed cost of a frame (offscreen)", base, [](UINT) {} },
        { "present", "clear + Present(0) through the frame latency waitable, like a game's frame", base, [](UINT) {}, true },
        { "draws", "5000 draws, root constants per draw", base, [](UINT f) {
            set_quad_state(p.list, flat);
            double t = qpc_ms();
            for (UINT i = 0; i < 5000; i++) small_quad(p.list, i, f * 0.01f);
            extras.add("record_us_per_draw", (qpc_ms() - t) * 1000.0 / 5000);
        } },
        { "draws_rootcbv", "5000 draws, a fresh root CBV per draw (Unity's pattern)", base, [](UINT f) {
            set_quad_state(p.list, flat);
            double t = qpc_ms();
            for (UINT i = 0; i < 5000; i++)
            {
                void *cpu;
                D3D12_GPU_VIRTUAL_ADDRESS cb = upload_alloc(64, &cpu);
                ((float *)cpu)[4] = (float)i;
                p.list->SetGraphicsRootConstantBufferView(1, cb);
                small_quad(p.list, i, f * 0.01f);
            }
            extras.add("record_us_per_draw", (qpc_ms() - t) * 1000.0 / 5000);
        } },
        { "draws_tables", "5000 draws, descriptor table switch per draw", [] {
            flat = create_pso({});
            for (UINT i = 8; i < 8 + 1024; i++) p.device->CreateShaderResourceView(p.textures[i % 8], nullptr, srv_cpu(i));
        }, [](UINT f) {
            set_quad_state(p.list, flat);
            double t = qpc_ms();
            for (UINT i = 0; i < 5000; i++)
            {
                p.list->SetGraphicsRootDescriptorTable(2, srv_gpu(8 + (i % 1000)));
                small_quad(p.list, i, f * 0.01f);
            }
            extras.add("record_us_per_draw", (qpc_ms() - t) * 1000.0 / 5000);
        } },
        { "draws_pso", "4000 draws over 16 pipelines, a pipeline change per draw", [] {
            for (unsigned i = 0; i < 16; i++) { PsoDesc d; d.blend = i & 1; d.salt = 1000 + i; psos[i] = create_pso(d); }
        }, [](UINT f) {
            set_quad_state(p.list, psos[0]);
            double t = qpc_ms();
            for (UINT i = 0; i < 4000; i++) { p.list->SetPipelineState(psos[i % 16]); small_quad(p.list, i, f * 0.01f); }
            extras.add("record_us_per_draw", (qpc_ms() - t) * 1000.0 / 4000);
        } },
        { "draws_vbib", "5000 indexed draws, vertex and index buffer binds per draw", [] {
            PsoDesc d; d.vs = "vs_input"; d.input = true; input = create_pso(d);
            vb = create_buffer(1 << 20, D3D12_HEAP_TYPE_UPLOAD);
            ib = create_buffer(1 << 16, D3D12_HEAP_TYPE_UPLOAD);
            float *v; CHECK(vb->Map(0, nullptr, (void **)&v));
            for (int i = 0; i < 4096; i++) { float q[5] = { (float)(i & 1), (float)(i >> 1 & 1), 0, 0, 0 }; memcpy(v + i * 5, q, sizeof(q)); }
            vb->Unmap(0, nullptr);
            uint16_t *ix; CHECK(ib->Map(0, nullptr, (void **)&ix));
            for (int i = 0; i < 1024; i++) ix[i] = (uint16_t)(i % 3);
            ib->Unmap(0, nullptr);
        }, [](UINT f) {
            set_quad_state(p.list, input);
            double t = qpc_ms();
            for (UINT i = 0; i < 5000; i++)
            {
                D3D12_VERTEX_BUFFER_VIEW vbv = { vb->GetGPUVirtualAddress() + (i % 256) * 20 * 4, 20 * 64, 20 };
                D3D12_INDEX_BUFFER_VIEW ibv = { ib->GetGPUVirtualAddress() + (i % 64) * 6, 600, DXGI_FORMAT_R16_UINT };
                p.list->IASetVertexBuffers(0, 1, &vbv);
                p.list->IASetIndexBuffer(&ibv);
                float rc[4] = { (float)(i % 64) / 32.0f - 1.0f, (float)(i / 64 % 64) / 32.0f - 1.0f, 0.01f, f * 0.01f };
                p.list->SetGraphicsRoot32BitConstants(0, 4, rc, 0);
                p.list->DrawIndexedInstanced(3, 1, 0, 0, 0);
            }
            extras.add("record_us_per_draw", (qpc_ms() - t) * 1000.0 / 5000);
        } },
        { "passes", "100 render passes of 6 draws across 3 HDR targets (load, clear, draw, sample)", [] {
            PsoDesc d; d.rt = HDR_FORMAT; flat_hdr = create_pso(d);
            for (int i = 0; i < 3; i++) p.device->CreateShaderResourceView(p.hdr_targets[i], nullptr, srv_cpu(2000 + i));
        }, [](UINT f) {
            double t = qpc_ms();
            for (UINT pass = 0; pass < 100; pass++)
            {
                ID3D12Resource *target = p.hdr_targets[pass % 3];
                barrier(p.list, target, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
                auto h = rtv(FRAMES_IN_FLIGHT + pass % 3);
                p.list->OMSetRenderTargets(1, &h, FALSE, nullptr);
                if (pass % 5 == 0) { float c[4] = { 0, 0, 0, 0 }; p.list->ClearRenderTargetView(h, c, 0, nullptr); }
                set_quad_state(p.list, flat_hdr);
                p.list->SetGraphicsRootDescriptorTable(2, srv_gpu(2000 + (pass + 1) % 3));
                for (UINT i = 0; i < 6; i++) small_quad(p.list, pass * 6 + i, f * 0.01f);
                barrier(p.list, target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }
            auto bb = rtv(p.frame_index);
            p.list->OMSetRenderTargets(1, &bb, FALSE, nullptr);
            extras.add("record_us_per_pass", (qpc_ms() - t) * 1000.0 / 100);
        } },
        { "clears", "40 full-screen color and depth clears into 1080p targets", base, [](UINT) {
            for (UINT i = 0; i < 20; i++)
            {
                ID3D12Resource *target = p.hdr_targets[i % 3];
                barrier(p.list, target, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
                float c[4] = { i * 0.05f, 0, 0, 1 };
                p.list->ClearRenderTargetView(rtv(FRAMES_IN_FLIGHT + i % 3), c, 0, nullptr);
                p.list->ClearDepthStencilView(dsv(), D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
                barrier(p.list, target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }
        } },
        { "barriers", "1000 transition barriers on textures between draws", base, [](UINT f) {
            set_quad_state(p.list, flat);
            double t = qpc_ms();
            for (UINT i = 0; i < 500; i++)
            {
                ID3D12Resource *tex = p.textures[i % 8];
                barrier(p.list, tex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                small_quad(p.list, i, f * 0.01f);
                barrier(p.list, tex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }
            extras.add("record_us_per_barrier", (qpc_ms() - t) * 1000.0 / 1000);
        } },
        // Starfield's pattern (dttdrv/mullion#32): ~300 aliasing barriers naming no resource a frame, while a reserved
        // buffer exists. a translation that drains the GPU at each runs at a few frames a second
        { "alias_barriers", "300 aliasing barriers (no resources named) between draws, with a reserved buffer alive", [] {
            flat = create_pso({});
            D3D12_RESOURCE_DESC desc = {};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = 1 << 20;
            desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            static ID3D12Resource *reserved;
            if (FAILED(p.device->CreateReservedResource(&desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&reserved))))
                fprintf(stderr, "alias_barriers: no reserved buffer\n");
        }, [](UINT f) {
            set_quad_state(p.list, flat);
            for (UINT i = 0; i < 300; i++)
            {
                D3D12_RESOURCE_BARRIER b = {};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
                p.list->ResourceBarrier(1, &b);
                small_quad(p.list, i, f * 0.01f);
            }
        } },
        { "upload_buffer", "4 MiB written to an upload heap and copied to a default buffer per frame", [] {
            flat = create_pso({});
            buffers_default = create_buffer(4 << 20, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        }, [](UINT f) {
            void *cpu;
            double t = qpc_ms();
            D3D12_GPU_VIRTUAL_ADDRESS va = upload_alloc(4 << 20, &cpu);
            memset(cpu, f & 0xff, 4 << 20);
            extras.add("memset_ms", qpc_ms() - t);
            p.list->CopyBufferRegion(buffers_default, 0, p.upload_ring, va - p.upload_ring->GetGPUVirtualAddress(), 4 << 20);
        } },
        { "upload_texture", "a 2048x2048 RGBA8 texture uploaded per frame (CopyTextureRegion)", [] {
            upload_texture_dst = create_texture(2048, 2048, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        }, [](UINT f) {
            void *cpu;
            D3D12_GPU_VIRTUAL_ADDRESS va = upload_alloc(2048 * 2048 * 4, &cpu);
            memset(cpu, f & 0xff, 2048 * 2048 * 4);
            D3D12_TEXTURE_COPY_LOCATION dst = { upload_texture_dst, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
            D3D12_TEXTURE_COPY_LOCATION src = { p.upload_ring, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
            src.PlacedFootprint.Offset = va - p.upload_ring->GetGPUVirtualAddress();
            src.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, 2048, 2048, 1, 2048 * 4 };
            p.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        } },
        { "map_unmap", "1000 Map/Unmap pairs on upload buffers per frame", [] {
            vb = create_buffer(1 << 20, D3D12_HEAP_TYPE_UPLOAD);
        }, [](UINT) {
            double t = qpc_ms();
            for (UINT i = 0; i < 1000; i++)
            {
                void *cpu;
                D3D12_RANGE none = { 0, 0 };
                CHECK(vb->Map(0, &none, &cpu));
                ((uint32_t *)cpu)[i] = i;
                vb->Unmap(0, nullptr);
            }
            extras.add("us_per_map_unmap", (qpc_ms() - t) * 1000.0 / 1000);
        } },
        { "resource_churn", "50 committed buffers and 10 textures created and released per frame", base, [](UINT) {
            double t = qpc_ms();
            std::vector<ID3D12Resource *> rs;
            for (UINT i = 0; i < 50; i++) rs.push_back(create_buffer(64 << 10, D3D12_HEAP_TYPE_DEFAULT));
            for (UINT i = 0; i < 10; i++) rs.push_back(create_texture(256, 256, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON));
            double t1 = qpc_ms();
            for (auto *r : rs) r->Release();
            extras.add("create_us_per_resource", (t1 - t) * 1000.0 / 60);
            extras.add("release_us_per_resource", (qpc_ms() - t1) * 1000.0 / 60);
        } },
        { "placed_churn", "200 placed buffers created and released per frame in one heap", [] {
            D3D12_HEAP_DESC hd = {};
            hd.SizeInBytes = 64 << 20;
            hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
            hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
            CHECK(p.device->CreateHeap(&hd, IID_PPV_ARGS(&placed_heap)));
        }, [](UINT) {
            D3D12_RESOURCE_DESC rd = {};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width = 64 << 10;
            rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
            rd.SampleDesc.Count = 1;
            rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            double t = qpc_ms();
            std::vector<ID3D12Resource *> rs(200);
            for (UINT i = 0; i < 200; i++)
                CHECK(p.device->CreatePlacedResource(placed_heap, (UINT64)i * (64 << 10), &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&rs[i])));
            double t1 = qpc_ms();
            for (auto *r : rs) r->Release();
            extras.add("create_us_per_resource", (t1 - t) * 1000.0 / 200);
            extras.add("release_us_per_resource", (qpc_ms() - t1) * 1000.0 / 200);
        } },
        { "descriptors", "5000 SRV/CBV writes and 5000 descriptor copies per frame", base, [](UINT) {
            double t = qpc_ms();
            for (UINT i = 0; i < 2500; i++)
            {
                p.device->CreateShaderResourceView(p.textures[i % 8], nullptr, staging_cpu(i));
                D3D12_CONSTANT_BUFFER_VIEW_DESC cbv = { p.upload_ring->GetGPUVirtualAddress() + i * 256, 256 };
                p.device->CreateConstantBufferView(&cbv, staging_cpu(2500 + i));
            }
            double t1 = qpc_ms();
            for (UINT i = 0; i < 5000; i++)
                p.device->CopyDescriptorsSimple(1, srv_cpu(4000 + i), staging_cpu(i), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            extras.add("us_per_view", (t1 - t) * 1000.0 / 5000);
            extras.add("us_per_copy", (qpc_ms() - t1) * 1000.0 / 5000);
        } },
        { "fence_sync", "8 mid-frame submissions, each waited on by the CPU (readback-style stalls)", [] {
            flat = create_pso({});
            for (int i = 0; i < 2; i++)
            {
                CHECK(p.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&extra_allocs[i])));
                CHECK(p.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, extra_allocs[i], nullptr, IID_PPV_ARGS(&extra_lists[i])));
                CHECK(extra_lists[i]->Close());
            }
        }, [](UINT f) {
            double wait = 0;
            for (UINT k = 0; k < 8; k++)
            {
                ID3D12GraphicsCommandList *l = extra_lists[0];
                CHECK(extra_allocs[0]->Reset());
                CHECK(l->Reset(extra_allocs[0], nullptr));
                auto h = rtv(FRAMES_IN_FLIGHT);
                barrier(l, p.hdr_targets[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
                l->OMSetRenderTargets(1, &h, FALSE, nullptr);
                viewport(l);
                set_quad_state(l, flat);
                for (UINT i = 0; i < 200; i++) small_quad(l, i, f * 0.01f);
                barrier(l, p.hdr_targets[0], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                double t = qpc_ms();
                submit_and_wait(l);
                wait += qpc_ms() - t;
            }
            extras.add("submit_wait_ms_each", wait / 8);
        } },
        { "readback", "a 1080p target copied to a readback buffer, waited for and mapped each frame", [] {
            readback = create_buffer((UINT64)W * H * 8, D3D12_HEAP_TYPE_READBACK);
            CHECK(p.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&extra_allocs[0])));
            CHECK(p.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, extra_allocs[0], nullptr, IID_PPV_ARGS(&extra_lists[0])));
            CHECK(extra_lists[0]->Close());
        }, [](UINT) {
            ID3D12GraphicsCommandList *l = extra_lists[0];
            CHECK(extra_allocs[0]->Reset());
            CHECK(l->Reset(extra_allocs[0], nullptr));
            barrier(l, p.hdr_targets[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION dst = { readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
            dst.PlacedFootprint.Footprint = { HDR_FORMAT, W, H, 1, W * 8 };
            D3D12_TEXTURE_COPY_LOCATION src = { p.hdr_targets[0], D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
            l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            barrier(l, p.hdr_targets[0], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            double t = qpc_ms();
            submit_and_wait(l);
            double t1 = qpc_ms();
            void *cpu;
            D3D12_RANGE r = { 0, (SIZE_T)W * H * 8 };
            CHECK(readback->Map(0, &r, &cpu));
            volatile uint64_t sum = 0;
            for (UINT i = 0; i < W * H; i += 64) sum += ((uint64_t *)cpu)[i];
            readback->Unmap(0, nullptr);
            extras.add("copy_wait_ms", t1 - t);
            extras.add("map_read_ms", qpc_ms() - t1);
        } },
        { "many_lists", "40 command lists per frame, each executed separately", [] {
            flat = create_pso({});
            for (UINT i = 0; i < 40 * FRAMES_IN_FLIGHT; i++)
                CHECK(p.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&extra_allocs[i])));
            for (int i = 0; i < 40; i++)
            {
                CHECK(p.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, extra_allocs[i], nullptr, IID_PPV_ARGS(&extra_lists[i])));
                CHECK(extra_lists[i]->Close());
            }
        }, [](UINT f) {
            // Allocators are reused only after the frame's fence (the main loop waits for it).
            double t = qpc_ms();
            for (UINT k = 0; k < 40; k++)
            {
                ID3D12GraphicsCommandList *l = extra_lists[k];
                ID3D12CommandAllocator *a = extra_allocs[p.frame_index * 40 + k];
                CHECK(a->Reset());
                CHECK(l->Reset(a, nullptr));
                auto h = rtv(p.frame_index);
                l->OMSetRenderTargets(1, &h, FALSE, nullptr);
                viewport(l);
                set_quad_state(l, flat);
                for (UINT i = 0; i < 25; i++) small_quad(l, k * 25 + i, f * 0.01f);
                CHECK(l->Close());
                ID3D12CommandList *lists[] = { l };
                p.queue->ExecuteCommandLists(1, lists);
            }
            extras.add("us_per_list", (qpc_ms() - t) * 1000.0 / 40);
        } },
        { "queries", "1000 timestamp queries and an occlusion query per draw, resolved per frame", [] {
            flat = create_pso({});
            D3D12_QUERY_HEAP_DESC qh = { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 1000 };
            CHECK(p.device->CreateQueryHeap(&qh, IID_PPV_ARGS(&ts_heap)));
            qh = { D3D12_QUERY_HEAP_TYPE_OCCLUSION, 500 };
            CHECK(p.device->CreateQueryHeap(&qh, IID_PPV_ARGS(&occlusion_heap)));
            readback = create_buffer(1500 * 8, D3D12_HEAP_TYPE_READBACK);
        }, [](UINT f) {
            set_quad_state(p.list, flat);
            for (UINT i = 0; i < 500; i++)
            {
                p.list->EndQuery(ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, i * 2);
                p.list->BeginQuery(occlusion_heap, D3D12_QUERY_TYPE_BINARY_OCCLUSION, i);
                small_quad(p.list, i, f * 0.01f);
                p.list->EndQuery(occlusion_heap, D3D12_QUERY_TYPE_BINARY_OCCLUSION, i);
                p.list->EndQuery(ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, i * 2 + 1);
            }
            p.list->ResolveQueryData(ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, 0, 1000, readback, 0);
            p.list->ResolveQueryData(occlusion_heap, D3D12_QUERY_TYPE_BINARY_OCCLUSION, 0, 500, readback, 8000);
        } },
        { "timestamps", "1000 timestamp queries between draws, resolved per frame", [] {
            flat = create_pso({});
            D3D12_QUERY_HEAP_DESC qh = { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 1000 };
            CHECK(p.device->CreateQueryHeap(&qh, IID_PPV_ARGS(&ts_heap)));
            readback = create_buffer(1000 * 8, D3D12_HEAP_TYPE_READBACK);
        }, [](UINT f) {
            set_quad_state(p.list, flat);
            for (UINT i = 0; i < 1000; i++)
            {
                p.list->EndQuery(ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, i);
                small_quad(p.list, i, f * 0.01f);
            }
            p.list->ResolveQueryData(ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, 0, 1000, readback, 0);
        } },
        { "occlusion", "500 binary occlusion queries, one per draw, resolved per frame", [] {
            flat = create_pso({});
            D3D12_QUERY_HEAP_DESC qh = { D3D12_QUERY_HEAP_TYPE_OCCLUSION, 500 };
            CHECK(p.device->CreateQueryHeap(&qh, IID_PPV_ARGS(&occlusion_heap)));
            readback = create_buffer(500 * 8, D3D12_HEAP_TYPE_READBACK);
        }, [](UINT f) {
            set_quad_state(p.list, flat);
            for (UINT i = 0; i < 500; i++)
            {
                p.list->BeginQuery(occlusion_heap, D3D12_QUERY_TYPE_BINARY_OCCLUSION, i);
                small_quad(p.list, i, f * 0.01f);
                p.list->EndQuery(occlusion_heap, D3D12_QUERY_TYPE_BINARY_OCCLUSION, i);
            }
            p.list->ResolveQueryData(occlusion_heap, D3D12_QUERY_TYPE_BINARY_OCCLUSION, 0, 500, readback, 0);
        } },
        { "execute_indirect", "ExecuteIndirect of 2000 draws with root constants", [] {
            flat = create_pso({});
            D3D12_INDIRECT_ARGUMENT_DESC args[2] = {};
            args[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
            args[0].Constant = { 0, 0, 4 };
            args[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
            D3D12_COMMAND_SIGNATURE_DESC cs = { 32, 2, args, 0 };
            CHECK(p.device->CreateCommandSignature(&cs, p.root_signature, IID_PPV_ARGS(&draw_signature)));
            indirect_args = create_buffer(2000 * 32, D3D12_HEAP_TYPE_UPLOAD);
            float *a; CHECK(indirect_args->Map(0, nullptr, (void **)&a));
            for (UINT i = 0; i < 2000; i++)
            {
                float rc[4] = { (float)(i % 64) / 32.0f - 1.0f, (float)(i / 64 % 64) / 32.0f - 1.0f, 0.01f, 0.5f };
                memcpy(a + i * 8, rc, 16);
                uint32_t draw[4] = { 3, 1, 0, 0 };
                memcpy(a + i * 8 + 4, draw, 16);
            }
            indirect_args->Unmap(0, nullptr);
        }, [](UINT) {
            set_quad_state(p.list, flat);
            p.list->ExecuteIndirect(draw_signature, 2000, indirect_args, 0, nullptr, 0);
        } },
        { "fill", "20 full-screen quads with a heavy pixel shader at 1080p (GPU ALU)", [] {
            PsoDesc d; d.ps = "ps_heavy"; heavy = create_pso(d);
        }, [](UINT f) {
            set_quad_state(p.list, heavy);
            for (UINT i = 0; i < 20; i++)
            {
                float rc[4] = { 0, 0, 1, f * 0.01f + i };
                p.list->SetGraphicsRoot32BitConstants(0, 4, rc, 0);
                p.list->DrawInstanced(3, 1, 0, 0);
            }
        } },
        { "sample", "20 full-screen quads sampling 8 textures each at 1080p (GPU texture)", [] {
            PsoDesc d; d.ps = "ps_tex"; texd = create_pso(d);
        }, [](UINT f) {
            set_quad_state(p.list, texd);
            for (UINT i = 0; i < 20; i++)
            {
                float rc[4] = { 0, 0, 1, f * 0.01f };
                p.list->SetGraphicsRoot32BitConstants(0, 4, rc, 0);
                p.list->DrawInstanced(3, 1, 0, 0);
            }
        } },
        { "blend_depth", "40 full-screen blended quads with depth test at 1080p (GPU ROP)", [] {
            PsoDesc d; d.blend = true; d.depth = true; flat_depth = create_pso(d);
        }, [](UINT f) {
            auto h = rtv(p.frame_index);
            auto d = dsv();
            p.list->ClearDepthStencilView(d, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
            p.list->OMSetRenderTargets(1, &h, FALSE, &d);
            set_quad_state(p.list, flat_depth);
            for (UINT i = 0; i < 40; i++)
            {
                float rc[4] = { 0, 0, 1, 0.5f + f * 0.001f };
                p.list->SetGraphicsRoot32BitConstants(0, 4, rc, 0);
                p.list->DrawInstanced(3, 1, 0, 0);
            }
        } },
        { "msaa", "4x MSAA 1080p target: 200 draws and a resolve per frame", [] {
            PsoDesc d; d.rt = HDR_FORMAT; d.samples = 4; flat_msaa = create_pso(d);
            D3D12_CLEAR_VALUE cv = { HDR_FORMAT, { 0, 0, 0, 0 } };
            msaa_target = create_texture(W, H, HDR_FORMAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE, 1, 4, &cv);
            p.device->CreateRenderTargetView(msaa_target, nullptr, rtv(FRAMES_IN_FLIGHT + 4));
            resolve_target = create_texture(W, H, HDR_FORMAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_RESOLVE_DEST);
        }, [](UINT f) {
            barrier(p.list, msaa_target, D3D12_RESOURCE_STATE_RESOLVE_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            auto h = rtv(FRAMES_IN_FLIGHT + 4);
            p.list->OMSetRenderTargets(1, &h, FALSE, nullptr);
            float c[4] = { 0, 0, 0, 0 };
            p.list->ClearRenderTargetView(h, c, 0, nullptr);
            set_quad_state(p.list, flat_msaa);
            for (UINT i = 0; i < 200; i++) small_quad(p.list, i, f * 0.01f);
            barrier(p.list, msaa_target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
            p.list->ResolveSubresource(resolve_target, 0, msaa_target, 0, HDR_FORMAT);
            auto bb = rtv(p.frame_index);
            p.list->OMSetRenderTargets(1, &bb, FALSE, nullptr);
        } },
        { "compute", "2000 small dispatches with a UAV barrier each", [] {
            compute = create_compute_pso();
        }, [](UINT f) {
            p.list->SetComputeRootSignature(p.compute_root_signature);
            p.list->SetPipelineState(compute);
            p.list->SetComputeRootUnorderedAccessView(1, p.uav_buffer->GetGPUVirtualAddress());
            D3D12_RESOURCE_BARRIER b = {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            b.UAV.pResource = p.uav_buffer;
            double t = qpc_ms();
            for (UINT i = 0; i < 2000; i++)
            {
                uint32_t rc[4] = { i + f, 0, 0, 0 };
                p.list->SetComputeRoot32BitConstants(0, 4, rc, 0);
                p.list->Dispatch(1, 1, 1);
                p.list->ResourceBarrier(1, &b);
            }
            extras.add("record_us_per_dispatch", (qpc_ms() - t) * 1000.0 / 2000);
        } },
        { "pso_cold", "4 new graphics pipelines per frame from never-seen shaders (compile stutter)", base, [](UINT f) {
            double compile_ms = 0, create_ms = 0;
            for (UINT i = 0; i < 4; i++)
            {
                PsoDesc d; d.ps = (i & 1) ? "ps_heavy" : "ps_tex"; d.salt = p.salt * 100000 + f * 4 + i + 1;
                ID3D12PipelineState *pso = create_pso(d, &compile_ms, &create_ms);
                set_quad_state(p.list, pso);
                small_quad(p.list, i, 0.5f);
                deferred_release(pso);
            }
            extras.add("hlsl_compile_ms_per_pso", compile_ms / 4);
            extras.add("create_ms_per_pso", create_ms / 4);
        } },
        { "pso_warm", "the same 4 graphics pipelines re-created per frame (cache hits)", base, [](UINT) {
            double compile_ms = 0, create_ms = 0;
            for (UINT i = 0; i < 4; i++)
            {
                PsoDesc d; d.ps = (i & 1) ? "ps_heavy" : "ps_tex"; d.salt = 7 + i;
                ID3D12PipelineState *pso = create_pso(d, &compile_ms, &create_ms);
                set_quad_state(p.list, pso);
                small_quad(p.list, i, 0.5f);
                deferred_release(pso);
            }
            extras.add("create_ms_per_pso", create_ms / 4);
        } },
    };
}

// ---- scenes added for the typical shape of a Unity frame and the costs of tile-based GPUs ----

// Records upload work on the first allocator outside the frame loop and waits for it (scene init).
static void base_init_none() {}

static void run_init_commands(const std::function<void(ID3D12GraphicsCommandList *)> &record)
{
    CHECK(p.allocators[0]->Reset());
    CHECK(p.list->Reset(p.allocators[0], nullptr));
    record(p.list);
    submit_and_wait(p.list);
}

static void fullscreen(ID3D12GraphicsCommandList *l, float value)
{
    float rc[4] = { 0, 0, 1, value };
    l->SetGraphicsRoot32BitConstants(0, 4, rc, 0);
    l->DrawInstanced(3, 1, 0, 0);
}

static ID3D12Resource *create_depth(UINT w, UINT h, DXGI_FORMAT typeless, DXGI_FORMAT dsv_format, D3D12_RESOURCE_STATES state, UINT dsv_slot)
{
    D3D12_CLEAR_VALUE cv = { dsv_format };
    cv.DepthStencil.Depth = 1.0f;
    ID3D12Resource *r = create_texture(w, h, typeless, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, state, 1, 1, &cv);
    D3D12_DEPTH_STENCIL_VIEW_DESC dd = {};
    dd.Format = dsv_format;
    dd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    p.device->CreateDepthStencilView(r, &dd, dsv(dsv_slot));
    return r;
}

static std::vector<Scene> more_scenes()
{
    // Heap slots: 8-1007 texture copies (bindless/tables), 10000+ the resources below.
    enum { SRV_SHADOW = 10000, UAV_IMAGE = 10001, SRV_HDR = 10010, SRV_BC7 = 10020, SRV_RGBA8 = 10030, CPU_UAV_IMAGE = 15001 };
    static ID3D12PipelineState *flat, *shadow, *opaque, *post, *final_pso, *ui, *alpha, *sample1, *stencil_w, *stencil_t, *flat_hdr,
        *cimg, *inst, *dense, *bindless, *rs_psos[2], *texd;
    static ID3D12RootSignature *cimg_rs, *bindless_rs, *rs_pair[2];
    static ID3D12Resource *shadow_map, *image, *atlas, *bc7, *rgba8, *count_buffer, *args, *dst_buffer;
    static ID3D12DescriptorHeap *second_heap;
    static ID3D12CommandSignature *draw_sig;

    auto hdr_srvs = [] { for (int i = 0; i < 3; i++) p.device->CreateShaderResourceView(p.hdr_targets[i], nullptr, srv_cpu(SRV_HDR + i)); };
    auto texture_copies = [] { for (UINT i = 8; i < 8 + 1000; i++) p.device->CreateShaderResourceView(p.textures[i % 8], nullptr, srv_cpu(i)); };
    auto make_image = [] {
        image = create_texture(W, H, HDR_FORMAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        p.device->CreateUnorderedAccessView(image, nullptr, nullptr, srv_cpu(UAV_IMAGE));
        p.device->CreateUnorderedAccessView(image, nullptr, nullptr, staging_cpu(CPU_UAV_IMAGE));
    };
    auto make_cimg = [] {
        D3D12_DESCRIPTOR_RANGE uav = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0 };
        D3D12_DESCRIPTOR_RANGE srv = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
        D3D12_ROOT_PARAMETER rp[3] = {};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rp[0].Constants = { 0, 0, 4 };
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[1].DescriptorTable = { 1, &uav };
        rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[2].DescriptorTable = { 1, &srv };
        D3D12_ROOT_SIGNATURE_DESC rsd = { 3, rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
        cimg_rs = serialize_root_signature(rsd);
        ID3DBlob *cs = compile(compute_image_hlsl, "cs_blur", "cs_5_0");
        D3D12_COMPUTE_PIPELINE_STATE_DESC c = {};
        c.pRootSignature = cimg_rs;
        c.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
        CHECK(p.device->CreateComputePipelineState(&c, IID_PPV_ARGS(&cimg)));
        cs->Release();
    };
    auto dispatch_image = [](ID3D12GraphicsCommandList *l, UINT count, UINT frame) {
        l->SetComputeRootSignature(cimg_rs);
        l->SetPipelineState(cimg);
        l->SetComputeRootDescriptorTable(1, srv_gpu(UAV_IMAGE));
        l->SetComputeRootDescriptorTable(2, srv_gpu(SRV_HDR));
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = image;
        for (UINT i = 0; i < count; i++)
        {
            float rc[4] = { frame * 0.001f, 0, 0, 0 };
            l->SetComputeRoot32BitConstants(0, 4, rc, 0);
            l->Dispatch(W / 8, H / 8, 1);
            l->ResourceBarrier(1, &b);
        }
    };
    auto make_shadow = [] {
        shadow_map = create_depth(2048, 2048, DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, 1);
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = DXGI_FORMAT_R32_FLOAT;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        p.device->CreateShaderResourceView(shadow_map, &sd, srv_cpu(SRV_SHADOW));
        PsoDesc d; d.ps = nullptr; d.num_rt = 0; d.depth = true; shadow = create_pso(d);
    };
    auto shadow_pass = [](ID3D12GraphicsCommandList *l, UINT draws, UINT frame) {
        barrier(l, shadow_map, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        auto d = dsv(1);
        l->OMSetRenderTargets(0, nullptr, FALSE, &d);
        l->ClearDepthStencilView(d, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        viewport(l, 2048, 2048);
        set_quad_state(l, shadow);
        for (UINT i = 0; i < draws; i++)
        {
            float rc[4] = { (float)(i % 64) / 32.0f - 1.0f, (float)(i / 64 % 64) / 32.0f - 1.0f, 0.04f, frame * 0.01f };
            l->SetGraphicsRoot32BitConstants(0, 4, rc, 0);
            l->DrawInstanced(3, 1, 0, 0);
        }
        barrier(l, shadow_map, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        viewport(l);
    };
    auto back_to_backbuffer = [](ID3D12GraphicsCommandList *l) {
        auto bb = rtv(p.frame_index);
        l->OMSetRenderTargets(1, &bb, FALSE, nullptr);
        viewport(l);
    };
    // One full-screen pass from target src (or the backbuffer if dst < 0) into hdr target dst.
    auto post_pass = [back_to_backbuffer](ID3D12GraphicsCommandList *l, ID3D12PipelineState *pso, int src, int dst, float value) {
        if (dst >= 0)
        {
            barrier(l, p.hdr_targets[dst], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            auto h = rtv(FRAMES_IN_FLIGHT + dst);
            l->OMSetRenderTargets(1, &h, FALSE, nullptr);
        }
        else
            back_to_backbuffer(l);
        set_quad_state(l, pso);
        l->SetGraphicsRootDescriptorTable(2, srv_gpu(SRV_HDR + src));
        fullscreen(l, value);
        if (dst >= 0)
            barrier(l, p.hdr_targets[dst], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    };
    auto make_bc = [] {
        // BC7 and RGBA8 1024x1024 textures with the same footprint in texels, 8 SRVs each.
        bc7 = create_texture(1024, 1024, DXGI_FORMAT_BC7_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        rgba8 = create_texture(1024, 1024, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        run_init_commands([](ID3D12GraphicsCommandList *l) {
            void *cpu;
            D3D12_GPU_VIRTUAL_ADDRESS va = upload_alloc(1024 * 1024 * 4, &cpu);
            uint32_t *w = (uint32_t *)cpu;
            for (UINT i = 0; i < 1024 * 1024; i++) w[i] = i * 2654435761u;
            D3D12_TEXTURE_COPY_LOCATION src = { p.upload_ring, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
            src.PlacedFootprint.Offset = va - p.upload_ring->GetGPUVirtualAddress();
            src.PlacedFootprint.Footprint = { DXGI_FORMAT_BC7_UNORM, 1024, 1024, 1, 4096 };
            D3D12_TEXTURE_COPY_LOCATION dst = { bc7, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
            l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            src.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, 1024, 1024, 1, 4096 };
            dst.pResource = rgba8;
            l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            barrier(l, bc7, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            barrier(l, rgba8, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        });
        for (UINT i = 0; i < 8; i++)
        {
            p.device->CreateShaderResourceView(bc7, nullptr, srv_cpu(SRV_BC7 + i));
            p.device->CreateShaderResourceView(rgba8, nullptr, srv_cpu(SRV_RGBA8 + i));
        }
        PsoDesc d; d.ps = "ps_tex"; texd = create_pso(d);
    };
    auto sample_frame = [](UINT base, UINT f) {
        set_quad_state(p.list, texd);
        p.list->SetGraphicsRootDescriptorTable(2, srv_gpu(base));
        for (UINT i = 0; i < 20; i++) fullscreen(p.list, f * 0.01f);
    };

    return {
        { "unity_frame", "a Unity-shaped frame: 400-draw shadow map, 800 opaque draws with a root CBV each, 4 post passes, "
                         "3 compute passes with UAV barriers, composite and 300 UI quads", [make_shadow, hdr_srvs, make_image, make_cimg, texture_copies] {
            make_shadow(); hdr_srvs(); make_image(); make_cimg(); texture_copies();
            PsoDesc d; d.rt = HDR_FORMAT; d.depth = true; d.ps = "ps_tex"; opaque = create_pso(d);
            d = {}; d.rt = HDR_FORMAT; d.ps = "ps_post"; post = create_pso(d);
            d = {}; d.ps = "ps_post"; final_pso = create_pso(d);
            d = {}; d.ps = "ps_ui"; d.blend = true; ui = create_pso(d);
        }, [shadow_pass, post_pass, dispatch_image, back_to_backbuffer](UINT f) {
            double t = qpc_ms();
            ID3D12GraphicsCommandList *l = p.list;
            shadow_pass(l, 400, f);
            // Opaque pass into hdr target 0 with depth; a fresh root CBV per draw, as Unity binds per-object data.
            barrier(l, p.hdr_targets[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            auto h = rtv(FRAMES_IN_FLIGHT);
            auto d = dsv(0);
            l->OMSetRenderTargets(1, &h, FALSE, &d);
            float c[4] = { 0, 0, 0, 0 };
            l->ClearRenderTargetView(h, c, 0, nullptr);
            l->ClearDepthStencilView(d, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
            set_quad_state(l, opaque);
            for (UINT i = 0; i < 800; i++)
            {
                void *cpu;
                D3D12_GPU_VIRTUAL_ADDRESS cb = upload_alloc(256, &cpu);
                ((float *)cpu)[4] = (float)i;
                l->SetGraphicsRootConstantBufferView(1, cb);
                if (i % 16 == 0) l->SetGraphicsRootDescriptorTable(2, srv_gpu(8 + (i / 16) * 8 % 992));
                small_quad(l, i, f * 0.01f);
            }
            barrier(l, p.hdr_targets[0], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            // Post chain: 0 -> 1 -> 2 -> 1 -> 2.
            post_pass(l, post, 0, 1, f * 0.01f);
            post_pass(l, post, 1, 2, f * 0.01f);
            post_pass(l, post, 2, 1, f * 0.01f);
            post_pass(l, post, 1, 2, f * 0.01f);
            dispatch_image(l, 3, f);
            // Composite into the backbuffer, then UI.
            post_pass(l, final_pso, 2, -1, f * 0.01f);
            set_quad_state(l, ui);
            for (UINT i = 0; i < 300; i++)
            {
                if (i % 10 == 0) l->SetGraphicsRootDescriptorTable(2, srv_gpu(8 + i % 1000));
                small_quad(l, i, 1.0f);
            }
            back_to_backbuffer(l);
            extras.add("record_ms", qpc_ms() - t);
        } },
        { "depth_only", "2000 draws into a 2048x2048 shadow map with no pixel shader", make_shadow, [shadow_pass](UINT f) {
            shadow_pass(p.list, 2000, f);
        } },
        { "overdraw_opaque", "50 full-screen depth-tested quads sampling a texture (baseline for alpha_test)", [] {
            PsoDesc d; d.ps = "ps_sample1"; d.depth = true; d.depth_write = false; sample1 = create_pso(d);
        }, [](UINT f) {
            auto h = rtv(p.frame_index);
            auto d = dsv(0);
            p.list->ClearDepthStencilView(d, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
            p.list->OMSetRenderTargets(1, &h, FALSE, &d);
            set_quad_state(p.list, sample1);
            for (UINT i = 0; i < 50; i++) fullscreen(p.list, 1.0f);
        } },
        { "alpha_test", "50 full-screen depth-tested quads with discard (Unity cutout shaders)", [] {
            PsoDesc d; d.ps = "ps_alpha"; d.depth = true; d.depth_write = false; alpha = create_pso(d);
        }, [](UINT f) {
            auto h = rtv(p.frame_index);
            auto d = dsv(0);
            p.list->ClearDepthStencilView(d, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
            p.list->OMSetRenderTargets(1, &h, FALSE, &d);
            set_quad_state(p.list, alpha);
            for (UINT i = 0; i < 50; i++) fullscreen(p.list, 1.0f);
        } },
        { "stencil", "200 quads writing stencil, then 20 full-screen quads tested against it (D24S8)", [] {
            create_depth(W, H, DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_D24_UNORM_S8_UINT, D3D12_RESOURCE_STATE_DEPTH_WRITE, 2);
            PsoDesc d; d.stencil_write = true; d.dsv = DXGI_FORMAT_D24_UNORM_S8_UINT; stencil_w = create_pso(d);
            d = {}; d.stencil_test = true; d.dsv = DXGI_FORMAT_D24_UNORM_S8_UINT; d.ps = "ps_heavy"; stencil_t = create_pso(d);
        }, [](UINT f) {
            auto h = rtv(p.frame_index);
            auto d = dsv(2);
            p.list->ClearDepthStencilView(d, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
            p.list->OMSetRenderTargets(1, &h, FALSE, &d);
            p.list->OMSetStencilRef(1);
            set_quad_state(p.list, stencil_w);
            for (UINT i = 0; i < 200; i++)
            {
                float rc[4] = { (float)(i % 16) / 8.0f - 0.9f, (float)(i / 16) / 8.0f - 0.9f, 0.05f, 0.2f };
                p.list->SetGraphicsRoot32BitConstants(0, 4, rc, 0);
                p.list->DrawInstanced(3, 1, 0, 0);
            }
            set_quad_state(p.list, stencil_t);
            for (UINT i = 0; i < 20; i++) fullscreen(p.list, f * 0.01f);
        } },
        { "postfx_chain", "10 full-screen post passes ping-ponging two 1080p HDR targets", [hdr_srvs] {
            hdr_srvs();
            PsoDesc d; d.rt = HDR_FORMAT; d.ps = "ps_post"; post = create_pso(d);
        }, [post_pass, back_to_backbuffer](UINT f) {
            double t = qpc_ms();
            for (int i = 0; i < 10; i++) post_pass(p.list, post, 1 + (i + 1) % 2, 1 + i % 2, f * 0.01f);
            back_to_backbuffer(p.list);
            extras.add("record_us_per_pass", (qpc_ms() - t) * 100.0);
        } },
        { "compute_image", "10 compute passes over a 1080p RWTexture2D with UAV barriers (compute post-processing)", [make_image, make_cimg, hdr_srvs] {
            make_image(); make_cimg(); hdr_srvs();
        }, [dispatch_image](UINT f) {
            dispatch_image(p.list, 10, f);
        } },
        { "clear_uav", "10 ClearUnorderedAccessViewFloat calls on a 1080p texture", make_image, [](UINT f) {
            float v[4] = { f * 0.001f, 0, 0, 1 };
            D3D12_RESOURCE_BARRIER b = {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            b.UAV.pResource = image;
            for (UINT i = 0; i < 10; i++)
            {
                p.list->ClearUnorderedAccessViewFloat(srv_gpu(UAV_IMAGE), staging_cpu(CPU_UAV_IMAGE), image, v, 0, nullptr);
                p.list->ResourceBarrier(1, &b);
            }
        } },
        { "render_pass_api", "50 D3D12 render passes (BeginRenderPass) of 6 draws, clear or discard on load, preserve on store", [] {
            PsoDesc d; d.rt = HDR_FORMAT; flat_hdr = create_pso(d);
        }, [back_to_backbuffer](UINT f) {
            ID3D12GraphicsCommandList4 *l4 = nullptr;
            CHECK(p.list->QueryInterface(IID_PPV_ARGS(&l4)));
            for (UINT pass = 0; pass < 50; pass++)
            {
                ID3D12Resource *target = p.hdr_targets[pass % 3];
                barrier(p.list, target, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
                D3D12_RENDER_PASS_RENDER_TARGET_DESC rt = {};
                rt.cpuDescriptor = rtv(FRAMES_IN_FLIGHT + pass % 3);
                rt.BeginningAccess.Type = pass & 1 ? D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_DISCARD : D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR;
                rt.BeginningAccess.Clear.ClearValue.Format = HDR_FORMAT;
                rt.EndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
                l4->BeginRenderPass(1, &rt, nullptr, D3D12_RENDER_PASS_FLAG_NONE);
                set_quad_state(p.list, flat_hdr);
                for (UINT i = 0; i < 6; i++) small_quad(p.list, pass * 6 + i, f * 0.01f);
                l4->EndRenderPass();
                barrier(p.list, target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }
            l4->Release();
            back_to_backbuffer(p.list);
        } },
        { "discard_resource", "50 passes, each preceded by DiscardResource on its target (should load nothing)", [] {
            PsoDesc d; d.rt = HDR_FORMAT; flat_hdr = create_pso(d);
        }, [back_to_backbuffer](UINT f) {
            for (UINT pass = 0; pass < 50; pass++)
            {
                ID3D12Resource *target = p.hdr_targets[pass % 3];
                barrier(p.list, target, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
                p.list->DiscardResource(target, nullptr);
                auto h = rtv(FRAMES_IN_FLIGHT + pass % 3);
                p.list->OMSetRenderTargets(1, &h, FALSE, nullptr);
                set_quad_state(p.list, flat_hdr);
                for (UINT i = 0; i < 6; i++) small_quad(p.list, pass * 6 + i, f * 0.01f);
                barrier(p.list, target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }
            back_to_backbuffer(p.list);
        } },
        { "bindless", "2000 draws indexing a 1000-texture unbounded table per draw (SM 5.1 dynamic indexing)", [texture_copies] {
            texture_copies();
            D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 1, 0 };
            D3D12_ROOT_PARAMETER rp[2] = {};
            rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            rp[0].Constants = { 0, 0, 8 };
            rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            rp[1].DescriptorTable = { 1, &range };
            D3D12_STATIC_SAMPLER_DESC ss = {};
            ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            ss.MaxLOD = D3D12_FLOAT32_MAX;
            D3D12_ROOT_SIGNATURE_DESC rsd = { 2, rp, 1, &ss, D3D12_ROOT_SIGNATURE_FLAG_NONE };
            bindless_rs = serialize_root_signature(rsd);
            PsoDesc d; d.hlsl = bindless_hlsl; d.ps = "ps_bindless"; d.vs_target = "vs_5_1"; d.ps_target = "ps_5_1"; d.rs = bindless_rs;
            bindless = create_pso(d);
        }, [](UINT f) {
            p.list->SetGraphicsRootSignature(bindless_rs);
            p.list->SetPipelineState(bindless);
            p.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            p.list->SetGraphicsRootDescriptorTable(1, srv_gpu(8));
            double t = qpc_ms();
            for (UINT i = 0; i < 2000; i++)
            {
                uint32_t rc[8];
                float pos[4] = { (float)(i % 64) / 32.0f - 1.0f, (float)(i / 64 % 64) / 32.0f - 1.0f, 0.02f, f * 0.01f };
                memcpy(rc, pos, 16);
                rc[4] = i % 1000; rc[5] = rc[6] = rc[7] = 0;
                p.list->SetGraphicsRoot32BitConstants(0, 8, rc, 0);
                p.list->DrawInstanced(3, 1, 0, 0);
            }
            extras.add("record_us_per_draw", (qpc_ms() - t) * 1000.0 / 2000);
        } },
        { "root_signature_switch", "3000 draws alternating two root signatures (and their pipelines), rebinding all parameters", [] {
            D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 8, 0, 0, 0 };
            for (int k = 0; k < 2; k++)
            {
                D3D12_ROOT_PARAMETER rp[3] = {};
                rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
                rp[0].Constants = { 0, 0, 4u + k * 4 };
                rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
                rp[1].Descriptor = { 1, 0 };
                rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                rp[2].DescriptorTable = { 1, &range };
                D3D12_STATIC_SAMPLER_DESC ss = {};
                ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
                ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
                ss.MaxLOD = D3D12_FLOAT32_MAX;
                D3D12_ROOT_SIGNATURE_DESC rsd = { 3, rp, 1, &ss, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
                rs_pair[k] = serialize_root_signature(rsd);
                PsoDesc d; d.rs = rs_pair[k]; d.blend = k; rs_psos[k] = create_pso(d);
            }
        }, [](UINT f) {
            p.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            void *cpu;
            D3D12_GPU_VIRTUAL_ADDRESS cb = upload_alloc(256, &cpu);
            double t = qpc_ms();
            for (UINT i = 0; i < 3000; i++)
            {
                int k = i & 1;
                p.list->SetGraphicsRootSignature(rs_pair[k]);
                p.list->SetPipelineState(rs_psos[k]);
                p.list->SetGraphicsRootConstantBufferView(1, cb);
                p.list->SetGraphicsRootDescriptorTable(2, srv_gpu(0));
                small_quad(p.list, i, f * 0.01f);
            }
            extras.add("record_us_per_draw", (qpc_ms() - t) * 1000.0 / 3000);
        } },
        { "heap_switch", "2000 draws alternating two shader-visible descriptor heaps", [] {
            flat = nullptr;
            D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 64, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
            CHECK(p.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&second_heap)));
            auto h = second_heap->GetCPUDescriptorHandleForHeapStart();
            for (UINT i = 0; i < 8; i++, h.ptr += p.srv_size) p.device->CreateShaderResourceView(p.textures[7 - i], nullptr, h);
            PsoDesc d; d.ps = "ps_tex"; texd = create_pso(d);
        }, [](UINT f) {
            set_quad_state(p.list, texd);
            double t = qpc_ms();
            for (UINT i = 0; i < 2000; i++)
            {
                ID3D12DescriptorHeap *heap = (i & 1) ? second_heap : p.srv_heap;
                p.list->SetDescriptorHeaps(1, &heap);
                p.list->SetGraphicsRootDescriptorTable(2, heap->GetGPUDescriptorHandleForHeapStart());
                small_quad(p.list, i, f * 0.01f);
            }
            ID3D12DescriptorHeap *heaps[] = { p.srv_heap };
            p.list->SetDescriptorHeaps(1, heaps);
            extras.add("record_us_per_draw", (qpc_ms() - t) * 1000.0 / 2000);
        } },
        { "ui_quads", "4000 small blended, textured quads, a texture change every 8", [texture_copies] {
            texture_copies();
            PsoDesc d; d.ps = "ps_ui"; d.blend = true; ui = create_pso(d);
        }, [](UINT f) {
            set_quad_state(p.list, ui);
            double t = qpc_ms();
            for (UINT i = 0; i < 4000; i++)
            {
                if (i % 8 == 0) p.list->SetGraphicsRootDescriptorTable(2, srv_gpu(8 + (i / 8) % 992));
                small_quad(p.list, i, 1.0f);
            }
            extras.add("record_us_per_draw", (qpc_ms() - t) * 1000.0 / 4000);
        } },
        { "small_copies", "1000 256-byte CopyBufferRegion uploads per frame (per-object constant uploads)", [] {
            dst_buffer = create_buffer(1000 * 256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        }, [](UINT f) {
            double t = qpc_ms();
            for (UINT i = 0; i < 1000; i++)
            {
                void *cpu;
                D3D12_GPU_VIRTUAL_ADDRESS va = upload_alloc(256, &cpu);
                ((uint32_t *)cpu)[0] = f + i;
                p.list->CopyBufferRegion(dst_buffer, i * 256, p.upload_ring, va - p.upload_ring->GetGPUVirtualAddress(), 256);
            }
            extras.add("record_us_per_copy", (qpc_ms() - t));
        } },
        { "small_texture_updates", "200 64x64 CopyTextureRegion updates into a 1024x1024 atlas per frame (fonts, UI)", [] {
            atlas = create_texture(1024, 1024, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }, [](UINT f) {
            barrier(p.list, atlas, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            for (UINT i = 0; i < 200; i++)
            {
                void *cpu;
                D3D12_GPU_VIRTUAL_ADDRESS va = upload_alloc(64 * 64 * 4, &cpu);
                memset(cpu, (f + i) & 0xff, 64 * 64 * 4);
                D3D12_TEXTURE_COPY_LOCATION src = { p.upload_ring, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
                src.PlacedFootprint.Offset = va - p.upload_ring->GetGPUVirtualAddress();
                src.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64, 1, 256 };
                D3D12_TEXTURE_COPY_LOCATION dst = { atlas, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                p.list->CopyTextureRegion(&dst, (i % 16) * 64, (i / 16 % 16) * 64, 0, &src, nullptr);
            }
            barrier(p.list, atlas, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        } },
        { "copy_resource", "10 CopyResource calls of a 1080p HDR target", base_init_none, [](UINT) {
            barrier(p.list, p.hdr_targets[1], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
            barrier(p.list, p.hdr_targets[2], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            for (UINT i = 0; i < 10; i++) p.list->CopyResource(p.hdr_targets[2], p.hdr_targets[1]);
            barrier(p.list, p.hdr_targets[1], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            barrier(p.list, p.hdr_targets[2], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        } },
        { "big_resources", "4 64 MiB textures (4096x4096 RGBA8) created and released per frame", base_init_none, [](UINT) {
            double t = qpc_ms();
            ID3D12Resource *r[4];
            for (int i = 0; i < 4; i++)
                r[i] = create_texture(4096, 4096, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
            double t1 = qpc_ms();
            for (int i = 0; i < 4; i++) r[i]->Release();
            extras.add("create_ms_per_resource", (t1 - t) / 4);
            extras.add("release_ms_per_resource", (qpc_ms() - t1) / 4);
        } },
        { "execute_indirect_count", "ExecuteIndirect with a count buffer: up to 4000 draws, 2000 executed", [] {
            flat = create_pso({});
            D3D12_INDIRECT_ARGUMENT_DESC a[2] = {};
            a[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
            a[0].Constant = { 0, 0, 4 };
            a[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
            D3D12_COMMAND_SIGNATURE_DESC cs = { 32, 2, a, 0 };
            CHECK(p.device->CreateCommandSignature(&cs, p.root_signature, IID_PPV_ARGS(&draw_sig)));
            args = create_buffer(4000 * 32, D3D12_HEAP_TYPE_UPLOAD);
            float *v; CHECK(args->Map(0, nullptr, (void **)&v));
            for (UINT i = 0; i < 4000; i++)
            {
                float rc[4] = { (float)(i % 64) / 32.0f - 1.0f, (float)(i / 64 % 64) / 32.0f - 1.0f, 0.01f, 0.5f };
                memcpy(v + i * 8, rc, 16);
                uint32_t draw[4] = { 3, 1, 0, 0 };
                memcpy(v + i * 8 + 4, draw, 16);
            }
            args->Unmap(0, nullptr);
            count_buffer = create_buffer(256, D3D12_HEAP_TYPE_UPLOAD);
            uint32_t *c; CHECK(count_buffer->Map(0, nullptr, (void **)&c));
            *c = 2000;
            count_buffer->Unmap(0, nullptr);
        }, [](UINT) {
            set_quad_state(p.list, flat);
            p.list->ExecuteIndirect(draw_sig, 4000, args, 0, count_buffer, 0);
        } },
        { "instancing", "one draw of 160000 small instances (vertex/instance throughput)", [] {
            PsoDesc d; d.vs = "vs_instanced"; inst = create_pso(d);
        }, [](UINT) {
            set_quad_state(p.list, inst);
            p.list->DrawInstanced(3, 160000, 0, 0);
        } },
        { "vertex_heavy", "one draw of 1M tiny triangles (vertex-bound geometry)", [] {
            PsoDesc d; d.vs = "vs_dense"; dense = create_pso(d);
        }, [](UINT f) {
            set_quad_state(p.list, dense);
            float rc[4] = { 0, 0, 1, f * 0.01f };
            p.list->SetGraphicsRoot32BitConstants(0, 4, rc, 0);
            p.list->DrawInstanced(3 * 1048576, 1, 0, 0);
        } },
        { "sample_bc7", "20 full-screen quads sampling 8 BC7 textures each (compressed bandwidth)", make_bc, [sample_frame](UINT f) { sample_frame(SRV_BC7, f); } },
        { "sample_rgba8", "the same with RGBA8 textures (baseline for sample_bc7)", make_bc, [sample_frame](UINT f) { sample_frame(SRV_RGBA8, f); } },
        { "present_vsync", "clear + Present(1): the display-locked frame", [] {}, [](UINT) {}, true, 1 },
    };
}

// ---- Shader Model 6 scenes (DXIL through dxil-spirv) ----

template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE T, typename V>
struct alignas(void *) StreamSubobject { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = T; V value; };

static bool g_prepare;   // "prepare" mode: setup and scene init only (fills the DXIL cache), no frames

static ID3D12PipelineState *compute_pso_from(ID3D12RootSignature *rs, const std::vector<char> &code)
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC c = {};
    c.pRootSignature = rs;
    c.CS = { code.data(), code.size() };
    ID3D12PipelineState *pso = nullptr;
    CHECK(p.device->CreateComputePipelineState(&c, IID_PPV_ARGS(&pso)));
    return pso;
}

static ID3D12RootSignature *root_signature_1_1(UINT count, const D3D12_ROOT_PARAMETER1 *params, D3D12_ROOT_SIGNATURE_FLAGS flags,
                                               UINT samplers = 0, const D3D12_STATIC_SAMPLER_DESC *sampler = nullptr)
{
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC v = {};
    v.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    v.Desc_1_1 = { count, params, samplers, sampler, flags };
    ID3DBlob *blob = nullptr, *errors = nullptr;
    CHECK(D3D12SerializeVersionedRootSignature(&v, &blob, &errors));
    ID3D12RootSignature *rs = nullptr;
    CHECK(p.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs)));
    blob->Release();
    return rs;
}

static D3D12_STATIC_SAMPLER_DESC linear_sampler()
{
    D3D12_STATIC_SAMPLER_DESC ss = {};
    ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    ss.MaxLOD = D3D12_FLOAT32_MAX;
    ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    return ss;
}

// Root signature: root SRV t0, root UAV u0 (ray tracing and plain compute scenes).
static ID3D12RootSignature *srv_uav_root_signature()
{
    D3D12_ROOT_PARAMETER rp[3] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[2].Constants = { 0, 0, 4 };
    D3D12_ROOT_SIGNATURE_DESC rsd = { 3, rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    return serialize_root_signature(rsd);
}

static void unsupported(const char *what)
{
    printf("extra unsupported=1\n");
    printf("unsupported: %s\n", what);
}

// A scene of ray-traced geometry: a BLAS of a 32x32 grid of triangles (2048), TLAS of N instances.
struct RtGeometry
{
    ID3D12Resource *vb = nullptr, *blas = nullptr, *tlas = nullptr, *scratch = nullptr, *instances = nullptr;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS blas_in = {}, tlas_in = {};
    D3D12_RAYTRACING_GEOMETRY_DESC geom = {};
};

static bool raytracing_supported()
{
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5 = {};
    p.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5));
    return o5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1;
}

static void build_blas(ID3D12GraphicsCommandList4 *l, RtGeometry &g, bool update_scratch_barrier = true)
{
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC b = {};
    b.Inputs = g.blas_in;
    b.DestAccelerationStructureData = g.blas->GetGPUVirtualAddress();
    b.ScratchAccelerationStructureData = g.scratch->GetGPUVirtualAddress();
    l->BuildRaytracingAccelerationStructure(&b, 0, nullptr);
    D3D12_RESOURCE_BARRIER u = {};
    u.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    u.UAV.pResource = g.blas;
    l->ResourceBarrier(1, &u);
}

static void build_tlas(ID3D12GraphicsCommandList4 *l, RtGeometry &g)
{
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC b = {};
    b.Inputs = g.tlas_in;
    b.DestAccelerationStructureData = g.tlas->GetGPUVirtualAddress();
    b.ScratchAccelerationStructureData = g.scratch->GetGPUVirtualAddress();
    l->BuildRaytracingAccelerationStructure(&b, 0, nullptr);
    D3D12_RESOURCE_BARRIER u = {};
    u.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    u.UAV.pResource = g.tlas;
    l->ResourceBarrier(1, &u);
}

static void make_rt_geometry(RtGeometry &g, UINT grid, UINT instance_count)
{
    ID3D12Device5 *dev5 = nullptr;
    CHECK(p.device->QueryInterface(IID_PPV_ARGS(&dev5)));
    UINT tris = grid * grid * 2;
    g.vb = create_buffer((UINT64)tris * 3 * 12, D3D12_HEAP_TYPE_UPLOAD);
    float *v;
    CHECK(g.vb->Map(0, nullptr, (void **)&v));
    for (UINT y = 0; y < grid; y++)
        for (UINT x = 0; x < grid; x++)
        {
            float x0 = (float)x / grid, y0 = (float)y / grid, d = 0.8f / grid;
            float q[2][3][3] = { { { x0, y0, 0.5f }, { x0 + d, y0, 0.5f }, { x0, y0 + d, 0.5f } },
                                 { { x0 + d, y0, 0.5f }, { x0 + d, y0 + d, 0.5f }, { x0, y0 + d, 0.5f } } };
            memcpy(v + ((y * grid + x) * 2) * 9, q, sizeof(q));
        }
    g.vb->Unmap(0, nullptr);
    g.geom.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    g.geom.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    g.geom.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    g.geom.Triangles.VertexCount = tris * 3;
    g.geom.Triangles.VertexBuffer = { g.vb->GetGPUVirtualAddress(), 12 };
    g.blas_in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    g.blas_in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    g.blas_in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    g.blas_in.NumDescs = 1;
    g.blas_in.pGeometryDescs = &g.geom;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO bi = {}, ti = {};
    dev5->GetRaytracingAccelerationStructurePrebuildInfo(&g.blas_in, &bi);
    g.tlas_in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    g.tlas_in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    g.tlas_in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    g.tlas_in.NumDescs = instance_count;
    dev5->GetRaytracingAccelerationStructurePrebuildInfo(&g.tlas_in, &ti);
    const D3D12_RESOURCE_FLAGS uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    g.blas = create_buffer(bi.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, uav, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    g.tlas = create_buffer(ti.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, uav, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    g.scratch = create_buffer(std::max(bi.ScratchDataSizeInBytes, ti.ScratchDataSizeInBytes), D3D12_HEAP_TYPE_DEFAULT, uav,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g.instances = create_buffer((UINT64)instance_count * sizeof(D3D12_RAYTRACING_INSTANCE_DESC), D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RAYTRACING_INSTANCE_DESC *inst;
    CHECK(g.instances->Map(0, nullptr, (void **)&inst));
    UINT side = (UINT)ceil(sqrt((double)instance_count));
    for (UINT i = 0; i < instance_count; i++)
    {
        memset(&inst[i], 0, sizeof(inst[i]));
        float s = 1.0f / side;
        inst[i].Transform[0][0] = inst[i].Transform[1][1] = s;
        inst[i].Transform[2][2] = 1.0f;
        inst[i].Transform[0][3] = (float)(i % side) * s;
        inst[i].Transform[1][3] = (float)(i / side) * s;
        inst[i].Transform[2][3] = (float)(i % 7) * 0.01f;
        inst[i].InstanceMask = 0xff;
        inst[i].AccelerationStructure = g.blas->GetGPUVirtualAddress();
    }
    g.instances->Unmap(0, nullptr);
    g.tlas_in.InstanceDescs = g.instances->GetGPUVirtualAddress();
    dev5->Release();
}

// Time from a draw with a new pipeline to its completion on the GPU, through a list of its own:
// covers compiles a driver defers to first use, on whichever thread.
static double first_use(ID3D12PipelineState *pso)
{
    static ID3D12CommandAllocator *a;
    static ID3D12GraphicsCommandList *l;
    if (!a)
    {
        CHECK(p.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)));
        CHECK(p.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a, nullptr, IID_PPV_ARGS(&l)));
        CHECK(l->Close());
    }
    double t = qpc_ms();
    CHECK(a->Reset());
    CHECK(l->Reset(a, nullptr));
    ID3D12DescriptorHeap *heaps[] = { p.srv_heap };
    l->SetDescriptorHeaps(1, heaps);
    auto h = rtv(FRAMES_IN_FLIGHT);
    barrier(l, p.hdr_targets[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    l->OMSetRenderTargets(1, &h, FALSE, nullptr);
    viewport(l);
    set_quad_state(l, pso);
    small_quad(l, 0, 0.5f);
    barrier(l, p.hdr_targets[0], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    submit_and_wait(l);
    return qpc_ms() - t;
}

static std::vector<Scene> sm6_scenes()
{
    static ID3D12PipelineState *fill6, *sample6, *mesh_pso, *rq_pso, *fb_pso, *fbbase_pso, *bindless6, *wave_pso, *atom_pso[3],
        *math16, *math32;
    static ID3D12RootSignature *mesh_rs, *rt_rs, *fb_rs, *bindless6_rs;
    static std::vector<D3D12_SHADER_BYTECODE> cold_vs, cold_ps;
    static std::vector<std::vector<char>> cold_code;
    static RtGeometry rt;
    static ID3D12Resource *rt_out, *table, *fb_tex, *fb_map, *wave_buf, *atom_buf;
    static ID3D12StateObject *rt_so;
    static std::string math_src16, math_src32;
    enum { SRV_FB = 11010, UAV_FB = 11011 };

    static const char *rq_hlsl = R"(
RaytracingAccelerationStructure scene : register(t0);
RWByteAddressBuffer o : register(u0);
[numthreads(8, 8, 1)]
void main(uint2 t : SV_DispatchThreadID)
{
    RayDesc r;
    r.Origin = float3(t.x / 1920.0, t.y / 1080.0, -1.0);
    r.Direction = float3(0, 0, 1);
    r.TMin = 0.0;
    r.TMax = 10.0;
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(scene, 0, 0xff, r);
    q.Proceed();
    o.Store((t.y * 1920 + t.x) * 4, q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? asuint(q.CommittedRayT()) : 0u);
}
)";
    static const char *rays_hlsl = R"(
RaytracingAccelerationStructure scene : register(t0);
RWByteAddressBuffer o : register(u0);
struct Payload { float t; };
[shader("raygeneration")] void rg()
{
    uint2 id = DispatchRaysIndex().xy;
    RayDesc r;
    r.Origin = float3(id.x / 1920.0, id.y / 1080.0, -1.0);
    r.Direction = float3(0, 0, 1);
    r.TMin = 0.0;
    r.TMax = 10.0;
    Payload pl = { -1.0 };
    TraceRay(scene, RAY_FLAG_NONE, 0xff, 0, 1, 0, r, pl);
    o.Store((id.y * 1920 + id.x) * 4, asuint(pl.t));
}
[shader("miss")] void ms(inout Payload pl) { pl.t = 0; }
[shader("closesthit")] void ch(inout Payload pl, BuiltInTriangleIntersectionAttributes a) { pl.t = RayTCurrent() + a.barycentrics.x; }
)";
    static const char *mesh_hlsl = R"(
struct Payload { uint base; };
groupshared Payload pl;
[numthreads(1, 1, 1)] void as_main(uint g : SV_GroupID) { pl.base = g * 64; DispatchMesh(64, 1, 1, pl); }
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
// 64 triangles of a 128-vertex strip per mesh group: 4096 groups x 64 = 262144 triangles per amplification group set.
[numthreads(128, 1, 1)] [outputtopology("triangle")]
void ms_main(uint t : SV_GroupThreadID, uint g : SV_GroupID, in payload Payload p, out vertices V v[128], out indices uint3 tri[64])
{
    SetMeshOutputCounts(128, 64);
    uint cell = p.base + g;
    float2 at = float2(cell % 64, cell / 64 % 64) / 32.0 - 1;
    float2 q = float2(t % 16, t / 16) / 512.0;
    v[t].pos = float4(at + q, 0.5, 1);
    v[t].uv = q;
    if (t < 64) tri[t] = uint3(t, t + 1, t + 16);
}
float4 ps_main(V i) : SV_Target { return float4(i.uv * 64, 0.5, 1); }
)";
    static const char *fb_hlsl = R"(
Texture2D<float4> tex : register(t0);
FeedbackTexture2D<SAMPLER_FEEDBACK_MIN_MIP> fb : register(u0);
RWByteAddressBuffer o : register(u1);
SamplerState s : register(s0);
cbuffer C : register(b0) { float4 rc; };
[numthreads(8, 8, 1)] void cs_feedback(uint2 t : SV_DispatchThreadID)
{
    float2 uv = t / float2(1920, 1080);
    fb.WriteSamplerFeedbackLevel(tex, s, uv, (t.x & 7) * 0.5);
}
[numthreads(8, 8, 1)] void cs_sample(uint2 t : SV_DispatchThreadID)
{
    float2 uv = t / float2(1920, 1080);
    float4 c = tex.SampleLevel(s, uv, (t.x & 7) * 0.5);
    if (c.r > 2.0) o.Store(0, 1);
}
)";
    static const char *bindless6_hlsl = R"(
cbuffer RC : register(b0) { float4 rc; uint4 idx; };
SamplerState smp : register(s0);
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; nointerpolation uint index : INDEX; };
V vs_quad(uint id : SV_VertexID)
{
    V o;
    float2 q = float2((id << 1) & 2, id & 2);
    o.uv = q;
    o.pos = float4((q * 2 - 1) * rc.z + rc.xy, 0.5, 1);
    o.index = idx.x;
    return o;
}
float4 ps_main(V i) : SV_Target { Texture2D t = ResourceDescriptorHeap[NonUniformResourceIndex(i.index)]; return t.Sample(smp, i.uv); }
)";
    static const char *wave_hlsl = R"(
RWByteAddressBuffer o : register(u0);
[numthreads(256, 1, 1)] void main(uint t : SV_DispatchThreadID)
{
    uint v = o.Load((t & 0xfffff) * 4);
    uint prefix = WavePrefixSum(v);
    uint total = WaveActiveSum(v);
    uint mx = WaveActiveMax(v + prefix);
    if (WaveIsFirstLane()) o.Store(((t >> 5) & 0xfffff) * 4, total + mx);
}
)";
    static const char *atom_hlsl = R"(
RWStructuredBuffer<uint64_t> o : register(u0);
RWStructuredBuffer<uint> o32 : register(u0);
cbuffer C : register(b0) { uint4 rc; };
[numthreads(64, 1, 1)] void contended(uint t : SV_DispatchThreadID) { InterlockedAdd(o[t & 15], (uint64_t)(t + 1) << 33); }
[numthreads(64, 1, 1)] void spread(uint t : SV_DispatchThreadID) { InterlockedAdd(o[t & 65535], (uint64_t)(t + 1) << 33); }
[numthreads(64, 1, 1)] void contended32(uint t : SV_DispatchThreadID) { InterlockedAdd(o32[t & 15], t + 1); }
)";

    auto rt_init = [](UINT grid, UINT instances) {
        if (!raytracing_supported()) return false;
        make_rt_geometry(rt, grid, instances);
        rt_out = create_buffer(1920 * 1080 * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        rt_rs = srv_uav_root_signature();
        run_init_commands([](ID3D12GraphicsCommandList *l) {
            ID3D12GraphicsCommandList4 *l4 = nullptr;
            CHECK(l->QueryInterface(IID_PPV_ARGS(&l4)));
            build_blas(l4, rt);
            build_tlas(l4, rt);
            l4->Release();
        });
        return true;
    };
    auto list4 = [] { ID3D12GraphicsCommandList4 *l4 = nullptr; CHECK(p.list->QueryInterface(IID_PPV_ARGS(&l4))); return l4; };
    auto math = [](const char *type, const char *type3) {
        return std::string("#define T ") + type + "\n#define T3 " + type3 + "\n" + common_hlsl + R"(
float4 ps_math(V i) : SV_Target
{
    T3 c = T3(i.uv, rc.w);
    [loop] for (int k = 0; k < 32; k++)
        c = frac(c * T(1.37) + sin(c.zxy * T(4.1) + T(k)) * T(0.5));
    return float4(c, 1);
}
)";
    };

    std::vector<Scene> v = {
        { "fill_dxil", "the fill scene's heavy pixel shader compiled as DXIL (compare fill, DXBC)", [] {
            PsoDesc d; d.ps = "ps_heavy"; d.vs_target = "vs_6_0"; d.ps_target = "ps_6_0"; fill6 = create_pso(d);
        }, [](UINT f) {
            set_quad_state(p.list, fill6);
            for (UINT i = 0; i < 20; i++) fullscreen(p.list, f * 0.01f + i);
        }, false, 0, true },
        { "sample_dxil", "the sample scene's 8-texture pixel shader compiled as DXIL (compare sample, DXBC)", [] {
            PsoDesc d; d.ps = "ps_tex"; d.vs_target = "vs_6_0"; d.ps_target = "ps_6_0"; sample6 = create_pso(d);
        }, [](UINT f) {
            set_quad_state(p.list, sample6);
            for (UINT i = 0; i < 20; i++) fullscreen(p.list, f * 0.01f);
        }, false, 0, true },
        { "pso_cold_dxil", "4 new graphics pipelines per frame from never-seen DXIL (driver compile: dxil-spirv, SPIRV-Cross, Metal)", [] {
            // Compile every variant the run will use now (the aarch64 prepare run fills the DXIL cache).
            UINT n = 4 * (g_frames_total + 1);
            for (UINT i = 0; i < n; i++)
            {
                std::string src = salted(common_hlsl, p.salt * 100000 + i + 1);
                cold_code.push_back(shader_code(src, "vs_quad", "vs_6_0"));
                cold_code.push_back(shader_code(src, (i & 1) ? "ps_heavy" : "ps_tex", "ps_6_0"));
            }
        }, [](UINT f) {
            double create_ms = 0, first_use_ms = 0;
            for (UINT i = 0; i < 4; i++)
            {
                UINT k = (f * 4 + i) % (cold_code.size() / 2);
                D3D12_GRAPHICS_PIPELINE_STATE_DESC g = {};
                g.pRootSignature = p.root_signature;
                g.VS = { cold_code[k * 2].data(), cold_code[k * 2].size() };
                g.PS = { cold_code[k * 2 + 1].data(), cold_code[k * 2 + 1].size() };
                g.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
                g.SampleMask = UINT_MAX;
                g.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
                g.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
                g.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                g.NumRenderTargets = 1;
                g.RTVFormats[0] = HDR_FORMAT;
                g.SampleDesc.Count = 1;
                ID3D12PipelineState *pso = nullptr;
                double t = qpc_ms();
                CHECK(p.device->CreateGraphicsPipelineState(&g, IID_PPV_ARGS(&pso)));
                create_ms += qpc_ms() - t;
                first_use_ms += first_use(pso);
                pso->Release();
            }
            extras.add("create_ms_per_pso", create_ms / 4);
            extras.add("first_use_ms_per_pso", first_use_ms / 4);
        }, false, 0, true },
        { "pso_cold_dxbc", "the same with DXBC compiled up front: driver pipeline creation only (dxbc-spirv front end)", [] {
            UINT n = 4 * (g_frames_total + 1);
            for (UINT i = 0; i < n; i++)
            {
                std::string src = salted(common_hlsl, p.salt * 100000 + i + 1);
                cold_code.push_back(shader_code(src, "vs_quad", "vs_5_0"));
                cold_code.push_back(shader_code(src, (i & 1) ? "ps_heavy" : "ps_tex", "ps_5_0"));
            }
        }, [](UINT f) {
            double create_ms = 0, first_use_ms = 0;
            for (UINT i = 0; i < 4; i++)
            {
                UINT k = (f * 4 + i) % (cold_code.size() / 2);
                D3D12_GRAPHICS_PIPELINE_STATE_DESC g = {};
                g.pRootSignature = p.root_signature;
                g.VS = { cold_code[k * 2].data(), cold_code[k * 2].size() };
                g.PS = { cold_code[k * 2 + 1].data(), cold_code[k * 2 + 1].size() };
                g.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
                g.SampleMask = UINT_MAX;
                g.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
                g.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
                g.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                g.NumRenderTargets = 1;
                g.RTVFormats[0] = HDR_FORMAT;
                g.SampleDesc.Count = 1;
                ID3D12PipelineState *pso = nullptr;
                double t = qpc_ms();
                CHECK(p.device->CreateGraphicsPipelineState(&g, IID_PPV_ARGS(&pso)));
                create_ms += qpc_ms() - t;
                first_use_ms += first_use(pso);
                pso->Release();
            }
            extras.add("create_ms_per_pso", create_ms / 4);
            extras.add("first_use_ms_per_pso", first_use_ms / 4);
        } },
        { "mesh", "amplification + mesh shaders: 64 x 64 mesh groups of 64 triangles (262144 triangles)", [] {
            std::vector<char> as = shader_code(mesh_hlsl, "as_main", "as_6_5"), ms = shader_code(mesh_hlsl, "ms_main", "ms_6_5"),
                              ps = shader_code(mesh_hlsl, "ps_main", "ps_6_5");
            D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7 = {};
            p.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof(o7));
            if (o7.MeshShaderTier < D3D12_MESH_SHADER_TIER_1) { unsupported("mesh shaders"); return; }
            D3D12_ROOT_SIGNATURE_DESC rsd = {};
            mesh_rs = serialize_root_signature(rsd);
            D3D12_RASTERIZER_DESC rast = {};
            rast.FillMode = D3D12_FILL_MODE_SOLID;
            rast.CullMode = D3D12_CULL_MODE_NONE;
            rast.DepthClipEnable = TRUE;
            D3D12_BLEND_DESC blend = {};
            blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            D3D12_RT_FORMAT_ARRAY rtv_formats = {};
            rtv_formats.NumRenderTargets = 1;
            rtv_formats.RTFormats[0] = BACKBUFFER_FORMAT;
            struct
            {
                StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature *> rs;
                StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS, D3D12_SHADER_BYTECODE> as;
                StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE> ms;
                StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> ps;
                StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> rast;
                StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC> blend;
                StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT> mask;
                StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> rtvs;
                StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC> sample;
            } stream;
            stream.rs.value = mesh_rs;
            stream.as.value = { as.data(), as.size() };
            stream.ms.value = { ms.data(), ms.size() };
            stream.ps.value = { ps.data(), ps.size() };
            stream.rast.value = rast;
            stream.blend.value = blend;
            stream.mask.value = ~0u;
            stream.rtvs.value = rtv_formats;
            stream.sample.value = { 1, 0 };
            D3D12_PIPELINE_STATE_STREAM_DESC sd = { sizeof(stream), &stream };
            ID3D12Device2 *dev2 = nullptr;
            CHECK(p.device->QueryInterface(IID_PPV_ARGS(&dev2)));
            CHECK(dev2->CreatePipelineState(&sd, IID_PPV_ARGS(&mesh_pso)));
            dev2->Release();
        }, [](UINT) {
            if (!mesh_pso) return;
            ID3D12GraphicsCommandList6 *l6 = nullptr;
            CHECK(p.list->QueryInterface(IID_PPV_ARGS(&l6)));
            l6->SetGraphicsRootSignature(mesh_rs);
            l6->SetPipelineState(mesh_pso);
            l6->DispatchMesh(64, 1, 1);
            l6->Release();
        }, false, 0, true },
        { "rt_blas_build", "a 32768-triangle bottom-level acceleration structure rebuilt every frame", [rt_init] {
            if (!rt_init(128, 1)) unsupported("raytracing 1.1");
        }, [list4](UINT) {
            if (!rt.blas) return;
            auto *l4 = list4();
            build_blas(l4, rt);
            l4->Release();
        } },
        { "rt_tlas_build", "a top-level acceleration structure of 4096 instances rebuilt every frame", [rt_init] {
            if (!rt_init(16, 4096)) unsupported("raytracing 1.1");
        }, [list4](UINT) {
            if (!rt.tlas) return;
            auto *l4 = list4();
            build_tlas(l4, rt);
            l4->Release();
        } },
        { "rt_query", "inline ray tracing (RayQuery) in compute: one ray per 1080p pixel into 1024 instances of a 2048-triangle mesh", [rt_init] {
            if (!rt_init(32, 1024)) { unsupported("raytracing 1.1"); return; }
            rq_pso = compute_pso_from(rt_rs, shader_code(rq_hlsl, "main", "cs_6_5"));
        }, [](UINT) {
            if (!rq_pso) return;
            p.list->SetComputeRootSignature(rt_rs);
            p.list->SetPipelineState(rq_pso);
            p.list->SetComputeRootShaderResourceView(0, rt.tlas->GetGPUVirtualAddress());
            p.list->SetComputeRootUnorderedAccessView(1, rt_out->GetGPUVirtualAddress());
            p.list->Dispatch(1920 / 8, 1080 / 8, 1);
        }, false, 0, true },
        { "rt_dispatch_rays", "DispatchRays: one ray per 1080p pixel with closest-hit and miss shaders, same scene as rt_query", [rt_init] {
            if (!rt_init(32, 1024)) { unsupported("raytracing 1.1"); return; }
            std::vector<char> lib = shader_code(rays_hlsl, "", "lib_6_3");
            ID3D12Device5 *dev5 = nullptr;
            CHECK(p.device->QueryInterface(IID_PPV_ARGS(&dev5)));
            D3D12_DXIL_LIBRARY_DESC lib_desc = {};
            lib_desc.DXILLibrary = { lib.data(), lib.size() };
            D3D12_HIT_GROUP_DESC hg = {};
            hg.HitGroupExport = L"hg";
            hg.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
            hg.ClosestHitShaderImport = L"ch";
            D3D12_RAYTRACING_SHADER_CONFIG sc = { 4, 8 };
            D3D12_RAYTRACING_PIPELINE_CONFIG pc = { 1 };
            D3D12_GLOBAL_ROOT_SIGNATURE grs = { rt_rs };
            D3D12_STATE_SUBOBJECT so[5] = {
                { D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &lib_desc },
                { D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hg },
                { D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &sc },
                { D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pc },
                { D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &grs },
            };
            D3D12_STATE_OBJECT_DESC sod = { D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, 5, so };
            CHECK(dev5->CreateStateObject(&sod, IID_PPV_ARGS(&rt_so)));
            ID3D12StateObjectProperties *props = nullptr;
            CHECK(rt_so->QueryInterface(IID_PPV_ARGS(&props)));
            table = create_buffer(256, D3D12_HEAP_TYPE_UPLOAD);
            uint8_t *t;
            CHECK(table->Map(0, nullptr, (void **)&t));
            memset(t, 0, 256);
            const wchar_t *names[3] = { L"rg", L"ms", L"hg" };
            for (int i = 0; i < 3; i++)
                if (void *id = props->GetShaderIdentifier(names[i])) memcpy(t + 64 * i, id, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
            table->Unmap(0, nullptr);
            props->Release();
            dev5->Release();
        }, [list4](UINT) {
            if (!rt_so) return;
            auto *l4 = list4();
            D3D12_GPU_VIRTUAL_ADDRESS base = table->GetGPUVirtualAddress();
            D3D12_DISPATCH_RAYS_DESC rays = {};
            rays.RayGenerationShaderRecord = { base, 32 };
            rays.MissShaderTable = { base + 64, 32, 32 };
            rays.HitGroupTable = { base + 128, 32, 32 };
            rays.Width = 1920;
            rays.Height = 1080;
            rays.Depth = 1;
            l4->SetComputeRootSignature(rt_rs);
            l4->SetPipelineState1(rt_so);
            l4->SetComputeRootShaderResourceView(0, rt.tlas->GetGPUVirtualAddress());
            l4->SetComputeRootUnorderedAccessView(1, rt_out->GetGPUVirtualAddress());
            l4->DispatchRays(&rays);
            l4->Release();
        }, false, 0, true },
        { "sampler_feedback", "WriteSamplerFeedbackLevel for every 1080p pixel into a MinMip map of a 2048x2048 texture", [] {
            D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7 = {};
            p.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof(o7));
            if (o7.SamplerFeedbackTier < D3D12_SAMPLER_FEEDBACK_TIER_0_9) { unsupported("sampler feedback"); return; }
            ID3D12Device8 *dev8 = nullptr;
            CHECK(p.device->QueryInterface(IID_PPV_ARGS(&dev8)));
            fb_tex = create_texture(2048, 2048, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE,
                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, 12);
            D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
            D3D12_RESOURCE_DESC1 fd = {};
            fd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            fd.Width = 2048; fd.Height = 2048; fd.DepthOrArraySize = 1; fd.MipLevels = 12;
            fd.Format = (DXGI_FORMAT)189;   // DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE
            fd.SampleDesc.Count = 1;
            fd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            fd.SamplerFeedbackMipRegion = { 64, 64, 1 };
            CHECK(dev8->CreateCommittedResource2(&hp, D3D12_HEAP_FLAG_NONE, &fd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, nullptr,
                                                 IID_PPV_ARGS(&fb_map)));
            p.device->CreateShaderResourceView(fb_tex, nullptr, srv_cpu(SRV_FB));
            dev8->CreateSamplerFeedbackUnorderedAccessView(fb_tex, fb_map, srv_cpu(UAV_FB));
            dev8->Release();
            D3D12_DESCRIPTOR_RANGE ranges[2] = { { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 }, { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1 } };
            D3D12_ROOT_PARAMETER rp[3] = {};
            rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            rp[0].DescriptorTable = { 2, ranges };
            rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
            rp[1].Descriptor = { 1, 0 };
            rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            rp[2].Constants = { 0, 0, 4 };
            D3D12_STATIC_SAMPLER_DESC ss = linear_sampler();
            D3D12_ROOT_SIGNATURE_DESC rsd = { 3, rp, 1, &ss };
            fb_rs = serialize_root_signature(rsd);
            fb_pso = compute_pso_from(fb_rs, shader_code(fb_hlsl, "cs_feedback", "cs_6_5"));
            rt_out = create_buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }, [](UINT) {
            if (!fb_pso) return;
            p.list->SetComputeRootSignature(fb_rs);
            p.list->SetPipelineState(fb_pso);
            p.list->SetComputeRootDescriptorTable(0, srv_gpu(SRV_FB));
            p.list->SetComputeRootUnorderedAccessView(1, rt_out->GetGPUVirtualAddress());
            p.list->Dispatch(1920 / 8, 1080 / 8, 1);
        }, false, 0, true },
        { "sampler_feedback_base", "the same compute pass sampling instead of writing feedback (baseline for sampler_feedback)", [] {
            fb_tex = create_texture(2048, 2048, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE,
                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, 12);
            p.device->CreateShaderResourceView(fb_tex, nullptr, srv_cpu(SRV_FB));
            D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
            D3D12_ROOT_PARAMETER rp[3] = {};
            rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            rp[0].DescriptorTable = { 1, &range };
            rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
            rp[1].Descriptor = { 1, 0 };
            rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            rp[2].Constants = { 0, 0, 4 };
            D3D12_STATIC_SAMPLER_DESC ss = linear_sampler();
            D3D12_ROOT_SIGNATURE_DESC rsd = { 3, rp, 1, &ss };
            fb_rs = serialize_root_signature(rsd);
            fbbase_pso = compute_pso_from(fb_rs, shader_code(fb_hlsl, "cs_sample", "cs_6_5"));
            rt_out = create_buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }, [](UINT) {
            p.list->SetComputeRootSignature(fb_rs);
            p.list->SetPipelineState(fbbase_pso);
            p.list->SetComputeRootDescriptorTable(0, srv_gpu(SRV_FB));
            p.list->SetComputeRootUnorderedAccessView(1, rt_out->GetGPUVirtualAddress());
            p.list->Dispatch(1920 / 8, 1080 / 8, 1);
        }, false, 0, true },
        { "bindless_sm66", "2000 draws sampling ResourceDescriptorHeap[index] (SM 6.6 directly indexed heap)", [] {
            for (UINT i = 8; i < 8 + 1000; i++) p.device->CreateShaderResourceView(p.textures[i % 8], nullptr, srv_cpu(i));
            D3D12_ROOT_PARAMETER1 rp = {};
            rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            rp.Constants = { 0, 0, 8 };
            D3D12_STATIC_SAMPLER_DESC ss = linear_sampler();
            bindless6_rs = root_signature_1_1(1, &rp, D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED, 1, &ss);
            PsoDesc d; d.hlsl = bindless6_hlsl; d.ps = "ps_main"; d.vs_target = "vs_6_6"; d.ps_target = "ps_6_6"; d.rs = bindless6_rs;
            bindless6 = create_pso(d);
        }, [](UINT f) {
            p.list->SetGraphicsRootSignature(bindless6_rs);
            p.list->SetPipelineState(bindless6);
            p.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            double t = qpc_ms();
            for (UINT i = 0; i < 2000; i++)
            {
                uint32_t rc[8];
                float pos[4] = { (float)(i % 64) / 32.0f - 1.0f, (float)(i / 64 % 64) / 32.0f - 1.0f, 0.02f, f * 0.01f };
                memcpy(rc, pos, 16);
                rc[4] = 8 + i % 1000; rc[5] = rc[6] = rc[7] = 0;
                p.list->SetGraphicsRoot32BitConstants(0, 8, rc, 0);
                p.list->DrawInstanced(3, 1, 0, 0);
            }
            extras.add("record_us_per_draw", (qpc_ms() - t) * 1000.0 / 2000);
        }, false, 0, true },
        { "wave_ops", "wave prefix sum, sum and max over 8M elements (WavePrefixSum, WaveActiveSum, WaveActiveMax)", [] {
            rt_rs = srv_uav_root_signature();
            wave_pso = compute_pso_from(rt_rs, shader_code(wave_hlsl, "main", "cs_6_0"));
            wave_buf = create_buffer(4 << 20, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }, [](UINT) {
            p.list->SetComputeRootSignature(rt_rs);
            p.list->SetPipelineState(wave_pso);
            p.list->SetComputeRootUnorderedAccessView(1, wave_buf->GetGPUVirtualAddress());
            p.list->Dispatch((8 << 20) / 256, 1, 1);
        }, false, 0, true },
        { "fp16_math", "the heavy-math pixel shader with native 16-bit floats (-enable-16bit-types), 20 full-screen quads", [math] {
            math_src16 = math("half", "half3");
            PsoDesc d; d.hlsl = math_src16.c_str(); d.ps = "ps_math"; d.vs_target = "vs_6_2"; d.ps_target = "ps_6_2"; d.dxc_arg = L"-enable-16bit-types";
            math16 = create_pso(d);
        }, [](UINT f) {
            set_quad_state(p.list, math16);
            for (UINT i = 0; i < 20; i++) fullscreen(p.list, f * 0.01f + i);
        }, false, 0, true },
        { "fp32_math", "the same shader with 32-bit floats (baseline for fp16_math)", [math] {
            math_src32 = math("float", "float3");
            PsoDesc d; d.hlsl = math_src32.c_str(); d.ps = "ps_math"; d.vs_target = "vs_6_2"; d.ps_target = "ps_6_2";
            math32 = create_pso(d);
        }, [](UINT f) {
            set_quad_state(p.list, math32);
            for (UINT i = 0; i < 20; i++) fullscreen(p.list, f * 0.01f + i);
        }, false, 0, true },
    };
    // 64-bit atomics (emulated with a lock table on Apple GPUs without native support) and a 32-bit baseline.
    const char *atom_names[3][3] = {
        { "atomics64_contended", "contended", "1M 64-bit InterlockedAdds per frame onto 16 addresses" },
        { "atomics64_spread", "spread", "1M 64-bit InterlockedAdds per frame over 64K addresses" },
        { "atomics32_contended", "contended32", "1M 32-bit InterlockedAdds onto 16 addresses (baseline for atomics64_contended)" },
    };
    for (int k = 0; k < 3; k++)
    {
        const char *entry = atom_names[k][1];
        v.push_back({ atom_names[k][0], atom_names[k][2], [k, entry] {
            rt_rs = srv_uav_root_signature();
            atom_pso[k] = compute_pso_from(rt_rs, shader_code(atom_hlsl, entry, "cs_6_6"));
            atom_buf = create_buffer(65536 * 8, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }, [k](UINT) {
            p.list->SetComputeRootSignature(rt_rs);
            p.list->SetPipelineState(atom_pso[k]);
            p.list->SetComputeRootUnorderedAccessView(1, atom_buf->GetGPUVirtualAddress());
            p.list->Dispatch((1 << 20) / 64, 1, 1);
        }, false, 0, true });
    }
    return v;
}

// ---- Features a D3D12 implementation emulates or translates on Metal (each against a baseline) ----

static const char *feature_hlsl_tail = R"(
uint4 ps_uint(V i) : SV_Target { return uint4(uint2(i.uv * 255), (uint)rc.w, 255); }
struct VC { float4 pos : SV_Position; float2 uv : TEXCOORD0; float cull : SV_CullDistance; };
VC vs_cull(uint id : SV_VertexID) { V v = vs_quad(id); VC o; o.pos = v.pos; o.uv = v.uv; o.cull = 1.0 - rc.w; return o; }
float4 ps_cull(VC i) : SV_Target { return float4(i.uv, 0, 1); }
struct SO { float4 pos : SV_Position; float4 data : TEXCOORD0; };
SO vs_so(uint id : SV_VertexID) { SO o; o.pos = float4(0, 0, 0, 1); o.data = float4(id, id * 2, rc.w, 1); return o; }
[maxvertexcount(2)] void gs_so(point SO i[1], inout PointStream<SO> s) { s.Append(i[0]); SO o = i[0]; o.data.y += 1; s.Append(o); }
V vs_point(uint id : SV_VertexID)
{
    V o;
    o.pos = float4(float2(id % 200, id / 200 % 100) / float2(100, 50) - 1, 0.5, 1);
    o.uv = 0;
    return o;
}
[maxvertexcount(4)] void gs_quad(point V i[1], inout TriangleStream<V> s)
{
    for (uint k = 0; k < 4; k++) { V o = i[0]; o.pos.xy += float2(k & 1, k >> 1) * 0.008; o.uv = float2(k & 1, k >> 1); s.Append(o); }
}
[maxvertexcount(3)] void gs_pass(triangle V i[3], inout TriangleStream<V> s) { for (uint k = 0; k < 3; k++) s.Append(i[k]); }
struct TV { float2 pos : POS; };
TV vs_patch(uint id : SV_VertexID)
{
    uint p = id / 3, c = id % 3;
    TV o;
    o.pos = float2(p % 40, p / 40 % 25) / float2(20, 12.5) - 1 + float2(c == 1, c == 2) * 0.045;
    return o;
}
struct HSC { float e[3] : SV_TessFactor; float in_ : SV_InsideTessFactor; };
HSC hs_const(InputPatch<TV, 3> ip) { HSC c; c.e[0] = c.e[1] = c.e[2] = 16; c.in_ = 16; return c; }
[domain("tri")] [partitioning("integer")] [outputtopology("triangle_cw")] [outputcontrolpoints(3)] [patchconstantfunc("hs_const")]
TV hs_main(InputPatch<TV, 3> ip, uint i : SV_OutputControlPointID) { return ip[i]; }
[domain("tri")] V ds_main(HSC c, float3 b : SV_DomainLocation, const OutputPatch<TV, 3> op)
{
    V o;
    o.pos = float4(op[0].pos * b.x + op[1].pos * b.y + op[2].pos * b.z, 0.5, 1);
    o.uv = b.xy;
    return o;
}
// The same triangles as the tessellated patches would produce, drawn directly (baseline).
V vs_grid(uint id : SV_VertexID)
{
    uint tri = id / 3, c = id % 3, patch = tri / 256, sub = tri % 256;
    float2 at = float2(patch % 40, patch / 40 % 25) / float2(20, 12.5) - 1 + float2(sub % 16, sub / 16) * (0.045 / 16);
    V o;
    o.pos = float4(at + float2(c == 1, c == 2) * (0.045 / 16), 0.5, 1);
    o.uv = 0;
    return o;
}
)";

static const char *viewid_hlsl_tail = R"(
V vs_view(uint id : SV_VertexID, uint view : SV_ViewID) { V o = vs_quad(id); o.pos.x += view * 0.01; return o; }
float4 ps_inner(V i, uint ic : SV_InnerCoverage) : SV_Target { return float4(ic, i.uv, 1); }
)";

static const char *buffer_cs_hlsl = R"(
cbuffer RC : register(b0) { uint4 rc; };
Buffer<float4> typed_src : register(t0);
RWBuffer<float4> typed_dst : register(u0);
StructuredBuffer<float4> sb_src : register(t1);
RWStructuredBuffer<float4> sb_dst : register(u1);
[numthreads(64, 1, 1)] void cs_typed(uint t : SV_DispatchThreadID) { typed_dst[t] = typed_src[t] * 2 + rc.x; }
[numthreads(64, 1, 1)] void cs_struct(uint t : SV_DispatchThreadID) { sb_dst[t] = sb_src[t] * 2 + rc.x; }
[numthreads(64, 1, 1)] void cs_robust(uint t : SV_DispatchThreadID) { sb_dst[t] = sb_src[t * rc.y] * 2 + typed_src[t * rc.y]; }
)";

// Graphics root signature like p.root_signature, with a given static sampler filter and flags.
static ID3D12RootSignature *graphics_rs(D3D12_FILTER filter, D3D12_ROOT_SIGNATURE_FLAGS flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT)
{
    D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 8, 0, 0, 0 };
    D3D12_ROOT_PARAMETER rp[3] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants = { 0, 0, 4 };
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rp[1].Descriptor = { 1, 0 };
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[2].DescriptorTable = { 1, &range };
    D3D12_STATIC_SAMPLER_DESC ss = {};
    ss.Filter = filter;
    ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    ss.MaxLOD = D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 3, rp, 1, &ss, flags };
    return serialize_root_signature(rsd);
}

// Graphics pipeline through a pipeline state stream: VS + PS (+ view instancing or depth bounds).
static ID3D12PipelineState *stream_pso(ID3D12RootSignature *rs, const std::vector<char> &vs, const std::vector<char> &ps, DXGI_FORMAT rt,
                                       bool depth_bounds, const D3D12_VIEW_INSTANCING_DESC *views)
{
    D3D12_RASTERIZER_DESC rast = {};
    rast.FillMode = D3D12_FILL_MODE_SOLID;
    rast.CullMode = D3D12_CULL_MODE_NONE;
    rast.DepthClipEnable = TRUE;
    D3D12_BLEND_DESC blend = {};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    D3D12_RT_FORMAT_ARRAY rts = {};
    rts.NumRenderTargets = 1;
    rts.RTFormats[0] = rt;
    D3D12_DEPTH_STENCIL_DESC1 ds = {};
    ds.DepthEnable = depth_bounds;
    ds.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    ds.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    ds.DepthBoundsTestEnable = depth_bounds;
    struct
    {
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature *> rs;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, D3D12_SHADER_BYTECODE> vs;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> ps;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> rast;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC> blend;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT> mask;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE> topo;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> rtvs;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, DXGI_FORMAT> dsv;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1, D3D12_DEPTH_STENCIL_DESC1> ds;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC> sample;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING, D3D12_VIEW_INSTANCING_DESC> views;
    } stream;
    stream.rs.value = rs;
    stream.vs.value = { vs.data(), vs.size() };
    stream.ps.value = { ps.data(), ps.size() };
    stream.rast.value = rast;
    stream.blend.value = blend;
    stream.mask.value = ~0u;
    stream.topo.value = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    stream.rtvs.value = rts;
    stream.dsv.value = depth_bounds ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_UNKNOWN;
    stream.ds.value = ds;
    stream.sample.value = { 1, 0 };
    stream.views.value = views ? *views : D3D12_VIEW_INSTANCING_DESC{ 0, nullptr, D3D12_VIEW_INSTANCING_FLAG_NONE };
    D3D12_PIPELINE_STATE_STREAM_DESC sd = { sizeof(stream), &stream };
    ID3D12Device2 *dev2 = nullptr;
    CHECK(p.device->QueryInterface(IID_PPV_ARGS(&dev2)));
    ID3D12PipelineState *pso = nullptr;
    CHECK(dev2->CreatePipelineState(&sd, IID_PPV_ARGS(&pso)));
    dev2->Release();
    return pso;
}

static std::vector<Scene> feature_scenes()
{
    static ID3D12PipelineState *pso_a;
    static ID3D12RootSignature *rs_a, *cs_rs;
    static ID3D12Resource *target, *aux, *so_buf, *so_counter, *zero, *res_a, *res_b, *res_c, *res_d, *reserved, *args;
    static ID3D12Heap *tile_heap;
    static ID3D12CommandSignature *sig;
    static std::string feat_src, viewid_src;
    static UINT tile_count, tile_side;
    enum { RTV_FEATURE = FRAMES_IN_FLIGHT + 8, SRV_FEATURE = 11100, SRV_TILED = 11200, CS_TABLE = 11300 };
    feat_src = std::string(common_hlsl) + feature_hlsl_tail;
    viewid_src = std::string(common_hlsl) + viewid_hlsl_tail;

    auto quads = [](UINT n, float scale, UINT frame) {
        for (UINT i = 0; i < n; i++)
        {
            float rc[4] = { (float)(i % 64) / 32.0f - 1.0f, (float)(i / 64 % 64) / 32.0f - 1.0f, scale, frame * 0.01f };
            p.list->SetGraphicsRoot32BitConstants(0, 4, rc, 0);
            p.list->DrawInstanced(3, 1, 0, 0);
        }
    };
    auto bind = [](ID3D12RootSignature *rs, ID3D12PipelineState *pso) {
        p.list->SetGraphicsRootSignature(rs ? rs : p.root_signature);
        p.list->SetPipelineState(pso);
        p.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        void *cpu;
        D3D12_GPU_VIRTUAL_ADDRESS cb = upload_alloc(256, &cpu);
        memset(cpu, 0, 256);
        p.list->SetGraphicsRootConstantBufferView(1, cb);
        p.list->SetGraphicsRootDescriptorTable(2, srv_gpu(0));
    };

    // Min/max sampler reduction (emulated in the shader on Apple GPUs): sample with a min, max or plain linear filter.
    auto sampler_scene = [](D3D12_FILTER filter) {
        return std::function<void()>([filter] {
            rs_a = graphics_rs(filter);
            PsoDesc d; d.ps = "ps_tex"; d.rs = rs_a; pso_a = create_pso(d);
        });
    };
    auto sampler_frame = [bind](UINT f) { bind(rs_a, pso_a); for (UINT i = 0; i < 20; i++) fullscreen(p.list, f * 0.01f); };

    // Conservative rasterization (a generated mesh stage on Metal) on 2000 small triangles.
    auto conservative_init = [](D3D12_CONSERVATIVE_RASTERIZATION_MODE mode, bool inner) {
        return std::function<void()>([mode, inner] {
            if (inner) { shader_code(viewid_src, "vs_quad", "vs_6_0"); shader_code(viewid_src, "ps_inner", "ps_6_0"); }
            D3D12_FEATURE_DATA_D3D12_OPTIONS o = {};
            p.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof(o));
            if (mode && o.ConservativeRasterizationTier < (inner ? D3D12_CONSERVATIVE_RASTERIZATION_TIER_3 : D3D12_CONSERVATIVE_RASTERIZATION_TIER_1))
            { unsupported("conservative rasterization tier"); return; }
            PsoDesc d;
            if (inner) { d.hlsl = viewid_src.c_str(); d.ps = "ps_inner"; d.vs_target = "vs_6_0"; d.ps_target = "ps_6_0"; }
            d.tweak = [mode](D3D12_GRAPHICS_PIPELINE_STATE_DESC &g) { g.RasterizerState.ConservativeRaster = mode; };
            pso_a = create_pso(d);
        });
    };
    auto conservative_frame = [bind, quads](UINT f) { if (!pso_a) return; bind(nullptr, pso_a); quads(2000, 0.015f, f); };

    // Depth bounds (emulated against a depth snapshot) vs the same depth-tested draws without.
    auto bounds_init = [](bool bounds) {
        return std::function<void()>([bounds] {
            D3D12_FEATURE_DATA_D3D12_OPTIONS2 o2 = {};
            p.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS2, &o2, sizeof(o2));
            if (bounds && !o2.DepthBoundsTestSupported) { unsupported("depth bounds"); return; }
            std::vector<char> vs = shader_code(common_hlsl, "vs_quad", "vs_5_0"), ps = shader_code(common_hlsl, "ps_flat", "ps_5_0");
            pso_a = stream_pso(p.root_signature, vs, ps, BACKBUFFER_FORMAT, true, nullptr);
            if (!bounds)
            {
                PsoDesc d; d.depth = true; d.depth_write = false; pso_a = create_pso(d);
            }
        });
    };
    auto bounds_frame = [bind](bool bounds) {
        return std::function<void(UINT)>([bind, bounds](UINT f) {
            if (!pso_a) return;
            auto h = rtv(p.frame_index);
            auto d = dsv(0);
            p.list->ClearDepthStencilView(d, D3D12_CLEAR_FLAG_DEPTH, 0.5f, 0, 0, nullptr);
            p.list->OMSetRenderTargets(1, &h, FALSE, &d);
            if (bounds)
            {
                ID3D12GraphicsCommandList1 *l1 = nullptr;
                CHECK(p.list->QueryInterface(IID_PPV_ARGS(&l1)));
                l1->OMSetDepthBounds(0.2f, 0.8f);
                l1->Release();
            }
            bind(nullptr, pso_a);
            for (UINT i = 0; i < 40; i++) fullscreen(p.list, f * 0.01f);
        });
    };

    // Logic ops (framebuffer fetch on Metal) into an RGBA8_UINT target vs a plain write.
    auto logic_init = [](bool logic) {
        return std::function<void()>([logic] {
            D3D12_FEATURE_DATA_D3D12_OPTIONS o = {};
            p.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof(o));
            if (logic && !o.OutputMergerLogicOp) { unsupported("logic ops"); return; }
            target = create_texture(W, H, DXGI_FORMAT_R8G8B8A8_UINT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET);
            p.device->CreateRenderTargetView(target, nullptr, rtv(RTV_FEATURE));
            PsoDesc d; d.hlsl = feat_src.c_str(); d.ps = "ps_uint"; d.rt = DXGI_FORMAT_R8G8B8A8_UINT;
            d.tweak = [logic](D3D12_GRAPHICS_PIPELINE_STATE_DESC &g) {
                g.BlendState.RenderTarget[0].LogicOpEnable = logic;
                g.BlendState.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_XOR;
            };
            pso_a = create_pso(d);
        });
    };
    auto logic_frame = [bind, quads](UINT f) {
        if (!pso_a) return;
        auto h = rtv(RTV_FEATURE);
        p.list->OMSetRenderTargets(1, &h, FALSE, nullptr);
        bind(nullptr, pso_a);
        for (UINT i = 0; i < 20; i++) fullscreen(p.list, (float)(f & 255));
        quads(1000, 0.02f, f);
        auto bb = rtv(p.frame_index);
        p.list->OMSetRenderTargets(1, &bb, FALSE, nullptr);
    };

    // Stream output (transform feedback, which Metal has none of) of 1M points per frame.
    auto xfb_init = [](const char *vs_target, bool gs) {
        return std::function<void()>([vs_target, gs] {
            rs_a = graphics_rs(D3D12_FILTER_MIN_MAG_MIP_LINEAR,
                               D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT);
            PsoDesc d; d.hlsl = feat_src.c_str(); d.vs = "vs_so"; d.ps = nullptr; d.num_rt = 0; d.rs = rs_a; d.vs_target = vs_target;
            if (gs) d.gs = "gs_so";
            d.tweak = [](D3D12_GRAPHICS_PIPELINE_STATE_DESC &g) {
                static D3D12_SO_DECLARATION_ENTRY decl[1] = { { 0, "TEXCOORD", 0, 0, 4, 0 } };
                static UINT strides[1] = { 16 };
                g.StreamOutput = { decl, 1, strides, 1, D3D12_SO_NO_RASTERIZED_STREAM };
                g.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
            };
            pso_a = create_pso(d);
            so_buf = create_buffer(64 << 20, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_STREAM_OUT);
            so_counter = create_buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_STREAM_OUT);
            zero = create_buffer(256, D3D12_HEAP_TYPE_UPLOAD);
            void *z; CHECK(zero->Map(0, nullptr, &z)); memset(z, 0, 256); zero->Unmap(0, nullptr);
        });
    };
    auto xfb_frame = [](bool gs) {
        return std::function<void(UINT)>([gs](UINT f) {
            barrier(p.list, so_counter, D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_DEST);
            p.list->CopyBufferRegion(so_counter, 0, zero, 0, 16);
            barrier(p.list, so_counter, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);
            p.list->SetGraphicsRootSignature(rs_a);
            p.list->SetPipelineState(pso_a);
            float rc[4] = { 0, 0, 1, f * 0.01f };
            p.list->SetGraphicsRoot32BitConstants(0, 4, rc, 0);
            p.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
            D3D12_STREAM_OUTPUT_BUFFER_VIEW view = { so_buf->GetGPUVirtualAddress(), 64 << 20, so_counter->GetGPUVirtualAddress() };
            p.list->SOSetTargets(0, 1, &view);
            p.list->DrawInstanced(gs ? (1 << 19) : (1 << 20), 1, 0, 0);
            p.list->SOSetTargets(0, 0, nullptr);
        });
    };

    // Geometry shaders (emulated through object/mesh stages on Metal).
    auto gs_init = [](const char *gs, const char *vs, const char *vs_target, D3D12_PRIMITIVE_TOPOLOGY_TYPE topo) {
        return std::function<void()>([gs, vs, vs_target, topo] {
            PsoDesc d; d.hlsl = feat_src.c_str(); d.vs = vs; d.vs_target = vs_target; d.ps_target = vs_target[3] == '6' ? "ps_6_0" : "ps_5_0";
            d.gs = gs;
            d.tweak = [topo](D3D12_GRAPHICS_PIPELINE_STATE_DESC &g) { g.PrimitiveTopologyType = topo; };
            pso_a = create_pso(d);
        });
    };
    // Tessellation (hull and domain shaders run in compute and mesh stages on Metal).
    auto tess_init = [](const char *vs_target) {
        return std::function<void()>([vs_target] {
            PsoDesc d; d.hlsl = feat_src.c_str(); d.vs = "vs_patch"; d.hs = "hs_main"; d.ds = "ds_main"; d.vs_target = vs_target;
            d.ps_target = vs_target[3] == '6' ? "ps_6_0" : "ps_5_0";
            pso_a = create_pso(d);
        });
    };

    // Typed and structured buffer access in compute; robustness with out-of-bounds indices and null descriptors.
    auto buffers_init = [](bool null_descriptors) {
        return std::function<void()>([null_descriptors] {
            D3D12_DESCRIPTOR_RANGE ranges[2] = { { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0 }, { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 2 } };
            D3D12_ROOT_PARAMETER rp[2] = {};
            rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            rp[0].Constants = { 0, 0, 4 };
            rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            rp[1].DescriptorTable = { 2, ranges };
            D3D12_ROOT_SIGNATURE_DESC rsd = { 2, rp };
            cs_rs = serialize_root_signature(rsd);
            const UINT n = 1 << 20;
            res_a = create_buffer((UINT64)n * 4 + 256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            res_b = create_buffer((UINT64)n * 16 + 256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            res_c = create_buffer((UINT64)n * 16, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            res_d = create_buffer((UINT64)n * 16, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            // Typed views at element offsets that aren't 16-byte aligned: RGBA8 SRV from element 1, RGBA32F UAV from element 3.
            D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
            sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Buffer = { 1, n, 0, D3D12_BUFFER_SRV_FLAG_NONE };
            p.device->CreateShaderResourceView(null_descriptors ? nullptr : res_a, &sd, srv_cpu(CS_TABLE));
            sd.Format = DXGI_FORMAT_UNKNOWN;
            sd.Buffer = { 0, n / 2, 16, D3D12_BUFFER_SRV_FLAG_NONE };   // half the elements: indices past it are out of bounds
            p.device->CreateShaderResourceView(res_c, &sd, srv_cpu(CS_TABLE + 1));
            D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
            ud.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            ud.Buffer = { 3, n, 0, 0, D3D12_BUFFER_UAV_FLAG_NONE };
            p.device->CreateUnorderedAccessView(res_b, nullptr, &ud, srv_cpu(CS_TABLE + 2));
            ud.Format = DXGI_FORMAT_UNKNOWN;
            ud.Buffer = { 0, n, 16, 0, D3D12_BUFFER_UAV_FLAG_NONE };
            p.device->CreateUnorderedAccessView(res_d, nullptr, &ud, srv_cpu(CS_TABLE + 3));
        });
    };
    auto buffers_frame = [](const char *entry, UINT stride) {
        return std::function<void(UINT)>([entry, stride](UINT f) {
            if (!pso_a) pso_a = compute_pso_from(cs_rs, shader_code(buffer_cs_hlsl, entry, "cs_5_0"));
            p.list->SetComputeRootSignature(cs_rs);
            p.list->SetPipelineState(pso_a);
            uint32_t rc[4] = { f, stride, 0, 0 };
            p.list->SetComputeRoot32BitConstants(0, 4, rc, 0);
            p.list->SetComputeRootDescriptorTable(1, srv_gpu(CS_TABLE));
            p.list->Dispatch((1 << 20) / 64, 1, 1);
        });
    };

    // Tiled (reserved) resources.
    auto tiled_init = [](bool three_d) {
        return std::function<void()>([three_d] {
            D3D12_FEATURE_DATA_D3D12_OPTIONS o = {};
            p.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof(o));
            if (o.TiledResourcesTier < (three_d ? D3D12_TILED_RESOURCES_TIER_3 : D3D12_TILED_RESOURCES_TIER_1)) { unsupported("tiled resources tier"); return; }
            D3D12_RESOURCE_DESC rd = {};
            rd.Dimension = three_d ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width = three_d ? 256 : 4096;
            rd.Height = three_d ? 256 : 4096;
            rd.DepthOrArraySize = three_d ? 256 : 1;
            rd.MipLevels = 1;
            rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            rd.SampleDesc.Count = 1;
            rd.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;
            CHECK(p.device->CreateReservedResource(&rd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&reserved)));
            UINT subresources = 1;
            D3D12_PACKED_MIP_INFO packed;
            D3D12_TILE_SHAPE shape;
            D3D12_SUBRESOURCE_TILING tiling;
            p.device->GetResourceTiling(reserved, &tile_count, &packed, &shape, &subresources, 0, &tiling);
            tile_side = tiling.WidthInTiles;
            D3D12_HEAP_DESC hd = { (UINT64)tile_count / 2 * 65536, { D3D12_HEAP_TYPE_DEFAULT }, 0,
                                   D3D12_HEAP_FLAG_DENY_BUFFERS | D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES };
            CHECK(p.device->CreateHeap(&hd, IID_PPV_ARGS(&tile_heap)));
            // Map every other tile.
            std::vector<D3D12_TILED_RESOURCE_COORDINATE> coords;
            std::vector<D3D12_TILE_REGION_SIZE> sizes;
            std::vector<UINT> starts, counts;
            std::vector<D3D12_TILE_RANGE_FLAGS> flags;
            for (UINT t = 0; t < tile_count; t += 2)
            {
                UINT x = t % tiling.WidthInTiles, y = t / tiling.WidthInTiles % tiling.HeightInTiles, z = t / (tiling.WidthInTiles * tiling.HeightInTiles);
                coords.push_back({ x, y, z, 0 });
                sizes.push_back({ 1, FALSE, 0, 0, 0 });
                starts.push_back(t / 2);
                counts.push_back(1);
                flags.push_back(D3D12_TILE_RANGE_FLAG_NONE);
            }
            p.queue->UpdateTileMappings(reserved, (UINT)coords.size(), coords.data(), sizes.data(), tile_heap, (UINT)starts.size(),
                                        flags.data(), starts.data(), counts.data(), D3D12_TILE_MAPPING_FLAG_NONE);
            for (UINT i = 0; i < 8; i++) p.device->CreateShaderResourceView(reserved, nullptr, srv_cpu(SRV_TILED + i));
            PsoDesc d; d.ps = "ps_tex";
            pso_a = three_d ? nullptr : create_pso(d);
            if (three_d)
            {
                // A separate root signature: table of one Texture3D SRV at t0.
                std::vector<char> vs = shader_code(common_hlsl, "vs_quad", "vs_5_0");
                std::string psrc = std::string("struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n") +
                                   "Texture3D<float4> vol : register(t0);\nSamplerState smp : register(s0);\n"
                                   "float4 ps_vol(V i) : SV_Target { float4 c = 0; [unroll] for (int k = 0; k < 8; k++) c += vol.Sample(smp, float3(i.uv, (k + 0.5) / 8)); return c * 0.125; }\n";
                std::vector<char> ps = shader_code(psrc, "ps_vol", "ps_5_0");
                D3D12_GRAPHICS_PIPELINE_STATE_DESC g = {};
                g.pRootSignature = p.root_signature;
                g.VS = { vs.data(), vs.size() };
                g.PS = { ps.data(), ps.size() };
                g.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
                g.SampleMask = UINT_MAX;
                g.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
                g.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
                g.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                g.NumRenderTargets = 1;
                g.RTVFormats[0] = BACKBUFFER_FORMAT;
                g.SampleDesc.Count = 1;
                CHECK(p.device->CreateGraphicsPipelineState(&g, IID_PPV_ARGS(&pso_a)));
            }
        });
    };
    auto tiled_sample_frame = [bind](UINT f) {
        if (!pso_a) return;
        bind(nullptr, pso_a);
        p.list->SetGraphicsRootDescriptorTable(2, srv_gpu(SRV_TILED));
        for (UINT i = 0; i < 20; i++) fullscreen(p.list, f * 0.01f);
    };

    std::vector<Scene> v = {
        { "sample_linear", "20 full-screen quads, 8 linear samples each (baseline for sample_min/max)", sampler_scene(D3D12_FILTER_MIN_MAG_MIP_LINEAR), sampler_frame },
        { "sample_min", "the same with a MINIMUM reduction sampler (reduction emulated in the shader)", sampler_scene(D3D12_FILTER_MINIMUM_MIN_MAG_MIP_LINEAR), sampler_frame },
        { "sample_max", "the same with a MAXIMUM reduction sampler", sampler_scene(D3D12_FILTER_MAXIMUM_MIN_MAG_MIP_LINEAR), sampler_frame },
        { "conservative_off", "2000 small triangles (baseline for conservative_t1/t3)", conservative_init(D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF, false), conservative_frame },
        { "conservative_t1", "the same with conservative rasterization (generated mesh stage)", conservative_init(D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON, false), conservative_frame },
        { "conservative_t3", "conservative rasterization with SV_InnerCoverage (tier 3)", conservative_init(D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON, true), conservative_frame, false, 0, true },
        { "vrs_tier2", "20 full-screen heavy quads at a 2x2 shading rate with a shading-rate image (compare fill)", [] {
            D3D12_FEATURE_DATA_D3D12_OPTIONS6 o6 = {};
            p.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &o6, sizeof(o6));
            if (o6.VariableShadingRateTier < D3D12_VARIABLE_SHADING_RATE_TIER_2) { unsupported("VRS tier 2"); return; }
            PsoDesc d; d.ps = "ps_heavy"; pso_a = create_pso(d);
            UINT tile = o6.ShadingRateImageTileSize;
            aux = create_texture((W + tile - 1) / tile, (H + tile - 1) / tile, DXGI_FORMAT_R8_UINT, D3D12_RESOURCE_FLAG_NONE,
                                 D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE);
        }, [bind](UINT f) {
            if (!pso_a) return;
            ID3D12GraphicsCommandList5 *l5 = nullptr;
            CHECK(p.list->QueryInterface(IID_PPV_ARGS(&l5)));
            D3D12_SHADING_RATE_COMBINER comb[2] = { D3D12_SHADING_RATE_COMBINER_MAX, D3D12_SHADING_RATE_COMBINER_MAX };
            l5->RSSetShadingRate(D3D12_SHADING_RATE_2X2, comb);
            l5->RSSetShadingRateImage(aux);
            bind(nullptr, pso_a);
            for (UINT i = 0; i < 20; i++) fullscreen(p.list, f * 0.01f + i);
            l5->RSSetShadingRateImage(nullptr);
            l5->RSSetShadingRate(D3D12_SHADING_RATE_1X1, nullptr);
            l5->Release();
        } },
        { "depth_bounds_off", "40 full-screen depth-tested quads (baseline for depth_bounds)", bounds_init(false), bounds_frame(false) },
        { "depth_bounds", "the same with OMSetDepthBounds (emulated against a depth snapshot)", bounds_init(true), bounds_frame(true) },
        { "logic_op_off", "20 full-screen quads and 1000 small ones into an RGBA8_UINT target (baseline for logic_op)", logic_init(false), logic_frame },
        { "logic_op", "the same with an XOR logic op (framebuffer fetch on Metal)", logic_init(true), logic_frame },
        { "xfb_vs_dxbc", "stream output of 1M points per frame from a vertex shader (DXBC)", xfb_init("vs_5_0", false), xfb_frame(false) },
        { "xfb_vs_dxil", "the same from a DXIL vertex shader", xfb_init("vs_6_0", false), xfb_frame(false), false, 0, true },
        { "xfb_gs_dxbc", "stream output of 512K points expanded to 1M by a geometry shader (DXBC)", xfb_init("vs_5_0", true), xfb_frame(true) },
        { "gs_quads_dxbc", "a geometry shader expanding 20000 points into quads (DXBC)", gs_init("gs_quad", "vs_point", "vs_5_0", D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT),
          [](UINT f) { set_quad_state(p.list, pso_a); p.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST); p.list->DrawInstanced(20000, 1, 0, 0); } },
        { "gs_quads_dxil", "the same geometry shader compiled as DXIL", gs_init("gs_quad", "vs_point", "vs_6_0", D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT),
          [](UINT f) { set_quad_state(p.list, pso_a); p.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST); p.list->DrawInstanced(20000, 1, 0, 0); }, false, 0, true },
        { "gs_passthrough", "a pass-through geometry shader on 5000 small triangles (compare draws)", gs_init("gs_pass", "vs_quad", "vs_5_0", D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE),
          [quads](UINT f) { set_quad_state(p.list, pso_a); quads(5000, 0.01f, f); } },
        { "tess_dxbc", "1000 triangle patches at tessellation factor 16 (256000 triangles), DXBC", tess_init("vs_5_0"),
          [](UINT) { set_quad_state(p.list, pso_a); p.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST); p.list->DrawInstanced(3000, 1, 0, 0); } },
        { "tess_dxil", "the same compiled as DXIL", tess_init("vs_6_0"),
          [](UINT) { set_quad_state(p.list, pso_a); p.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST); p.list->DrawInstanced(3000, 1, 0, 0); }, false, 0, true },
        { "tess_baseline", "the same 256000 triangles drawn without tessellation (baseline for tess_*)", [] {
            PsoDesc d; d.hlsl = feat_src.c_str(); d.vs = "vs_grid"; pso_a = create_pso(d);
        }, [](UINT) { set_quad_state(p.list, pso_a); p.list->DrawInstanced(1000 * 256 * 3, 1, 0, 0); } },
        { "view_instancing", "2000 small quads rendered to 2 views of a texture array in one pass (SV_ViewID)", [] {
            // Compile first: the aarch64 prepare run must fill the DXIL cache even where the tier differs.
            std::vector<char> vs = shader_code(viewid_src, "vs_view", "vs_6_1"), ps = shader_code(viewid_src, "ps_flat", "ps_6_1");
            D3D12_FEATURE_DATA_D3D12_OPTIONS3 o3 = {};
            p.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &o3, sizeof(o3));
            if (o3.ViewInstancingTier < D3D12_VIEW_INSTANCING_TIER_1) { unsupported("view instancing"); return; }
            D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
            D3D12_RESOURCE_DESC rd = {};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width = W; rd.Height = H; rd.DepthOrArraySize = 2; rd.MipLevels = 1;
            rd.Format = BACKBUFFER_FORMAT;
            rd.SampleDesc.Count = 1;
            rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            CHECK(p.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)));
            D3D12_RENDER_TARGET_VIEW_DESC rv = {};
            rv.Format = BACKBUFFER_FORMAT;
            rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            rv.Texture2DArray = { 0, 0, 2, 0 };
            p.device->CreateRenderTargetView(target, &rv, rtv(RTV_FEATURE));
            static const D3D12_VIEW_INSTANCE_LOCATION loc[2] = { { 0, 0 }, { 0, 1 } };
            D3D12_VIEW_INSTANCING_DESC vid = { 2, loc, D3D12_VIEW_INSTANCING_FLAG_NONE };
            pso_a = stream_pso(p.root_signature, vs, ps, BACKBUFFER_FORMAT, false, &vid);
        }, [bind, quads](UINT f) {
            if (!pso_a) return;
            auto h = rtv(RTV_FEATURE);
            p.list->OMSetRenderTargets(1, &h, FALSE, nullptr);
            ID3D12GraphicsCommandList1 *l1 = nullptr;
            CHECK(p.list->QueryInterface(IID_PPV_ARGS(&l1)));
            l1->SetViewInstanceMask(3);
            l1->Release();
            bind(nullptr, pso_a);
            quads(2000, 0.02f, f);
            auto bb = rtv(p.frame_index);
            p.list->OMSetRenderTargets(1, &bb, FALSE, nullptr);
        }, false, 0, true },
        { "view_twice", "the same 2000 quads drawn separately into both slices (baseline for view_instancing)", [] {
            D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
            D3D12_RESOURCE_DESC rd = {};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width = W; rd.Height = H; rd.DepthOrArraySize = 2; rd.MipLevels = 1;
            rd.Format = BACKBUFFER_FORMAT;
            rd.SampleDesc.Count = 1;
            rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            CHECK(p.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)));
            for (UINT k = 0; k < 2; k++)
            {
                D3D12_RENDER_TARGET_VIEW_DESC rv = {};
                rv.Format = BACKBUFFER_FORMAT;
                rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                rv.Texture2DArray = { 0, k, 1, 0 };
                p.device->CreateRenderTargetView(target, &rv, rtv(RTV_FEATURE + k));
            }
            pso_a = create_pso({});
        }, [bind, quads](UINT f) {
            for (UINT k = 0; k < 2; k++)
            {
                auto h = rtv(RTV_FEATURE + k);
                p.list->OMSetRenderTargets(1, &h, FALSE, nullptr);
                bind(nullptr, pso_a);
                quads(2000, 0.02f, f);
            }
            auto bb = rtv(p.frame_index);
            p.list->OMSetRenderTargets(1, &bb, FALSE, nullptr);
        } },
        { "execute_indirect_vbib", "ExecuteIndirect of 2000 commands with root constant, vertex and index buffer tokens (compare execute_indirect)", [] {
            PsoDesc d; d.vs = "vs_input"; d.input = true; pso_a = create_pso(d);
            D3D12_INDIRECT_ARGUMENT_DESC ad[4] = {};
            ad[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
            ad[0].Constant = { 0, 0, 4 };
            ad[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW;
            ad[1].VertexBuffer.Slot = 0;
            ad[2].Type = D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW;
            ad[3].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
            struct Cmd { float rc[4]; D3D12_VERTEX_BUFFER_VIEW vbv; D3D12_INDEX_BUFFER_VIEW ibv; D3D12_DRAW_INDEXED_ARGUMENTS draw; };
            D3D12_COMMAND_SIGNATURE_DESC csd = { sizeof(Cmd), 4, ad, 0 };
            CHECK(p.device->CreateCommandSignature(&csd, p.root_signature, IID_PPV_ARGS(&sig)));
            res_a = create_buffer(1 << 20, D3D12_HEAP_TYPE_UPLOAD);   // vertices
            res_b = create_buffer(1 << 16, D3D12_HEAP_TYPE_UPLOAD);   // indices
            float *vtx; CHECK(res_a->Map(0, nullptr, (void **)&vtx));
            for (int i = 0; i < 4096; i++) { float q[5] = { (float)(i & 1), (float)(i >> 1 & 1), 0, 0, 0 }; memcpy(vtx + i * 5, q, sizeof(q)); }
            res_a->Unmap(0, nullptr);
            uint16_t *ix; CHECK(res_b->Map(0, nullptr, (void **)&ix));
            for (int i = 0; i < 1024; i++) ix[i] = (uint16_t)(i % 3);
            res_b->Unmap(0, nullptr);
            args = create_buffer(2000 * sizeof(Cmd), D3D12_HEAP_TYPE_UPLOAD);
            Cmd *c; CHECK(args->Map(0, nullptr, (void **)&c));
            for (UINT i = 0; i < 2000; i++)
            {
                float rc[4] = { (float)(i % 64) / 32.0f - 1.0f, (float)(i / 64 % 64) / 32.0f - 1.0f, 0.01f, 0.5f };
                memcpy(c[i].rc, rc, 16);
                c[i].vbv = { res_a->GetGPUVirtualAddress() + (i % 256) * 20 * 4, 20 * 64, 20 };
                c[i].ibv = { res_b->GetGPUVirtualAddress() + (i % 64) * 6, 600, DXGI_FORMAT_R16_UINT };
                c[i].draw = { 3, 1, 0, 0, 0 };
            }
            args->Unmap(0, nullptr);
        }, [bind](UINT) {
            bind(nullptr, pso_a);
            p.list->ExecuteIndirect(sig, 2000, args, 0, nullptr, 0);
        } },
        { "cull_distance", "5000 small quads whose vertex shader writes SV_CullDistance (compare draws)", [] {
            PsoDesc d; d.hlsl = feat_src.c_str(); d.vs = "vs_cull"; d.ps = "ps_cull"; pso_a = create_pso(d);
        }, [bind, quads](UINT f) { bind(nullptr, pso_a); quads(5000, 0.01f, f); } },
        { "typed_buffer", "1M typed loads (RGBA8 SRV) and stores (RGBA32F UAV) at unaligned element offsets in compute", buffers_init(false), buffers_frame("cs_typed", 1) },
        { "structured_buffer", "1M structured loads and stores in compute (baseline for typed_buffer and robustness)", buffers_init(false), buffers_frame("cs_struct", 1) },
        { "robust_oob", "1M loads, half of them out of bounds, from structured and typed views (robust access)", buffers_init(false), buffers_frame("cs_robust", 2) },
        { "null_descriptors", "the same reading through a null typed descriptor", buffers_init(true), buffers_frame("cs_robust", 2) },
        { "tiled_update", "UpdateTileMappings of 256 tiles per frame on a 4096x4096 reserved texture (streaming)", tiled_init(false), [](UINT f) {
            if (!reserved) return;
            std::vector<D3D12_TILED_RESOURCE_COORDINATE> coords(256);
            std::vector<D3D12_TILE_REGION_SIZE> sizes(256, { 1, FALSE, 0, 0, 0 });
            std::vector<UINT> starts(256), counts(256, 1);
            std::vector<D3D12_TILE_RANGE_FLAGS> flags(256, D3D12_TILE_RANGE_FLAG_NONE);
            for (UINT i = 0; i < 256; i++)
            {
                UINT t = (f * 256 + i * 2 + 1) % tile_count;
                coords[i] = { t % tile_side, t / tile_side, 0, 0 };
                starts[i] = (f * 256 + i) % (tile_count / 2);
            }
            double t = qpc_ms();
            p.queue->UpdateTileMappings(reserved, 256, coords.data(), sizes.data(), tile_heap, 256, flags.data(), starts.data(), counts.data(),
                                        D3D12_TILE_MAPPING_FLAG_NONE);
            extras.add("update_ms", qpc_ms() - t);
        } },
        { "tiled_sample", "20 full-screen quads sampling a half-mapped 4096x4096 reserved texture (compare sample)", tiled_init(false), tiled_sample_frame },
        { "tiled_3d_sample", "20 full-screen quads taking 8 samples each from a half-mapped 256^3 reserved 3D texture (tier 3)", tiled_init(true), tiled_sample_frame },
    };
    return v;
}

// ---- Heavier patterns of other engines (deferred shading, async queues, render graphs, GPU culling) ----

static const char *engine_hlsl_tail = R"(
struct GB { float4 a : SV_Target0; float4 b : SV_Target1; float4 c : SV_Target2; float4 d : SV_Target3; };
GB ps_gbuffer(V i)
{
    GB o;
    float4 t = tex[0].Sample(smp, i.uv);
    o.a = t; o.b = float4(i.uv, rc.w, 1); o.c = t * 4; o.d = float4(t.rgb * 0.5, 1);
    return o;
}
float4 ps_lighting(V i) : SV_Target
{
    float4 a = tex[0].Sample(smp, i.uv * 0.5), b = tex[1].Sample(smp, i.uv * 0.5), c = tex[2].Sample(smp, i.uv * 0.5), d = tex[3].Sample(smp, i.uv * 0.5);
    float3 n = normalize(b.xyz * 2 - 1 + 0.001);
    float l = saturate(dot(n, normalize(float3(0.3, 0.6, 0.7))));
    return float4(a.rgb * l + c.rgb * 0.1 + d.rgb * 0.2, 1);
}
TextureCube cube : register(t0);
Texture3D vol3 : register(t1);
Texture2DArray arr : register(t2);
float4 ps_cube3d(V i) : SV_Target
{
    float3 dir = normalize(float3(i.uv * 2 - 1, 0.5));
    float4 c = cube.Sample(smp, dir) + vol3.Sample(smp, float3(i.uv, 0.5)) + arr.Sample(smp, float3(i.uv, 3));
    c += cube.Sample(smp, dir.zxy) + vol3.Sample(smp, float3(i.uv.yx, 0.25)) + arr.Sample(smp, float3(i.uv.yx, 1));
    return c * 0.166;
}
)";

static const char *engine_cs_hlsl = R"(
cbuffer RC : register(b0) { uint4 rc; };
RWTexture2D<float4> img_a : register(u0);
RWTexture2D<float4> img_b : register(u1);
RWByteAddressBuffer args : register(u2);
[numthreads(8, 8, 1)] void cs_typed_rmw(uint2 t : SV_DispatchThreadID)
{
    img_a[t] = img_a[t] * 0.5 + 0.25;   // R11G11B10_FLOAT typed load + store
    img_b[t] = img_b[t] * 0.5 + 0.25;   // RGBA8_UNORM typed load + store
}
// GPU culling: one thread per object writes an indirect draw (root constants + draw) when "visible".
[numthreads(64, 1, 1)] void cs_cull(uint t : SV_DispatchThreadID)
{
    if (t >= 4000) return;
    bool visible = ((t * 2654435761u + rc.x) >> 28) < 8;   // about half
    if (!visible) return;
    uint slot;
    args.InterlockedAdd(4000 * 32, 1, slot);
    float4 r = float4(float(t % 64) / 32.0 - 1.0, float(t / 64 % 64) / 32.0 - 1.0, 0.01, 0.5);
    args.Store4(slot * 32, asuint(r));
    args.Store4(slot * 32 + 16, uint4(3, 1, 0, 0));
}
[numthreads(1, 1, 1)] void cs_reset(uint t : SV_DispatchThreadID) { args.Store(4000 * 32, 0); }
// Plain ALU work for the async compute queue.
[numthreads(64, 1, 1)] void cs_alu(uint t : SV_DispatchThreadID)
{
    float x = t;
    [loop] for (int k = 0; k < 256; k++) x = frac(x * 1.37 + sin(x));
    if (x > 2) args.Store(0, 1);
}
)";

static std::vector<Scene> engine_scenes()
{
    static ID3D12PipelineState *pso_a, *pso_b, *pso_c, *library_psos[20];
    static ID3D12RootSignature *cs_rs, *cbv_rs;
    static ID3D12Resource *mrt[4], *res_a, *res_b, *res_c, *aliases[20], *bc7_mips[4];
    static ID3D12Heap *alias_heap;
    static ID3D12CommandQueue *other_queue;
    static ID3D12CommandAllocator *other_alloc[FRAMES_IN_FLIGHT];
    static ID3D12GraphicsCommandList *other_list;
    static ID3D12Fence *other_fence;
    static UINT64 other_value;
    static ID3D12CommandSignature *sig;
    static ID3D12QueryHeap *occlusion;
    static std::vector<char> library_blob;
    static std::string eng_src;
    enum { RTV_MRT = FRAMES_IN_FLIGHT + 10, SRV_MRT = 12000, SRV_CUBE3D = 12010, UAV_ENGINE = 12020, CBV_RING = 12100, CPU_UAV = 15200 };
    eng_src = std::string(common_hlsl) + engine_hlsl_tail;

    auto quads = [](ID3D12GraphicsCommandList *l, UINT n, float scale, UINT frame) {
        for (UINT i = 0; i < n; i++)
        {
            float rc[4] = { (float)(i % 64) / 32.0f - 1.0f, (float)(i / 64 % 64) / 32.0f - 1.0f, scale, frame * 0.01f };
            l->SetGraphicsRoot32BitConstants(0, 4, rc, 0);
            l->DrawInstanced(3, 1, 0, 0);
        }
    };
    auto make_cs_rs = [] {
        D3D12_DESCRIPTOR_RANGE uavs = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 0, 0, 0 };
        D3D12_ROOT_PARAMETER rp[2] = {};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rp[0].Constants = { 0, 0, 4 };
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[1].DescriptorTable = { 1, &uavs };
        D3D12_ROOT_SIGNATURE_DESC rsd = { 2, rp };
        cs_rs = serialize_root_signature(rsd);
    };
    // A second queue (compute or copy) with its own allocators, list and fence.
    auto make_other_queue = [](D3D12_COMMAND_LIST_TYPE type) {
        D3D12_COMMAND_QUEUE_DESC qd = { type };
        CHECK(p.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&other_queue)));
        for (UINT i = 0; i < FRAMES_IN_FLIGHT; i++)
            CHECK(p.device->CreateCommandAllocator(type, IID_PPV_ARGS(&other_alloc[i])));
        CHECK(p.device->CreateCommandList(0, type, other_alloc[0], nullptr, IID_PPV_ARGS(&other_list)));
        CHECK(other_list->Close());
        CHECK(p.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&other_fence)));
    };
    // Submits other_list's work for this frame; the direct queue waits for it before the frame's own work.
    auto run_other = [](const std::function<void(ID3D12GraphicsCommandList *)> &record) {
        ID3D12CommandAllocator *a = other_alloc[p.frame_index];
        // The slot's previous use finished: its value is at most the direct queue's frame fence for this slot.
        if (other_fence->GetCompletedValue() + FRAMES_IN_FLIGHT < other_value)
        {
            CHECK(other_fence->SetEventOnCompletion(other_value - FRAMES_IN_FLIGHT, p.event));
            WaitForSingleObject(p.event, INFINITE);
        }
        CHECK(a->Reset());
        CHECK(other_list->Reset(a, nullptr));
        record(other_list);
        CHECK(other_list->Close());
        ID3D12CommandList *lists[] = { other_list };
        other_queue->ExecuteCommandLists(1, lists);
        CHECK(other_queue->Signal(other_fence, ++other_value));
        CHECK(p.queue->Wait(other_fence, other_value));
    };

    std::vector<Scene> v = {
        { "gbuffer_mrt", "deferred shading: 1500 draws into 4 render targets (RGBA8 x2, RGBA16F, R11G11B10) + depth, then a lighting pass", [] {
            const DXGI_FORMAT formats[4] = { DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R11G11B10_FLOAT };
            for (int i = 0; i < 4; i++)
            {
                D3D12_CLEAR_VALUE cv = { formats[i], { 0, 0, 0, 0 } };
                mrt[i] = create_texture(W, H, formats[i], D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, 1, 1, &cv);
                p.device->CreateRenderTargetView(mrt[i], nullptr, rtv(RTV_MRT + i));
                p.device->CreateShaderResourceView(mrt[i], nullptr, srv_cpu(SRV_MRT + i));
            }
            for (int i = 4; i < 8; i++) p.device->CreateShaderResourceView(p.textures[i], nullptr, srv_cpu(SRV_MRT + i));
            PsoDesc d; d.hlsl = eng_src.c_str(); d.ps = "ps_gbuffer"; d.depth = true;
            d.tweak = [formats](D3D12_GRAPHICS_PIPELINE_STATE_DESC &g) {
                g.NumRenderTargets = 4;
                for (int i = 0; i < 4; i++) { g.RTVFormats[i] = formats[i]; g.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL; }
            };
            pso_a = create_pso(d);
            d = {}; d.hlsl = eng_src.c_str(); d.ps = "ps_lighting"; pso_b = create_pso(d);
        }, [quads](UINT f) {
            for (int i = 0; i < 4; i++) barrier(p.list, mrt[i], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            D3D12_CPU_DESCRIPTOR_HANDLE h[4] = { rtv(RTV_MRT), rtv(RTV_MRT + 1), rtv(RTV_MRT + 2), rtv(RTV_MRT + 3) };
            auto d = dsv(0);
            float c[4] = { 0, 0, 0, 0 };
            for (int i = 0; i < 4; i++) p.list->ClearRenderTargetView(h[i], c, 0, nullptr);
            p.list->ClearDepthStencilView(d, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
            p.list->OMSetRenderTargets(4, h, FALSE, &d);
            set_quad_state(p.list, pso_a);
            quads(p.list, 1500, 0.06f, f);
            for (int i = 0; i < 4; i++) barrier(p.list, mrt[i], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            auto bb = rtv(p.frame_index);
            p.list->OMSetRenderTargets(1, &bb, FALSE, nullptr);
            set_quad_state(p.list, pso_b);
            p.list->SetGraphicsRootDescriptorTable(2, srv_gpu(SRV_MRT));
            fullscreen(p.list, 1.0f);
        } },
        { "async_compute", "a compute queue running 40 ALU dispatches while the direct queue renders 20 heavy full-screen quads; joined by a fence", [make_cs_rs, make_other_queue] {
            make_cs_rs();
            pso_a = compute_pso_from(cs_rs, shader_code(engine_cs_hlsl, "cs_alu", "cs_5_0"));
            PsoDesc d; d.ps = "ps_heavy"; pso_b = create_pso(d);
            res_a = create_buffer(4000 * 32 + 256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
            ud.Format = DXGI_FORMAT_R32_TYPELESS;
            ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            ud.Buffer = { 0, (4000 * 32 + 256) / 4, 0, 0, D3D12_BUFFER_UAV_FLAG_RAW };
            p.device->CreateUnorderedAccessView(res_a, nullptr, &ud, srv_cpu(UAV_ENGINE + 2));
            make_other_queue(D3D12_COMMAND_LIST_TYPE_COMPUTE);
        }, [run_other](UINT f) {
            run_other([](ID3D12GraphicsCommandList *l) {
                ID3D12DescriptorHeap *heaps[] = { p.srv_heap };
                l->SetDescriptorHeaps(1, heaps);
                l->SetComputeRootSignature(cs_rs);
                l->SetPipelineState(pso_a);
                l->SetComputeRootDescriptorTable(1, srv_gpu(UAV_ENGINE));
                for (int i = 0; i < 40; i++) l->Dispatch(256, 1, 1);
            });
            set_quad_state(p.list, pso_b);
            for (UINT i = 0; i < 20; i++) fullscreen(p.list, f * 0.01f + i);
        } },
        { "copy_queue_streaming", "a copy queue uploading 16 MiB per frame into default buffers; the direct queue waits for it", [make_other_queue] {
            make_other_queue(D3D12_COMMAND_LIST_TYPE_COPY);
            res_a = create_buffer(16 << 20, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON);
        }, [run_other](UINT f) {
            void *cpu;
            D3D12_GPU_VIRTUAL_ADDRESS va = upload_alloc(16 << 20, &cpu);
            memset(cpu, f & 0xff, 16 << 20);
            UINT64 offset = va - p.upload_ring->GetGPUVirtualAddress();
            run_other([offset](ID3D12GraphicsCommandList *l) { l->CopyBufferRegion(res_a, 0, p.upload_ring, offset, 16 << 20); });
        } },
        { "aliasing", "20 transient 1024x1024 RGBA16F render targets placed over the same heap memory, with aliasing barriers", [] {
            D3D12_RESOURCE_DESC rd = {};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width = rd.Height = 1024;
            rd.DepthOrArraySize = rd.MipLevels = 1;
            rd.Format = HDR_FORMAT;
            rd.SampleDesc.Count = 1;
            rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_RESOURCE_ALLOCATION_INFO ai = p.device->GetResourceAllocationInfo(0, 1, &rd);
            D3D12_HEAP_DESC hd = { ai.SizeInBytes, { D3D12_HEAP_TYPE_DEFAULT }, ai.Alignment, D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES };
            CHECK(p.device->CreateHeap(&hd, IID_PPV_ARGS(&alias_heap)));
            D3D12_CLEAR_VALUE cv = { HDR_FORMAT, { 0, 0, 0, 0 } };
            for (int i = 0; i < 20; i++)
            {
                CHECK(p.device->CreatePlacedResource(alias_heap, 0, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&aliases[i])));
                p.device->CreateRenderTargetView(aliases[i], nullptr, rtv(FRAMES_IN_FLIGHT + 4 + (i % 2)));
            }
            PsoDesc d; d.rt = HDR_FORMAT; pso_a = create_pso(d);
        }, [quads](UINT f) {
            for (int i = 0; i < 20; i++)
            {
                D3D12_RESOURCE_BARRIER b = {};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
                b.Aliasing.pResourceBefore = aliases[(i + 19) % 20];
                b.Aliasing.pResourceAfter = aliases[i];
                p.list->ResourceBarrier(1, &b);
                p.device->CreateRenderTargetView(aliases[i], nullptr, rtv(FRAMES_IN_FLIGHT + 4));
                auto h = rtv(FRAMES_IN_FLIGHT + 4);
                float c[4] = { 0, 0, 0, 0 };
                p.list->ClearRenderTargetView(h, c, 0, nullptr);   // required after aliasing
                p.list->OMSetRenderTargets(1, &h, FALSE, nullptr);
                viewport(p.list, 1024, 1024);
                set_quad_state(p.list, pso_a);
                quads(p.list, 20, 0.05f, f);
            }
            auto bb = rtv(p.frame_index);
            p.list->OMSetRenderTargets(1, &bb, FALSE, nullptr);
            viewport(p.list);
        } },
        { "gpu_culling", "compute culls 4000 objects into indirect draws, then ExecuteIndirect with a count buffer", [make_cs_rs] {
            make_cs_rs();
            pso_a = compute_pso_from(cs_rs, shader_code(engine_cs_hlsl, "cs_cull", "cs_5_0"));
            pso_c = compute_pso_from(cs_rs, shader_code(engine_cs_hlsl, "cs_reset", "cs_5_0"));
            pso_b = create_pso({});
            res_a = create_buffer(4000 * 32 + 256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
            ud.Format = DXGI_FORMAT_R32_TYPELESS;
            ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            ud.Buffer = { 0, (4000 * 32 + 256) / 4, 0, 0, D3D12_BUFFER_UAV_FLAG_RAW };
            p.device->CreateUnorderedAccessView(res_a, nullptr, &ud, srv_cpu(UAV_ENGINE + 2));
            D3D12_INDIRECT_ARGUMENT_DESC a[2] = {};
            a[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
            a[0].Constant = { 0, 0, 4 };
            a[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
            D3D12_COMMAND_SIGNATURE_DESC cs = { 32, 2, a, 0 };
            CHECK(p.device->CreateCommandSignature(&cs, p.root_signature, IID_PPV_ARGS(&sig)));
        }, [](UINT f) {
            p.list->SetComputeRootSignature(cs_rs);
            p.list->SetComputeRootDescriptorTable(1, srv_gpu(UAV_ENGINE));
            p.list->SetPipelineState(pso_c);
            p.list->Dispatch(1, 1, 1);
            D3D12_RESOURCE_BARRIER u = {};
            u.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            u.UAV.pResource = res_a;
            p.list->ResourceBarrier(1, &u);
            uint32_t rc[4] = { f, 0, 0, 0 };
            p.list->SetComputeRoot32BitConstants(0, 4, rc, 0);
            p.list->SetPipelineState(pso_a);
            p.list->Dispatch(4000 / 64 + 1, 1, 1);
            barrier(p.list, res_a, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
            set_quad_state(p.list, pso_b);
            p.list->ExecuteIndirect(sig, 4000, res_a, 0, res_a, 4000 * 32);
            barrier(p.list, res_a, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        } },
        { "predication", "100 occlusion-tested proxies, then 100 groups of 20 draws predicated on their results", [] {
            pso_a = create_pso({});
            D3D12_QUERY_HEAP_DESC qh = { D3D12_QUERY_HEAP_TYPE_OCCLUSION, 100 };
            CHECK(p.device->CreateQueryHeap(&qh, IID_PPV_ARGS(&occlusion)));
            res_a = create_buffer(100 * 8, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_PREDICATION);
        }, [quads](UINT f) {
            set_quad_state(p.list, pso_a);
            for (UINT i = 0; i < 100; i++)
            {
                p.list->BeginQuery(occlusion, D3D12_QUERY_TYPE_BINARY_OCCLUSION, i);
                small_quad(p.list, i * 37, 0.5f);
                p.list->EndQuery(occlusion, D3D12_QUERY_TYPE_BINARY_OCCLUSION, i);
            }
            barrier(p.list, res_a, D3D12_RESOURCE_STATE_PREDICATION, D3D12_RESOURCE_STATE_COPY_DEST);
            p.list->ResolveQueryData(occlusion, D3D12_QUERY_TYPE_BINARY_OCCLUSION, 0, 100, res_a, 0);
            barrier(p.list, res_a, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PREDICATION);
            for (UINT i = 0; i < 100; i++)
            {
                p.list->SetPredication(res_a, i * 8, D3D12_PREDICATION_OP_EQUAL_ZERO);
                quads(p.list, 20, 0.01f, f);
            }
            p.list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
        } },
        { "cube_3d_array", "20 full-screen quads sampling a cube map, a 3D texture and a texture array (6 samples each)", [] {
            D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
            D3D12_RESOURCE_DESC rd = {};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width = rd.Height = 512; rd.DepthOrArraySize = 6; rd.MipLevels = 1;
            rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rd.SampleDesc.Count = 1;
            CHECK(p.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&res_a)));
            rd.DepthOrArraySize = 8;
            CHECK(p.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&res_c)));
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
            rd.Width = rd.Height = 128; rd.DepthOrArraySize = 128;
            CHECK(p.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&res_b)));
            D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
            sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
            sd.TextureCube.MipLevels = 1;
            p.device->CreateShaderResourceView(res_a, &sd, srv_cpu(SRV_CUBE3D));
            p.device->CreateShaderResourceView(res_b, nullptr, srv_cpu(SRV_CUBE3D + 1));
            p.device->CreateShaderResourceView(res_c, nullptr, srv_cpu(SRV_CUBE3D + 2));
            PsoDesc d; d.hlsl = eng_src.c_str(); d.ps = "ps_cube3d"; pso_a = create_pso(d);
        }, [](UINT f) {
            set_quad_state(p.list, pso_a);
            p.list->SetGraphicsRootDescriptorTable(2, srv_gpu(SRV_CUBE3D));
            for (UINT i = 0; i < 20; i++) fullscreen(p.list, f * 0.01f);
        } },
        { "typed_uav_load", "compute read-modify-write of 1080p R11G11B10_FLOAT and RGBA8_UNORM UAV textures (typed UAV loads)", [make_cs_rs] {
            make_cs_rs();
            pso_a = compute_pso_from(cs_rs, shader_code(engine_cs_hlsl, "cs_typed_rmw", "cs_5_0"));
            res_a = create_texture(W, H, DXGI_FORMAT_R11G11B10_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            res_b = create_texture(W, H, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            res_c = create_buffer(4000 * 32 + 256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            p.device->CreateUnorderedAccessView(res_a, nullptr, nullptr, srv_cpu(UAV_ENGINE));
            p.device->CreateUnorderedAccessView(res_b, nullptr, nullptr, srv_cpu(UAV_ENGINE + 1));
            D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
            ud.Format = DXGI_FORMAT_R32_TYPELESS;
            ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            ud.Buffer = { 0, (4000 * 32 + 256) / 4, 0, 0, D3D12_BUFFER_UAV_FLAG_RAW };
            p.device->CreateUnorderedAccessView(res_c, nullptr, &ud, srv_cpu(UAV_ENGINE + 2));
        }, [](UINT) {
            p.list->SetComputeRootSignature(cs_rs);
            p.list->SetPipelineState(pso_a);
            p.list->SetComputeRootDescriptorTable(1, srv_gpu(UAV_ENGINE));
            p.list->Dispatch(W / 8, H / 8, 1);
            D3D12_RESOURCE_BARRIER u[2] = {};
            u[0].Type = u[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            u[0].UAV.pResource = res_a;
            u[1].UAV.pResource = res_b;
            p.list->ResourceBarrier(2, u);
        } },
        { "pipeline_library", "20 graphics pipelines loaded from a serialized ID3D12PipelineLibrary per frame (startup path)", [] {
            ID3D12Device1 *dev1 = nullptr;
            CHECK(p.device->QueryInterface(IID_PPV_ARGS(&dev1)));
            ID3D12PipelineLibrary *lib = nullptr;
            if (FAILED(dev1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&lib)))) { unsupported("pipeline libraries"); dev1->Release(); return; }
            for (int i = 0; i < 20; i++)
            {
                PsoDesc d; d.ps = (i & 1) ? "ps_heavy" : "ps_tex"; d.salt = 500 + i;
                library_psos[i] = create_pso(d);
                wchar_t name[16];
                swprintf(name, 16, L"pso%d", i);
                CHECK(lib->StorePipeline(name, library_psos[i]));
            }
            library_blob.resize(lib->GetSerializedSize());
            CHECK(lib->Serialize(library_blob.data(), library_blob.size()));
            lib->Release();
            dev1->Release();
        }, [](UINT) {
            if (library_blob.empty()) return;
            ID3D12Device1 *dev1 = nullptr;
            CHECK(p.device->QueryInterface(IID_PPV_ARGS(&dev1)));
            double t = qpc_ms();
            ID3D12PipelineLibrary *lib = nullptr;
            CHECK(dev1->CreatePipelineLibrary(library_blob.data(), library_blob.size(), IID_PPV_ARGS(&lib)));
            double load = 0;
            for (int i = 0; i < 20; i++)
            {
                PsoDesc d; d.ps = (i & 1) ? "ps_heavy" : "ps_tex"; d.salt = 500 + i;
                std::string src = salted(common_hlsl, d.salt);
                static std::vector<std::vector<char>> code(40);
                if (code[i * 2].empty()) { code[i * 2] = shader_code(src, d.vs, "vs_5_0"); code[i * 2 + 1] = shader_code(src, d.ps, "ps_5_0"); }
                D3D12_GRAPHICS_PIPELINE_STATE_DESC g = {};
                g.pRootSignature = p.root_signature;
                g.VS = { code[i * 2].data(), code[i * 2].size() };
                g.PS = { code[i * 2 + 1].data(), code[i * 2 + 1].size() };
                g.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
                g.SampleMask = UINT_MAX;
                g.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
                g.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
                g.RasterizerState.DepthClipEnable = TRUE;
                g.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                g.NumRenderTargets = 1;
                g.RTVFormats[0] = BACKBUFFER_FORMAT;
                g.SampleDesc.Count = 1;
                wchar_t name[16];
                swprintf(name, 16, L"pso%d", i);
                ID3D12PipelineState *pso = nullptr;
                double t1 = qpc_ms();
                if (SUCCEEDED(lib->LoadGraphicsPipeline(name, &g, IID_PPV_ARGS(&pso)))) pso->Release();
                load += qpc_ms() - t1;
            }
            lib->Release();
            dev1->Release();
            extras.add("library_ms", qpc_ms() - t);
            extras.add("load_ms_per_pso", load / 20);
        } },
        { "cbv_descriptors", "3000 draws, each creating a constant buffer view into a descriptor table (older engines)", [] {
            D3D12_DESCRIPTOR_RANGE cbv = { D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 1, 0, 0 };
            D3D12_DESCRIPTOR_RANGE srv = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 8, 0, 0, 0 };
            D3D12_ROOT_PARAMETER rp[3] = {};
            rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            rp[0].Constants = { 0, 0, 4 };
            rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            rp[1].DescriptorTable = { 1, &cbv };
            rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            rp[2].DescriptorTable = { 1, &srv };
            D3D12_STATIC_SAMPLER_DESC ss = linear_sampler();
            D3D12_ROOT_SIGNATURE_DESC rsd = { 3, rp, 1, &ss, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
            cbv_rs = serialize_root_signature(rsd);
            PsoDesc d; d.rs = cbv_rs; pso_a = create_pso(d);
        }, [](UINT f) {
            p.list->SetGraphicsRootSignature(cbv_rs);
            p.list->SetPipelineState(pso_a);
            p.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            p.list->SetGraphicsRootDescriptorTable(2, srv_gpu(0));
            UINT base = CBV_RING + (p.frame_index * 3000);
            double t = qpc_ms();
            for (UINT i = 0; i < 3000; i++)
            {
                void *cpu;
                D3D12_GPU_VIRTUAL_ADDRESS cb = upload_alloc(256, &cpu);
                ((float *)cpu)[4] = (float)i;
                D3D12_CONSTANT_BUFFER_VIEW_DESC cv = { cb, 256 };
                p.device->CreateConstantBufferView(&cv, srv_cpu(base + i));
                p.list->SetGraphicsRootDescriptorTable(1, srv_gpu(base + i));
                small_quad(p.list, i, f * 0.01f);
            }
            extras.add("record_us_per_draw", (qpc_ms() - t) * 1000.0 / 3000);
        } },
        { "mip_streaming", "4 BC7 2048x2048 textures uploaded per frame, mips 0-9 (texture streaming)", [] {
            for (int i = 0; i < 4; i++)
                bc7_mips[i] = create_texture(2048, 2048, DXGI_FORMAT_BC7_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, 12);
        }, [](UINT f) {
            for (int i = 0; i < 4; i++)
                for (UINT m = 0; m < 10; m++)   // mips down to 4x4 (one block)
                {
                    UINT w = 2048u >> m, blocks = w / 4;
                    UINT pitch = std::max(blocks * 16, 256u), size = pitch * blocks;
                    void *cpu;
                    D3D12_GPU_VIRTUAL_ADDRESS va = upload_alloc(size + 512, &cpu);
                    UINT64 offset = (va - p.upload_ring->GetGPUVirtualAddress() + 511) & ~511ull;
                    D3D12_TEXTURE_COPY_LOCATION src = { p.upload_ring, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
                    src.PlacedFootprint.Offset = offset;
                    src.PlacedFootprint.Footprint = { DXGI_FORMAT_BC7_UNORM, blocks * 4, blocks * 4, 1, pitch };
                    D3D12_TEXTURE_COPY_LOCATION dst = { bc7_mips[i], D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                    dst.SubresourceIndex = m;
                    p.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                }
        } },
    };
    return v;
}

// Submission-side costs below the game: many command lists recorded in parallel and executed together
// (the driver encodes them on one queue thread), per-draw descriptor changes, and render target rebinds
// split by unrelated barriers (each can end a render pass, and each pass is a Metal render encoder).
static std::vector<Scene> submit_scenes()
{
    enum { THREADS = 8, MT_DRAWS = 500, BIND_DRAWS = 4000, REBIND_GROUPS = 64 };
    static ID3D12PipelineState *tex_pso, *tex_pso2;
    static ID3D12CommandAllocator *mt_alloc[THREADS][FRAMES_IN_FLIGHT];
    static ID3D12GraphicsCommandList *mt_list[THREADS];
    static ID3D12Resource *mt_upload[THREADS], *uav_dummy;
    static uint8_t *mt_upload_ptr[THREADS];
    static std::thread workers[THREADS];
    static std::mutex lock;
    static std::condition_variable start_cv, done_cv;
    static UINT generation, done, work_frame;

    // hdr target 0 stays a render target for these scenes; each thread has its own upload ring.
    auto common_init = [] {
        PsoDesc d; d.rt = HDR_FORMAT; d.ps = "ps_tex"; tex_pso = create_pso(d);
        d = {}; d.rt = HDR_FORMAT; d.ps = "ps_ui"; d.blend = true; tex_pso2 = create_pso(d);
        CHECK(p.allocators[0]->Reset());
        CHECK(p.list->Reset(p.allocators[0], nullptr));
        barrier(p.list, p.hdr_targets[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        submit_and_wait(p.list);
    };
    // Draws into hdr target 0 with a fresh root CBV per draw and a new descriptor table every `table_every` draws.
    auto draws = [](ID3D12GraphicsCommandList *l, UINT n, UINT table_every, uint8_t *ring, D3D12_GPU_VIRTUAL_ADDRESS ring_va,
                    UINT64 &offset, UINT seed, float value) {
        for (UINT i = 0; i < n; i++)
        {
            if (offset + 256 > (4u << 20)) offset = 0;
            ((float *)(ring + offset))[4] = (float)(seed + i);
            l->SetGraphicsRootConstantBufferView(1, ring_va + offset);
            offset += 256;
            if (i % table_every == 0) l->SetGraphicsRootDescriptorTable(2, srv_gpu(8 + (seed + i) * 8 % 992));
            small_quad(l, seed + i, value);
        }
    };
    auto begin_hdr = [](ID3D12GraphicsCommandList *l, ID3D12PipelineState *pso) {
        ID3D12DescriptorHeap *heaps[] = { p.srv_heap };
        l->SetDescriptorHeaps(1, heaps);
        auto h = rtv(FRAMES_IN_FLIGHT);
        l->OMSetRenderTargets(1, &h, FALSE, nullptr);
        viewport(l);
        // PROBE_TINY_SCISSOR: a 1x1 scissor leaves the GPU almost no work, so submission costs dominate.
        static const bool tiny = getenv("PROBE_TINY_SCISSOR") != nullptr;
        if (tiny) { D3D12_RECT sc = { 0, 0, 1, 1 }; l->RSSetScissorRects(1, &sc); }
        l->SetGraphicsRootSignature(p.root_signature);
        l->SetPipelineState(pso);
        l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    };

    return {
        { "mt_submit", "8 command lists of 500 draws (root CBV per draw) recorded on 8 threads, executed together each frame",
          [common_init, draws, begin_hdr] {
            common_init();
            for (UINT t = 0; t < THREADS; t++)
            {
                for (UINT i = 0; i < FRAMES_IN_FLIGHT; i++)
                    CHECK(p.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&mt_alloc[t][i])));
                CHECK(p.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, mt_alloc[t][0], nullptr, IID_PPV_ARGS(&mt_list[t])));
                CHECK(mt_list[t]->Close());
                mt_upload[t] = create_buffer(4u << 20, D3D12_HEAP_TYPE_UPLOAD);
                CHECK(mt_upload[t]->Map(0, nullptr, (void **)&mt_upload_ptr[t]));
                workers[t] = std::thread([t, draws, begin_hdr] {
                    UINT seen = 0;
                    UINT64 offset = 0;
                    for (;;)
                    {
                        UINT f;
                        {
                            std::unique_lock<std::mutex> g(lock);
                            start_cv.wait(g, [&] { return generation != seen; });
                            seen = generation;
                            f = work_frame;
                        }
                        ID3D12GraphicsCommandList *l = mt_list[t];
                        CHECK(mt_alloc[t][p.frame_index]->Reset());
                        CHECK(l->Reset(mt_alloc[t][p.frame_index], nullptr));
                        begin_hdr(l, tex_pso);
                        static const UINT n = getenv("PROBE_MT_DRAWS") ? atoi(getenv("PROBE_MT_DRAWS")) : MT_DRAWS;
                        draws(l, n, 16, mt_upload_ptr[t], mt_upload[t]->GetGPUVirtualAddress(), offset, t * 1000, f * 0.01f);
                        CHECK(l->Close());
                        std::lock_guard<std::mutex> g(lock);
                        if (++done == THREADS) done_cv.notify_one();
                    }
                });
                workers[t].detach();
            }
        }, [](UINT f) {
            double t = qpc_ms();
            {
                std::unique_lock<std::mutex> g(lock);
                done = 0;
                work_frame = f;
                generation++;
                start_cv.notify_all();
                done_cv.wait(g, [] { return done == THREADS; });
            }
            double recorded = qpc_ms();
            ID3D12CommandList *lists[THREADS];
            for (UINT i = 0; i < THREADS; i++) lists[i] = mt_list[i];
            p.queue->ExecuteCommandLists(THREADS, lists);
            extras.add("record_ms", recorded - t);
            extras.add("execute_ms", qpc_ms() - recorded);
        } },
        { "draw_bindings", "4000 draws, each with its own root CBV and descriptor table (per-draw binding cost)",
          [common_init] { common_init(); }, [draws, begin_hdr](UINT f) {
            static UINT64 offset;
            double t = qpc_ms();
            begin_hdr(p.list, tex_pso);
            draws(p.list, BIND_DRAWS, 1, p.upload_ptr, p.upload_ring->GetGPUVirtualAddress(), offset, 0, f * 0.01f);
            extras.add("record_ms", qpc_ms() - t);
        } },
        { "rt_rebind", "64 groups of 6 draws, the same render target rebound each group after an unrelated UAV barrier",
          [common_init] {
            common_init();
            uav_dummy = create_buffer(65536, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }, [draws, begin_hdr](UINT f) {
            static UINT64 offset;
            double t = qpc_ms();
            for (UINT g = 0; g < REBIND_GROUPS; g++)
            {
                begin_hdr(p.list, g & 1 ? tex_pso2 : tex_pso);
                draws(p.list, 6, 6, p.upload_ptr, p.upload_ring->GetGPUVirtualAddress(), offset, g * 6, f * 0.01f);
                D3D12_RESOURCE_BARRIER b = {};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                b.UAV.pResource = uav_dummy;
                p.list->ResourceBarrier(1, &b);
            }
            extras.add("record_ms", qpc_ms() - t);
        } },
    };
}

static std::vector<Scene> scenes()
{
    std::vector<Scene> all = base_scenes(), more = more_scenes(), sm6 = sm6_scenes(), feat = feature_scenes(), eng = engine_scenes(),
                       sub = submit_scenes();
    all.insert(all.end(), more.begin(), more.end());
    all.insert(all.end(), sm6.begin(), sm6.end());
    all.insert(all.end(), feat.begin(), feat.end());
    all.insert(all.end(), eng.begin(), eng.end());
    all.insert(all.end(), sub.begin(), sub.end());
    return all;
}

// ---- main loop ----

static double percentile(std::vector<double> v, double q)
{
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, (size_t)(q * (v.size() - 1) + 0.5))];
}

static void report(const char *what, const std::vector<double> &v)
{
    double sum = 0, mx = 0;
    for (double x : v) { sum += x; mx = std::max(mx, x); }
    printf("%s frames %zu mean %.3f median %.3f p95 %.3f max %.3f ms\n", what, v.size(), v.empty() ? 0 : sum / v.size(),
           percentile(v, 0.5), percentile(v, 0.95), mx);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2 || !strcmp(argv[1], "list"))
    {
        // Listing needs no device: the scenes' functions only run when selected.
        for (const Scene &s : scenes()) printf("%-18s %s%s\n", s.name, s.dxil ? "[dxil] " : "", s.desc);
        return argc < 2 ? 1 : 0;
    }
    // prepare SCENE [frames] [warmup] [salt]: setup and scene init only (fills the DXIL cache from the aarch64 probe).
    if (!strcmp(argv[1], "prepare"))
    {
        g_prepare = true;
        argv++;
        argc--;
    }
    UINT frames = argc > 2 ? atoi(argv[2]) : 200, warmup = argc > 3 ? atoi(argv[3]) : 30;
    p.salt = argc > 4 ? atoi(argv[4]) : GetTickCount();
    g_frames_total = frames + warmup + 60;   // + the serialized frames

    std::vector<Scene> all = scenes();
    const Scene *scene = nullptr;
    for (const Scene &s : all) if (!strcmp(s.name, argv[1])) scene = &s;
    if (!scene) { printf("unknown scene %s\n", argv[1]); return 2; }

    setup();
    double t = qpc_ms();
    scene->init();
    printf("extra init_ms=%.3f\n", qpc_ms() - t);
    if (g_prepare)
    {
        printf("D3D12_PERF_PROBE_OK %s\n", scene->name);
        return 0;
    }

    std::vector<double> cpu, gpu, gpu_ts, work;
    double first = 0, last_present = 0;
    // Timed frames, then serialized frames: each is submitted and waited for, and its
    // submit-to-completion time is the "gpu" figure (independent of timestamp queries).
    const UINT serialized = 60;
    for (UINT f = 0; f < warmup + frames + serialized; f++)
    {
        bool timed = f >= warmup && f < warmup + frames, sync = f >= warmup + frames;
        if (f == warmup + frames)
            wait_fence(p.fence_value);
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
        if (scene->present)
            WaitForSingleObjectEx(p.waitable, 1000, TRUE);
        double frame_start = qpc_ms();
        p.frame_index = scene->present ? p.swapchain->GetCurrentBackBufferIndex() : f % FRAMES_IN_FLIGHT;
        UINT i = p.frame_index;
        wait_fence(p.frame_fence[i]);
        if (p.frame_fence[i] && timed && f > warmup + FRAMES_IN_FLIGHT)
        {
            uint64_t *ts;
            D3D12_RANGE r = { i * 16, i * 16 + 16 };
            CHECK(p.timestamp_readback->Map(0, &r, (void **)&ts));
            static int dumped;
            if (getenv("PROBE_DEBUG_TS") && dumped++ < 5)
                printf("ts slot %u: %llu %llu\n", i, (unsigned long long)ts[i * 2], (unsigned long long)ts[i * 2 + 1]);
            if (ts[i * 2 + 1] > ts[i * 2])
                gpu_ts.push_back(1000.0 * (double)(ts[i * 2 + 1] - ts[i * 2]) / (double)p.timestamp_frequency);
            p.timestamp_readback->Unmap(0, nullptr);
        }
        release_completed();
        CHECK(p.allocators[i]->Reset());
        CHECK(p.list->Reset(p.allocators[i], nullptr));
        ID3D12DescriptorHeap *heaps[] = { p.srv_heap };
        p.list->SetDescriptorHeaps(1, heaps);
        p.list->EndQuery(p.timestamps, D3D12_QUERY_TYPE_TIMESTAMP, i * 2);
        barrier(p.list, p.backbuffers[i], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        auto h = rtv(i);
        float clear[4] = { 0.1f, 0.1f, 0.12f, 1 };
        p.list->ClearRenderTargetView(h, clear, 0, nullptr);
        p.list->OMSetRenderTargets(1, &h, FALSE, nullptr);
        viewport(p.list);
        scene->frame(f);
        barrier(p.list, p.backbuffers[i], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        p.list->EndQuery(p.timestamps, D3D12_QUERY_TYPE_TIMESTAMP, i * 2 + 1);
        p.list->ResolveQueryData(p.timestamps, D3D12_QUERY_TYPE_TIMESTAMP, i * 2, 2, p.timestamp_readback, i * 16);
        CHECK(p.list->Close());
        ID3D12CommandList *lists[] = { p.list };
        double submit = qpc_ms();
        p.queue->ExecuteCommandLists(1, lists);
        p.frame_fence[i] = signal_queue();
        if (sync)
        {
            wait_fence(p.frame_fence[i]);
            gpu.push_back(qpc_ms() - submit);
        }
        if (timed)
            work.push_back(qpc_ms() - frame_start);
        if (scene->present)
            CHECK(p.swapchain->Present(scene->sync_interval, scene->sync_interval ? 0 : DXGI_PRESENT_ALLOW_TEARING));
        double now = qpc_ms();
        if (f == 0) first = now - t;
        else if (timed) cpu.push_back(now - last_present);
        last_present = now;
        if (f + 1 == warmup) extras.values.clear();
        if (f + 1 == warmup + frames) { for (auto &e : extras.values) e.second /= frames; extras.frozen = true; }
    }
    wait_fence(p.fence_value);

    report("cpu", cpu);
    report("work", work);
    report("gpu", gpu);
    if (!gpu_ts.empty()) report("gputs", gpu_ts);
    printf("first %.3f ms\n", first);
    // Extras are per frame over the timed frames (accumulation during the serialized frames is dropped).
    for (auto &e : extras.values) printf("extra %s=%.4f\n", e.first.c_str(), e.second);
    printf("D3D12_PERF_PROBE_OK %s\n", scene->name);
    return 0;
}
