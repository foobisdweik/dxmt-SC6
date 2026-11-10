// contract: each UAV owns its hidden counter, reset/preserve applies on binding, and counters wrap modulo 2^32.
// "Multiple UAVs created on the same Buffer with this flag will thus have multiple independent counters."
// "initialized to 0" (D3D11.3 5.3.10.1). "Specifying -1 means maintain the current counter value";
// "no overflow or underflow clamping" (5.3.10.2). increment returns the old value, decrement the new value
// (22.17.17/22.17.18). append records may be reordered, but must be packed into the count-sized interval
// (5.3.10.1). expected counter values use unsigned arithmetic; record comparisons use sorted multisets.
// CopyStructureCount copies the count to another buffer (5.3.10.2), not the records or adjacent words.
// 20.4.7: "CSInvocations must honor Compute Shader invocations from both Dispatch() and DispatchIndirect() APIs."
// the copied count also drives DispatchIndirect without CPU intervention; each dispatched thread writes its ID.
#include "d3d11_test.hpp"
#include <algorithm>
#include <array>
#include <climits>

static const char hlsl[] = R"hlsl(
cbuffer Params : register(b0) { uint amount; uint first; };
#if MODE < 2
RWStructuredBuffer<uint> records : register(u0);
RWStructuredBuffer<uint> result : register(u1);
[numthreads(1, 1, 1)] void cs() {
  for (uint i = 0; i < amount; i++) {
#if MODE == 0
    result[i] = records.IncrementCounter();
#else
    result[i] = records.DecrementCounter();
#endif
  }
}
#elif MODE == 2
AppendStructuredBuffer<uint4> records : register(u0);
[numthreads(1, 1, 1)] void cs() {
  for (uint i = 0; i < amount; i++) {
    uint id = first + i;
    records.Append(uint4(id, id + 1, ~id, id ^ first));
  }
}
#elif MODE == 3
ConsumeStructuredBuffer<uint4> records : register(u0);
RWStructuredBuffer<uint4> result : register(u1);
[numthreads(1, 1, 1)] void cs() {
  for (uint i = 0; i < amount; i++)
    result[i] = records.Consume();
}
#elif MODE == 4
RWStructuredBuffer<uint> result : register(u1);
[numthreads(1, 1, 1)] void cs(uint3 id : SV_DispatchThreadID) { result[id.x] = id.x + 1; }
#else
[numthreads(GROUP_X, GROUP_Y, GROUP_Z)] void cs() {}
#endif
)hlsl";

