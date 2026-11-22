# Kinect phone modules of the official 1.0.2 package (test fixtures)

`pose.mjs` and `demo_body.mjs` are copies of `assets/kinect/pose.mjs` and
`assets/kinect/demo_body.mjs` from the official PS5X360 1.0.2 package (`PPSA50011.zip`, the one
`assemble.py` pins), with LF line ends. They are MIT licensed:
Copyright (c) 2026 Xbox360PS5 contributors.

`tools/check-kinect-tracker.mjs` (fix 106) imports them next to the title's tracker
(`port/phone_camera_tracker.hpp`): the tracker imports `./pose.mjs`, and `demo_body.mjs` makes the
synthetic bodies the check feeds it. The console still serves both files from the package.
