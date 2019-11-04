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

#include <stdbool.h>

typedef struct ObjectList * ObjectList;

typedef void (*ObjectFreeFn)(void * object);

ObjectList   objectlist_new   (ObjectFreeFn free_fn);
void         objectlist_free  (ObjectList * ol);
int          objectlist_push  (ObjectList ol, void * object);
void *       objectlist_pop   (ObjectList ol);
bool         objectlist_remove(ObjectList ol, unsigned int index);
unsigned int objectlist_count (ObjectList ol);
void *       objectlist_at    (ObjectList ol, unsigned int index);

// generic free method
void objectlist_free_item(void *object);