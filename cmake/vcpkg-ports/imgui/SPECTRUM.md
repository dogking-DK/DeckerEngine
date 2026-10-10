# Spectrum overlay

The `spectrum` feature applies [spectrum.patch](spectrum.patch) to the fixed official
`v1.92.9b-docking` source archive. The upstream archive SHA512 and root builtin-baseline remain
unchanged. The local port revision is `1.92.9#1`; the added feature requires `docking-experimental`.

## Origin

- Adobe source: [3252c1c6aaa72ef9b59c6039e3127e2307171c76](https://github.com/adobe/imgui/tree/3252c1c6aaa72ef9b59c6039e3127e2307171c76).
- Spectrum delta was extracted against upstream `v1.92.8`, then merged into `v1.92.9b-docking`.
- The four changed core files are `imgui.cpp`, `imgui_internal.h`, `imgui_draw.cpp` and
  `imgui_widgets.cpp`; the patch adds `imgui_spectrum.h` and `imgui_spectrum.cpp`.
- [Adobe license](LICENSE-spectrum.txt) is installed with the upstream license by vcpkg.
- [Adobe integration notes](https://github.com/adobe/imgui/blob/3252c1c6aaa72ef9b59c6039e3127e2307171c76/docs/Spectrum.md)
  describe the intentionally incomplete Spectrum widget coverage.

## Local adaptations

- Preserve newer selectable rounding and docking tab behavior when resolving merge conflicts.
- Express chevron PathStroke using the current thickness/flags argument order.
- Apply global/disabled alpha to direct palette draws and preserve it in selectable highlight alpha.
- Calculate mixed checkbox padding from the actual inset box to keep its dash visible.
- Omit Adobe's unused IsComboPopup field and optional embedded Source Sans Pro font/LoadFont function;
  the editor retains its existing system Chinese font and DPI handling.
- Complete the newer ImGui, table and docking colors in both palettes.
- `SetLinearColorOutput(bool)` controls conversion of raw sRGB palette literals through
  `ColorToOutput()`. It preserves alpha; direct draw-list colors must additionally use GetColorU32
  exactly once. PushStyleColor values leave alpha to the standard ImGui draw helper.

StyleColorsSpectrum returns sRGB style colors. The editor saves that original style and converts
its RGB values for an sRGB swapchain, while selecting the same output mode for extra palette colors.
Swapchain recreation always starts from the original values, avoiding cumulative conversion.
The palette pointer and output mode follow Adobe's process-wide theme model; the editor owns one
ImGui context and applies all changes on its UI thread. A future independent multi-context theme
would require moving these settings into context storage.

## Updating

When changing the upstream version, rebase the patch against that exact source. Check docking
render helpers and text signatures, and retain the upstream backends. Do not import the full
master-to-docking difference as a theme patch. Keep source attribution, license and these notes
with the patch; do not edit vcpkg's temporary or installed sources.

Validation uses editor theme tests plus real background workbench/interaction tests and screenshot
inspection. See [0088](../../../spec/development/0088-editor-spectrum-theme.md).
