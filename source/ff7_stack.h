/*
 * x86 stack accesses without mk_address (see ff7_stack.c).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef FF7_STACK_H
#define FF7_STACK_H

/** Hooks start_ff7_core(). Call before ff7_memmap_patch(). */
void ff7_stack_patch(void);

/** Logs off-stack accesses seen in check mode (nothing otherwise). */
void ff7_stack_report(double seconds);

#endif // FF7_STACK_H
