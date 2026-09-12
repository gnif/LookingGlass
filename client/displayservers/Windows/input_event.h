/**
 * Looking Glass
 * Copyright © 2017-2026 The Looking Glass Authors
 * https://looking-glass.io
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 59
 * Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#ifndef _H_LG_WINDOWS_INPUT_EVENT_
#define _H_LG_WINDOWS_INPUT_EVENT_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "input/keycodes.h"

#include "../input.h"

typedef struct WindowsInput
{
  const LG_DSInputSink * sink;
  void                 * opaque;
  uint32_t               buttons;
  double                 wheel;
  uint8_t                ctrlKeys;
  uint8_t                shiftKeys;
  uint8_t                altKeys;
  uint8_t                superKeys;
  int                    printScreenKey;
  bool                   entered;
  bool                   leavePending;
  bool                   focused;
  bool                   pointerGrabbed;
  bool                   keyboardGrabbed;
  bool                   numLock;
  bool                   capsLock;
  bool                   scrollLock;
  bool                   numLockPressed;
  bool                   capsLockPressed;
  bool                   scrollLockPressed;
}
WindowsInput;

void windowsInputInit(WindowsInput * input, const LG_DSInputSink * sink,
    void * opaque);

void windowsInputSetPointerGrabbed(WindowsInput * input, bool grabbed);
bool windowsInputIsPointerGrabbed(const WindowsInput * input);
void windowsInputSetKeyboardGrabbed(WindowsInput * input, bool grabbed);
bool windowsInputIsKeyboardGrabbed(const WindowsInput * input);

bool windowsInputFocus(WindowsInput * input, bool focused);
bool windowsInputPointerEnter(WindowsInput * input, double x, double y);
bool windowsInputPointerLeave(WindowsInput * input);
void windowsInputPointerMotion(WindowsInput * input, double x, double y);
void windowsInputRelativeMotion(WindowsInput * input, double x, double y);
bool windowsInputPointerButton(WindowsInput * input, unsigned int button,
    bool pressed);
void windowsInputReleaseButtons(WindowsInput * input);
void windowsInputPointerWheel(WindowsInput * input, double motion);

int  windowsInputMapScanCode(uint8_t scanCode, bool extended);
int  windowsInputMapPrintScreen(bool altDown);
int  windowsInputMapPrintScreenEvent(WindowsInput * input,
    bool altDown, bool pressed);
bool windowsInputKeyCode(WindowsInput * input, int key, bool pressed,
    bool grabbedEvent);
bool windowsInputKey(WindowsInput * input, uint8_t scanCode, bool extended,
    bool pressed, bool grabbedEvent);
size_t windowsInputSyncHeldKeys(WindowsInput * input,
    const int * keys, size_t count, bool grabbedEvent);
void windowsInputText(WindowsInput * input, const char * text);
void windowsInputState(WindowsInput * input,
    bool ctrl, bool shift, bool alt, bool super,
    bool numLock, bool capsLock, bool scrollLock);
void windowsInputTrackedState(WindowsInput * input);

#endif
