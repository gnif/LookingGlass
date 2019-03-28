# LookingGlass
An extremely low latency KVMFR (KVM FrameRelay) implementation for guests with VGA PCI Passthrough.

* Project Website: https://looking-glass.hostfission.com
* Support Forum: https://forum.level1techs.com/t/looking-glass-triage/130952

## Obtaining and using Looking Glass

Please see https://looking-glass.hostfission.com/quickstart

## Latest Version

If you would like to use the latest bleeding edge version of Looking Glass please be aware there will be no support at this time.
Latest bleeding edge builds of the Windows host application can be obtained from: https://ci.appveyor.com/project/gnif/lookingglass/build/artifacts

## Key Bindings

By default Looking Glass uses the `Scroll Lock` key as the escape key for commands as well as the input capture mode toggle, this can be changed using the `-m` switch if you desire a different key.
Below are a list of current key bindings:

| Command | Description |
|-|-|
| <kbd>ScrLk</kbd>   | Toggle cursor screen capture |
| <kbd>ScrLk</kbd>+<kbd>F</kbd> | Full Screen toggle |
| <kbd>ScrLk</kbd>+<kbd>I</kbd> | Spice keyboard & mouse enable toggle |
| <kbd>ScrLk</kbd>+<kbd>N</kbd> | Cycle through 4 brigtness levels (EGL renderer only!) |

# Help and support

## Web
https://forum.level1techs.com/t/looking-glass-triage/130952

## IRC
Join us in the #LookingGlass channel on the FreeNode network

# Trello

* https://trello.com/b/tI1Xbwsg/looking-glass
