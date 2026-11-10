// contract: see ../ia_formats.hpp for the format table, conversion rules and enumerated inputs.
// D3D11.3 14.2.1: stream output "must simply dump out the 32 bits (per component) of data out unaltered".
// the same input layouts and bytes pass through D3D10.0, D3D10.1, and D3D11 immediate/deferred contexts.
#include "d3d11_test.hpp"
#include "../ia_formats.hpp"
#include <d3d10_1.h>

int
main() {
  using namespace ia_formats;
  const char *entries[] = {"as_float", "as_uint", "as_int"};
  ComPtr<ID3DBlob> codes[2][std::size(entries)];
  step("compile all shaders before creating devices");
  for (UINT legacy = 0; legacy < std::size(codes); legacy++)
    for (UINT kind = 0; kind < std::size(entries); kind++) {
      codes[legacy][kind] = compile(hlsl, entries[kind], legacy ? "vs_4_0" : "vs_5_0");
      if (!expect(bool(codes[legacy][kind]), "HLSL did not compile"))
        return verdict();
    }
  auto create10 = reinterpret_cast<decltype(&D3D10CreateDevice1)>(
      (void *)GetProcAddress(LoadLibraryA("d3d10_1.dll"), "D3D10CreateDevice1"));
  if (!expect(create10 != nullptr, "no D3D10CreateDevice1"))
    return verdict();
  for (UINT api : {11u, 10u, 101u}) {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> immediate, deferred;
    ComPtr<ID3D10Device1> device10;
    if (api == 11) {
      const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
      CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
                              &device, nullptr, &immediate));
      CHECK(device->CreateDeferredContext(0, &deferred));
    } else {
      CHECK(create10(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0,
                     api == 10 ? D3D10_FEATURE_LEVEL_10_0 : D3D10_FEATURE_LEVEL_10_1,
                     D3D10_1_SDK_VERSION, &device10));
      CHECK(device10.As(&device));
      device->GetImmediateContext(&immediate);
    }
    const UINT output_stride = sizeof(UINT[4]), sentinel = 0xa5a5a5a5;
    const D3D11_SO_DECLARATION_ENTRY declaration{0, "VALUE", 0, 0, 4, 0};
    ComPtr<ID3D11VertexShader> vertex[std::size(entries)];
    ComPtr<ID3D11GeometryShader> stream[std::size(entries)];
    ComPtr<ID3D10VertexShader> vertex10[std::size(entries)];
    ComPtr<ID3D10GeometryShader> stream10[std::size(entries)];
    for (UINT kind = 0; kind < std::size(entries); kind++) {
      auto code = codes[api != 11][kind].Get();
      if (device10) {
        const D3D10_SO_DECLARATION_ENTRY entry{"VALUE", 0, 0, 4, 0};
        CHECK(device10->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), &vertex10[kind]));
        CHECK(device10->CreateGeometryShaderWithStreamOutput(code->GetBufferPointer(), code->GetBufferSize(), &entry,
            1, output_stride, &stream10[kind]));
      } else {
        CHECK(device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &vertex[kind]));
        CHECK(device->CreateGeometryShaderWithStreamOutput(code->GetBufferPointer(), code->GetBufferSize(), &declaration,
            1, &output_stride, 1, D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &stream[kind]));
      }
    }
    for (auto format : other_formats()) {
      step("D3D%u non-IA format %u: capability and rejected layout", api, UINT(format));
      UINT support = 0;
      HRESULT hr = device10 ? device10->CheckFormatSupport(format, &support) : device->CheckFormatSupport(format, &support);
      expect(hr == S_OK || hr == E_INVALIDARG || hr == E_FAIL, "format query returned %#lx", hr);
      if (hr == S_OK)
        expect(!(support & D3D11_FORMAT_SUPPORT_IA_VERTEX_BUFFER), "non-IA format advertised for vertex input");
      ComPtr<ID3D11InputLayout> layout;
      ComPtr<ID3D10InputLayout> layout10;
      auto code = codes[api != 11][0].Get();
      if (device10) {
        const D3D10_INPUT_ELEMENT_DESC element{"DATA", 0, format};
        hr = device10->CreateInputLayout(&element, 1, code->GetBufferPointer(), code->GetBufferSize(), &layout10);
      } else {
        const D3D11_INPUT_ELEMENT_DESC element{"DATA", 0, format};
        hr = device->CreateInputLayout(&element, 1, code->GetBufferPointer(), code->GetBufferSize(), &layout);
      }
      expect(hr == E_INVALIDARG || hr == E_FAIL, "non-IA layout returned %#lx", hr);
      expect(!layout && !layout10, "non-IA format returned a layout");
    }
    for (const auto &f : formats) {
      const UINT kind = f.kind == Uint ? 1 : f.kind == Sint ? 2 : 0;
      auto code = codes[api != 11][kind].Get();
      step("D3D%u %s: capability and layout", api, f.name);
      UINT support = 0;
      const HRESULT queried = device10 ? device10->CheckFormatSupport(f.format, &support) : device->CheckFormatSupport(f.format, &support);
      expect(queried == S_OK || (!f.required && (queried == E_INVALIDARG || queried == E_FAIL)), "format query returned %#lx", queried);
      const bool supported = queried == S_OK && (support & D3D11_FORMAT_SUPPORT_IA_VERTEX_BUFFER);
      expect(!f.required || supported, "required vertex format is not advertised");
      const UINT slots = api == 10 ? D3D10_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT : D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT;
      for (const auto &input_layout : layouts(f, slots, D3D11_APPEND_ALIGNED_ELEMENT,
                                              D3D11_STANDARD_MAXIMUM_ELEMENT_ALIGNMENT_BYTE_MULTIPLE)) {
        step("D3D%u %s: %s", api, f.name, input_layout.name);
        ComPtr<ID3D11InputLayout> layout;
        ComPtr<ID3D10InputLayout> layout10;
        HRESULT hr;
        if (device10) {
          std::vector<D3D10_INPUT_ELEMENT_DESC> elements;
          for (const auto &e : input_layout.elements)
            elements.push_back({e.name, 0, e.format, e.slot, e.offset, D3D10_INPUT_PER_VERTEX_DATA, 0});
          hr = device10->CreateInputLayout(elements.data(), elements.size(), code->GetBufferPointer(), code->GetBufferSize(), &layout10);
        } else {
          std::vector<D3D11_INPUT_ELEMENT_DESC> elements;
          for (const auto &e : input_layout.elements)
            elements.push_back({e.name, 0, e.format, e.slot, e.offset, D3D11_INPUT_PER_VERTEX_DATA, 0});
          hr = device->CreateInputLayout(elements.data(), elements.size(), code->GetBufferPointer(), code->GetBufferSize(), &layout);
        }
        expect(supported ? hr == S_OK : FAILED(hr), "layout returned %#lx, advertised=%u", hr, supported);
        expect(bool(layout || layout10) == SUCCEEDED(hr), "wrong layout out parameter");
        if (!supported || FAILED(hr))
          continue;
        auto bytes = data(f);
        const UINT offset = input_layout.binding_offset, zero = 0;
        bytes.insert(bytes.begin(), offset + input_layout.element_offset, uint8_t(sentinel));
        const UINT vertices = count(f), input_stride = stride(f);
        D3D11_BUFFER_DESC vb_desc{UINT(bytes.size()), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER};
        const D3D11_SUBRESOURCE_DATA initial{bytes.data()};
        ComPtr<ID3D11Buffer> input;
        CHECK(device->CreateBuffer(&vb_desc, &initial, &input));
        auto output = buffer(device.Get(), (vertices + 1) * output_stride, D3D11_BIND_STREAM_OUTPUT, sentinel);
        if (!expect(bool(output), "no stream output buffer"))
          return verdict();
        ComPtr<ID3D10Buffer> input10, output10;
        if (device10) {
          CHECK(input.As(&input10));
          CHECK(output.As(&output10));
        }
        for (UINT recorded = 0; recorded < (api == 11 ? 2u : 1u); recorded++) {
          step("D3D%u %s: %u encodings, recorded=%u", api, f.name, vertices, recorded);
          auto context = recorded ? deferred.Get() : immediate.Get();
          std::vector<UINT> fill((vertices + 1) * 4, sentinel);
          context->UpdateSubresource(output.Get(), 0, nullptr, fill.data(), 0, 0);
          if (device10) {
            device10->IASetInputLayout(layout10.Get());
            device10->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_POINTLIST);
            device10->IASetVertexBuffers(input_layout.slot, 1, input10.GetAddressOf(), &input_stride, &offset);
            device10->VSSetShader(vertex10[kind].Get());
            device10->GSSetShader(stream10[kind].Get());
            device10->PSSetShader(nullptr);
            device10->SOSetTargets(1, output10.GetAddressOf(), &zero);
            device10->Draw(vertices, 0);
            device10->SOSetTargets(0, nullptr, nullptr);
          } else {
            context->IASetInputLayout(layout.Get());
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
            context->IASetVertexBuffers(input_layout.slot, 1, input.GetAddressOf(), &input_stride, &offset);
            context->VSSetShader(vertex[kind].Get(), nullptr, 0);
            context->GSSetShader(stream[kind].Get(), nullptr, 0);
            context->SOSetTargets(1, output.GetAddressOf(), &zero);
            context->Draw(vertices, 0);
            context->SOSetTargets(0, nullptr, nullptr);
          }
          if (recorded) {
            ComPtr<ID3D11CommandList> list;
            CHECK(context->FinishCommandList(FALSE, &list));
            immediate->ExecuteCommandList(list.Get(), TRUE);
          }
          auto got = read(device.Get(), immediate.Get(), output.Get());
          if (!expect(got.size() == fill.size(), "read %zu words, want %zu", got.size(), fill.size()))
            continue;
          UINT wrong = 0;
          for (UINT v = 0; v < vertices; v++)
            for (UINT c = 0; c < 4; c++) {
              const UINT out = f.bgra && c < 3 ? 2 - c : c;
              if (!matches(f, v, c, got[v * 4 + out]) && wrong++ < 4)
                expect(false, "vertex %u component %u is %#x, want %#x", v, out, got[v * 4 + out], expected(f, v, c));
            }
          expect(wrong <= 4, "%u additional wrong components", wrong > 4 ? wrong - 4 : 0);
          for (UINT c = 0; c < 4; c++)
            expect(got[vertices * 4 + c] == sentinel, "wrote beyond the last vertex");
        }
      }
    }
  }
  return verdict();
}
