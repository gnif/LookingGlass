---
name: Bug report
about: Report a reproducible Looking Glass fault
---

## Before reporting

Use the forum or Discord for setup help:

- https://forum.level1techs.com/c/software/lookingGlass/142
- https://discord.gg/52SMupxkvt

Use GitHub issues for reproducible bugs and feature requests. Check the current
documentation and existing issues first. Do not report a protocol mismatch
until the client, IDD and OBS plugin have been updated to the same release.

## Describe the fault

What happened, what did you expect, and what exact steps reproduce it?

```
DESCRIPTION AND STEPS
```

Does it reproduce every time? If not, how often?

```
REPRODUCTION RATE
```

## Versions and system

- Looking Glass Client version:
- Looking Glass IDD or legacy Host version:
- OBS plugin version, if applicable:
- Linux distribution and kernel:
- X11 or Wayland, and compositor:
- Host GPU and driver:
- Guest GPU and driver, or IDD software mode:
- Guest resolution and refresh rate:
- Primary transport (`lgmp` or `spice`):
- DMA enabled and successfully imported:

## Client output

Run `looking-glass-client` from a terminal, reproduce the fault and include its
complete output from startup. Do not include only the final error.

```
PASTE COMPLETE CLIENT OUTPUT
```

## Windows logs

For the IDD, use its notification-area helper to open
`C:\ProgramData\Looking Glass (IDD)` and attach the current files plus rotated
files from the affected run:

- `looking-glass-idd.txt`
- `looking-glass-input.txt`
- `looking-glass-idd-service.txt`
- `looking-glass-idd-helper.txt`

For the legacy Host Application, attach:

- `C:\ProgramData\Looking Glass (host)\looking-glass-host.txt`
- `C:\ProgramData\Looking Glass (host)\looking-glass-host-service.txt`

## Crash backtrace

For a client crash without a complete built-in trace, run the client under
`gdb`, reproduce it, then run:

```
thread apply all bt full
```

Attach that output and keep the matching `looking-glass-client.debug` file
available.

## Additional evidence

Attach screenshots, timing graphs, OBS logs or short recordings when they make
the fault clearer. State which settings differ from their defaults.
