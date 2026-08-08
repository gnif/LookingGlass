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

#include "HIDReports.h"

static const uint8_t REPORT_DESCRIPTOR[] =
{
  // Absolute tablet, report ID 1.
  0x05, 0x01,             // Usage Page (Generic Desktop)
  0x09, 0x02,             // Usage (Mouse)
  0xA1, 0x01,             // Collection (Application)
  0x85, HID_REPORT_ID_TABLET,
  0x09, 0x01,             // Usage (Pointer)
  0xA1, 0x00,             // Collection (Physical)
  0x05, 0x09,             // Usage Page (Button)
  0x19, 0x01,             // Usage Minimum (Button 1)
  0x29, 0x05,             // Usage Maximum (Button 5)
  0x15, 0x00,             // Logical Minimum (0)
  0x25, 0x01,             // Logical Maximum (1)
  0x75, 0x01,             // Report Size (1)
  0x95, 0x05,             // Report Count (5)
  0x81, 0x02,             // Input (Data, Variable, Absolute)
  0x75, 0x03,             // Report Size (3)
  0x95, 0x01,             // Report Count (1)
  0x81, 0x03,             // Input (Constant)
  0x05, 0x01,             // Usage Page (Generic Desktop)
  0x09, 0x30,             // Usage (X)
  0x09, 0x31,             // Usage (Y)
  0x15, 0x00,             // Logical Minimum (0)
  0x26, 0xFF, 0x7F,       // Logical Maximum (32767)
  0x75, 0x10,             // Report Size (16)
  0x95, 0x02,             // Report Count (2)
  0x81, 0x02,             // Input (Data, Variable, Absolute)
  0xC0,                   // End Collection
  0xC0,                   // End Collection

  // Relative mouse, report ID 2.
  0x05, 0x01,             // Usage Page (Generic Desktop)
  0x09, 0x02,             // Usage (Mouse)
  0xA1, 0x01,             // Collection (Application)
  0x85, HID_REPORT_ID_MOUSE,
  0x09, 0x01,             // Usage (Pointer)
  0xA1, 0x00,             // Collection (Physical)
  0x05, 0x09,             // Usage Page (Button)
  0x19, 0x01,             // Usage Minimum (Button 1)
  0x29, 0x05,             // Usage Maximum (Button 5)
  0x15, 0x00,             // Logical Minimum (0)
  0x25, 0x01,             // Logical Maximum (1)
  0x75, 0x01,             // Report Size (1)
  0x95, 0x05,             // Report Count (5)
  0x81, 0x02,             // Input (Data, Variable, Absolute)
  0x75, 0x03,             // Report Size (3)
  0x95, 0x01,             // Report Count (1)
  0x81, 0x03,             // Input (Constant)
  0x05, 0x01,             // Usage Page (Generic Desktop)
  0x09, 0x30,             // Usage (X)
  0x09, 0x31,             // Usage (Y)
  0x16, 0x00, 0x80,       // Logical Minimum (-32768)
  0x26, 0xFF, 0x7F,       // Logical Maximum (32767)
  0x75, 0x10,             // Report Size (16)
  0x95, 0x02,             // Report Count (2)
  0x81, 0x06,             // Input (Data, Variable, Relative)
  0x09, 0x38,             // Usage (Wheel)
  0x15, 0x81,             // Logical Minimum (-127)
  0x25, 0x7F,             // Logical Maximum (127)
  0x75, 0x08,             // Report Size (8)
  0x95, 0x01,             // Report Count (1)
  0x81, 0x06,             // Input (Data, Variable, Relative)
  0xC0,                   // End Collection
  0xC0,                   // End Collection

  // Keyboard, report ID 3.
  0x05, 0x01,             // Usage Page (Generic Desktop)
  0x09, 0x06,             // Usage (Keyboard)
  0xA1, 0x01,             // Collection (Application)
  0x85, HID_REPORT_ID_KEYBOARD,
  0x05, 0x07,             // Usage Page (Keyboard)
  0x19, 0xE0,             // Usage Minimum (Left Control)
  0x29, 0xE7,             // Usage Maximum (Right GUI)
  0x15, 0x00,             // Logical Minimum (0)
  0x25, 0x01,             // Logical Maximum (1)
  0x75, 0x01,             // Report Size (1)
  0x95, 0x08,             // Report Count (8)
  0x81, 0x02,             // Input (Data, Variable, Absolute)
  0x95, 0x01,             // Report Count (1)
  0x75, 0x08,             // Report Size (8)
  0x81, 0x03,             // Input (Constant)
  0x05, 0x08,             // Usage Page (LEDs)
  0x19, 0x01,             // Usage Minimum (Num Lock)
  0x29, 0x05,             // Usage Maximum (Kana)
  0x95, 0x05,             // Report Count (5)
  0x75, 0x01,             // Report Size (1)
  0x91, 0x02,             // Output (Data, Variable, Absolute)
  0x95, 0x01,             // Report Count (1)
  0x75, 0x03,             // Report Size (3)
  0x91, 0x03,             // Output (Constant)
  0x05, 0x07,             // Usage Page (Keyboard)
  0x19, 0x00,             // Usage Minimum (0)
  0x2A, 0xE7, 0x00,       // Usage Maximum (231)
  0x15, 0x00,             // Logical Minimum (0)
  0x26, 0xE7, 0x00,       // Logical Maximum (231)
  0x75, 0x08,             // Report Size (8)
  0x95, 0x06,             // Report Count (6)
  0x81, 0x00,             // Input (Data, Array, Absolute)
  0xC0,                   // End Collection
};

const uint8_t * HIDGetReportDescriptor()
{
  return REPORT_DESCRIPTOR;
}

size_t HIDGetReportDescriptorSize()
{
  return sizeof(REPORT_DESCRIPTOR);
}

size_t HIDGetInputReportSize(uint8_t reportId)
{
  switch (reportId)
  {
    case HID_REPORT_ID_TABLET:
      return sizeof(HIDTabletReport);

    case HID_REPORT_ID_MOUSE:
      return sizeof(HIDMouseReport);

    case HID_REPORT_ID_KEYBOARD:
      return sizeof(HIDKeyboardReport);

    default:
      return 0;
  }
}
