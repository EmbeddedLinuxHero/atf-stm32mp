/*
 * Copyright (c) 2015-2021, ARM Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <assert.h>
#include <string.h>

#include <platform_def.h>

#include <arch_helpers.h>
#include <common/bl_common.h>
#include <common/debug.h>
#include <context.h>
#include <drivers/arm/gicv2.h>
#include <drivers/arm/tzc400.h>
#include <drivers/generic_delay_timer.h>
#include <drivers/regulator.h>
#include <drivers/st/bsec.h>
#include <drivers/st/etzpc.h>
#include <drivers/st/regulator_fixed.h>
#include <drivers/st/stm32_gpio.h>
#include <drivers/st/stm32_iwdg.h>
#include <drivers/st/stm32_rng.h>
#include <drivers/st/stm32_rtc.h>
#include <drivers/st/stm32_tamp.h>
#include <drivers/st/stm32mp_pmic.h>
#include <drivers/st/stm32mp_reset.h>
#include <drivers/st/stm32mp1_clk.h>
#include <dt-bindings/clock/stm32mp1-clks.h>
#include <lib/el3_runtime/context_mgmt.h>
#include <lib/mmio.h>
#include <lib/xlat_tables/xlat_tables_v2.h>
#include <plat/common/platform.h>

#include <platform_sp_min.h>

/******************************************************************************
 * Placeholder variables for copying the arguments that have been passed to
 * BL32 from BL2.
 ******************************************************************************/
static entry_point_info_t bl33_image_ep_info;

static const char * const tamper_name[] = {
	[INT_TAMP1] = "RTC power domain",
	[INT_TAMP2] = "Temperature monitoring",
	[INT_TAMP3] = "LSE monitoring",
	[INT_TAMP4] = "HSE monitoring",
};

static int stm32mp1_tamper_action(int id)
{
	const char *tamp_name = NULL;

	if ((id >= 0) && ((size_t)id < ARRAY_SIZE(tamper_name))) {
		tamp_name = tamper_name[id];
	}
	ERROR("Tamper %u (%s) occurs\n", id, tamp_name);

	return 1; /* ack TAMPER and reset system */
}

static void disable_usb_phy_regulator(void)
{
	if (dt_pmic_status() > 0) {
		struct rdev *regul = dt_get_usb_phy_regulator();
		int ret;

		if (regul == NULL) {
			return;
		}

		if (regulator_is_enabled(regul) == 1) {
			ret = regulator_disable(regul);
			if (ret < 0) {
				WARN("USBPHYC phy-supply (%s) disable failed\n", regul->reg_name);
			}
		}
	}
}

/*******************************************************************************
 * Interrupt handler for FIQ (secure IRQ)
 ******************************************************************************/
void sp_min_plat_fiq_handler(uint32_t id)
{
	switch (id & INT_ID_MASK) {
	case STM32MP1_IRQ_TZC400:
		tzc400_init(STM32MP1_TZC_BASE);
		(void)tzc400_it_handler();
		panic();
		break;
	case STM32MP1_IRQ_TAMPSERRS:
		stm32_tamp_it_handler();
		break;
	case STM32MP1_IRQ_AXIERRIRQ:
		ERROR("STM32MP1_IRQ_AXIERRIRQ generated\n");
		panic();
		break;
	default:
		ERROR("SECURE IT handler not define for it : %u", id);
		break;
	}
}

/*******************************************************************************
 * Return a pointer to the 'entry_point_info' structure of the next image for
 * the security state specified. BL33 corresponds to the non-secure image type
 * while BL32 corresponds to the secure image type. A NULL pointer is returned
 * if the image does not exist.
 ******************************************************************************/
entry_point_info_t *sp_min_plat_get_bl33_ep_info(void)
{
	entry_point_info_t *next_image_info;

	next_image_info = &bl33_image_ep_info;

	if (next_image_info->pc == 0U) {
		return NULL;
	}

	return next_image_info;
}

CASSERT((STM32MP_SEC_SYSRAM_BASE == STM32MP_SYSRAM_BASE) &&
	((STM32MP_SEC_SYSRAM_BASE + STM32MP_SEC_SYSRAM_SIZE) <=
	 (STM32MP_SYSRAM_BASE + STM32MP_SYSRAM_SIZE)),
	assert_secure_sysram_fits_at_begining_of_sysram);

#ifdef STM32MP_NS_SYSRAM_BASE
CASSERT((STM32MP_NS_SYSRAM_BASE >= STM32MP_SEC_SYSRAM_BASE) &&
	((STM32MP_NS_SYSRAM_BASE + STM32MP_NS_SYSRAM_SIZE) ==
	 (STM32MP_SYSRAM_BASE + STM32MP_SYSRAM_SIZE)),
	assert_non_secure_sysram_fits_at_end_of_sysram);

CASSERT((STM32MP_NS_SYSRAM_BASE & (PAGE_SIZE_4KB - U(1))) == 0U,
	assert_non_secure_sysram_base_is_4kbyte_aligned);

#define TZMA1_SECURE_RANGE \
	(((STM32MP_NS_SYSRAM_BASE - STM32MP_SYSRAM_BASE) >> FOUR_KB_SHIFT) - 1U)
