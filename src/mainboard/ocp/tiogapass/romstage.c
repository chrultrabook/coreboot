/*
 * This file is part of the coreboot project.
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <fsp/api.h>
#include <FspmUpd.h>
#include <soc/romstage.h>
#include <string.h>
#include <gpio.h>
#include <soc/lewisburg_pch_gpio_defs.h>
#include <skxsp_tp_iio.h>

static uint8_t iio_table_buf[sizeof(tp_iio_bifur_table)];

static void oem_update_iio(FSPM_UPD *mupd)
{
	/* Read GPIO to decide IIO bifurcation at run-time. */
	int slot_config0 = gpio_get(GPP_C15);
	int slot_config1 = gpio_get(GPP_C16);

	/* It's a single side 3 slots riser card, to tell which AICs are on each slot requires
	   reading the GPIO expander PCA9555 via SMBUS, and then configure the bifurcation
	   accordingly is left for future work.	*/
	if (!slot_config0 && slot_config1)
		mupd->FspmConfig.IioBifurcationConfig.IIoBifurcationTable[Skt0_Iou0].Bifurcation
			= IIO_BIFURCATE_xxx8xxx8;
}

static void mainboard_config_iio(FSPM_UPD *mupd)
{
	memcpy(iio_table_buf, tp_iio_bifur_table, sizeof(tp_iio_bifur_table));
	mupd->FspmConfig.IioBifurcationConfig.IIoBifurcationTable =
		(UPD_IIO_BIFURCATION_DATA_ENTRY *) iio_table_buf;
	mupd->FspmConfig.IioBifurcationConfig.NumberOfEntries =
		ARRAY_SIZE(tp_iio_bifur_table);

	mupd->FspmConfig.IioPciConfig.ConfigurationTable =
		(UPD_PCI_PORT_CONFIG *) tp_iio_pci_port_skt0;
	mupd->FspmConfig.IioPciConfig.NumberOfEntries =
		ARRAY_SIZE(tp_iio_pci_port_skt0);

	mupd->FspmConfig.PchPciConfig.PciPortConfig =
		(UPD_PCH_PCIE_PORT *) tp_pch_pci_port_skt0;
	mupd->FspmConfig.PchPciConfig.NumberOfEntries =
		ARRAY_SIZE(tp_pch_pci_port_skt0);

	mupd->FspmConfig.PchPciConfig.RootPortFunctionSwapping = 0x00;
	mupd->FspmConfig.PchPciConfig.PciePllSsc = 0x00;
	oem_update_iio(mupd);
}

void mainboard_memory_init_params(FSPM_UPD *mupd)
{
	mainboard_config_iio(mupd);

	/* do not configure GPIO controller inside FSP-M */
	mupd->FspmConfig.GpioConfig.GpioTable = NULL;
	mupd->FspmConfig.GpioConfig.NumberOfEntries = 0;
}
