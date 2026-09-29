/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef UI_POWEREMU_ACTIVITY_H
#define UI_POWEREMU_ACTIVITY_H

#ifdef CONFIG_COCOA
void *poweremu_activity_begin(void);
void poweremu_activity_end(void *activity);
#else
static inline void *poweremu_activity_begin(void) { return NULL; }
static inline void poweremu_activity_end(void *activity) { }
#endif

#endif