int
main() {
  const std::array<UINT, 3> group_size{3, 4, 5}; // D3D11.3 20.4.7's thread group example
  ComPtr<ID3DBlob> code[6];
  for (UINT mode = 0; mode < std::size(code); mode++) {
    code[mode] = compile(hlsl, "cs", "cs", {"MODE=" + std::to_string(mode),
        "GROUP_X=" + std::to_string(group_size[0]), "GROUP_Y=" + std::to_string(group_size[1]),
        "GROUP_Z=" + std::to_string(group_size[2])});
    if (!code[mode])
      return 1;
  }
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> immediate, deferred;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
                         &device, nullptr, &immediate));
  CHECK(device->CreateDeferredContext(0, &deferred));
  ComPtr<ID3D11ComputeShader> shaders[std::size(code)];
  for (UINT mode = 0; mode < std::size(code); mode++)
    CHECK(device->CreateComputeShader(code[mode]->GetBufferPointer(), code[mode]->GetBufferSize(), nullptr,
                                       &shaders[mode]));
  using Record = std::array<UINT, 4>;
  constexpr UINT sentinel = 0x45abcdef, first_element = 3;
  const UINT capacity = MULLION_PAGE_SIZE / sizeof(Record) + 1;
  auto constants = buffer(device.Get(), sizeof(Record), D3D11_BIND_CONSTANT_BUFFER);
  if (!expect(!!constants, "constant buffer was not created"))
    return verdict();
  auto structured = [&](UINT stride, ComPtr<ID3D11Buffer> &out) {
    std::vector<UINT> words((capacity + first_element * 2) * stride / sizeof(UINT), sentinel);
    D3D11_BUFFER_DESC desc{UINT(words.size() * sizeof(UINT)), D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS,
                            0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, stride};
    D3D11_SUBRESOURCE_DATA data{words.data()};
    return device->CreateBuffer(&desc, &data, &out);
  };
  auto submit = [&](bool listed) -> HRESULT {
    if (!listed)
      return S_OK;
    ComPtr<ID3D11CommandList> commands;
    HRESULT hr = deferred->FinishCommandList(FALSE, &commands);
    if (SUCCEEDED(hr))
      immediate->ExecuteCommandList(commands.Get(), FALSE);
    return hr;
  };
  for (bool listed : {false, true}) {
    auto context = listed ? deferred.Get() : immediate.Get();
    ComPtr<ID3D11Buffer> storage, output;
    CHECK(structured(sizeof(UINT), storage));
    CHECK(structured(sizeof(UINT), output));
    ComPtr<ID3D11UnorderedAccessView> counters[2], result;
    D3D11_UNORDERED_ACCESS_VIEW_DESC desc{DXGI_FORMAT_UNKNOWN, D3D11_UAV_DIMENSION_BUFFER};
    desc.Buffer = {first_element, capacity, D3D11_BUFFER_UAV_FLAG_COUNTER};
    for (auto &view : counters)
      CHECK(device->CreateUnorderedAccessView(storage.Get(), &desc, &view));
    desc.Buffer = {0, capacity, 0};
    CHECK(device->CreateUnorderedAccessView(output.Get(), &desc, &result));
    auto count = [&](ID3D11UnorderedAccessView *view, UINT want) -> int {
      for (bool staging : {false, true}) {
        const UINT words_count = 7;
        std::vector<UINT> words(words_count, sentinel);
        D3D11_BUFFER_DESC bd{words_count * UINT(sizeof(UINT)),
                              staging ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT,
                              0, staging ? D3D11_CPU_ACCESS_READ : 0u};
        D3D11_SUBRESOURCE_DATA initial{words.data()};
        ComPtr<ID3D11Buffer> copied;
        CHECK(device->CreateBuffer(&bd, &initial, &copied));
        for (UINT offset : {0u, UINT(sizeof(UINT)), bd.ByteWidth - UINT(sizeof(UINT))}) {
          context->CopyStructureCount(copied.Get(), offset, view);
          words[offset / sizeof(UINT)] = want;
        }
        CHECK(submit(listed));
        std::vector<UINT> got;
        if (staging) {
          D3D11_MAPPED_SUBRESOURCE mapped;
          CHECK(immediate->Map(copied.Get(), 0, D3D11_MAP_READ, 0, &mapped));
          got.resize(words_count);
          memcpy(got.data(), mapped.pData, bd.ByteWidth);
          immediate->Unmap(copied.Get(), 0);
        } else
          got = read(device.Get(), immediate.Get(), copied.Get());
        if (!expect(got == words, "counter %#x or adjacent words changed, staging %u", want, staging)) {
          for (UINT i = 0; i < got.size(); i++)
            expect(got[i] == words[i], "word %u: %#x, want %#x", i, got[i], words[i]);
          return 1;
        }
      }
      return 0;
    };
    step("new counters are independent, listed %u", listed);
    if (count(counters[0].Get(), 0) || count(counters[1].Get(), 0))
      return verdict();
    for (UINT mode : {0u, 1u})
      for (UINT initial : {0u, 1u, UINT_MAX - 1})
        for (UINT amount : {0u, 1u, 3u, 31u, 32u, 33u, 63u, 64u, 65u, capacity - 1, capacity}) {
          step("counter mode %u initial %#x amount %u listed %u", mode, initial, amount, listed);
          context->CSSetShader(shaders[mode].Get(), nullptr, 0);
          context->CSSetConstantBuffers(0, 1, constants.GetAddressOf());
          std::vector<UINT> untouched((capacity + first_element * 2), sentinel);
          context->UpdateSubresource(output.Get(), 0, nullptr, untouched.data(), 0, 0);
          const Record params{amount};
          context->UpdateSubresource(constants.Get(), 0, nullptr, params.data(), 0, 0);
          context->CSSetUnorderedAccessViews(0, 1, counters[0].GetAddressOf(), &initial);
          context->CSSetUnorderedAccessViews(1, 1, result.GetAddressOf(), nullptr);
          context->Dispatch(1, 1, 1);
          const UINT final = mode ? initial - amount : initial + amount;
          if (count(counters[0].Get(), final))
            return verdict();
          auto returned = read(device.Get(), immediate.Get(), output.Get());
          if (!expect(returned.size() >= amount, "counter return readback is incomplete"))
            return verdict();
          for (UINT i = 0; i < returned.size(); i++)
            expect(returned[i] == (i >= amount ? sentinel : mode ? initial - i - 1 : initial + i),
                   "returned counter %u: %#x", i, returned[i]);
          const UINT preserve = UINT_MAX;
          context->CSSetUnorderedAccessViews(0, 1, counters[0].GetAddressOf(), &preserve);
          if (count(counters[0].Get(), final) || count(counters[1].Get(), 0))
            return verdict();
        }
    ComPtr<ID3D11Buffer> appended, consumed;
    CHECK(structured(sizeof(Record), appended));
    CHECK(structured(sizeof(Record), consumed));
    ComPtr<ID3D11UnorderedAccessView> append_view, consume_result;
    desc.Buffer = {first_element, capacity, D3D11_BUFFER_UAV_FLAG_APPEND};
    CHECK(device->CreateUnorderedAccessView(appended.Get(), &desc, &append_view));
    desc.Buffer = {0, capacity, 0};
    CHECK(device->CreateUnorderedAccessView(consumed.Get(), &desc, &consume_result));
    for (UINT amount : {0u, 1u, 31u, 32u, 33u, 63u, 64u, 65u, capacity - 1, capacity}) {
      step("append/consume multiset amount %u listed %u", amount, listed);
      const UINT reset = 0, preserve = UINT_MAX;
      context->CSSetConstantBuffers(0, 1, constants.GetAddressOf());
      context->CSSetShader(shaders[2].Get(), nullptr, 0);
      context->CSSetUnorderedAccessViews(0, 1, append_view.GetAddressOf(), &reset);
      std::vector<Record> expected;
      for (UINT pass = 0; pass < 2; pass++) {
        const UINT first = pass ? amount / 2 : 0;
        const UINT n = pass ? amount - first : amount / 2;
        const Record params{n, first};
        context->UpdateSubresource(constants.Get(), 0, nullptr, params.data(), 0, 0);
        context->CSSetUnorderedAccessViews(0, 1, append_view.GetAddressOf(), &preserve);
        context->Dispatch(1, 1, 1);
        for (UINT id = first; id < first + n; id++)
          expected.push_back({id, id + 1, ~id, id ^ first});
      }
      if (count(append_view.Get(), amount))
        return verdict();
      auto words = read(device.Get(), immediate.Get(), appended.Get());
      if (!expect(words.size() == (capacity + first_element * 2) * std::tuple_size_v<Record>, "append readback size"))
        return verdict();
      std::vector<Record> packed(amount);
      if (amount)
        memcpy(packed.data(), words.data() + first_element * std::tuple_size_v<Record>, amount * sizeof(Record));
      for (UINT i = 0; i < words.size(); i++)
        if (i < first_element * std::tuple_size_v<Record> ||
            i >= (first_element + capacity) * std::tuple_size_v<Record>)
          expect(words[i] == sentinel, "append wrote outside its view at word %u", i);
      std::sort(packed.begin(), packed.end());
      std::sort(expected.begin(), expected.end());
      expect(packed == expected, "packed append records differ");
      context->CSSetConstantBuffers(0, 1, constants.GetAddressOf());
      context->CSSetShader(shaders[3].Get(), nullptr, 0);
      std::vector<UINT> untouched((capacity + first_element * 2) * std::tuple_size_v<Record>, sentinel);
      context->UpdateSubresource(consumed.Get(), 0, nullptr, untouched.data(), 0, 0);
      const Record params{amount};
      context->UpdateSubresource(constants.Get(), 0, nullptr, params.data(), 0, 0);
      context->CSSetUnorderedAccessViews(0, 1, append_view.GetAddressOf(), &preserve);
      context->CSSetUnorderedAccessViews(1, 1, consume_result.GetAddressOf(), nullptr);
      context->Dispatch(1, 1, 1);
      if (count(append_view.Get(), 0))
        return verdict();
      words = read(device.Get(), immediate.Get(), consumed.Get());
      if (!expect(words.size() >= amount * std::tuple_size_v<Record>, "consume readback size"))
        return verdict();
      if (amount)
        memcpy(packed.data(), words.data(), amount * sizeof(Record));
      std::sort(packed.begin(), packed.end());
      expect(packed == expected, "consumed records differ");
      for (UINT i = amount * std::tuple_size_v<Record>; i < words.size(); i++)
        expect(words[i] == sentinel, "consume wrote past its records at word %u", i);
    }
    for (UINT amount : {0u, 1u, 31u, 32u, 33u, capacity - 1, capacity}) {
      step("counter drives DispatchIndirect amount %u listed %u", amount, listed);
      std::vector<UINT> untouched(capacity + first_element * 2, sentinel);
      context->UpdateSubresource(output.Get(), 0, nullptr, untouched.data(), 0, 0);
      std::array<UINT, 6> args{sentinel, 0, 1, 1, sentinel, sentinel};
      constexpr UINT args_offset = sizeof(UINT);
      const D3D11_BUFFER_DESC args_desc{sizeof(args), D3D11_USAGE_DEFAULT, 0, 0,
                                       D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS};
      const D3D11_SUBRESOURCE_DATA initial{args.data()};
      ComPtr<ID3D11Buffer> arguments;
      CHECK(device->CreateBuffer(&args_desc, &initial, &arguments));
      context->CSSetUnorderedAccessViews(0, 1, counters[0].GetAddressOf(), &amount);
      context->CopyStructureCount(arguments.Get(), args_offset, counters[0].Get());
      context->CSSetShader(shaders[4].Get(), nullptr, 0);
      context->CSSetUnorderedAccessViews(1, 1, result.GetAddressOf(), nullptr);
      const D3D11_QUERY_DESC query_desc{D3D11_QUERY_PIPELINE_STATISTICS};
      ComPtr<ID3D11Query> query;
      CHECK(device->CreateQuery(&query_desc, &query));
      context->Begin(query.Get());
      context->DispatchIndirect(arguments.Get(), args_offset);
      context->End(query.Get());
      CHECK(submit(listed));
      auto got = read(device.Get(), immediate.Get(), output.Get());
      if (!expect(got.size() == untouched.size(), "indirect result readback size"))
        return verdict();
      for (UINT i = 0; i < got.size(); i++)
        expect(got[i] == (i < amount ? i + 1 : sentinel), "indirect word %u: %#x", i, got[i]);
      args[args_offset / sizeof(UINT)] = amount;
      got = read(device.Get(), immediate.Get(), arguments.Get());
      expect(got.size() == args.size() && !memcmp(got.data(), args.data(), sizeof(args)), "indirect arguments changed");
      D3D11_QUERY_DATA_PIPELINE_STATISTICS stats{};
      HRESULT hr = S_FALSE;
      while (hr == S_FALSE)
        hr = immediate->GetData(query.Get(), &stats, sizeof(stats), 0);
      expect(hr == S_OK && stats.CSInvocations == amount, "indirect CSInvocations %#lx: %llu, want %u",
             hr, stats.CSInvocations, amount);
    }
    const D3D11_QUERY_DESC query_desc{D3D11_QUERY_PIPELINE_STATISTICS};
    ComPtr<ID3D11Query> queries[2];
    for (auto &query : queries)
      CHECK(device->CreateQuery(&query_desc, &query));
    const UINT dispatches = MULLION_PAGE_SIZE / sizeof(UINT64) + 1;
    for (const auto &groups : {std::array<UINT, 3>{0, 2, 3}, {2, 0, 3}, {2, 3, 0}, {1, 1, 1}, {2, 3, 4}})
      for (bool indirect : {false, true}) {
        step("dispatch statistics %u,%u,%u indirect %u listed %u", groups[0], groups[1], groups[2], indirect, listed);
        const D3D11_BUFFER_DESC desc{sizeof(groups), D3D11_USAGE_DEFAULT, 0, 0,
                                    D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS};
        const D3D11_SUBRESOURCE_DATA initial{groups.data()};
        ComPtr<ID3D11Buffer> arguments;
        CHECK(device->CreateBuffer(&desc, &initial, &arguments));
        context->CSSetShader(shaders[std::size(shaders) - 1].Get(), nullptr, 0);
        for (auto &query : queries)
          context->Begin(query.Get());
        for (UINT dispatch = 0; dispatch < dispatches; dispatch++) {
          if (indirect)
            context->DispatchIndirect(arguments.Get(), 0);
          else
            context->Dispatch(groups[0], groups[1], groups[2]);
        }
        for (auto it = std::rbegin(queries); it != std::rend(queries); ++it)
          context->End(it->Get());
        const std::array<UINT, 3> overwritten{1, 1, 1};
        context->UpdateSubresource(arguments.Get(), 0, nullptr, overwritten.data(), 0, 0);
        CHECK(submit(listed));
        const UINT64 want = UINT64(dispatches) * groups[0] * groups[1] * groups[2] *
                            group_size[0] * group_size[1] * group_size[2];
        for (auto &query : queries) {
          D3D11_QUERY_DATA_PIPELINE_STATISTICS stats{};
          HRESULT hr = S_FALSE;
          while (hr == S_FALSE)
            hr = immediate->GetData(query.Get(), &stats, sizeof(stats), 0);
          expect(hr == S_OK && stats.CSInvocations == want, "CSInvocations %#lx: %llu, want %llu",
                 hr, stats.CSInvocations, want);
          expect(!stats.IAVertices && !stats.IAPrimitives && !stats.VSInvocations && !stats.PSInvocations &&
                     !stats.CInvocations && !stats.CPrimitives, "dispatch changed graphics statistics");
        }
      }
    immediate->ClearState();
    deferred->ClearState();
  }
  return verdict();
}
