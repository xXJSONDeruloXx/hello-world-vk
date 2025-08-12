# test_vk Vulkan Layer (HELLO WORLD + FidelityFX Optical Flow bootstrap)

Minimal explicit Vulkan layer. Two optional features controlled by environment variables:

1. `test_vk=1` overlays the phrase **HELLO WORLD** using a tiny built‑in bitmap font (implemented as lots of 1x1 buffer-to-image copies each frame — intentionally naive for educational clarity).
2. `TEST_VK_OF=1` initializes an AMD FidelityFX Optical Flow context (via the bundled SDK) for the current swapchain resolution. At present this code only creates/destroys the Optical Flow context; it does NOT yet dispatch the Optical Flow passes nor visualize motion vectors. (Planned: replace the text overlay with summarized motion magnitude / vector field.)

If both are set, you will see the existing HELLO WORLD overlay plus log messages indicating Optical Flow context creation.

## Build (Makefile fallback)

`cmake` isn't required; a simple Makefile is provided:

```bash
git clone <repo-or-path>
cd test-vk
make            # builds libVK_LAYER_LUNARG_test_vk.so
make install    # installs .so and manifest to ~/.local
```

Artifacts after `make install`:
- Library: `~/.local/lib/libVK_LAYER_LUNARG_test_vk.so`
- Manifest (explicit layer): `~/.local/share/vulkan/explicit_layer.d/VK_LAYER_LUNARG_test_vk.json`

## Run / Test (HELLO overlay)

Enable the explicit layer via `VK_INSTANCE_LAYERS` and toggle overlay with `test_vk`:

```bash
VK_INSTANCE_LAYERS=VK_LAYER_LUNARG_test_vk test_vk=1 vkcube
```

No overlay (pass-through):

```bash
vkcube
```

Diagnostic (see layer chaining & presents):

```bash
VK_INSTANCE_LAYERS=VK_LAYER_LUNARG_test_vk test_vk=1 vkcube 2>&1 | grep -E '\[test_vk\]' | head
```

You should see lines like:
```
[test_vk] vkCreateInstance chain ok
[test_vk] vkCreateDevice intercepted
[test_vk] vkQueuePresentKHR overlay path
```

## Optical Flow (early integration status)

Environment toggle: set `TEST_VK_OF=1` alongside enabling the layer, e.g.:

```bash
VK_INSTANCE_LAYERS=VK_LAYER_LUNARG_test_vk TEST_VK_OF=1 test_vk=1 vkcube
```

What happens today:
* Allocates backend scratch memory sized by `ffxGetScratchMemorySizeVK`.
* Acquires a backend interface (`ffxGetInterfaceVK`).
* Creates an `FfxOpticalflowContext` with swapchain dimensions (or 1024x1024 fallback until first swapchain is known).
* Destroys the context on device destruction.

What is NOT yet implemented (roadmap):
* Wrapping swapchain images as `FfxResource` inputs and creating required output resources.
* Calling `ffxOpticalflowContextDispatch` each frame.
* Reading back / visualizing motion vectors (planned overlay: per‑pixel hue for direction + intensity or textual min/avg/max magnitude numbers in place of HELLO WORLD).

Planned quick next step: introduce a simplified text renderer (instead of fixed HELLO WORLD bitmap) to print per‑frame Optical Flow stats once dispatch & readback are wired.

If you need full Optical Flow visualization immediately, see Potential Next Improvements below and consider contributing a patch.

## Notes

## How it works (quick sketch)
1. Explicit layer manifest enables you to opt-in with `VK_INSTANCE_LAYERS`.
2. Layer negotiation + link info capture next `vkGet*ProcAddr` pointers.
3. On each `vkQueuePresentKHR`, if `test_vk` is set, a staging buffer of white pixels is built representing only the glyph pixels (scaled 4x).
4. Each pixel becomes a `VkBufferImageCopy` region copied into the swapchain image at top-left. (Still using an unsafe assumed layout.)

## Limitations / TODO
- No proper image layout transitions (copies use `VK_IMAGE_LAYOUT_PRESENT_SRC_KHR`, undefined behavior but works on many drivers).
- Recreates staging resources every frame (inefficient).
- Single-instance / single-device assumptions; minimal dispatch logic.
- Per-pixel copies are numerous (inefficient); could batch into an alpha-blended quad via a small pipeline instead.
- No synchronization correctness beyond queue order & fence wait.
- Hard-coded position, scale, color.

## Possible Next Improvements
- Add layout transitions and restore original layout.
- Cache/reuse buffer & command pool.
- Environment variables for position (e.g. TEST_VK_POS=top-right), scale, color.
- Proper font rendering (stb_truetype) and UTF-8 text config.
- Optical Flow dispatch & motion field overlay (replace or augment HELLO WORLD).
- GPU-side vector-to-color compute shader instead of CPU pixel stamping.
- Single copy of tightly-packed image instead of many 1x1 copies.

Use only for experimentation / learning.
