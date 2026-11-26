/*
  Simple DirectMedia Layer
  Copyright (C) 2026 Visionik <visionik@pobox.com>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not
     be misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#ifndef SDL_kittyparallel_h_
#define SDL_kittyparallel_h_

#include <stddef.h>

/*
 * Portable parallel-for fan-out for Kitty hot paths (parallel RFC 1950 zlib,
 * fused convert+tile-diff, multi-rect a=f):
 *   - macOS: Apple GCD dispatch_apply_f (QOS_CLASS_USER_INTERACTIVE)
 *   - Linux / POSIX: persistent pthread worker pool (main thread participates)
 */
typedef void (*KITTY_ParallelFn)(void *arg, size_t idx);

void KITTY_ParallelFor(size_t count, KITTY_ParallelFn fn, void *arg);

#endif /* SDL_kittyparallel_h_ */
