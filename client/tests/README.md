# Client rendering tests

These gtests run the production client with the synthetic `test` transport,
capture the fully composed framebuffer immediately before presentation, and
compare every pixel with an independent reference implementation.

The default CTest entry starts an isolated headless Weston compositor using
Mesa software rendering. It covers:

- BGRA, RGBA, packed BGR32, RGB24, PQ RGBA10, and scRGB RGBA16F;
- odd widths and 24-bit row pitches;
- the Cartesian matrix of every format with full-frame, moving old/new-box,
  overlapping, maximum-count, invalid, null, and zero-area damage;
- HDR-to-SDR transfer, gamut conversion, and tone mapping.

When run on a color-managed Wayland compositor with a compatible EGL surface,
the same tests validate native RGB10A2 or FP16 output. Native PQ tests also
check the requested BT.2020 image description, reference white, mastering
luminance, MaxCLL, and MaxFALL in the client log.

## Building and running

```sh
cmake -S client -B client/build \
  -DENABLE_TEST_TRANSPORT=ON \
  -DENABLE_RENDER_TESTS=ON
cmake --build client/build
ctest --test-dir client/build --output-on-failure \
  -R render-tests
```

GoogleTest and Weston are required when `ENABLE_RENDER_TESTS` is enabled.
Failed cases retain their capture and client log under `/tmp` and print the
artifact paths.

To exercise native HDR on the current Wayland session instead of the headless
SDR compositor:

```sh
client/build/tests/render-tests \
  --gtest_filter='*rgba10*:*rgba16f*'
```

Framebuffer readback verifies the signal produced by Looking Glass. Verifying
a compositor's scanout and a physical panel's luminance remains a separate
hardware test.
