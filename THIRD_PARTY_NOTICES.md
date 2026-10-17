# Third-party notices

Dear ImGui 1.91.9b is vendored under `third_party/imgui`, including its GLFW/Vulkan
backends and example used as the desktop lifecycle foundation. Copyright and MIT
license terms are retained in `third_party/imgui/LICENSE.txt` and the source headers.

Selected KiCad community symbol and footprint data is included in the generated
example and test fixtures. The KiCad library license is CC-BY-SA 4.0 with its
electronic-design exception. See `docs/v2/KICAD_LIBRARY_LICENSE.md` and the example's
`libraries/NOTICE.txt`. The adapter flattens selected inherited symbols for export;
the bundled library snapshots retain their provenance and license notice.

GLFW, Vulkan and KiCad executables are external dependencies, not bundled binaries.
PyTorch is an optional external dependency used only by the classifier retraining
script. Runtime inference uses the included C implementation and weights.

These notices do not change ownership or licensing of the original project.