#else
#define TZMA1_SECURE_RANGE		STM32MP1_ETZPC_TZMA_ALL_SECURE
#endif /* STM32MP_NS_SYSRAM_BASE */
#define TZMA0_SECURE_RANGE		STM32MP1_ETZPC_TZMA_ALL_SECURE

static void stm32mp1_etzpc_early_setup(void)
{
	if (etzpc_init() != 0) {
		panic();
	}

	etzpc_configure_tzma(STM32MP1_ETZPC_TZMA_ROM, TZMA0_SECURE_RANGE);
	etzpc_configure_tzma(STM32MP1_ETZPC_TZMA_SYSRAM, TZMA1_SECURE_RANGE);
}

/*******************************************************************************
 * Perform any BL32 specific platform actions.
 ******************************************************************************/
void sp_min_early_platform_setup2(u_register_t arg0, u_register_t arg1,
				  u_register_t arg2, u_register_t arg3)
{
	bl_params_t *params_from_bl2 = (bl_params_t *)arg0;
#if STM32MP_USE_STM32IMAGE
	uintptr_t dt_addr = STM32MP_DTB_BASE;
#else
	uintptr_t dt_addr = arg1;
#endif

	stm32mp_setup_early_console();

	/* Imprecise aborts can be masked in NonSecure */
	write_scr(read_scr() | SCR_AW_BIT);

	mmap_add_region(BL_CODE_BASE, BL_CODE_BASE,
			BL_CODE_END - BL_CODE_BASE,
			MT_CODE | MT_SECURE);

	configure_mmu();

	assert(params_from_bl2 != NULL);
	assert(params_from_bl2->h.type == PARAM_BL_PARAMS);
	assert(params_from_bl2->h.version >= VERSION_2);

	bl_params_node_t *bl_params = params_from_bl2->head;

	/*
	 * Copy BL33 entry point information.
	 * They are stored in Secure RAM, in BL2's address space.
	 */
	while (bl_params != NULL) {
		if (bl_params->image_id == BL33_IMAGE_ID) {
			bl33_image_ep_info = *bl_params->ep_info;
			/*
			 *  Check if hw_configuration is given to BL32 and
			 *  share it to BL33.
			 */
			if (arg2 != 0U) {
				bl33_image_ep_info.args.arg0 = 0U;
				bl33_image_ep_info.args.arg1 = 0U;
				bl33_image_ep_info.args.arg2 = arg2;
			}

			break;
		}

		bl_params = bl_params->next_params_info;
	}

	if (dt_open_and_check(dt_addr) < 0) {
		panic();
	}

	if (bsec_probe() != 0) {
		panic();
	}

	if (stm32mp1_clk_probe() < 0) {
		panic();
	}

	(void)stm32mp_uart_console_setup();

	stm32mp1_etzpc_early_setup();

	generic_delay_timer_init();

	if (dt_pmic_status() > 0) {
		initialize_pmic();
	}

	fixed_regulator_register();

	if (regulator_core_config() != 0) {
		ERROR("Regulator core config error\n");
		panic();
	}

	disable_usb_phy_regulator();
}

static void init_sec_peripherals(void)
{
	int ret;

	/* Disable MCU subsystem protection */
	stm32mp1_clk_mcuss_protect(false);

	/* Init rtc driver */
	ret = stm32_rtc_init();
	if (ret < 0) {
		WARN("RTC driver init error %i\n", ret);
	}

	/*  Init rng driver */
	ret = stm32_rng_init();
	if (ret < 0) {
		WARN("RNG driver init error %i\n", ret);
	}

	/* Init tamper */
	if (stm32_tamp_init() > 0) {
		struct bkpregs_conf bkpregs_conf = {
			.nb_zone1_regs = TAMP_BKP_SEC_NUMBER,
			.nb_zone2_regs = 0 /* no register in zone 2 */
			/* zone3 all remaining */
		};

		/* Enable BKP Register protection */
		if (stm32_tamp_set_secure_bkpregs(&bkpregs_conf) < 0) {
			panic();
		}

		stm32_tamp_configure_secure_access(TAMP_REGS_IT_SECURE);

		stm32_tamp_configure_internal(INT_TAMP1, TAMP_ENABLE, stm32mp1_tamper_action);
		stm32_tamp_configure_internal(INT_TAMP2, TAMP_ENABLE, stm32mp1_tamper_action);
		stm32_tamp_configure_internal(INT_TAMP3, TAMP_ENABLE, stm32mp1_tamper_action);
		stm32_tamp_configure_internal(INT_TAMP4, TAMP_ENABLE, stm32mp1_tamper_action);

		ret = stm32_tamp_set_config();
		if (ret < 0) {
			panic();
		}

		/* Enable timestamp for tamper */
		stm32_rtc_set_tamper_timestamp();
	}
}

/*******************************************************************************
 * Initialize the MMU, security and the GIC.
 ******************************************************************************/
void sp_min_platform_setup(void)
{
	stm32mp_gic_init();

	init_sec_peripherals();

	if (stm32_iwdg_init() < 0) {
		panic();
	}

	stm32mp_lock_periph_registering();

	stm32mp1_init_scmi_server();

	regulator_core_cleanup();
}

void sp_min_plat_arch_setup(void)
{
}
