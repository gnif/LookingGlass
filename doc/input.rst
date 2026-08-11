.. _client_input:

Keyboard and mouse input
########################

The current IDD provides keyboard and mouse input directly over LGMP. It uses
absolute positioning for normal desktop use and relative movement in capture
mode. SPICE remains available as a fallback when direct input is unavailable.

The client changes input provider automatically. If the displayed video has
fallen back to SPICE, the client also uses SPICE input so that the picture and
pointer always refer to the same guest.

Only one LGMP client controls direct input at a time. Starting another client
does not give both clients simultaneous control. Disconnecting or restarting
the IDD releases held keys and buttons before ownership changes.

Normal input
------------

Move the pointer into the guest view to use absolute desktop input. This keeps
the guest pointer aligned without requiring capture.

Direct IDD input supports the keyboard, media keys and up to 32 mouse buttons.
SPICE fallback uses relative mouse input and supports fewer extra buttons.

Capture mode
------------

Press the escape key, :kbd:`ScrLk` by default, to capture the mouse. Capture
mode confines the pointer and sends relative motion, which is appropriate for
games and applications that lock the cursor. Press the escape key again to
leave capture mode.

Common capture options are:

``input:captureOnFocus``
   Enter capture mode whenever the client receives focus.

``input:captureOnStart``
   Start captured.

``input:captureOnly``
   Disable guest input outside capture mode.

``input:rawMouse``
   Use raw relative mouse input in capture mode. This is usually best for
   games.

``input:mouseSens``
   Adjust relative sensitivity from -9 to 9. The default is 0.

``input:hideCursor``
   Hide the host cursor while Looking Glass renders the guest cursor.

Automatic keyboard capture
--------------------------

Set ``input:autoCapture=yes`` to grab the keyboard when the pointer enters the
guest view. Looking Glass predicts when the next mouse movement would leave
the guest area and releases the keyboard before that movement. This makes it
possible to move naturally between Looking Glass and the Linux desktop without
using the escape key for the keyboard.

Automatic keyboard capture is disabled by ``input:captureOnly=yes``. It also
keeps the keyboard grabbed while a mouse button is held so that dragging does
not unexpectedly leave the guest.

This is separate from full mouse capture: the pointer remains in normal
absolute mode. Use regular capture mode when an application requires relative
mouse input.

``input:grabKeyboardOnFocus`` instead keeps the keyboard grabbed while the
guest view is active. ``input:grabKeyboard`` controls whether full capture
mode grabs the keyboard.

SPICE fallback
--------------

Keep ``spice:input=yes`` when SPICE input fallback is wanted. It uses the VM's
default PS/2 keyboard and mouse, so no additional virtio input devices or
tablet are required.

To disable all SPICE input while keeping SPICE audio or clipboard services:

.. code-block:: ini

   [spice]
   input=no

If direct input is also unavailable, the client will display video without
being able to control the guest.

Evdev capture
-------------

Advanced users may list Linux evdev devices in ``input:evdev``. They become
active whenever Looking Glass grabs the keyboard, including capture mode and
automatic keyboard capture. The client user must be allowed to read those
devices. Keep ``input:evdevExclusive=yes`` unless duplicate input from the
window system is specifically required.
