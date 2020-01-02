/*
KVMGFX Client - A KVM Client for VGA Passthrough
Copyright (C) 2017-2019 Geoffrey McRae <geoff@hostfission.com>
https://looking-glass.hostfission.com

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include <GL/glx.h>

int sysinfo_gfx_max_multisample()
{
  Display * dpy = XOpenDisplay(NULL);
  if (!dpy)
    return -1;

  XVisualInfo queryTemplate;
  queryTemplate.screen = 0;

  int visualCount;
  int maxSamples = -1;
  XVisualInfo * visuals = XGetVisualInfo(dpy, VisualScreenMask, &queryTemplate, &visualCount);

  for (int i = 0; i < visualCount; i++)
  {
    XVisualInfo * visual = &visuals[i];

    int res, supportsGL;
    // Some GLX visuals do not use GL, and these must be ignored in our search.
    if ((res = glXGetConfig(dpy, visual, GLX_USE_GL, &supportsGL)) != 0 || !supportsGL)
      continue;

    int sampleBuffers, samples;
    if ((res = glXGetConfig(dpy, visual, GLX_SAMPLE_BUFFERS, &sampleBuffers)) != 0)
      continue;

    // Will be 1 if this visual supports multisampling
    if (sampleBuffers != 1)
      continue;

    if ((res = glXGetConfig(dpy, visual, GLX_SAMPLES, &samples)) != 0)
      continue;

    // Track the largest number of samples supported
    if (samples > maxSamples)
      maxSamples = samples;

  }

  XCloseDisplay(dpy);

  return maxSamples;
}