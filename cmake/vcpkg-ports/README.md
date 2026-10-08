# Same-version dependency overlays

Copied from microsoft/vcpkg commit `33d78c1ed898a06938f31312167c7abefd229455`,
`ports/tracy` (Tracy 0.14.1). The upstream port and its patches are preserved.
The only port change adds `-DTRACY_ENABLE=ON`: this Tracy release defaults that
option to OFF, while the official port does not set it.

This does not pin an older dependency. When upgrading the registry, compare the
official port and remove the overlay if it enables the client itself. Keep the
root and tools/profiling manifests on the same verified baseline.
The copied port files are covered by the adjacent vcpkg LICENSE.txt.
Their patch contents retain upstream context markers. The path-specific
.gitattributes rule keeps LF and permits the required space-only context lines;
normal source/document whitespace checks are unchanged.

## mimalloc 3.5.3

The official vcpkg 3.5.3 port, source SHA512 and existing patch are preserved. Only MI_WIN_REDIRECT=OFF is added: upstream enables the redirect DLL even when the override feature is off. DeckerEngine uses explicit per-domain heaps and must not inject CRT allocation hooks or emit redirect startup diagnostics. Memory builds enable this overlay; dependency version and builtin-baseline are unchanged. Check and remove this workaround when the official port makes the same guarantee. The copied port files share the adjacent vcpkg license.

## ImGui 1.92.9 (v1.92.9b-docking)

The official fixed-baseline port, source tag and SHA512 are preserved. The only change adds
PRIVATE VK_NO_PROTOTYPES when IMGUI_BUILD_VULKAN_BINDING is enabled. DeckerEngine fills the
backend's private dispatch table through ImGui_ImplVulkan_LoadFunctions and its Device resolver.
This avoids collisions between Vulkan function symbols and Volk's same-named global variables.
No source vendoring or floating download was introduced. Compare this one-line build adjustment
when upgrading the official port. Copied port files share the adjacent vcpkg license.
