// contract: a device's last Release returns even when its workers are starting, idle or compiling queued work.
// "Due to symmetry, destruction of objects will also be re-entrant." (D3D11.3 6.2).
// IUnknown::Release: "When the reference count on an object reaches zero, Release must cause the interface pointer
// to free itself." https://learn.microsoft.com/en-us/windows/win32/api/unknwn/nf-unknwn-iunknown-release
// the 30 s watchdog is a test deadline, not an API timing limit. four concurrent callers repeat each case 64 times.
// a single pipeline and a burst past 2 * hardware_concurrency (mullion_tasks.hpp's worker cap) meet active work and
// queued dependencies without a GPU loop. completed cases read every shader's output before releasing the device;
// pending cases release without a flush. the API cannot identify an internal worker's exact instruction: the
// lost-wake fault synchronizes the shutdown notification into the predicate-to-sleep window.
#include "d3d11_test.hpp"
#include <array>
#include <atomic>
#include <thread>

struct Result {
  std::atomic<const char *> at{"create device"};
  HRESULT hr = E_PENDING;
  ULONG references = ~ULONG(0);
  std::vector<uint32_t> words;
};

static HRESULT
exercise(Result &result, const std::vector<ComPtr<ID3DBlob>> &code, UINT count, bool complete, bool idle) {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
                               &device, nullptr, count ? context.GetAddressOf() : nullptr);
  if (FAILED(hr))
    return hr;
  if (idle)
    Sleep(1);
  if (count) {
    result.at = "prepare pipeline work";
    std::vector<uint32_t> initial(count, ~uint32_t(0));
    D3D11_BUFFER_DESC desc{UINT(initial.size() * sizeof(initial[0])), D3D11_USAGE_DEFAULT,
                          D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, sizeof(initial[0])};
    D3D11_SUBRESOURCE_DATA data{initial.data()};
    ComPtr<ID3D11Buffer> output;
    ComPtr<ID3D11UnorderedAccessView> view;
    if (FAILED(hr = device->CreateBuffer(&desc, &data, &output)) ||
        FAILED(hr = device->CreateUnorderedAccessView(output.Get(), nullptr, &view)))
      return hr;
    std::vector<ComPtr<ID3D11ComputeShader>> shaders(count);
    for (UINT i = 0; i < count; i++)
      if (FAILED(hr = device->CreateComputeShader(code[i]->GetBufferPointer(), code[i]->GetBufferSize(), nullptr,
                                                 &shaders[i])))
        return hr;
    auto uav = view.Get();
    context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    for (auto &shader : shaders) {
      context->CSSetShader(shader.Get(), nullptr, 0);
      context->Dispatch(1, 1, 1);
    }
    if (complete) {
      result.at = "read completed work";
      result.words = read(device.Get(), context.Get(), output.Get());
      if (result.words.size() != count)
        return E_FAIL;
    }
    result.at = "release pipeline children";
    context->ClearState();
  }
  result.at = "release immediate context";
  context.Reset();
  result.at = "last device Release";
  result.references = device.Detach()->Release();
  result.at = "returned";
  return S_OK;
}

int
main(int argc, char **argv) {
  SYSTEM_INFO system{};
  GetSystemInfo(&system);
  const UINT burst = 2 * system.dwNumberOfProcessors + 1;
  const char hlsl[] = R"(
    RWStructuredBuffer<uint> values : register(u0);
    [numthreads(1, 1, 1)]
    void cs() { values[INDEX] = INDEX + 1; }
  )";
  step("compile every shader before creating a device");
  std::vector<ComPtr<ID3DBlob>> code;
  for (UINT i = 0; i < burst; i++) {
    auto shader = compile(hlsl, "cs", "cs", {"INDEX=" + std::to_string(i)});
    if (!expect(shader != nullptr, "compute shader %u did not compile", i))
      return verdict();
    code.push_back(shader);
  }
  if (argc > 1 && !strcmp(argv[1], "shaders"))
    return verdict();

  const struct {
    const char *name;
    UINT count;
    bool complete, idle;
  } cases[] = {{"released immediately", 0, false, false},
               {"released after idle opportunity", 0, false, true},
               {"one pending pipeline", 1, false, false},
               {"burst of pending pipelines", burst, false, false},
               {"one completed pipeline", 1, true, false},
               {"burst of completed pipelines", burst, true, false}};
  const UINT repetitions = 64;
  const DWORD deadline_ms = 30000;
  std::array<HANDLE, 4> done{};
  for (auto &event : done) {
    event = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (!expect(event != nullptr, "watchdog event could not be created: %lu", GetLastError()))
      ExitProcess(verdict());
  }
  for (const auto &test : cases) {
    for (UINT iteration = 0; iteration < repetitions; iteration++) {
      step("device teardown: %s, repetition %u/%u, %zu concurrent callers, %u pipelines", test.name, iteration + 1,
           repetitions, done.size(), test.count);
      std::array<Result, std::tuple_size_v<decltype(done)>> results{};
      std::array<std::thread, std::tuple_size_v<decltype(done)>> callers;
      for (size_t i = 0; i < callers.size(); i++) {
        if (!expect(!!ResetEvent(done[i]), "watchdog event %zu could not reset: %lu", i, GetLastError()))
          ExitProcess(verdict());
        callers[i] = std::thread([&, i] {
          results[i].hr = exercise(results[i], code, test.count, test.complete, test.idle);
          SetEvent(done[i]);
        });
      }
      auto waited = WaitForMultipleObjects(done.size(), done.data(), TRUE, deadline_ms);
      if (waited != WAIT_OBJECT_0) {
        for (size_t i = 0; i < done.size(); i++)
          if (WaitForSingleObject(done[i], 0) != WAIT_OBJECT_0)
            expect(false, "device teardown watchdog: %s, caller %zu, %s did not return in %lu ms (wait %#lx)",
                   test.name, i, results[i].at.load(), deadline_ms, waited);
        expect(false, "device teardown watchdog did not complete: %#lx, error %lu", waited, GetLastError());
        ExitProcess(verdict());
      }
      for (size_t i = 0; i < callers.size(); i++) {
        callers[i].join();
        expect(results[i].hr == S_OK, "caller %zu returned %#lx during %s", i, results[i].hr, results[i].at.load());
        expect(results[i].references == 0, "caller %zu last Release returned %lu", i, results[i].references);
        if (test.complete) {
          expect(results[i].words.size() == test.count, "caller %zu read %zu words, expected %u", i,
                 results[i].words.size(), test.count);
          for (size_t word = 0; word < results[i].words.size(); word++)
            expect(results[i].words[word] == word + 1, "caller %zu word %zu is %u, expected %zu", i, word,
                   results[i].words[word], word + 1);
        }
      }
      if (trace::wrong)
        return verdict();
    }
  }
  for (auto event : done)
    CloseHandle(event);
  return verdict();
}
