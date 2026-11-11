# OpenVDB CPU/NanoVDB overlay

Based on official vcpkg `f38855d514df2b21c2e911a6c6865703abb8da15`, OpenVDB `12.0.1#0`.
The upstream archive URL/version and SHA512, dependency list, patches and license are retained.

The `nanovdb` feature removes the CUDA dependency and sets `NANOVDB_USE_CUDA=OFF`,
`NANOVDB_USE_INTRINSICS=OFF`, and disables examples/unit tests. It builds CPU conversion
and installs the same-version NanoVDB/PNanoVDB headers for Vulkan/Slang consumers.
No CUDA compiler, CUDA toolkit or GPU runtime is required. The engine does not enable
AX, Python, GUI tools or NanoVDB command-line tools.

Remove this overlay when an official port supports this CPU/header configuration;
revalidate the converter/accessor/layout together before changing versions.
Implementation and verification: [M23.2 record](../../../spec/development/0160-voxel-cpu-sparse.md).
