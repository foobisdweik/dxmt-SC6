#include "airconv_context.hpp"
#include "airconv_public.h"
#include "airconv_switches.hpp"
#include "metallib_writer.hpp"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/DiagnosticPrinter.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/Passes/OptimizationLevel.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/WithColor.h"
#include <cstdio>
#include <system_error>
#include <vector>

#ifdef __WIN32
#include "d3dcompiler.h"
#endif
#include "dxbc_converter.hpp"

using namespace llvm;

static cl::opt<std::string>
  InputFilename(cl::Positional, cl::desc("<input dxbc>"), cl::init("-"));

static cl::opt<std::string> OutputFilename(
  "o", cl::desc("Override output filename"), cl::value_desc("filename")
);

static cl::opt<std::string>
  HullBeforeDomain("hull-before-domain", cl::desc("Compile domain shader with supplied hull shader"));

static cl::opt<std::string>
  VertexBeforeHull("vertex-before-hull", cl::desc("Compile hull shader with supplied vertex shader"));

static cl::opt<std::string>
  HullAfterVertex("hull-after-vertex", cl::desc("Compile vertex shader with supplied hull shader"));

static cl::opt<std::string>
  VertexBeforeGeometry("vertex-before-geometry", cl::desc("Compile geometry shader with supplied vertex shader"));

static cl::opt<std::string>
  GeometryAfterVertex("geometry-after-vertex", cl::desc("Compile vertex shader with supplied geometry shader"));

// SC6 fork: the runtime passes these to geometry pipelines (d3d11_shader.cpp); without them the vertex half has no
// vertex input description.
static cl::opt<std::string> IALayout(
  "ia-layout",
  cl::desc("Input layout for vertex pulling. One element per line: reg slot offset mtl_attribute_format step_function step_rate")
);

static cl::opt<unsigned> IndexFormat(
  "index-format", cl::init(0), cl::desc("Index buffer format for vertex pulling: 0 none, 1 uint16, 2 uint32")
);

static cl::opt<bool>
  StripTopology("strip-topology", cl::init(false), cl::desc("The geometry pipeline draws a strip topology"));

// SC6 fork: SM50_SHADER_FLAG bits added to the compilation's flags, for example 0x10000 for
// SM50_SHADER_FLAG_SKIP_CBUFFER_BOUNDS, to see what a path switch of d3d11.dll changes (airconv_switches.hpp)
static cl::opt<unsigned> ExtraShaderFlags("shader-flags", cl::init(0), cl::desc("SM50_SHADER_FLAG bits to add"));

// SC6 fork
static cl::opt<bool> GSLayered(
  "gs-layered", cl::init(false),
  cl::desc("With --geometry-after-vertex: build the ordinary vertex function that runs a layered geometry shader too")
);

static cl::opt<std::string> RootSignature(
  "root-signature", cl::desc("Bind resources as a serialized D3D12 root signature does (D3D12SerializeRootSignature)")
);

static cl::opt<bool> NoCustomBorder(
  "no-custom-border", cl::init(false),
  cl::desc("Leave out custom border colors, as a pipeline made before any sampler has one does")
);

static cl::opt<bool>
  EmitLLVM("S", cl::init(false), cl::desc("Write output as LLVM assembly"));

static cl::opt<bool>
  EmitMetallib("A", cl::init(false), cl::desc("Write output as .metallib"));

static cl::opt<bool> DisassembleDXBC(
  "disas-dxbc", cl::init(false), cl::desc("Disassemble dxbc shader")
);

static cl::opt<bool>
  OptLevelO0("O0", cl::desc("Optimization level 0. Similar to clang -O0. "));

static cl::opt<bool>
  OptLevelO1("O1", cl::desc("Optimization level 1. Similar to clang -O1. "));

static cl::opt<bool>
  OptLevelO2("O2", cl::desc("Optimization level 2. Similar to clang -O2. "));

static cl::opt<bool> PreserveBitcodeUseListOrder(
  "preserve-bc-uselistorder",
  cl::desc("Preserve use-list order when writing LLVM bitcode."),
  cl::init(false), cl::Hidden
);

static cl::opt<bool> PreserveAssemblyUseListOrder(
  "preserve-ll-uselistorder",
  cl::desc("Preserve use-list order when writing LLVM assembly."),
  cl::init(false), cl::Hidden
);

