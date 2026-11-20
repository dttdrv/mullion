// contract: cnd selects src1 when the comparison holds and src2 otherwise. ps_1_1 through ps_1_3 compare src0's alpha
// once for the whole instruction, ps_1_4 compares each component of src0, and the comparison is strictly greater than
// 0.5 (Microsoft Learn, cnd - ps; Wine visual.c cnd_test, ids 7794-7838).
#include <windows.h>
#include <d3d9.h>
#include <wrl/client.h>
#include <cstdio>

using Microsoft::WRL::ComPtr;

#define CHECK(x)                                                                                                       \
  if (FAILED(x)) {                                                                                                     \
    printf("failed: %s (line %d)\n", #x, __LINE__);                                                                    \
    return 1;                                                                                                          \
  }

static const UINT kClient = 64;
static const D3DCOLOR kClear = D3DCOLOR_XRGB(0, 0, 255);

int
main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  unsigned wrong = 0, checks = 0;
  // a lambda that returns void: a CHECK inside one would make its deduced return type int and fall off the end of the
  // lambda, which LLVM compiles into an unreachable trap, so a draw's calls are recorded as checks instead
  auto expect = [&](bool ok, const char *what) {
    checks++;
    if (!ok && wrong++ < 16)
      printf("wrong: %s\n", what);
  };

  // ps_1_1: cnd always compares the alpha, and a two-component FVF's fourth coordinate is 1, so the shader copies u
  // into alpha with the dp3 Wine's own cnd shader uses (a ps_1_1 source swizzle cannot move .x to .a directly). c1 is
  // red and c2 green, so u <= 0.5 selects green and u > 0.5 selects red
  static const DWORD cnd_11[] = {
      0xffff0101u,
      0x00000051u, 0xa00f0000u, 0x3f800000u, 0x00000000u, 0x00000000u, 0x00000000u, // def c0, 1, 0, 0, 0
      0x00000040u, 0xb00f0000u,                                                     // texcoord t0
      0x00000001u, 0x800f0000u, 0xb0e40000u,                                        // mov r0, t0
      0x00000008u, 0x800f0001u, 0x80e40000u, 0xa0e40000u,                           // dp3 r1, r0, c0
      0x00000001u, 0x80080000u, 0x80ff0001u,                                        // mov r0.a, r1.a
      0x00000050u, 0x800f0000u, 0x80ff0000u, 0xa0e40001u, 0xa0e40002u,              // cnd r0, r0.a, c1, c2
      0x0000ffffu,
  };

  // Wine visual.c cnd_test's coissue pairs: the alpha instruction runs, then the +cnd colour one rides on it. a
  // shader model 1 colour coissue always selects src1 (the alpha the pair's first instruction just wrote makes the
  // comparison hold: Wine's comment "cnd always selects the 2nd source"); ps_1_4 has no separate coissue behavior
  // (Wine visual.c: "ps_1_4 does not have a different cnd behavior"), and an alpha coissue compares for real
  static const DWORD cnd_11_coissue[] = {
      0xffff0101u,
      0x00000051u, 0xa00f0000u, 0x3f800000u, 0x00000000u, 0x00000000u, 0x00000000u, // def c0, 1, 0, 0, 0
      0x00000040u, 0xb00f0000u,                                                     // texcoord t0
      0x00000001u, 0x800f0000u, 0xb0e40000u,                                        // mov r0, t0
      0x00000008u, 0x800f0001u, 0x80e40000u, 0xa0e40000u,                           // dp3 r1, r0, c0
      0x00000001u, 0x80080000u, 0x80ff0001u,                                        // mov r0.a, r1.a
      0x40000050u, 0x80070000u, 0x80ff0000u, 0xa0e40001u, 0xa0e40002u,              // +cnd r0.xyz, r0.a, c1, c2
      0x0000ffffu,
  };

  static const DWORD cnd_11_coissue_2[] = {
      0xffff0101u,
      0x00000051u, 0xa00f0000u, 0x3f800000u, 0x00000000u, 0x00000000u, 0x00000000u, // def c0, 1, 0, 0, 0
      0x00000040u, 0xb00f0000u,                                                     // texcoord t0
      0x00000001u, 0x800f0000u, 0xb0e40000u,                                        // mov r0, t0
      0x00000008u, 0x800f0001u, 0x80e40000u, 0xa0e40000u,                           // dp3 r1, r0, c0
      0x00000001u, 0x800f0000u, 0x80e40001u,                                        // mov r0, r1
      0x40000050u, 0x80080000u, 0x80ff0000u, 0xa0e40001u, 0xa0e40002u,              // +cnd r0.a, r0.a, c1, c2
      0x00000001u, 0x80070000u, 0x80ff0000u,                                        // mov r0.xyz, r0.a
      0x0000ffffu,
  };

  // Wine visual.c's own ps_1_1 coissue_2 shader: the dp3 pair the reference run draws with (the extra mov r0.xyz,
  // r1.a is what the reference carries, so the coissue alpha comparison reads a value the rgb path also holds)
  static const DWORD cnd_11_coissue_wine[] = {
      0xffff0101u,
      0x00000051u, 0xa00f0000u, 0x3f800000u, 0x00000000u, 0x00000000u, 0x00000000u, // def c0, 1, 0, 0, 0
      0x00000051u, 0xa00f0003u, 0x3f000000u, 0x3f000000u, 0x3f000000u, 0x00000000u, // def c3, 0.5, 0.5, 0.5, 0
      0x00000040u, 0xb00f0000u,                                                     // texcoord t0
      0x00000001u, 0x800f0000u, 0xb0e40000u,                                        // mov r0, t0
      0x00000003u, 0x800f0000u, 0x80e40000u, 0xa0e40003u,                           // sub r0, r0, c3
      0x00000002u, 0x800f0001u, 0xa0e40000u, 0xa0e40000u,                           // add r1, c0, c0
      0x00000008u, 0x800f0001u, 0x80e40000u, 0x80e40001u,                           // dp3 r1, r0, r1
      0x00000001u, 0x800f0000u, 0x80e40001u,                                        // mov r0, r1
      0x00000001u, 0x80070000u, 0x80ff0001u,                                        // mov r0.xyz, r1.a
      0x40000050u, 0x80080000u, 0x80ff0000u, 0xa0e40001u, 0xa0e40002u,              // +cnd r0.a, r0.a, c1, c2
      0x00000001u, 0x80070000u, 0x80ff0000u,                                        // mov r0.xyz, r0.a
      0x0000ffffu,
  };

  // bisect: the reference's own shader minus its sub/add chain (dp3 of the raw texcoord), then with the sub, then
  // with both, to find which instruction the comparison's value loses
  static const DWORD cnd_11_coissue_ref_mov[] = {
      0xffff0101u,
      0x00000051u, 0xa00f0000u, 0x3f800000u, 0x00000000u, 0x00000000u, 0x00000000u, // def c0, 1, 0, 0, 0
      0x00000040u, 0xb00f0000u,                                                     // texcoord t0
      0x00000001u, 0x800f0000u, 0xb0e40000u,                                        // mov r0, t0
      0x00000008u, 0x800f0001u, 0x80e40000u, 0xa0e40000u,                           // dp3 r1, r0, c0
      0x00000001u, 0x800f0000u, 0x80e40001u,                                        // mov r0, r1
      0x00000001u, 0x80070000u, 0x80ff0001u,                                        // mov r0.xyz, r1.a
      0x40000050u, 0x80080000u, 0x80ff0000u, 0xa0e40001u, 0xa0e40002u,              // +cnd r0.a, r0.a, c1, c2
      0x00000001u, 0x80070000u, 0x80ff0000u,                                        // mov r0.xyz, r0.a
      0x0000ffffu,
  };
  static const DWORD cnd_11_coissue_ref_sub[] = {
      0xffff0101u,
      0x00000051u, 0xa00f0000u, 0x3f800000u, 0x00000000u, 0x00000000u, 0x00000000u, // def c0, 1, 0, 0, 0
      0x00000051u, 0xa00f0003u, 0x3f000000u, 0x3f000000u, 0x3f000000u, 0x00000000u, // def c3, 0.5, 0.5, 0.5, 0
      0x00000040u, 0xb00f0000u,                                                     // texcoord t0
      0x00000001u, 0x800f0000u, 0xb0e40000u,                                        // mov r0, t0
      0x00000003u, 0x800f0000u, 0x80e40000u, 0xa0e40003u,                           // sub r0, r0, c3
      0x00000002u, 0x800f0001u, 0xa0e40000u, 0xa0e40000u,                           // add r1, c0, c0
      0x00000008u, 0x800f0001u, 0x80e40000u, 0x80e40001u,                           // dp3 r1, r0, r1
      0x00000001u, 0x800f0000u, 0x80e40001u,                                        // mov r0, r1
      0x40000050u, 0x80080000u, 0x80ff0000u, 0xa0e40001u, 0xa0e40002u,              // +cnd r0.a, r0.a, c1, c2
      0x00000001u, 0x80070000u, 0x80ff0000u,                                        // mov r0.xyz, r0.a
      0x0000ffffu,
  };

  // ps_1_4: texcrd r0.xyz, t0 / mov r0.a, c0.a / cnd r0, r0, c1, c2. each component of (u, v, 0) is compared on its
  // own, so the four corners are the four mixtures of red (c1) and green (c2) that per-component selection makes
  static const DWORD cnd_14[] = {
      0xffff0104u,
      0x00000051u, 0xa00f0000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x3f800000u, // def c0, 0, 0, 0, 1
      0x00000040u, 0x80070000u, 0xb0e40000u,                                        // texcrd r0.xyz, t0
      0x00000001u, 0x80080000u, 0xa0ff0000u,                                        // mov r0.a, c0.a
      0x00000050u, 0x800f0000u, 0x80e40000u, 0xa0e40001u, 0xa0e40002u,              // cnd r0, r0, c1, c2
      0x0000ffffu,
  };

  WNDCLASSA wc{0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr, "d3d9_shader_cnd"};
  RegisterClassA(&wc);
  HWND window = CreateWindowA("d3d9_shader_cnd", "d3d9_shader_cnd", WS_OVERLAPPEDWINDOW, 0, 0, kClient, kClient,
                              nullptr, nullptr, wc.hInstance, nullptr);
  if (!window) {
    printf("failed: no window\n");
    return 1;
  }
  ComPtr<IDirect3D9> d3d9;
  d3d9.Attach(Direct3DCreate9(D3D_SDK_VERSION));
  if (!d3d9) {
    printf("failed: Direct3DCreate9 made no object\n");
    return 1;
  }
  D3DPRESENT_PARAMETERS present{};
  present.BackBufferWidth = kClient;
  present.BackBufferHeight = kClient;
  present.BackBufferFormat = D3DFMT_X8R8G8B8;
  present.BackBufferCount = 1;
  present.SwapEffect = D3DSWAPEFFECT_DISCARD;
  present.hDeviceWindow = window;
  present.Windowed = TRUE;
  present.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  ComPtr<IDirect3DDevice9> device;
  CHECK(d3d9->CreateDevice(0, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &present, &device));
  CHECK(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));
  CHECK(device->SetRenderState(D3DRS_LIGHTING, FALSE));
  // the FVF names one texcoord, which is what the cnd programs read; no texture is bound, as in the Wine test, so the
  // texcoord has to reach the program through the fixed-function vertex path alone
  CHECK(device->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1));
  const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
  const float green[4] = {0.0f, 1.0f, 0.0f, 1.0f};
  const float red_a0[4] = {1.0f, 0.0f, 0.0f, 0.0f};
  CHECK(device->SetPixelShaderConstantF(1, red, 1));
  CHECK(device->SetPixelShaderConstantF(2, green, 1));

  // u runs left to right, and v runs from 0 at the bottom to 1 at the top: Direct3D's viewport maps clip y = -1 to the
  // bottom of the target and y = +1 to the top, so the first two vertices are the bottom edge
  const float quad[4][5] = {
      {-1.0f, -1.0f, 0.0f, 0.0f, 0.0f},
      { 1.0f, -1.0f, 0.0f, 1.0f, 0.0f},
      {-1.0f,  1.0f, 0.0f, 0.0f, 1.0f},
      { 1.0f,  1.0f, 0.0f, 1.0f, 1.0f},
  };

  ComPtr<IDirect3DSurface9> readback, back;
  CHECK(device->CreateOffscreenPlainSurface(kClient, kClient, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, nullptr));
  auto pixel = [&](UINT x, UINT y) -> D3DCOLOR {
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)))
      return 0;
    D3DLOCKED_RECT locked{};
    if (FAILED(device->GetRenderTargetData(back.Get(), readback.Get())) ||
        FAILED(readback->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
      back.Reset();
      return 0;
    }
    const DWORD *row = (const DWORD *)((const char *)locked.pBits + (size_t)y * locked.Pitch);
    const D3DCOLOR got = row[x] & 0x00FFFFFF;
    readback->UnlockRect();
    back.Reset();
    return got;
  };

  // the four pixels are read once per draw: a repeated GetRenderTargetData of the same frame trips a readback lifetime
  // bug this suite records separately, and holding the values also makes each expectation use the frame it names.
  // y = 48 is v = 0.25 (near the bottom) and y = 16 is v = 0.75 (near the top), away from the comparison at 0.5
  struct Shot {
    D3DCOLOR left_bottom = 0, right_bottom = 0, left_top = 0, right_top = 0;
  } shot;
  auto draw = [&](IDirect3DPixelShader9 *shader) {
    expect(SUCCEEDED(device->SetPixelShader(shader)), "SetPixelShader was refused");
    expect(SUCCEEDED(device->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1.0f, 0)), "Clear was refused");
    expect(SUCCEEDED(device->BeginScene()), "BeginScene was refused");
    const HRESULT hr = device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, 5 * sizeof(float));
    expect(SUCCEEDED(device->EndScene()), "EndScene was refused");
    shot = Shot{pixel(8, 48), pixel(56, 48), pixel(8, 16), pixel(56, 16)};
    expect(SUCCEEDED(hr), "the cnd draw was refused");
  };

  ComPtr<IDirect3DPixelShader9> shader_11, shader_14, shader_11_coissue, shader_11_coissue_2;
  CHECK(device->CreatePixelShader(cnd_11, &shader_11));
  CHECK(device->CreatePixelShader(cnd_14, &shader_14));
  CHECK(device->CreatePixelShader(cnd_11_coissue, &shader_11_coissue));
  CHECK(device->CreatePixelShader(cnd_11_coissue_2, &shader_11_coissue_2));
  draw(shader_11.Get());
  expect(shot.left_bottom == 0x0000FF00, "ps_1_1 cnd did not select c2 where alpha was at most 0.5");
  expect(shot.right_bottom == 0x00FF0000, "ps_1_1 cnd did not select c1 where alpha was above 0.5");
  // the same u at both v: a comparison of more than alpha would make these two rows differ
  expect(shot.left_top == 0x0000FF00 && shot.right_top == 0x00FF0000, "ps_1_1 cnd compared more than the alpha");

  draw(shader_14.Get());
  expect(shot.left_bottom == 0x0000FF00, "ps_1_4 cnd did not select c2's green for both low coordinates");
  expect(shot.right_bottom == 0x00FFFF00, "ps_1_4 cnd did not mix the two sources per component");
  expect(shot.left_top == 0x00000000, "ps_1_4 cnd did not select c1 for both high coordinates");
  expect(shot.right_top == 0x00FF0000, "ps_1_4 cnd did not select c1's red where u was above 0.5");

  auto named = [&](const char *what, D3DCOLOR got) {
    printf("wrong: %s (got %06x)\n", what, (unsigned)got);
    wrong++;
  };
  auto check = [&](bool ok, const char *what, D3DCOLOR got) {
    checks++;
    if (!ok && wrong < 24)
      named(what, got);
    else if (!ok)
      wrong++;
  };

  // the coissue colour pair: c1's colour rides on the alpha the first instruction wrote, whatever u held, and the
  // alpha the output keeps is r0.a from the pair's first instruction (the output's alpha is the comparison's value)
  draw(shader_11_coissue.Get());
  check(shot.left_bottom == 0x00FF0000, "ps_1_1 +cnd colour did not select c1 at the left", shot.left_bottom);
  check(shot.right_bottom == 0x00FF0000, "ps_1_1 +cnd colour did not select c1 at the right", shot.right_bottom);
  check(shot.left_top == 0x00FF0000, "ps_1_1 +cnd colour did not select c1 at the top left", shot.left_top);
  check(shot.right_top == 0x00FF0000, "ps_1_1 +cnd colour did not select c1 at the top right", shot.right_top);

  // the coissue alpha pair: the comparison is the real one, so u <= 0.5 keeps c2.a and u > 0.5 takes c1.a, and the
  // program copies that alpha into its colour. c1.a is 0 and c2.a is 1, so the left is white and the right is black
  CHECK(device->SetPixelShaderConstantF(1, red_a0, 1));
  draw(shader_11_coissue_2.Get());
  check(shot.left_bottom == 0x00FFFFFF, "ps_1_1 +cnd alpha did not keep c2.a at the left", shot.left_bottom);
  check(shot.right_bottom == 0x00000000, "ps_1_1 +cnd alpha did not take c1.a where u was above 0.5", shot.right_bottom);

  // the reference's own shader: c3 = 0.5 and r1 = c0 + c0 make the comparison's value the u scaled to [-1;1], whose
  // right side saturates, and the two movs around the cnd give the same colours the one above does
  ComPtr<IDirect3DPixelShader9> shader_11_coissue_wine, shader_11_coissue_ref_mov, shader_11_coissue_ref_sub;
  CHECK(device->CreatePixelShader(cnd_11_coissue_wine, &shader_11_coissue_wine));
  CHECK(device->CreatePixelShader(cnd_11_coissue_ref_mov, &shader_11_coissue_ref_mov));
  CHECK(device->CreatePixelShader(cnd_11_coissue_ref_sub, &shader_11_coissue_ref_sub));
  draw(shader_11_coissue_wine.Get());
  check(shot.left_bottom == 0x00FFFFFF || shot.left_bottom == 0x00000000,
        "ps_1_1 reference +cnd alpha kept neither alpha at the left", shot.left_bottom);
  check(shot.right_bottom == 0x00000000, "ps_1_1 reference +cnd alpha did not take c1.a where u was above 0.5",
        shot.right_bottom);

  draw(shader_11_coissue_ref_mov.Get());
  check(shot.right_bottom == 0x00000000, "ps_1_1 bisect (the extra mov) did not take c1.a at the right",
        shot.right_bottom);

  draw(shader_11_coissue_ref_sub.Get());
  check(shot.right_bottom == 0x00000000, "ps_1_1 bisect (the sub chain) did not take c1.a at the right",
        shot.right_bottom);

  printf("%s: %u checks, %u wrong\n", wrong ? "failed" : "passed", checks, wrong);
  return wrong ? 1 : 0;
}
