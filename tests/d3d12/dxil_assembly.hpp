// DXC's assembler accepts textual LLVM IR but does not validate it. keep compilation metadata and validate
// explicit opcode witnesses before signing them (DXC dxcassembler.cpp, IDxcAssembler and IDxcValidator).
#pragma once
#include "d3d12_test.hpp"

inline std::string
dxil_disassemble(const Compiler &compiler, const std::string &code) {
  DxcBuffer source{code.data(), code.size(), 0};
  ComPtr<IDxcResult> result;
  ComPtr<IDxcBlobUtf8> text;
  if (!expect(
          SUCCEEDED(compiler.dxc->Disassemble(&source, IID_PPV_ARGS(&result))) &&
              SUCCEEDED(result->GetOutput(DXC_OUT_DISASSEMBLY, IID_PPV_ARGS(&text), nullptr)) && text,
          "DXIL disassembly"
      ))
    return {};
  return {text->GetStringPointer(), text->GetStringLength()};
}

inline std::string
dxil_assemble(const std::string &ir, const char *name) {
  auto create = (DxcCreateInstanceProc)GetProcAddress(GetModuleHandleA("dxcompiler.dll"), "DxcCreateInstance");
  ComPtr<IDxcUtils> utils;
  ComPtr<IDxcAssembler> assembler;
  ComPtr<IDxcValidator> validator;
  if (!expect(
          create && SUCCEEDED(create(CLSID_DxcUtils, IID_PPV_ARGS(&utils))) &&
              SUCCEEDED(create(CLSID_DxcAssembler, IID_PPV_ARGS(&assembler))) &&
              SUCCEEDED(create(CLSID_DxcValidator, IID_PPV_ARGS(&validator))),
          "DXIL assembler and structural validator"
      ))
    return {};
  ComPtr<IDxcBlobEncoding> source;
  ComPtr<IDxcOperationResult> assembled, validated;
  if (!expect(
          SUCCEEDED(utils->CreateBlob(ir.data(), ir.size(), DXC_CP_UTF8, &source)) &&
              SUCCEEDED(assembler->AssembleToContainer(source.Get(), &assembled)) && assembled,
          "%s assembly started", name
      ))
    return {};
  auto successful = [&](IDxcOperationResult *result, const char *operation) {
    HRESULT status = E_FAIL;
    const HRESULT obtained = result->GetStatus(&status);
    if (FAILED(obtained) || FAILED(status)) {
      ComPtr<IDxcBlobEncoding> errors;
      if (SUCCEEDED(result->GetErrorBuffer(&errors)) && errors)
        printf(
            "%s %s: %.*s\n", name, operation, int(errors->GetBufferSize()), (const char *)errors->GetBufferPointer()
        );
    }
    return expect(SUCCEEDED(obtained) && SUCCEEDED(status), "%s %s HRESULT %#lx", name, operation, status);
  };
  if (!successful(assembled.Get(), "assembly"))
    return {};
  ComPtr<IDxcBlob> object;
  if (!expect(SUCCEEDED(assembled->GetResult(&object)) && object, "%s container", name) ||
      !expect(
          SUCCEEDED(validator->Validate(object.Get(), DxcValidatorFlags_Default, &validated)) && validated,
          "%s validation started", name
      ) ||
      !successful(validated.Get(), "structural validation"))
    return {};
  std::string code((const char *)object->GetBufferPointer(), object->GetBufferSize());
  dxbc::sign(code);
  return kept(code, name);
}
