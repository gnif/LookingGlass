# Looking Glass Client

This is the Looking Glass client application that is designed to work in tandem with the Looking Glass Host application

---

## Building the Application

### Build Dependencies

* binutils-dev
* cmake
* fonts-freefont-ttf
* libsdl2-dev
* libsdl2-ttf-dev
* libspice-protocol-dev
* libfontconfig1-dev
* libx11-dev
* nettle-dev
* libxss-dev
* libxi-dev

#### Debian (and maybe Ubuntu)

    apt-get install binutils-dev cmake fonts-freefont-ttf libsdl2-dev libsdl2-ttf-dev libspice-protocol-dev libfontconfig1-dev libx11-dev nettle-dev libxss-dev libxi-dev

### Building

    mkdir build
    cd build
    cmake ../
    make

Should this all go well you should be left with the file `looking-glass-client`

### Removing Wayland or X11 support

Wayland and/or X11 support can be disabled with the compile options
`ENABLE_WAYLAND` and `ENABLE_X11`, if both are specified only `SDL2` will remain
and the client will fallback to using it.

    cmake ../ -DENABLE_WAYLAND=OFF

At this time, X11 is the perferred and best supported interface. Wayland is not
far behind, however it lacks some of the seamless interaction features that X11
has due to the lack of cursor warp (programmatic movement of the local cusror) on
Wayland.

---

## Usage Tips

### Key Bindings

By default Looking Glass uses the `Scroll Lock` key as the escape key for commands as well as the input capture mode toggle, this can be changed using the `-m` switch if you desire a different key.
Below are a list of current key bindings:

| Command | Description |
|-|-|
| <kbd>ScrLk</kbd>                   | Toggle cursor screen capture |
| <kbd>ScrLk</kbd>+<kbd>F</kbd>      | Full Screen toggle |
| <kbd>ScrLk</kbd>+<kbd>V</kbd>      | Video stream toggle |
| <kbd>ScrLk</kbd>+<kbd>I</kbd>      | Spice keyboard & mouse enable toggle |
| <kbd>ScrLk</kbd>+<kbd>N</kbd>      | Toggle night vision mode (EGL renderer only!) |
| <kbd>ScrLk</kbd>+<kbd>R</kbd>      | Rotate the output clockwise by 90 degree increments |
| <kbd>ScrLk</kbd>+<kbd>Q</kbd>      | Quit |
| <kbd>ScrLk</kbd>+<kbd>Insert</kbd> | Increase mouse sensitivity (in capture mode only) |
| <kbd>ScrLk</kbd>+<kbd>Del</kbd>    | Decrease mouse sensitivity (in capture mode only) |
| <kbd>ScrLk</kbd>+<kbd>F1</kbd>     | Send <kbd>Ctrl</kbd>+<kbd>Alt</kbd>+<kbd>F1</kbd> to the guest |
| <kbd>ScrLk</kbd>+<kbd>F2</kbd>     | Send <kbd>Ctrl</kbd>+<kbd>Alt</kbd>+<kbd>F2</kbd> to the guest |
| <kbd>ScrLk</kbd>+<kbd>F3</kbd>     | Send <kbd>Ctrl</kbd>+<kbd>Alt</kbd>+<kbd>F3</kbd> to the guest |
| <kbd>ScrLk</kbd>+<kbd>F4</kbd>     | Send <kbd>Ctrl</kbd>+<kbd>Alt</kbd>+<kbd>F4</kbd> to the guest |
| <kbd>ScrLk</kbd>+<kbd>F5</kbd>     | Send <kbd>Ctrl</kbd>+<kbd>Alt</kbd>+<kbd>F5</kbd> to the guest |
| <kbd>ScrLk</kbd>+<kbd>F6</kbd>     | Send <kbd>Ctrl</kbd>+<kbd>Alt</kbd>+<kbd>F6</kbd> to the guest |
| <kbd>ScrLk</kbd>+<kbd>F7</kbd>     | Send <kbd>Ctrl</kbd>+<kbd>Alt</kbd>+<kbd>F7</kbd> to the guest |
| <kbd>ScrLk</kbd>+<kbd>F8</kbd>     | Send <kbd>Ctrl</kbd>+<kbd>Alt</kbd>+<kbd>F8</kbd> to the guest |
| <kbd>ScrLk</kbd>+<kbd>F9</kbd>     | Send <kbd>Ctrl</kbd>+<kbd>Alt</kbd>+<kbd>F9</kbd> to the guest |
| <kbd>ScrLk</kbd>+<kbd>F10</kbd>    | Send <kbd>Ctrl</kbd>+<kbd>Alt</kbd>+<kbd>F10</kbd> to the guest |
| <kbd>ScrLk</kbd>+<kbd>F11</kbd>    | Send <kbd>Ctrl</kbd>+<kbd>Alt</kbd>+<kbd>F11</kbd> to the guest |
| <kbd>ScrLk</kbd>+<kbd>F12</kbd>    | Send <kbd>Ctrl</kbd>+<kbd>Alt</kbd>+<kbd>F12</kbd> to the guest |
| <kbd>ScrLk</kbd>+<kbd>LWin</kbd>   | Send <kbd>LWin</kbd> to the guest |
| <kbd>ScrLk</kbd>+<kbd>RWin</kbd>   | Send <kbd>RWin</kbd> to the guest |



