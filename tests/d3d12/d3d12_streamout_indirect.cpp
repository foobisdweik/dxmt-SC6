// contract: indirect DRAW has the same stream-output bytes, counts and pixels as the corresponding direct call.
// ExecuteIndirect: "The semantics of this API are defined with the following pseudo-code"; its DRAW signature
// interprets D3D12_DRAW_ARGUMENTS once per command. reuse the direct test's independent CPU expectations.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-executeindirect
#define MULLION_TEST_INDIRECT_STREAM_OUTPUT
#include "d3d12_streamout.cpp"
