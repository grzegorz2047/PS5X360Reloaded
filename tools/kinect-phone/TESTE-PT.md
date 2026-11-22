> Translated from the original author's Portuguese text.

# First motion test

This prototype tests the camera and body tracking. It can now send joints to the PS5 research receiver. It does not run Kinect games yet.

## On the PC

1. Open a terminal in the project folder.
2. Run `python -m http.server 8780 --bind 127.0.0.1 --directory tools/kinect-phone`.
3. Open http://localhost:8780 in the browser.
4. Click **Start camera** and allow camera access. The first use downloads the model and may take a while.
5. Stand in a well-lit environment, with your whole body visible. Step back far enough for your feet and hands to appear.
6. Raise each arm, bend your knees and take steps to the sides. Check that the points follow your body and that the hands do not swap sides.
7. Step out of the frame: a message asking for a visible body should appear. Come back and check that tracking resumes.
8. Click **Export pose** to save a sample. **Stop** shuts down the camera; switching to another tab also shuts it down.

Record the device/browser, perceived response time, tracking loss, approximate distance and lighting. Share only the exported JSON if you want the mapping analyzed; the prototype does not send images or movements automatically.

## On the phone

The same page must be served over **HTTPS** with a certificate the device accepts. The PC's or the PS5's local HTTP address does not unlock the phone's camera. Do not disable the browser's security to work around this.

The app uses only the RGB camera. Depth and distance are estimated, not measured by a Kinect sensor. The prototype exports 20 approximate joints; head, hands and spine need calibration and validation.

## Next steps

1. Validate tracking on the chosen device and calibrate position/orientation.
2. Implement an authenticated connection on the local network, with handling of disconnection and expired data.
3. Implement the NUI services that games query in the emulator core.
4. Validate one Kinect game at a time in a separate build. Only then announce the sensor as available to the game.

None of these functions is enabled in the current emulator. Games that do not use Kinect keep the existing input path.

## Test with the PS5 (build .19)

1. Turn on the settings page under System and Camera motion (research) under Controls.
2. Stop the old static server and run `python tools/kinect-phone/server.py --console 192.168.0.19`.
3. On the PC, open http://localhost:8780, type the key shown on the PS5 and click Connect PS5. Then click Start camera.
4. Open Dragon Ball Z for Kinect. The research panel should draw the joints over the game.
5. Leave the camera view or click Stop: the drawing should stop tracking within 500 ms.
6. The game may still warn that there is no Kinect. The Motion research logs show which services it requests; emulating those services will be the next step.

The key changes when the emulator restarts. It is not the AutoLog URL. The connection is local and does not send video. The receiver is off by default.
