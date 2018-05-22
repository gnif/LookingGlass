# Debugging the Looking Glass Client

If you are asked to provide debugging information to resolve an issue please
follow the following procedure.

## If you're experiencing a crash:

Run the program under the `gdb` debugger (you may need to install gdb), for
example:

    gdb ./looking-glass-client

If you need to set any arguments, do so now by running `set args ARGS`, for
example:

    set args -F -k

Now start the program by typing `r`. When the application crashes you will be
dumped back into the debugger, the application may appear to be frozen. Run
the following command:

    thread apply all bt

Once you have this information please pastebin the log from looking-glass as
well as the information resulting from this command.

## If you're experencing high CPU load and/or poor performance.

The steps here are identical to the above, except instead of waiting for the
program to crash, in the debugger press `CTRL+C` while the program is
exhibiting the problem, then run `thread apply all bt` and pastebin your log
and the results of the command.
