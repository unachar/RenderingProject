# Dual quaternion skinning

Apply normalized, hemisphere-corrected dual quaternion blending to ordinary animated vertices in the shared GPU skinning compute shader. This improves twist volume retention for PMX, FBX and VRM without changing import formats, bone indices or GPU buffer layouts. Preserve PMX SDEF. If any contributing matrix includes scale, shear, reflection, non-affine or non-finite data, retain existing linear blend skinning for that entire vertex. Zero/invalid influences retain the existing bind vertex behavior.

Use final skin matrices (including inverse bind), row-vector rotation and translation from the logical HLSL matrix row 3. Normalize both quaternion halves and remove their non-orthogonal component. Transform normals by the blended rotation only. All passes consuming the computed vertex buffer, including outlines, shadows, velocity history and DXR, keep that common output.

Alternatives: replacing SDEF would discard authored PMX semantics; blindly converting scaled matrices would discard their scale; adding a second bone palette introduces layout and upload work unnecessarily. Select shader-local conversion with rigid-matrix validation; its cost is conversion per weighted vertex, to profile later on Windows.

Validation: compile the exact shared HLSL math as C++ using a test-only vector shim; exercise identity, translation, all matrix-to-quaternion branches, rotation plus translation, weighted opposing twists, antipodal signs, non-unit weight sums, and unsupported matrices. Add a Windows GPU test for the actual compute entry point if the fixture supports it. Linux cannot validate D3D12 rendering or Windows builds.
