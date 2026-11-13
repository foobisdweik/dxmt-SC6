// contract: an effect compiled before device creation creates through d3d10.dll
// and its pass sets the shaders and constants used by the next draw.
// ID3D10EffectPass::Apply: "Set the state contained in a pass to the device."
// https://learn.microsoft.com/en-us/windows/win32/api/d3d10effect/nf-d3d10effect-id3d10effectpass-apply
// D3D10CreateEffectFromMemory: "Creates an ID3D10Effect from a buffer
// containing a compiled effect."
// https://learn.microsoft.com/en-us/windows/win32/api/d3d10effect/nf-d3d10effect-d3d10createeffectfrommemory
// Wine supplies d3d10.dll and the Effects runtime; its device calls must reach
// Mullion's d3d10core. A present runtime that cannot apply its pass fails.
// Every RGBA32_FLOAT texel must equal the exactly representable variable.
#include "../d3d11/d3d11_test.hpp"
#include <d3d10_1.h>
#include <iterator>

static const char hlsl[] = R"hlsl(
cbuffer Constants { float4 Color; };
float4 vs(uint id : SV_VertexID) : SV_Position {
  return float4(float2((id << 1) & 2, id & 2) * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps() : SV_Target { return Color; }
technique10 Render {
  pass Paint {
    SetVertexShader(CompileShader(vs_4_0, vs()));
    SetGeometryShader(NULL);
    SetPixelShader(CompileShader(ps_4_0, ps()));
  }
}
)hlsl";

int
main() {
  ComPtr<ID3DBlob> code, errors;
  HRESULT hr = D3DCompile(hlsl, sizeof(hlsl) - 1, nullptr, nullptr, nullptr, nullptr, "fx_4_0", 0, 0, &code, &errors);
  if (FAILED(hr) && errors)
    printf("fx_4_0: %.*s\n", int(errors->GetBufferSize()), static_cast<const char *>(errors->GetBufferPointer()));
  if (!expect(hr == S_OK && code, "effect compilation returned %#lx", hr))
    return verdict();
  auto create = reinterpret_cast<decltype(&D3D10CreateDevice1)>(
      GetProcAddress(LoadLibraryA("d3d10_1.dll"), "D3D10CreateDevice1")
  );
  auto create_effect = reinterpret_cast<decltype(&D3D10CreateEffectFromMemory)>(
      GetProcAddress(LoadLibraryA("d3d10.dll"), "D3D10CreateEffectFromMemory")
  );
  if (!expect(create && create_effect, "D3D10 creation or Effects export is unavailable"))
    return verdict();
  const float colors[][D3D10_STANDARD_VECTOR_SIZE] = {{1, 0, 0.25f, 0.5f}, {0, 1, 0.5f, 0.25f}, {-1, 2, -0.5f, 1}};
  for (auto level : {D3D10_FEATURE_LEVEL_10_0, D3D10_FEATURE_LEVEL_10_1}) {
    ComPtr<ID3D10Device1> device;
    CHECK(create(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0, level, D3D10_1_SDK_VERSION, &device));
    ComPtr<ID3D10Effect> effect;
    CHECK(create_effect(code->GetBufferPointer(), code->GetBufferSize(), 0, device.Get(), nullptr, &effect));
    // IsValid is Wine's d3d10.dll, a stub that answers FALSE; Mullion supplies only d3d10core
    expect(!effect->IsPool(), "compiled effect is a pool");
    ComPtr<ID3D10Device> owner;
    CHECK(effect->GetDevice(&owner));
    expect(owner.Get() == device.Get(), "effect has a different device");
    auto technique = effect->GetTechniqueByName("Render");
    if (!expect(technique && technique->IsValid(), "named technique is invalid"))
      return verdict();
    D3D10_TECHNIQUE_DESC technique_desc{};
    CHECK(technique->GetDesc(&technique_desc));
    expect(technique_desc.Passes == 1 && !strcmp(technique_desc.Name, "Render"), "technique description");
    auto pass = technique->GetPassByName("Paint");
    auto variable = effect->GetVariableByName("Color")->AsVector();
    if (!expect(pass && pass->IsValid() && variable && variable->IsValid(), "pass or color variable is invalid"))
      return verdict();
    expect(
        !effect->GetTechniqueByName("missing")->IsValid() && !effect->GetVariableByName("missing")->IsValid(),
        "missing effect names are valid"
    );
    const UINT width = std::size(colors), height = D3D10_STANDARD_VECTOR_SIZE;
    D3D10_TEXTURE2D_DESC desc{
        width, height, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, {1, 0}, D3D10_USAGE_DEFAULT, D3D10_BIND_RENDER_TARGET
    };
    ComPtr<ID3D10Texture2D> target, staging;
    ComPtr<ID3D10RenderTargetView> view;
    CHECK(device->CreateTexture2D(&desc, nullptr, &target));
    CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &view));
    desc.Usage = D3D10_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
    CHECK(device->CreateTexture2D(&desc, nullptr, &staging));
    for (const auto &color : colors) {
      step("level %#x Effects pass color (%g,%g,%g,%g)", level, color[0], color[1], color[2], color[3]);
      device->ClearState();
      const float poison[4]{};
      device->ClearRenderTargetView(view.Get(), poison);
      float value[D3D10_STANDARD_VECTOR_SIZE];
      memcpy(value, color, sizeof(value));
      CHECK(variable->SetFloatVector(value));
      CHECK(pass->Apply(0));
      ComPtr<ID3D10VertexShader> vertex;
      ComPtr<ID3D10PixelShader> pixel;
      device->VSGetShader(&vertex);
      device->PSGetShader(&pixel);
      expect(vertex && pixel, "effect did not bind both shaders");
      D3D10_VIEWPORT viewport{0, 0, width, height, 0, 1};
      device->RSSetViewports(1, &viewport);
      device->OMSetRenderTargets(1, view.GetAddressOf(), nullptr);
      device->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      device->Draw(3, 0);
      device->CopyResource(staging.Get(), target.Get());
      D3D10_MAPPED_TEXTURE2D mapped{};
      CHECK(staging->Map(0, D3D10_MAP_READ, 0, &mapped));
      bool same = true;
      for (UINT y = 0; y < height; y++) {
        auto row = static_cast<const char *>(mapped.pData) + y * mapped.RowPitch;
        for (UINT x = 0; x < width; x++)
          same &= !memcmp(row + x * sizeof(color), color, sizeof(color));
      }
      expect(same, "effect pass did not render its variable into every texel");
      staging->Unmap(0);
    }
    device->ClearState();
  }
  return verdict();
}
