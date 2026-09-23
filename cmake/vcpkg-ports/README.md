# Tracy overlay

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
