# Reconstructed headers (not the author's code)

PS5X360 1.0.0/1.0.2 sources include these headers, but no published source drop contains
them (they lived in the author's unpublished `build/` folders: `ui-kinect-20261020`,
`disc-20261015`, `kinect-relay-20261015`, `dual-renderer`, ...). Each file here declares only
what the published sources use, written from those uses, and the behaviour is the smallest
safe one. Features behind them do not work like the official 1.0.2 (see docs/SOURCE-1.0.2.md).

Exception: `xbox360ps5/usb_kinect.hpp` (fix 44, `kinect-relay-20261015`) is written from the official
executable's own code (the USB Kinect and relay paths, va 0x746510-0x7481e4) and the package's
`native/kinect/kinect_tracker.c`, with the official log lines, so it does what the official one does;
`tools/check-kinect-usb.cpp` drives it with a fake relay and a simulated camera.
