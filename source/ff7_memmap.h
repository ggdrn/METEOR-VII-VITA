/*
 * Faster x86 address translation (mk_address) for libjni_ff7.so
 * (see ff7_memmap.c).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef FF7_MEMMAP_H
#define FF7_MEMMAP_H

/** Rewrites mk_address and hooks manage_import to resolve constant
 *  addresses. Call after relocation, after the other patches. */
void ff7_memmap_patch(void);

#endif // FF7_MEMMAP_H
