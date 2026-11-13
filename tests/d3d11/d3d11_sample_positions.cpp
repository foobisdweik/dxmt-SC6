// contract: out-of-range and unbound sample positions are zero (D3D11.3 22.4.22, quoted in sample_positions.hpp).
// every advertised multisample count is queried through both immediate dispatch and a deferred command list.
#include "d3d11_test.hpp"
#include "../sample_positions.hpp"

int
main()
{
  auto code = compile(sample_positions::hlsl, "cs", "cs");
  if (!expect(!!code, "sample position HLSL compiled"))
    return verdict();
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> immediate, deferred;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr,
                          &immediate));
  CHECK(device->CreateDeferredContext(0, &deferred));
  ComPtr<ID3D11ComputeShader> shader;
  CHECK(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader));
  D3D11_BUFFER_DESC desc{sample_positions::bytes,
                         D3D11_USAGE_DEFAULT,
                         D3D11_BIND_UNORDERED_ACCESS,
                         0,
                         D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
                         sample_positions::components * sizeof(uint32_t)};
  ComPtr<ID3D11Buffer> output;
  ComPtr<ID3D11UnorderedAccessView> uav;
  CHECK(device->CreateBuffer(&desc, nullptr, &output));
  CHECK(device->CreateUnorderedAccessView(output.Get(), nullptr, &uav));
  for (UINT count = 0; count <= D3D11_MAX_MULTISAMPLE_SAMPLE_COUNT; count = count ? count * 2 : 1) {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    if (count) {
      UINT qualities = 0;
      CHECK(device->CheckMultisampleQualityLevels(DXGI_FORMAT_R32_FLOAT, count, &qualities));
      if (!qualities) {
        printf("not supported: %ux R32_FLOAT\n", count);
        continue;
      }
      D3D11_TEXTURE2D_DESC texture_desc{
          1, 1, 1, 1, DXGI_FORMAT_R32_FLOAT, {count, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE};
      CHECK(device->CreateTexture2D(&texture_desc, nullptr, &texture));
      D3D11_SHADER_RESOURCE_VIEW_DESC view{DXGI_FORMAT_R32_FLOAT, D3D11_SRV_DIMENSION_TEXTURE2DMS};
      CHECK(device->CreateShaderResourceView(texture.Get(), &view, &srv));
    }
    for (auto *context : {immediate.Get(), deferred.Get()}) {
      step("samplepos %ux, %s", count, context == immediate.Get() ? "immediate" : "deferred");
      const UINT sentinel[4] = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
      context->ClearUnorderedAccessViewUint(uav.Get(), sentinel);
      context->CSSetShader(shader.Get(), nullptr, 0);
      context->CSSetShaderResources(0, 1, srv.GetAddressOf());
      context->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
      context->Dispatch(1, 1, 1);
      context->ClearState();
      if (context == deferred.Get()) {
        ComPtr<ID3D11CommandList> commands;
        CHECK(deferred->FinishCommandList(FALSE, &commands));
        immediate->ExecuteCommandList(commands.Get(), FALSE);
      }
      auto got = read(device.Get(), immediate.Get(), output.Get());
      if (expect(got.size() * sizeof(uint32_t) == sample_positions::bytes, "sample position readback size"))
        sample_positions::check(got.data(), count);
    }
  }
  return verdict();
}
