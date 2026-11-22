> Translated from the original author's Portuguese text.

# First Xbox360PS5 test (M1)

This application tests, on the PS5, the Xenia instruction decoder, VideoOut
presentation and the DualSense controller. It does not load games yet and does not
initialize the JIT, Vulkan or the Xbox 360 guest kernel.

Extract `PPSA50008` to `/data/homebrew/PPSA50008` and register the folder with
ShadowMountPlus. Open **Xbox360PS5 Platform Test** from the PS5 menu.

Check:

1. The `XBOX360PS5 M1` screen appears.
2. All nine cases show `PASS`; `FAILURES` is zero.
3. The status shows `DUALSENSE READY`.
4. Pressing X increases `CROSS PRESSES` and `RUNS`.
5. Moving the left analog stick changes both values under `LEFT STICK`.
6. Close the application from the PS5 menu.

The diagnostics are written to `/download0/xbox360ps5-m1.log`, inside the title's
sandbox. This makes it possible to recover the results even if presentation fails.
The test uses its own title ID and does not replace Castation.

Passing this test proves only the components above. Emulation of the
processor, the Xenos GPU, XEX loading and running games remain
pending.

## Initial deployment

On 2026-10-01, the test was sent to `192.168.0.19` and registered as
`source_type=folder`, `installed=true`, `source_available=true`, with no
registration error. The user confirmed that it opens, shows an image and responds to the controller. The title uses
`/data/homebrew/PPSA50008`.

The console's FTP exposes the SELF as an ELF and zeroes some unmapped metadata.
The verification compares the header, the segment table and all
mapped/dynamic segments; it tolerates only version notes outside the memory
segments that the FTP has zeroed entirely. Modified code or a mismatching
header is rejected. The tests for this verification are in
`tools/test_deploy_verify.py`. The icon and param.json are checked byte for byte.

The log was read over FTP with the application open: 53 runs of the nine cases,
always with zero failures. The pad handle was opened successfully. The user
confirmed that the screen, the X counter and the analog stick values respond.
This result validates milestone M1, without attesting to game execution, the JIT or Vulkan.
The record tied to the executable's hash is in `docs/evidence/M1_HARDWARE.json`.
