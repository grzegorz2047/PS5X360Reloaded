> Translated from the original author's Portuguese text.

# Reference driver: Mihawk PS5_Vulkan

Source consulted on 2026-10-01:
https://github.com/mihawk-99/PS5_Vulkan

The project contains two different routes: RADV/Mesa as the main driver and
ps5vk as an earlier implementation. The documentation advertises Vulkan 1.4 for
RADV, but keeps `conformanceVersion` at zero until full qualification
is complete. Do not treat the version number as proof that
all the features Xenia needs are available.

## Points relevant to Xbox360PS5

- RADV is built from PS5_Mesa with a PS5 winsys. Linking a desktop Vulkan
  library is not enough: allocation, submission and presentation are native.
- Presentation through `VK_KHR_display` and VideoOut. Xenia at this revision has
  desktop/Android surfaces in `ui/vulkan/vulkan_instance.cc`; it will need
  a PS5-specific instance, surface and presenter path.
- `tools/radv-link.sh` combines the driver, the C++ runtime, unwind, emulated TLS and
  the SDK platform layer. The recipe also handles the heap and threads. The SDK
  currently used by our probe has not been checked against this recipe.
- The whole-archive linking of RADV preserves entries referenced weakly
  by the dispatch tables. Do not reproduce this indiscriminately for
  all libraries.
- The CTS tests and the gap records help choose regression cases.
  Results reported by the author are evidence about the reference driver,
  not tests run by this project on the user's console.

## Verification required for Xenos

In Xenia pinned at `95a5c3ee`, `VulkanDevice::CreateIfSupported` requires
`independentBlend` for GPU emulation. This is only an initial check;
the command processor and the texture cache also query features, limits and
formats. Query the effective capabilities on the console before starting Xenos.

Integration sequence: pin compatible driver/Mesa/SDK revisions,
compile and link a minimal Vulkan application, enumerate features and formats,
validate clear/readback and presentation, then integrate the Xenia provider and renderer.
The PowerPC CPU, guest kernel and game loader remain separate
work. No driver was compiled, installed or enabled in this reference
revision.

## Sources

- https://github.com/mihawk-99/PS5_Vulkan
- https://github.com/mihawk-99/PS5_Vulkan/blob/main/tools/radv-link.sh
- https://github.com/mihawk-99/PS5_Vulkan/blob/main/docs/CTS_GAPS.md
- Local Xenia: `src/xenia/ui/vulkan/vulkan_device.cc` and `vulkan_instance.cc`.