cl::list<std::string> f("f", cl::Prefix, cl::Hidden);

namespace {

struct LLVMDisDiagnosticHandler : public DiagnosticHandler {
  char *Prefix;
  LLVMDisDiagnosticHandler(char *PrefixPtr) : Prefix(PrefixPtr) {}
  bool handleDiagnostics(const DiagnosticInfo &DI) override {
    raw_ostream &OS = errs();
    OS << Prefix << ": ";
    switch (DI.getSeverity()) {
    case DS_Error:
      WithColor::error(OS);
      break;
    case DS_Warning:
      WithColor::warning(OS);
      break;
    case DS_Remark:
      OS << "remark: ";
      break;
    case DS_Note:
      WithColor::note(OS);
      break;
    }

    DiagnosticPrinterRawOStream DP(OS);
    DI.print(DP);
    OS << '\n';

    if (DI.getSeverity() == DS_Error)
      exit(1);
    return true;
  }
};
} // namespace

namespace mullion::dxbc {
llvm::Error convertDXBC(
  sm50_shader_t pShader, const char *name, llvm::LLVMContext &context,
  llvm::Module &module, SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs
);
}

static ExitOnError ExitOnErr;

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);

  ExitOnErr.setBanner(std::string(argv[0]) + ": error: ");

  LLVMContext Context;
  Context.setDiagnosticHandler(
    std::make_unique<LLVMDisDiagnosticHandler>(argv[0])
  );
  Context.setOpaquePointers(false);
  cl::ParseCommandLineOptions(argc, argv, "DXBC to Metal AIR transpiler\n");

  // bool FastMath = true;
  // for (StringRef Flag : f) {
  //   if (Flag == "no-fast-math") {
  //     FastMath = false;
  //   }
  // }

  if (OutputFilename.empty()) { // Unspecified output, infer it.
    if (InputFilename == "-") {
      OutputFilename = "-";
    } else {
      StringRef IFN = InputFilename;
      OutputFilename = (IFN.endswith(".cso")    ? IFN.drop_back(4)
                        : IFN.endswith(".fxc")  ? IFN.drop_back(4)
                        : IFN.endswith(".obj")  ? IFN.drop_back(4)
                        : IFN.endswith(".o")    ? IFN.drop_back(2)
                        : IFN.endswith(".dxbc") ? IFN.drop_back(5)
                                                : IFN)
                         .str();
      OutputFilename += DisassembleDXBC ? ".txt"
                        : EmitMetallib  ? ".metallib"
                        : EmitLLVM      ? ".ll"
                                        : ".air";
    }
  }

  ErrorOr<std::unique_ptr<MemoryBuffer>> FileOrErr =
    MemoryBuffer::getFileOrSTDIN(InputFilename, /*IsText=*/false);
  if (std::error_code EC = FileOrErr.getError()) {
    SMDiagnostic(
      InputFilename, SourceMgr::DK_Error,
      "Could not open input file: " + EC.message()
    )
      .print(argv[0], errs());
    return 1;
  }
  auto MemRef = FileOrErr->get()->getMemBufferRef();

  if (EmitLLVM && !MemRef.getBuffer().startswith("DXBC")) {
    std::error_code EC;
    ToolOutputFile Out(OutputFilename, EC, sys::fs::OF_TextWithCRLF);
    if (EC) {
      errs() << EC.message() << '\n';
      return 1;
    }
    auto bytes = MemRef.getBuffer();
    unsigned libraries = 0;
    for (size_t at = bytes.find("MTLB"); at != StringRef::npos; at = bytes.find("MTLB", at)) {
      mullion::metallib::MTLBHeader header;
      if (bytes.size() - at < sizeof(header)) {
        errs() << "Truncated Metal library header at " << at << '\n';
        return 1;
      }
      memcpy(&header, bytes.data() + at, sizeof(header));
      if (header.FileSize > bytes.size() - at || header.FileSize < sizeof(header) ||
          header.BitcodeOffset > header.FileSize || header.BitcodeSize > header.FileSize - header.BitcodeOffset) {
        errs() << "Invalid Metal library bounds at " << at << '\n';
        return 1;
      }
      auto bitcode = bytes.substr(at + header.BitcodeOffset, header.BitcodeSize);
      auto module = ExitOnErr(parseBitcodeFile(MemoryBufferRef(bitcode, InputFilename), Context));
      module->print(Out.os(), nullptr);
      libraries++;
      at += header.FileSize;
    }
    if (!libraries) {
      errs() << "No Metal libraries in input\n";
      return 1;
    }
    Out.keep();
    return 0;
  }

  if (DisassembleDXBC) {
#ifdef __WIN32
    std::error_code EC;
    std::unique_ptr<ToolOutputFile> Out(
      new ToolOutputFile(OutputFilename, EC, sys::fs::OF_TextWithCRLF)
    );
    if (EC) {
      errs() << EC.message() << '\n';
      return 1;
    }

    ID3DBlob *blob;
    D3DDisassemble(
      MemRef.getBufferStart(), MemRef.getBufferSize(), 0, nullptr, &blob
    );

    Out->os().write(
      (const char *)blob->GetBufferPointer(), blob->GetBufferSize() - 1
    );
    // Declare success.
    Out->keep();

    blob->Release();
    return 0;
#else
    errs() << "Disassemble only supported on Windows" << '\n';
    return 1;
#endif
  }

  Module M("default", Context);
  mullion::initializeModule(M);

  sm50_shader_t sm50;
  sm50_error_t err;
  if (SM50Initialize(
        MemRef.getBufferStart(), MemRef.getBufferSize(), &sm50, nullptr, &err
      )) {
    errs() << SM50GetErrorMessageString(err) << '\n';
    SM50FreeError(err);
    return 1;
  }

  SM50_SHADER_COMMON_DATA data;
  data.metal_version = SM50_SHADER_METAL_320;
  data.flags = NoCustomBorder ? SM50_SHADER_FLAG_NO_CUSTOM_BORDER : SM50_SHADER_FLAG{};
  data.simd_width = 0; // no device to ask; a vertex shader's wave has no lane count here
  data.next = 0;
  data.type = SM50_SHADER_COMMON;
  data.flags = SM50_SHADER_FLAG(data.flags | ExtraShaderFlags);
  // the CLI converts without SM50Compile*, so it opens the flags' scope itself
  mullion::AirconvSkipScope skip_scope((SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&data);

  std::unique_ptr<MemoryBuffer> RootSignatureBuffer;
  SM50_SHADER_ROOT_SIGNATURE_DATA rootsig{};
  if (!RootSignature.getValue().empty()) {
    auto FileOrErr = MemoryBuffer::getFile(RootSignature, /*IsText=*/false);
    if (std::error_code EC = FileOrErr.getError()) {
      SMDiagnostic(RootSignature, SourceMgr::DK_Error, "Could not open root signature: " + EC.message())
        .print(argv[0], errs());
      return 1;
    }
    RootSignatureBuffer = std::move(*FileOrErr);
    rootsig.type = SM50_SHADER_ROOT_SIGNATURE;
    rootsig.bytecode = RootSignatureBuffer->getBufferStart();
    rootsig.bytecode_length = RootSignatureBuffer->getBufferSize();
    data.next = &rootsig;
  }

  std::vector<SM50_IA_INPUT_ELEMENT> ia_elements;
  SM50_SHADER_IA_INPUT_LAYOUT_DATA ia_layout = {};
  ia_layout.type = SM50_SHADER_IA_INPUT_LAYOUT;
  ia_layout.next = &data;
  ia_layout.index_buffer_format = (SM50_INDEX_BUFFER_FORMAT)IndexFormat.getValue();
  if (!IALayout.getValue().empty()) {
    auto LayoutOrErr = MemoryBuffer::getFile(IALayout, /*IsText=*/true);
    if (std::error_code EC = LayoutOrErr.getError()) {
      SMDiagnostic(IALayout, SourceMgr::DK_Error, "Could not open input layout: " + EC.message())
        .print(argv[0], errs());
      return 1;
    }
    SmallVector<StringRef, 32> lines;
    LayoutOrErr->get()->getBuffer().split(lines, '\n', -1, false);
    for (StringRef line : lines) {
      unsigned reg, slot, offset, format, step_function, step_rate;
      if (sscanf(line.str().c_str(), "%u %u %u %u %u %u", &reg, &slot, &offset, &format, &step_function,
                 &step_rate) != 6)
        continue;
      SM50_IA_INPUT_ELEMENT e = {};
      e.reg = reg;
      e.slot = slot;
      e.aligned_byte_offset = offset;
      e.format = format;
      e.step_function = step_function;
      e.step_rate = step_rate;
      ia_elements.push_back(e);
      ia_layout.slot_mask |= 1u << slot;
    }
  }
  ia_layout.num_elements = ia_elements.size();
  ia_layout.elements = ia_elements.data();

  SM50_SHADER_PSO_GEOMETRY_SHADER_DATA geometry = {};
  geometry.type = SM50_SHADER_PSO_GEOMETRY_SHADER;
  geometry.strip_topology = StripTopology;

  if (!HullBeforeDomain.getValue().empty()) {
    ErrorOr<std::unique_ptr<MemoryBuffer>> FileOrErr =
      MemoryBuffer::getFile(HullBeforeDomain, /*IsText=*/false);
    if (std::error_code EC = FileOrErr.getError()) {
      SMDiagnostic(
        HullBeforeDomain, SourceMgr::DK_Error,
        "Could not open input file: " + EC.message()
      )
        .print(argv[0], errs());
      return 1;
    }
    auto MemRef = FileOrErr->get()->getMemBufferRef();
    sm50_shader_t sm50_hull;
    if (SM50Initialize(
          MemRef.getBufferStart(), MemRef.getBufferSize(), &sm50_hull, nullptr,
          &err
        )) {
      errs() << SM50GetErrorMessageString(err) << '\n';
      SM50FreeError(err);
      return 1;
    }
    if (auto err = mullion::dxbc::convert_dxbc_tesselator_domain_shader(
          (mullion::dxbc::SM50ShaderInternal *)sm50, "shader_main",
          (mullion::dxbc::SM50ShaderInternal *)sm50_hull, Context, M, (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&data
        )) {
      errs() << toString(std::move(err)) << '\n';
      return 1;
    }
  } else if (!VertexBeforeHull.getValue().empty()) {
    ErrorOr<std::unique_ptr<MemoryBuffer>> FileOrErr =
      MemoryBuffer::getFile(VertexBeforeHull, /*IsText=*/false);
    if (std::error_code EC = FileOrErr.getError()) {
      SMDiagnostic(
        VertexBeforeHull, SourceMgr::DK_Error,
        "Could not open input file: " + EC.message()
      )
        .print(argv[0], errs());
      return 1;
    }
    auto MemRef = FileOrErr->get()->getMemBufferRef();
    sm50_shader_t sm50_vertex;
    if (SM50Initialize(
          MemRef.getBufferStart(), MemRef.getBufferSize(), &sm50_vertex, nullptr,
          &err
        )) {
      errs() << SM50GetErrorMessageString(err) << '\n';
      SM50FreeError(err);
      return 1;
    }
    if (auto err = mullion::dxbc::convert_dxbc_vertex_hull_shader(
          (mullion::dxbc::SM50ShaderInternal *)sm50_vertex, (mullion::dxbc::SM50ShaderInternal *)sm50,
          "shader_main", Context, M, (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&data
        )) {
      errs() << toString(std::move(err)) << '\n';
      return 1;
    }
  } else if (!HullAfterVertex.getValue().empty()) {
    ErrorOr<std::unique_ptr<MemoryBuffer>> FileOrErr =
      MemoryBuffer::getFile(HullAfterVertex, /*IsText=*/false);
    if (std::error_code EC = FileOrErr.getError()) {
      SMDiagnostic(
        HullAfterVertex, SourceMgr::DK_Error,
        "Could not open input file: " + EC.message()
      )
        .print(argv[0], errs());
      return 1;
    }
    auto MemRef = FileOrErr->get()->getMemBufferRef();
    sm50_shader_t sm50_hull;
    if (SM50Initialize(
          MemRef.getBufferStart(), MemRef.getBufferSize(), &sm50_hull, nullptr,
          &err
        )) {
      errs() << SM50GetErrorMessageString(err) << '\n';
      SM50FreeError(err);
      return 1;
    }
    if (auto err = mullion::dxbc::convert_dxbc_vertex_hull_shader(
          (mullion::dxbc::SM50ShaderInternal *)sm50, (mullion::dxbc::SM50ShaderInternal *)sm50_hull,
          "shader_main", Context, M, (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&data
        )) {
      errs() << toString(std::move(err)) << '\n';
      return 1;
    }
  } else if (!VertexBeforeGeometry.getValue().empty()) {
    ErrorOr<std::unique_ptr<MemoryBuffer>> FileOrErr =
      MemoryBuffer::getFile(VertexBeforeGeometry, /*IsText=*/false);
    if (std::error_code EC = FileOrErr.getError()) {
      SMDiagnostic(
        VertexBeforeGeometry, SourceMgr::DK_Error,
        "Could not open input file: " + EC.message()
      )
        .print(argv[0], errs());
      return 1;
    }
    auto MemRef = FileOrErr->get()->getMemBufferRef();
    sm50_shader_t sm50_vertex;
    if (SM50Initialize(
          MemRef.getBufferStart(), MemRef.getBufferSize(), &sm50_vertex, nullptr,
          &err
        )) {
      errs() << SM50GetErrorMessageString(err) << '\n';
      SM50FreeError(err);
      return 1;
    }
    geometry.next = &data;
    if (auto err = mullion::dxbc::convert_dxbc_geometry_shader(
          (mullion::dxbc::SM50ShaderInternal *)sm50, "shader_main",
          (mullion::dxbc::SM50ShaderInternal *)sm50_vertex, Context, M, (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&geometry
        )) {
      errs() << toString(std::move(err)) << '\n';
      return 1;
    }
  } else if (!GeometryAfterVertex.getValue().empty()) {
    ErrorOr<std::unique_ptr<MemoryBuffer>> FileOrErr =
      MemoryBuffer::getFile(GeometryAfterVertex, /*IsText=*/false);
    if (std::error_code EC = FileOrErr.getError()) {
      SMDiagnostic(
        GeometryAfterVertex, SourceMgr::DK_Error,
        "Could not open input file: " + EC.message()
      )
        .print(argv[0], errs());
      return 1;
    }
    auto MemRef = FileOrErr->get()->getMemBufferRef();
    sm50_shader_t sm50_geometry;
    if (SM50Initialize(
          MemRef.getBufferStart(), MemRef.getBufferSize(), &sm50_geometry, nullptr,
          &err
        )) {
      errs() << SM50GetErrorMessageString(err) << '\n';
      SM50FreeError(err);
      return 1;
    }
    geometry.next = &ia_layout;
    if (GSLayered) {
      auto faces = ((mullion::dxbc::SM50ShaderInternal *)sm50_geometry)->gs_layered_faces;
      errs() << "layered faces: " << faces << '\n';
      if (!faces)
        return 1;
    }
    if (auto err = GSLayered ? mullion::dxbc::convert_dxbc_vertex_for_layered_geometry_shader(
                                   (mullion::dxbc::SM50ShaderInternal *)sm50, "shader_main",
                                   (mullion::dxbc::SM50ShaderInternal *)sm50_geometry, Context, M,
                                   (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&ia_layout
                               )
                             : mullion::dxbc::convert_dxbc_vertex_for_geometry_shader(
          (mullion::dxbc::SM50ShaderInternal *)sm50, "shader_main",
          (mullion::dxbc::SM50ShaderInternal *)sm50_geometry, Context, M, (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&geometry
        )) {
      errs() << toString(std::move(err)) << '\n';
      return 1;
    }
  } else {
    if (auto err =
          mullion::dxbc::convertDXBC(sm50, "shader_main", Context, M, (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&data)) {
      errs() << toString(std::move(err)) << '\n';
      return 1;
    }
  }

  SM50Destroy(sm50);

  if (OptLevelO0) {
    // do nothing
  } else {
    if (!mullion::runOptimizationPasses(M, errs()))
      return 1;
  }

  mullion::linkMSAD(M);
  mullion::linkSamplePos(M);
  mullion::linkTessellation(M);

  std::error_code EC;
  std::unique_ptr<ToolOutputFile> Out(new ToolOutputFile(
    OutputFilename, EC,
    (EmitLLVM || EmitMetallib) ? sys::fs::OF_TextWithCRLF : sys::fs::OF_None
  ));
  if (EC) {
    errs() << EC.message() << '\n';
    return 1;
  }

  if (EmitLLVM) {
    M.print(Out->os(), nullptr, PreserveAssemblyUseListOrder);
  } else if (EmitMetallib) {
    mullion::metallib::MetallibWriter writer;
    writer.Write(M, Out->os());
  } else {
    WriteBitcodeToFile(
      M, Out->os(), PreserveBitcodeUseListOrder, nullptr, true
    );
  }

  // Declare success.
  Out->keep();

  return 0;
}