### Setting options via command line arguments

The syntax is simple: `module:name=value`, for example:

    ./looking-glass-client win:fullScreen=yes egl:nvGain=1

### Setting options via configuration files

By default the application will look for and load the config files in the following locations

  * /etc/looking-glass-client.ini
  * ~/.looking-glass-client.ini

The format of this file is the commonly known INI format, for example:

    [win]
    fullScreen=yes

    [egl]
    nvGain=1

Command line arguments will override any options loaded from the config files.

### Supported options

```
|--------------------------------------------------------------------------------------------------------------------------------------------------|
| Long                   | Short | Value                  | Description                                                                            |
|--------------------------------------------------------------------------------------------------------------------------------------------------|
| app:configFile         | -C    | NULL                   | A file to read additional configuration from                                           |
| app:renderer           | -g    | auto                   | Specify the renderer to use                                                            |
| app:license            | -l    | no                     | Show the license for this application and then terminate                               |
| app:cursorPollInterval |       | 1000                   | How often to check for a cursor update in microseconds                                 |
| app:framePollInterval  |       | 1000                   | How often to check for a frame update in microseconds                                  |
| app:allowDMA           |       | yes                    | Allow direct DMA transfers if possible (VM-VM only for now)                            |
| app:shmFile            | -f    | /dev/shm/looking-glass | The path to the shared memory file, or the name of the kvmfr device to use, ie: kvmfr0 |
|--------------------------------------------------------------------------------------------------------------------------------------------------|

|---------------------------------------------------------------------------------------------------------------------------------|
| Long                    | Short | Value                  | Description                                                          |
|---------------------------------------------------------------------------------------------------------------------------------|
| win:title               |       | Looking Glass (client) | The window title                                                     |
| win:position            |       | center                 | Initial window position at startup                                   |
| win:size                |       | 1024x768               | Initial window size at startup                                       |
| win:autoResize          | -a    | no                     | Auto resize the window to the guest                                  |
| win:allowResize         | -n    | yes                    | Allow the window to be manually resized                              |
| win:keepAspect          | -r    | yes                    | Maintain the correct aspect ratio                                    |
| win:forceAspect         |       | yes                    | Force the window to maintain the aspect ratio                        |
| win:dontUpscale         |       | no                     | Never try to upscale the window                                      |
| win:borderless          | -d    | no                     | Borderless mode                                                      |
| win:fullScreen          | -F    | no                     | Launch in fullscreen borderless mode                                 |
| win:maximize            | -T    | no                     | Launch window maximized                                              |
| win:minimizeOnFocusLoss |       | yes                    | Minimize window on focus loss                                        |
| win:fpsMin              | -K    | -1                     | Frame rate minimum (0 = disable - not recommended, -1 = auto detect) |
| win:showFPS             | -k    | no                     | Enable the FPS & UPS display                                         |
| win:ignoreQuit          | -Q    | no                     | Ignore requests to quit (ie: Alt+F4)                                 |
| win:noScreensaver       | -S    | no                     | Prevent the screensaver from starting                                |
| win:alerts              | -q    | yes                    | Show on screen alert messages                                        |
| win:quickSplash         |       | no                     | Skip fading out the splash screen when a connection is established   |
| win:rotate              |       | 0                      | Rotate the displayed image (0, 90, 180, 270)                         |
|---------------------------------------------------------------------------------------------------------------------------------|

|----------------------------------------------------------------------------------------------------------------------------------------------|
| Long                      | Short | Value           | Description                                                                            |
|----------------------------------------------------------------------------------------------------------------------------------------------|
| input:grabKeyboard        | -G    | yes             | Grab the keyboard in capture mode                                                      |
| input:grabKeyboardOnFocus |       | yes             | Grab the keyboard when focused                                                         |
| input:escapeKey           | -m    | 71 = ScrollLock | Specify the escape key, see https://wiki.libsdl.org/SDLScancodeLookup for valid values |
| input:ignoreWindowsKeys   |       | no              | Do not pass events for the windows keys to the guest                                   |
| input:hideCursor          | -M    | yes             | Hide the local mouse cursor                                                            |
| input:mouseSens           |       | 0               | Initial mouse sensitivity when in capture mode (-9 to 9)                               |
| input:mouseSmoothing      |       | yes             | Apply simple mouse smoothing when rawMouse is not in use (helps reduce aliasing)       |
| input:rawMouse            |       | no              | Use RAW mouse input when in capture mode (good for gaming)                             |
| input:mouseRedraw         |       | yes             | Mouse movements trigger redraws (ignores FPS minimum)                                  |
| input:autoCapture         |       | no              | Try to keep the mouse captured when needed                                             |
| input:captureOnly         |       | no              | Only enable input via SPICE if in capture mode                                         |
|----------------------------------------------------------------------------------------------------------------------------------------------|

|------------------------------------------------------------------------------------------------------------------|
| Long                   | Short | Value     | Description                                                         |
|------------------------------------------------------------------------------------------------------------------|
| spice:enable           | -s    | yes       | Enable the built in SPICE client for input and/or clipboard support |
| spice:host             | -c    | 127.0.0.1 | The SPICE server host or UNIX socket                                |
| spice:port             | -p    | 5900      | The SPICE server port (0 = unix socket)                             |
| spice:input            |       | yes       | Use SPICE to send keyboard and mouse input events to the guest      |
| spice:clipboard        |       | yes       | Use SPICE to syncronize the clipboard contents with the guest       |
| spice:clipboardToVM    |       | yes       | Allow the clipboard to be syncronized TO the VM                     |
| spice:clipboardToLocal |       | yes       | Allow the clipboard to be syncronized FROM the VM                   |
| spice:scaleCursor      | -j    | yes       | Scale cursor input position to screen size when up/down scaled      |
| spice:captureOnStart   |       | no        | Capture mouse and keyboard on start                                 |
| spice:alwaysShowCursor |       | no        | Always show host cursor                                             |
|------------------------------------------------------------------------------------------------------------------|

|--------------------------------------------------------------------------------------------------------------|
| Long             | Short | Value | Description                                                               |
|--------------------------------------------------------------------------------------------------------------|
| egl:vsync        |       | no    | Enable vsync                                                              |
| egl:doubleBuffer |       | no    | Enable double buffering                                                   |
| egl:multisample  |       | yes   | Enable Multisampling                                                      |
| egl:nvGainMax    |       | 1     | The maximum night vision gain                                             |
| egl:nvGain       |       | 0     | The initial night vision gain at startup                                  |
| egl:cbMode       |       | 0     | Color Blind Mode (0 = Off, 1 = Protanope, 2 = Deuteranope, 3 = Tritanope) |
|--------------------------------------------------------------------------------------------------------------|

|------------------------------------------------------------------------------------|
| Long                 | Short | Value | Description                                 |
|------------------------------------------------------------------------------------|
| opengl:mipmap        |       | yes   | Enable mipmapping                           |
| opengl:vsync         |       | no    | Enable vsync                                |
| opengl:preventBuffer |       | yes   | Prevent the driver from buffering frames    |
| opengl:amdPinnedMem  |       | yes   | Use GL_AMD_pinned_memory if it is available |
|------------------------------------------------------------------------------------|
```
