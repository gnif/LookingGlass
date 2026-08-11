# Debugging the Looking Glass Client

Start by running `looking-glass-client` in a terminal and retain its complete
output from startup through the fault. It records the selected transport,
renderer, display server, import method and audio backend.

## Crashes

Run the matching binary under `gdb`:

```text
gdb ./looking-glass-client
```

Set arguments when needed:

```text
set args -F -k
```

Start with `run`. After the crash, collect every thread and local variable:

```text
thread apply all bt full
```

Provide that output together with the complete client log. Keep
`looking-glass-client.debug` from the same build; installed builds place it in
the standard `bin/.debug` location.

## Hangs or high CPU use

Run under `gdb` as above. While the fault is visible, press `Ctrl+C` in the
debugger and collect:

```text
thread apply all bt full
```

For an intermittent stall, collect several samples rather than one. Include
the frame-timing graph when it helps identify the affected stage.

## IDD faults

Use the Windows IDD helper to open
`C:\ProgramData\Looking Glass (IDD)`. Collect the IDD, input, service and helper
logs, including rotated `.1` through `.4` files when the fault happened before
the latest restart. See the end-user IDD diagnostics page for the exact file
names.
