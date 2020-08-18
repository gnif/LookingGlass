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

#### Debian (and maybe Ubuntu)

    apt-get install binutils-dev cmake fonts-freefont-ttf libsdl2-dev libsdl2-ttf-dev libspice-protocol-dev libfontconfig1-dev libx11-dev nettle-dev

### Building

    mkdir build
    cd build
    cmake ../
    make

Should this all go well you should be left with the file `looking-glass-client`

---

## Usage Tips

### Key Bindings

By default Looking Glass uses the `Scroll Lock` key as the escape key for commands as well as the input capture mode toggle, this can be changed using the `-m` switch if you desire a different key.
Below are a list of current key bindings:

| Command | Description |
|-|-|
| <kbd>ScrLk</kbd>                   | Toggle cursor screen capture |
| <kbd>ScrLk</kbd>+<kbd>F</kbd>      | Full Screen toggle |
| <kbd>ScrLk</kbd>+<kbd>I</kbd>      | Spice keyboard & mouse enable toggle |
| <kbd>ScrLk</kbd>+<kbd>N</kbd>      | Toggle night vision mode (EGL renderer only!) |
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
|-------------------------------------------------------------------------------------------------------------------------|
| Long                   | Short | Value                  | Description                                                   |
|-------------------------------------------------------------------------------------------------------------------------|
| app:configFile         | -C    | NULL                   | A file to read additional configuration from                  |
| app:shmFile            | -f    | /dev/shm/looking-glass | The path to the shared memory file                            |
| app:shmSize            | -L    | 0                      | Specify the size in MB of the shared memory file (0 = detect) |
| app:renderer           | -g    | auto                   | Specify the renderer to use                                   |
| app:license            | -l    | no                     | Show the license for this application and then terminate      |
| app:cursorPollInterval |       | 1000                   | How often to check for a cursor update in microseconds        |
| app:framePollInterval  |       | 1000                   | How often to check for a frame update in microseconds         |
|-------------------------------------------------------------------------------------------------------------------------|

|-------------------------------------------------------------------------------------------------------------|
| Long                    | Short | Value                  | Description                                      |
|-------------------------------------------------------------------------------------------------------------|
| win:title               |       | Looking Glass (client) | The window title                                 |
| win:position            |       | center                 | Initial window position at startup               |
| win:size                |       | 1024x768               | Initial window size at startup                   |
| win:autoResize          | -a    | no                     | Auto resize the window to the guest              |
| win:allowResize         | -n    | yes                    | Aallow the window to be manually resized         |
| win:keepAspect          | -r    | yes                    | Maintain the correct aspect ratio                |
| win:borderless          | -d    | no                     | Borderless mode                                  |
| win:fullScreen          | -F    | no                     | Launch in fullscreen borderless mode             |
| win:maximize            | -T    | no                     | Launch window maximized                          |
| win:minimizeOnFocusLoss |       | yes                    | Minimize window on focus loss                    |
| win:fpsLimit            | -K    | 200                    | Frame rate limit (0 = disable - not recommended) |
| win:showFPS             | -k    | no                     | Enable the FPS & UPS display                     |
| win:ignoreQuit          | -Q    | no                     | Ignore requests to quit (ie: Alt+F4)             |
| win:noScreensaver       | -S    | no                     | Prevent the screensaver from starting            |
| win:alerts              | -q    | yes                    | Show on screen alert messages                    |
|-------------------------------------------------------------------------------------------------------------|

|---------------------------------------------------------------------------------------------------------------------------------------|
| Long               | Short | Value           | Description                                                                            |
|---------------------------------------------------------------------------------------------------------------------------------------|
| input:grabKeyboard | -G    | yes             | Grab the keyboard in capture mode                                                      |
| input:escapeKey    | -m    | 71 = ScrollLock | Specify the escape key, see https://wiki.libsdl.org/SDLScancodeLookup for valid values |
| input:hideCursor   | -M    | yes             | Hide the local mouse cursor                                                            |
| input:mouseSens    |       | 0               | Initial mouse sensitivity when in capture mode (-9 to 9)                               |
|---------------------------------------------------------------------------------------------------------------------------------------|

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
|------------------------------------------------------------------------------------------------------------------|

|--------------------------------------------------------------------------|
| Long          | Short | Value | Description                              |
|--------------------------------------------------------------------------|
| egl:vsync     |       | no    | Enable vsync                             |
| egl:nvGainMax |       | 1     | The maximum night vision gain            |
| egl:nvGain    |       | 0     | The initial night vision gain at startup |
|--------------------------------------------------------------------------|

|------------------------------------------------------------------------------------|
| Long                 | Short | Value | Description                                 |
|------------------------------------------------------------------------------------|
| opengl:mipmap        |       | yes   | Enable mipmapping                           |
| opengl:vsync         |       | yes   | Enable vsync                                |
| opengl:preventBuffer |       | yes   | Prevent the driver from buffering frames    |
| opengl:amdPinnedMem  |       | yes   | Use GL_AMD_pinned_memory if it is available |
|------------------------------------------------------------------------------------|
```
