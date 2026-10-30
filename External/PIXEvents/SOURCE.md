# PIX event encoding headers

Pinned source: https://github.com/microsoft/PixEvents/tree/b0caa735f8510f4ff60c29ef1c88101620defc5e

Only the five public encoding headers, LICENSE and NOTICE are vendored. No runtime DLL or library is linked. Debug GPU annotations use the pinned legacy GPU encoder directly; CPU PIX event APIs are not called. Release annotations use the basic ANSI D3D12 marker format.

These headers were added to diagnose missing DRED pass contexts. They are not a claim that the renderer hang is fixed. Preserve the Microsoft MIT license and notice when updating.

