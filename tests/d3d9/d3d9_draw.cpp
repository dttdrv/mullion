#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>

static int failures = 0;

static void
check(const char *name, bool ok, DWORD got = 0, DWORD want = 0) {
  if (ok)
    printf("PASS: %s\n", name);
  else
    printf("FAIL: %s (got %08lx want %08lx)\n", name, got, want);
  if (!ok)
    failures++;
}

static bool
close(DWORD a, DWORD b, int tolerance = 2) {
  for (int shift = 0; shift < 32; shift += 8) {
    int x = (a >> shift) & 0xFF, y = (b >> shift) & 0xFF;
    if (x - y > tolerance || y - x > tolerance)
      return false;
  }
  return true;
}

static IDirect3DDevice9 *device;
static IDirect3DSurface9 *rt, *readback;

static DWORD
pixel(int x, int y) {
  if (FAILED(device->GetRenderTargetData(rt, readback)))
    return 0xDEADBEEF;
  D3DLOCKED_RECT lr;
  if (FAILED(readback->LockRect(&lr, nullptr, D3DLOCK_READONLY)))
    return 0xDEADBEEF;
  DWORD value = ((DWORD *)((char *)lr.pBits + y * lr.Pitch))[x];
  readback->UnlockRect();
  return value;
}

static void
expect(const char *name, int x, int y, DWORD want, int tolerance = 2) {
  DWORD got = pixel(x, y);
  check(name, close(got, want, tolerance), got, want);
}

static void *
compile(const char *source, const char *target) {
  ID3DBlob *code = nullptr, *errors = nullptr;
  if (FAILED(D3DCompile(source, strlen(source), nullptr, nullptr, nullptr, "main", target, 0, 0, &code, &errors))) {
    printf("FAIL: compile %s: %s\n", target, errors ? (const char *)errors->GetBufferPointer() : "?");
    failures++;
    return nullptr;
  }
  return code->GetBufferPointer();
}

static IDirect3DVertexShader9 *
vs(const char *source) {
  IDirect3DVertexShader9 *shader = nullptr;
  if (auto code = compile(source, "vs_3_0"))
    device->CreateVertexShader((const DWORD *)code, &shader);
  return shader;
}

static IDirect3DPixelShader9 *
ps(const char *source) {
  IDirect3DPixelShader9 *shader = nullptr;
  if (auto code = compile(source, "ps_3_0"))
    device->CreatePixelShader((const DWORD *)code, &shader);
  return shader;
}

struct Vertex {
  float x, y, z;
  DWORD color;
  float u, v;
};

static const char *kVS = "struct O { float4 p : POSITION; float4 c : COLOR0; float2 t : TEXCOORD0; };"
                         "O main(float3 p : POSITION, float4 c : COLOR0, float2 t : TEXCOORD0) {"
                         "  O o; o.p = float4(p, 1); o.c = c; o.t = t; return o; }";
static const char *kPSColor = "float4 main(float4 c : COLOR0) : COLOR0 { return c; }";
static const char *kPSConst = "float4 c0 : register(c0); float4 main() : COLOR0 { return c0; }";
static const char *kPSTex = "sampler2D s : register(s0);"
                            "float4 main(float2 t : TEXCOORD0) : COLOR0 { return tex2D(s, t); }";

static void
quad(Vertex *v, float x0, float y0, float x1, float y1, float z, DWORD color) {
  v[0] = {x0, y0, z, color, 0, 1};
  v[1] = {x0, y1, z, color, 0, 0};
  v[2] = {x1, y0, z, color, 1, 1};
  v[3] = {x1, y1, z, color, 1, 0};
}

