/*
 * Copyright (C) 2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  patch.c
 * @brief Patching some of the .so internal functions or bridging them to native
 *        for better compatibility.
 */

#include <kubridge.h>
#include <so_util/so_util.h>
#include <stdio.h>
#include <string.h>
#include <vitasdk.h>

#ifdef __cplusplus
extern "C"
{
#endif
	extern so_module so_mod;
#ifdef __cplusplus
};
#endif

#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RX (0x0C20D050)

#include "utils/logger.h"
#include "utils/dialog.h"
#include "reimpl/sys.h"
#include "ff7_texture.h"
#include "ff7_anim.h"
#include "ff7_lzss.h"
#include "ff7_swirl.h"
#include "ff7_light.h"
#include "ff7_bg.h"
#include "ff7_math.h"
#include "ff7_ui.h"
#include "ff7_memmap.h"
#include "ff7_tri.h"
#include "ff7_d2ulz.h"
#include "ff7_ftol.h"
#include "ff7_highway.h"
#include "ff7_engine.h"
#include "ff7_chocobo.h"
#include "ff7_snowboard.h"
#include "ff7_dsound.h"
#include "ff7_framecap.h"
#include "ff7_coaster.h"
#include "ff7_submarine.h"
#include "ff7_fieldbg.h"
#include "ff7_vcolor.h"
#include "ff7_interp.h"
#include "ff7_world.h"
#include "ff7_music.h"
#include "ff7_screen.h"
#include "ff7_stack.h"
#include "ff7_buttons.h"
#include "ff7_selftest.h"
#include "utils/utils.h"
#include <stdbool.h>

bool ff7_selftests_on;

void __kuser_memory_barrier(void) {
	__sync_synchronize();
}

void kuser_patch(void) {
	SceKernelAllocMemBlockKernelOpt opt;
	memset(&opt, 0, sizeof(SceKernelAllocMemBlockKernelOpt));
	opt.size = sizeof(SceKernelAllocMemBlockKernelOpt);
	opt.attr = 0x1;
	opt.field_C = (SceUInt32)0x9A000000;
	if (kuKernelAllocMemBlock("atomic", SCE_KERNEL_MEMBLOCK_TYPE_USER_RX, 0x1000, &opt) < 0)
		fatal_error("Error could not allocate atomic block.");
	kuKernelMemProtect((void *)0x9A000000, (SceSize)0x1000, KU_KERNEL_PROT_EXEC | KU_KERNEL_PROT_READ | KU_KERNEL_PROT_WRITE);

	hook_addr(0x9A000FA0, (uintptr_t)__kuser_memory_barrier);
	hook_addr(0x9A000FC0, (uintptr_t)__atomic_cmpxchg);

	uint32_t patched_addr;
	for (uint32_t addr = so_mod.text_base; addr < so_mod.text_base + so_mod.text_size; addr += 4) {
		uint32_t *a = (uint32_t *)addr;
		if (*a == 0xFFFF0FC0) {
			l_debug("Patching 0x%x -> __kuser_cmpxchg", a);
			patched_addr = 0x9A000FC0;
			kuKernelCpuUnrestrictedMemcpy((void *)(addr), &patched_addr, sizeof(uint32_t));
		}
		else if (*a == 0xFFFF0FA0) {
			l_debug("Patching 0x%x -> __kuser_memory_barrier", a);
			patched_addr = 0x9A000FA0;
			kuKernelCpuUnrestrictedMemcpy((void *)(addr), &patched_addr, sizeof(uint32_t));
		}
	}
}

void so_patch(void) {
	kuser_patch();
	ff7_selftests_on = file_exists(SELFTEST_SWITCH);
	l_info("selftests: %s (%s)", ff7_selftests_on ? "on" : "off", SELFTEST_SWITCH);
	ff7_texture_patch();
	ff7_anim_patch();
	ff7_lzss_patch();
	ff7_swirl_patch();
	ff7_light_patch();
	ff7_bg_patch();
	ff7_math_patch();
	ff7_ui_patch();
	ff7_tri_patch();
	ff7_d2ulz_patch();
	ff7_ftol_patch();
	ff7_highway_patch();
	ff7_engine_patch();
	ff7_chocobo_patch();
	ff7_snowboard_patch();
	ff7_coaster_patch();
	ff7_submarine_input_patch();
	ff7_submarine_patch();
	ff7_fieldbg_patch();
	ff7_vcolor_patch();
	ff7_interp_patch();
	ff7_framecap_patch();
	ff7_world_patch();
	ff7_music_patch();
	ff7_dsound_patch();
	ff7_screen_patch();
	ff7_stack_patch();
	ff7_buttons_patch();
	ff7_memmap_patch(); // last: its site checks must see the other hooks
	// Sample hook with symbol name
	// hook_addr((uintptr_t)so_symbol(&so_mod, "_ZN6glitch2os7Printer5printEPKcz"), (uintptr_t)&hookedFunction);
	// Or with offset
	// hook_addr((uintptr_t)so_mod.text_base + 0xdeadbabe, (uintptr_t)&hookedFunction);
	// If you use SO_CONTINUE, define a so_hook before the function and assign to it
	// function_hook = hook_addr(...);
}
