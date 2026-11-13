// contract: "x*y*z <= 1024" and shared storage "32kB" (D3D11.3 22.3.40, 22.3.45-46).
// the full memory-fence contract and CPU oracle are in shader_groups.hpp; both D3D11 context paths read every word.
#include "d3d11_test.hpp"
#include "../shader_groups.hpp"

int
main()
{
  struct Case {
    shader_groups::Shape shape;
    UINT words;
    ComPtr<ID3DBlob> code;
  };
  std::vector<Case> cases;
  for (auto shape : shader_groups::shapes(D3D11_CS_THREAD_GROUP_MAX_X, D3D11_CS_THREAD_GROUP_MAX_Z,
                                          D3D11_CS_THREAD_GROUP_MAX_THREADS_PER_GROUP))
    for (UINT words : {D3D11_CS_TGSM_REGISTER_COUNT - 1u, UINT(D3D11_CS_TGSM_REGISTER_COUNT)}) {
      auto code = compile(shader_groups::hlsl, "cs", "cs", shader_groups::defines(shape, words));
      if (!expect(!!code, "group %ux%ux%u, %u shared words compiled", shape.x, shape.y, shape.z, words))
        return verdict();
      cases.push_back({shape, words, code});
    }
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> immediate, deferred;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr,
                          &immediate));
  CHECK(device->CreateDeferredContext(0, &deferred));
  unsigned skipped = 0;
  for (auto &c : cases) {
    if (!shader_groups::runs(c.words, D3D11_CS_TGSM_REGISTER_COUNT)) {
      skipped++;
      continue;
    }
    const UINT bytes =
        shader_groups::group_count * shader_groups::rounds * c.words * shader_groups::components * sizeof(uint32_t);
    D3D11_BUFFER_DESC desc{bytes,
                           D3D11_USAGE_DEFAULT,
                           D3D11_BIND_UNORDERED_ACCESS,
                           0,
                           D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
                           shader_groups::components * sizeof(uint32_t)};
    ComPtr<ID3D11Buffer> output;
    ComPtr<ID3D11UnorderedAccessView> uav;
    ComPtr<ID3D11ComputeShader> shader;
    CHECK(device->CreateBuffer(&desc, nullptr, &output));
    CHECK(device->CreateUnorderedAccessView(output.Get(), nullptr, &uav));
    CHECK(device->CreateComputeShader(c.code->GetBufferPointer(), c.code->GetBufferSize(), nullptr, &shader));
    for (auto *context : {immediate.Get(), deferred.Get()}) {
      step("group %ux%ux%u, %u shared words, %s", c.shape.x, c.shape.y, c.shape.z, c.words,
           context == immediate.Get() ? "immediate" : "deferred");
      const UINT sentinel[] = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
      context->ClearUnorderedAccessViewUint(uav.Get(), sentinel);
      context->CSSetShader(shader.Get(), nullptr, 0);
      context->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
      context->Dispatch(shader_groups::groups.x, shader_groups::groups.y, shader_groups::groups.z);
      context->ClearState();
      if (context == deferred.Get()) {
        ComPtr<ID3D11CommandList> commands;
        CHECK(deferred->FinishCommandList(FALSE, &commands));
        immediate->ExecuteCommandList(commands.Get(), FALSE);
      }
      auto got = read(device.Get(), immediate.Get(), output.Get());
      if (expect(got.size() * sizeof(uint32_t) == bytes, "full group output readback"))
        shader_groups::check(got.data(), c.shape, c.words);
    }
  }
  return shader_groups::verdict(skipped);
}