int WINAPI
WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
  WNDCLASSW wc = {};
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = instance;
  wc.lpszClassName = L"d3d9_draw";
  RegisterClassW(&wc);
  HWND hwnd = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 128, 128, nullptr, nullptr, instance, nullptr);

  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  D3DPRESENT_PARAMETERS pp = {};
  pp.Windowed = TRUE;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.BackBufferWidth = 64;
  pp.BackBufferHeight = 64;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.hDeviceWindow = hwnd;
  if (!d3d || FAILED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device))) {
    printf("FAIL: device creation\n");
    return 1;
  }

  IDirect3DSurface9 *ds = nullptr;
  device->CreateRenderTarget(64, 64, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, nullptr);
  device->CreateDepthStencilSurface(64, 64, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &ds, nullptr);
  device->CreateOffscreenPlainSurface(64, 64, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, nullptr);
  device->SetRenderTarget(0, rt);
  device->SetDepthStencilSurface(ds);
  device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  device->SetRenderState(D3DRS_ZENABLE, FALSE);

  D3DVERTEXELEMENT9 elements[] = {
      {0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0},
      {0, 12, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_COLOR, 0},
      {0, 16, D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_TEXCOORD, 0},
      D3DDECL_END(),
  };
  IDirect3DVertexDeclaration9 *decl = nullptr;
  device->CreateVertexDeclaration(elements, &decl);
  auto vertex_shader = vs(kVS);
  auto ps_color = ps(kPSColor), ps_const = ps(kPSConst), ps_tex = ps(kPSTex);
  if (!vertex_shader || !ps_color || !ps_const || !ps_tex) {
    printf("FAIL: shader creation\n");
    return 1;
  }
  device->SetVertexDeclaration(decl);
  device->SetVertexShader(vertex_shader);

  IDirect3DVertexBuffer9 *vb = nullptr;
  device->CreateVertexBuffer(64 * sizeof(Vertex), D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &vb, nullptr);
  auto fill_vb = [&](const Vertex *v, int count) {
    void *data;
    vb->Lock(0, 0, &data, 0);
    memcpy(data, v, count * sizeof(Vertex));
    vb->Unlock();
  };

  device->BeginScene();

  {
    device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
    device->SetPixelShader(ps_const);
    float c[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    device->SetPixelShaderConstantF(0, c, 1);
    Vertex tri[3] = {{-1, -1, 0, 0, 0, 0}, {-1, 3, 0, 0, 0, 0}, {3, -1, 0, 0, 0, 0}};
    device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, sizeof(Vertex));
    expect("UP draw with pixel shader constant", 32, 32, 0xFF4080BF);
  }

  device->SetPixelShader(ps_color);
  {
    Vertex v[12];
    quad(v + 0, -1, 0, 0, 1, 0, 0xFFFF0000);
    quad(v + 4, 0, 0, 1, 1, 0, 0xFF00FF00);
    quad(v + 8, -1, -1, 0, 0, 0, 0xFF0000FF);
    fill_vb(v, 12);
    device->SetStreamSource(0, vb, 0, sizeof(Vertex));
    device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
    device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
    device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 4, 2);
    expect("strip quad top-left", 16, 16, 0xFFFF0000);
    expect("strip quad top-right", 48, 16, 0xFF00FF00);
    expect("unrendered bottom-left", 16, 48, 0xFF000000);

    IDirect3DIndexBuffer9 *ib = nullptr;
    device->CreateIndexBuffer(6 * sizeof(WORD), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &ib, nullptr);
    WORD *idx;
    ib->Lock(0, 0, (void **)&idx, 0);
    WORD quad_indices[6] = {0, 1, 2, 2, 1, 3};
    memcpy(idx, quad_indices, sizeof(quad_indices));
    ib->Unlock();
    device->SetIndices(ib);
    device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 8, 0, 4, 0, 2);
    expect("indexed draw with base vertex", 16, 48, 0xFF0000FF);
    ib->Release();
  }

  {
    Vertex fan[4];
    fan[0] = {0, -1, 0, 0xFFFFFF00, 0, 0};
    fan[1] = {0, 0, 0, 0xFFFFFF00, 0, 0};
    fan[2] = {1, 0, 0, 0xFFFFFF00, 0, 0};
    fan[3] = {1, -1, 0, 0xFFFFFF00, 0, 0};
    device->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, 2, fan, sizeof(Vertex));
    expect("triangle fan", 48, 48, 0xFFFFFF00);
  }

  {
    Vertex v[4];
    quad(v, -1, -1, 1, 1, 0, 0xFF808080);
    WORD indices[6] = {0, 1, 2, 2, 1, 3};
    device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
    device->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, 4, 2, indices, D3DFMT_INDEX16, v, sizeof(Vertex));
    expect("indexed UP draw", 10, 50, 0xFF808080);
  }

  {
    IDirect3DTexture9 *tex = nullptr;
    device->CreateTexture(2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr);
    D3DLOCKED_RECT lr;
    tex->LockRect(0, &lr, nullptr, 0);
    ((DWORD *)lr.pBits)[0] = 0xFFFF0000;
    ((DWORD *)lr.pBits)[1] = 0xFF00FF00;
    ((DWORD *)((char *)lr.pBits + lr.Pitch))[0] = 0xFF0000FF;
    ((DWORD *)((char *)lr.pBits + lr.Pitch))[1] = 0xFFFFFFFF;
    tex->UnlockRect(0);
    device->SetTexture(0, tex);
    device->SetPixelShader(ps_tex);
    Vertex v[4];
    quad(v, -1, -1, 1, 1, 0, 0);
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vertex));
    expect("texture texel (0,0)", 16, 16, 0xFFFF0000);
    expect("texture texel (1,0)", 48, 16, 0xFF00FF00);
    expect("texture texel (0,1)", 16, 48, 0xFF0000FF);
    expect("texture texel (1,1)", 48, 48, 0xFFFFFFFF);
    device->SetTexture(0, nullptr);
    tex->Release();
  }

  device->SetPixelShader(ps_color);
  {
    device->SetRenderState(D3DRS_ZENABLE, TRUE);
    device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESS);
    device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF000000, 1.0f, 0);
    Vertex v[4];
    quad(v, -1, -1, 1, 1, 0.5f, 0xFFFF0000);
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vertex));
    quad(v, -1, -1, 1, 1, 0.7f, 0xFF00FF00);
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vertex));
    expect("depth test rejects farther", 32, 32, 0xFFFF0000);
    quad(v, -1, -1, 0, 1, 0.3f, 0xFF0000FF);
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vertex));
    expect("depth test accepts nearer", 16, 32, 0xFF0000FF);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
  }

  {
    device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFFFF0000, 1.0f, 0);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    Vertex v[4];
    quad(v, -1, -1, 1, 1, 0, 0x80FFFFFF);
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vertex));
    // alpha blends too: 0.5 * 0.5 + 0.5 * 1.0
    expect("alpha blending", 32, 32, 0xBFFF8080, 3);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
  }

  {
    device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
    RECT scissor = {0, 0, 32, 64};
    device->SetScissorRect(&scissor);
    Vertex v[4];
    quad(v, -1, -1, 1, 1, 0, 0xFF00FFFF);
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vertex));
    expect("scissor inside", 16, 32, 0xFF00FFFF);
    expect("scissor outside", 48, 32, 0xFF000000);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
  }

  {
    device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_STENCIL, 0xFF000000, 1.0f, 0);
    device->SetRenderState(D3DRS_STENCILENABLE, TRUE);
    device->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_ALWAYS);
    device->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_REPLACE);
    device->SetRenderState(D3DRS_STENCILREF, 1);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, 0);
    Vertex v[4];
    quad(v, -1, -1, 0, 1, 0, 0xFFFFFFFF);
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vertex));
    device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    device->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_EQUAL);
    device->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_KEEP);
    quad(v, -1, -1, 1, 1, 0, 0xFF00FF00);
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vertex));
    expect("stencil pass", 16, 32, 0xFF00FF00);
    expect("stencil reject", 48, 32, 0xFF000000);
    device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
  }

  {
    struct FvfVertex {
      float x, y, z;
      DWORD color;
    } v[4] = {{-1, -1, 0, 0xFF102030}, {-1, 1, 0, 0xFF102030}, {1, -1, 0, 0xFF102030}, {1, 1, 0, 0xFF102030}};
    device->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
    DWORD fvf = 0;
    device->GetFVF(&fvf);
    check("GetFVF", fvf == (D3DFVF_XYZ | D3DFVF_DIFFUSE), fvf, D3DFVF_XYZ | D3DFVF_DIFFUSE);
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(FvfVertex));
    expect("FVF declaration", 32, 32, 0xFF102030);
    device->SetVertexDeclaration(decl);
  }

  {
    D3DVERTEXELEMENT9 ub_elements[] = {
        {0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_UBYTE4, 0, D3DDECLUSAGE_COLOR, 0},
        D3DDECL_END(),
    };
    IDirect3DVertexDeclaration9 *ub_decl = nullptr;
    device->CreateVertexDeclaration(ub_elements, &ub_decl);
    auto ub_vs = vs("struct O { float4 p : POSITION; float4 c : COLOR0; };"
                    "O main(float3 p : POSITION, float4 c : COLOR0) { O o; o.p = float4(p, 1); o.c = c / 255; return o; }");
    struct UbVertex {
      float x, y, z;
      BYTE b[4];
    } v[4] = {{-1, -1, 0, {64, 128, 192, 255}}, {-1, 1, 0, {64, 128, 192, 255}},
              {1, -1, 0, {64, 128, 192, 255}}, {1, 1, 0, {64, 128, 192, 255}}};
    device->SetVertexDeclaration(ub_decl);
    device->SetVertexShader(ub_vs);
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(UbVertex));
    expect("UBYTE4 vertex fetch", 32, 32, 0xFF4080C0);
    device->SetVertexDeclaration(decl);
    device->SetVertexShader(vertex_shader);
    if (ub_vs)
      ub_vs->Release();
    ub_decl->Release();
  }

  {
    D3DVERTEXELEMENT9 inst_elements[] = {
        {0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0},
        {1, 0, D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_TEXCOORD, 1},
        {1, 8, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_COLOR, 0},
        D3DDECL_END(),
    };
    IDirect3DVertexDeclaration9 *inst_decl = nullptr;
    device->CreateVertexDeclaration(inst_elements, &inst_decl);
    auto inst_vs = vs("struct O { float4 p : POSITION; float4 c : COLOR0; };"
                      "O main(float3 p : POSITION, float2 off : TEXCOORD1, float4 c : COLOR0) {"
                      "  O o; o.p = float4(p.xy + off, p.z, 1); o.c = c; return o; }");
    Vertex shape[4];
    quad(shape, -1, -1, 0, 0, 0, 0);
    struct Instance {
      float dx, dy;
      DWORD color;
    } instances[2] = {{0, 1, 0xFFFF00FF}, {1, 0, 0xFF00FFFF}};
    IDirect3DVertexBuffer9 *inst_vb = nullptr;
    device->CreateVertexBuffer(sizeof(instances), 0, 0, D3DPOOL_DEFAULT, &inst_vb, nullptr);
    void *data;
    inst_vb->Lock(0, 0, &data, 0);
    memcpy(data, instances, sizeof(instances));
    inst_vb->Unlock();
    fill_vb(shape, 4);
    IDirect3DIndexBuffer9 *ib = nullptr;
    device->CreateIndexBuffer(6 * sizeof(WORD), 0, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &ib, nullptr);
    WORD *idx;
    ib->Lock(0, 0, (void **)&idx, 0);
    WORD quad_indices[6] = {0, 1, 2, 2, 1, 3};
    memcpy(idx, quad_indices, sizeof(quad_indices));
    ib->Unlock();

    device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
    device->SetVertexDeclaration(inst_decl);
    device->SetVertexShader(inst_vs);
    device->SetStreamSource(0, vb, 0, sizeof(Vertex));
    device->SetStreamSource(1, inst_vb, 0, sizeof(Instance));
    device->SetStreamSourceFreq(0, D3DSTREAMSOURCE_INDEXEDDATA | 2);
    device->SetStreamSourceFreq(1, D3DSTREAMSOURCE_INSTANCEDATA | 1);
    device->SetIndices(ib);
    device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2);
    device->SetStreamSourceFreq(0, 1);
    device->SetStreamSourceFreq(1, 1);
    expect("instance 0", 16, 16, 0xFFFF00FF);
    expect("instance 1", 48, 48, 0xFF00FFFF);
    expect("not instanced", 16, 48, 0xFF000000);
    device->SetVertexDeclaration(decl);
    device->SetVertexShader(vertex_shader);
    ib->Release();
    inst_vb->Release();
    if (inst_vs)
      inst_vs->Release();
    inst_decl->Release();
  }

  device->EndScene();

  vb->Release();
  decl->Release();
  vertex_shader->Release();
  ps_color->Release();
  ps_const->Release();
  ps_tex->Release();
  readback->Release();
  ds->Release();
  rt->Release();
  device->Release();
  d3d->Release();
  printf("%s: %d failure(s)\n", failures ? "failed" : "passed", failures);
  return failures;
}
