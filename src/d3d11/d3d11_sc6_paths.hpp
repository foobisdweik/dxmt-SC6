#pragma once
#include "airconv_public.h"

namespace mullion {

/* SC6 fork: Mullion's paths that a run can turn off, to compare the GPU cost and the output with and without each.
 * Every path is on by default. A path is off when any of these says so:
 *   - its own config option, for example `mullion.cbufferBounds = False` (Sc6PathInfo's option),
 *   - the config option `mullion.disable`, a comma-separated list of path names, or `all`,
 *   - the environment variable MULLION_SC6_DISABLE (or DXMT_SC6_DISABLE), the same kind of list.
 * The converter paths reach winemetal.so as SM50_SHADER_FLAG_SKIP_* bits, which are in the shader cache's key, so a
 * shader converted with a path off is never mistaken for one converted with it on. Both are read once per process. */
enum class Sc6Path : unsigned {
  /* rows past a constant buffer's size read zeros (D3D11.3 7.5) */
  CbufferBounds,
  /* frc as a subtraction of floor in place of Metal's fract with a select */
  FracFloor,
  /* one sample for the branches of a sampling branch, after conversion */
  SampleBranchMerge,
  /* the same, for minimum and maximum filters */
  MinMaxBranchMerge,
  /* a device-scope fence at each group sync of shaders whose groups work together, on GPUs before Apple9 */
  GroupSyncFence,
  /* a layered geometry shader runs inside the vertex function (src/airconv/dxbc_converter_gs_layered.cpp) */
  GSLayered,
  /* the Metal pipeline archive and its warm-up of recorded pipelines (d3d11.pipelineArchive must be on too) */
  PipelineArchive,
  /* an anisotropic sampler with a LOD bias samples with explicit gradients, so that Metal keeps the bias; every 2D
     sample of a pixel shader carries a branch for it */
  AnisoGradients,
  /* shaders leave out the minimum and maximum filter emulation until a sampler needs it (SamplerEmulationsLeftOut) */
  LazySamplerEmulation,
  Count,
};

struct Sc6PathInfo {
  /* the name in the lists, which matches MULLION_AIRCONV_SKIP's for the converter paths */
  const char *name;
  /* the config option that turns this path off with False */
  const char *option;
  /* the shader flag that leaves a converter path out, or 0 */
  SM50_SHADER_FLAG skip_flag;
};

const Sc6PathInfo &Sc6PathDescription(Sc6Path path);

/* whether the path is on for this process */
bool Sc6PathEnabled(Sc6Path path);

/* the SM50_SHADER_FLAG_SKIP_* bits of the converter paths that are off */
SM50_SHADER_FLAG Sc6SkipShaderFlags();

/* SC6 fork: Mullion's converter adds two emulations to every texture sample: a border color Metal lacks, and a minimum
 * or maximum filter where Metal's sampler cannot reduce. D3D12 leaves each out of its shaders until a sampler needs it
 * (MTLD3D12Device::sampler_emulations). D3D11 passed neither flag, so every sample of every shader carried both, and
 * SC6's vertex shaders grew from 36252 to 67365 instructions (92 shaders, before Metal's compiler).
 * D3D11 never makes a sampler with a custom border (CreateSamplerState passes none), so that emulation is always left
 * out. The filter emulation is left out until CreateSamplerState makes a sampler whose metadata asks the shader to
 * reduce. A shader compiled before that sampler keeps Metal's average for it; the flags are in the shader cache's key,
 * so shaders made after it are converted again with the emulation. */
void NoteSamplerNeedsMinMaxEmulation();
/* SM50_SHADER_FLAG_NO_* bits of the sampler emulations that no D3D11 sampler has needed so far */
SM50_SHADER_FLAG SamplerEmulationsLeftOut();

} // namespace mullion
