# test_vk HELLO WORLD Vulkan Layer

Minimal explicit Vulkan layer that, when `test_vk=1` is set, overlays the phrase **HELLO WORLD** using a tiny built‑in bitmap font scaled up for visibility (drawn as per‑pixel copies) each frame.

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

## Run / Test

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
- Single copy of tightly-packed image instead of many 1x1 copies.

Use only for experimentation / learning.
