/*
Looking Glass - KVM FrameRelay (KVMFR) Client
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

#pragma once
#include "interface/decoder.h"

extern const LG_Decoder LGD_NULL;
extern const LG_Decoder LGD_YUV420;

const LG_Decoder * LG_Decoders[] =
{
  &LGD_NULL,
  &LGD_YUV420,
  NULL // end of array sentinal
};

#define LG_DECODER_COUNT ((sizeof(LG_Decoders) / sizeof(LG_Decoder *)) - 1)