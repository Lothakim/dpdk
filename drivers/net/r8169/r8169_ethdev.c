/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Realtek Corporation. All rights reserved
 */

#include <stdio.h>
#include <errno.h>
#include <stdint.h>

#include <rte_eal.h>

#include <rte_common.h>
#include <rte_interrupts.h>
#include <rte_byteorder.h>
#include <rte_pci.h>
#include <bus_pci_driver.h>
#include <rte_ether.h>
#include <ethdev_driver.h>
#include <ethdev_pci.h>
#include <rte_memory.h>
#include <rte_malloc.h>
#include <dev_driver.h>

#include "r8169_ethdev.h"
#include "r8169_compat.h"
#include "r8169_logs.h"
#include "r8169_hw.h"

static int rtl_dev_configure(struct rte_eth_dev *dev);
static int rtl_dev_start(struct rte_eth_dev *dev);
static int rtl_dev_stop(struct rte_eth_dev *dev);
static int rtl_dev_reset(struct rte_eth_dev *dev);
static int rtl_dev_close(struct rte_eth_dev *dev);
static int rtl_dev_link_update(struct rte_eth_dev *dev, int wait);
static int rtl_dev_set_link_up(struct rte_eth_dev *dev);
static int rtl_dev_set_link_down(struct rte_eth_dev *dev);

/*
 * The set of PCI devices this driver supports
 */
static const struct rte_pci_id pci_id_r8169_map[] = {
	{ RTE_PCI_DEVICE(PCI_VENDOR_ID_REALTEK, 0x8125) },
	{ RTE_PCI_DEVICE(PCI_VENDOR_ID_REALTEK, 0x8162) },
	{ RTE_PCI_DEVICE(PCI_VENDOR_ID_REALTEK, 0x8126) },
	{ RTE_PCI_DEVICE(PCI_VENDOR_ID_REALTEK, 0x5000) },
	{.vendor_id = 0, /* sentinel */ },
};

static const struct eth_dev_ops rtl_eth_dev_ops = {
	.dev_configure	      = rtl_dev_configure,
	.dev_start	      = rtl_dev_start,
	.dev_stop	      = rtl_dev_stop,
	.dev_close	      = rtl_dev_close,
	.dev_reset	      = rtl_dev_reset,
	.dev_set_link_up      = rtl_dev_set_link_up,
	.dev_set_link_down    = rtl_dev_set_link_down,

	.link_update          = rtl_dev_link_update,
};

static int
rtl_dev_configure(struct rte_eth_dev *dev __rte_unused)
{
	return 0;
}

static void
rtl_disable_intr(struct rtl_hw *hw)
{
	PMD_INIT_FUNC_TRACE();
	RTL_W32(hw, IMR0_8125, 0x0000);
	RTL_W32(hw, ISR0_8125, RTL_R32(hw, ISR0_8125));
}

static void
rtl_enable_intr(struct rtl_hw *hw)
{
	PMD_INIT_FUNC_TRACE();
	RTL_W32(hw, IMR0_8125, LinkChg);
}

static int
_rtl_setup_link(struct rte_eth_dev *dev)
{
	struct rtl_adapter *adapter = RTL_DEV_PRIVATE(dev);
	struct rtl_hw *hw = &adapter->hw;
	u64 adv = 0;
	u32 *link_speeds = &dev->data->dev_conf.link_speeds;

	/* Setup link speed and duplex */
	if (*link_speeds == RTE_ETH_LINK_SPEED_AUTONEG) {
		rtl_set_link_option(hw, AUTONEG_ENABLE, SPEED_5000, DUPLEX_FULL, rtl_fc_full);
	} else if (*link_speeds != 0) {
		if (*link_speeds & ~(RTE_ETH_LINK_SPEED_10M_HD | RTE_ETH_LINK_SPEED_10M |
				     RTE_ETH_LINK_SPEED_100M_HD | RTE_ETH_LINK_SPEED_100M |
				     RTE_ETH_LINK_SPEED_1G | RTE_ETH_LINK_SPEED_2_5G |
				     RTE_ETH_LINK_SPEED_5G | RTE_ETH_LINK_SPEED_FIXED))
			goto error_invalid_config;

		if (*link_speeds & RTE_ETH_LINK_SPEED_10M_HD) {
			hw->speed = SPEED_10;
			hw->duplex = DUPLEX_HALF;
			adv |= ADVERTISE_10_HALF;
		}
		if (*link_speeds & RTE_ETH_LINK_SPEED_10M) {
			hw->speed = SPEED_10;
			hw->duplex = DUPLEX_FULL;
			adv |= ADVERTISE_10_FULL;
		}
		if (*link_speeds & RTE_ETH_LINK_SPEED_100M_HD) {
			hw->speed = SPEED_100;
			hw->duplex = DUPLEX_HALF;
			adv |= ADVERTISE_100_HALF;
		}
		if (*link_speeds & RTE_ETH_LINK_SPEED_100M) {
			hw->speed = SPEED_100;
			hw->duplex = DUPLEX_FULL;
			adv |= ADVERTISE_100_FULL;
		}
		if (*link_speeds & RTE_ETH_LINK_SPEED_1G) {
			hw->speed = SPEED_1000;
			hw->duplex = DUPLEX_FULL;
			adv |= ADVERTISE_1000_FULL;
		}
		if (*link_speeds & RTE_ETH_LINK_SPEED_2_5G) {
			hw->speed = SPEED_2500;
			hw->duplex = DUPLEX_FULL;
			adv |= ADVERTISE_2500_FULL;
		}
		if (*link_speeds & RTE_ETH_LINK_SPEED_5G) {
			hw->speed = SPEED_5000;
			hw->duplex = DUPLEX_FULL;
			adv |= ADVERTISE_5000_FULL;
		}

		hw->autoneg = AUTONEG_ENABLE;
		hw->advertising = adv;
	}

	rtl_set_speed(hw);

	return 0;

error_invalid_config:
	PMD_INIT_LOG(ERR, "Invalid advertised speeds (%u) for port %u",
		     dev->data->dev_conf.link_speeds, dev->data->port_id);
	return -EINVAL;
}

static int
rtl_setup_link(struct rte_eth_dev *dev)
{
#ifdef RTE_EXEC_ENV_FREEBSD
	struct rtl_adapter *adapter = RTL_DEV_PRIVATE(dev);
	struct rtl_hw *hw = &adapter->hw;
	struct rte_eth_link link;
	int count;
#endif

	_rtl_setup_link(dev);

#ifdef RTE_EXEC_ENV_FREEBSD
	for (count = 0; count < R8169_LINK_CHECK_TIMEOUT; count++) {
		if (!(RTL_R16(hw, PHYstatus) & LinkStatus)) {
			rte_delay_ms(R8169_LINK_CHECK_INTERVAL);
			continue;
		}

		rtl_dev_link_update(dev, 0);

		rte_eth_linkstatus_get(dev, &link);

		return 0;
	}
#endif
	return 0;
}

/*
 * Configure device link speed and setup link.
 * It returns 0 on success.
 */
static int
rtl_dev_start(struct rte_eth_dev *dev)
{
	struct rtl_adapter *adapter = RTL_DEV_PRIVATE(dev);
	struct rtl_hw *hw = &adapter->hw;
	struct rte_pci_device *pci_dev = RTE_ETH_DEV_TO_PCI(dev);
	struct rte_intr_handle *intr_handle = pci_dev->intr_handle;
	int err;

	/* Disable uio/vfio intr/eventfd mapping */
	rte_intr_disable(intr_handle);

	rtl_powerup_pll(hw);

	rtl_hw_ephy_config(hw);

	rtl_hw_phy_config(hw);

	rtl_hw_config(hw);

	/* Initialize transmission unit */
	rtl_tx_init(dev);

	/* This can fail when allocating mbufs for descriptor rings */
	err = rtl_rx_init(dev);
	if (err) {
		PMD_INIT_LOG(ERR, "Unable to initialize RX hardware");
		goto error;
	}

	/* Enable uio/vfio intr/eventfd mapping */
	rte_intr_enable(intr_handle);

	/* Resume enabled intr since hw reset */
	rtl_enable_intr(hw);

	rtl_setup_link(dev);

	rtl_mdio_write(hw, 0x1F, 0x0000);

	return 0;
error:
	return -EIO;
}

/*
 * Stop device: disable RX and TX functions to allow for reconfiguring.
 */
static int
rtl_dev_stop(struct rte_eth_dev *dev)
{
	struct rtl_adapter *adapter = RTL_DEV_PRIVATE(dev);
	struct rtl_hw *hw = &adapter->hw;
	struct rte_eth_link link;

	rtl_disable_intr(hw);

	rtl_nic_reset(hw);

	switch (hw->mcfg) {
	case CFG_METHOD_48 ... CFG_METHOD_57:
	case CFG_METHOD_69 ... CFG_METHOD_71:
		rtl_mac_ocp_write(hw, 0xE00A, hw->mcu_pme_setting);
		break;
	}

	rtl_powerdown_pll(hw);

	/* Clear the recorded link status */
	memset(&link, 0, sizeof(link));
	rte_eth_linkstatus_set(dev, &link);

	return 0;
}

static int
rtl_dev_set_link_up(struct rte_eth_dev *dev)
{
	struct rtl_adapter *adapter = RTL_DEV_PRIVATE(dev);
	struct rtl_hw *hw = &adapter->hw;

	rtl_powerup_pll(hw);

	return 0;
}

static int
rtl_dev_set_link_down(struct rte_eth_dev *dev)
{
	struct rtl_adapter *adapter = RTL_DEV_PRIVATE(dev);
	struct rtl_hw *hw = &adapter->hw;

	/* mcu pme intr masks */
	switch (hw->mcfg) {
	case CFG_METHOD_48 ... CFG_METHOD_57:
	case CFG_METHOD_69 ... CFG_METHOD_71:
		rtl_mac_ocp_write(hw, 0xE00A, hw->mcu_pme_setting & ~(BIT_11 | BIT_14));
		break;
	}

	rtl_powerdown_pll(hw);

	return 0;
}

/* Return 0 means link status changed, -1 means not changed */
static int
rtl_dev_link_update(struct rte_eth_dev *dev, int wait __rte_unused)
{
	struct rte_eth_link link, old;
	struct rtl_adapter *adapter = RTL_DEV_PRIVATE(dev);
	struct rtl_hw *hw = &adapter->hw;
	u32 speed;
	u16 status;

	link.link_status = RTE_ETH_LINK_DOWN;
	link.link_speed = 0;
	link.link_duplex = RTE_ETH_LINK_FULL_DUPLEX;
	link.link_autoneg = RTE_ETH_LINK_AUTONEG;

	memset(&old, 0, sizeof(old));

	/* Load old link status */
	rte_eth_linkstatus_get(dev, &old);

	/* Read current link status */
	status = RTL_R16(hw, PHYstatus);

	if (status & LinkStatus) {
		link.link_status = RTE_ETH_LINK_UP;

		if (status & FullDup) {
			link.link_duplex = RTE_ETH_LINK_FULL_DUPLEX;
			if (hw->mcfg == CFG_METHOD_2)
				RTL_W32(hw, TxConfig, (RTL_R32(hw, TxConfig) |
						       (BIT_24 | BIT_25)) & ~BIT_19);

		} else {
			link.link_duplex = RTE_ETH_LINK_HALF_DUPLEX;
			if (hw->mcfg == CFG_METHOD_2)
				RTL_W32(hw, TxConfig, (RTL_R32(hw, TxConfig) | BIT_25) &
					~(BIT_19 | BIT_24));
		}

		if (status & _5000bpsF)
			speed = 5000;
		else if (status & _2500bpsF)
			speed = 2500;
		else if (status & _1000bpsF)
			speed = 1000;
		else if (status & _100bps)
			speed = 100;
		else
			speed = 10;

		link.link_speed = speed;
	}

	if (link.link_status == old.link_status)
		return -1;

	rte_eth_linkstatus_set(dev, &link);

	return 0;
}

static void
rtl_dev_interrupt_handler(void *param)
{
	struct rte_eth_dev *dev = (struct rte_eth_dev *)param;
	struct rtl_adapter *adapter = RTL_DEV_PRIVATE(dev);
	struct rtl_hw *hw = &adapter->hw;
	uint32_t intr;

	intr = RTL_R32(hw, ISR0_8125);

	/* Clear all cause mask */
	rtl_disable_intr(hw);

	if (intr & LinkChg)
		rtl_dev_link_update(dev, 0);
	else
		PMD_DRV_LOG(ERR, "r8169: interrupt unhandled.");

	rtl_enable_intr(hw);
}

/*
 * Reset and stop device.
 */
static int
rtl_dev_close(struct rte_eth_dev *dev)
{
	struct rte_pci_device *pci_dev = RTE_ETH_DEV_TO_PCI(dev);
	struct rte_intr_handle *intr_handle = pci_dev->intr_handle;
	struct rtl_adapter *adapter = RTL_DEV_PRIVATE(dev);
	struct rtl_hw *hw = &adapter->hw;
	int retries = 0;
	int ret_unreg, ret_stp;

	if (rte_eal_process_type() != RTE_PROC_PRIMARY)
		return 0;

	ret_stp = rtl_dev_stop(dev);

	/* Reprogram the RAR[0] in case user changed it. */
	rtl_rar_set(hw, hw->mac_addr);

	/* Disable uio intr before callback unregister */
	rte_intr_disable(intr_handle);

	do {
		ret_unreg = rte_intr_callback_unregister(intr_handle, rtl_dev_interrupt_handler,
							 dev);
		if (ret_unreg >= 0 || ret_unreg == -ENOENT)
			break;
		else if (ret_unreg != -EAGAIN)
			PMD_DRV_LOG(ERR, "r8169: intr callback unregister failed: %d", ret_unreg);

		rte_delay_ms(100);
	} while (retries++ < (10 + 90));

	return ret_stp;
}

static int
rtl_dev_init(struct rte_eth_dev *dev)
{
	struct rte_pci_device *pci_dev = RTE_ETH_DEV_TO_PCI(dev);
	struct rte_intr_handle *intr_handle = pci_dev->intr_handle;
	struct rtl_adapter *adapter = RTL_DEV_PRIVATE(dev);
	struct rtl_hw *hw = &adapter->hw;
	struct rte_ether_addr *perm_addr = (struct rte_ether_addr *)hw->mac_addr;
	char buf[RTE_ETHER_ADDR_FMT_SIZE];

	dev->dev_ops = &rtl_eth_dev_ops;
	dev->tx_pkt_burst = &rtl_xmit_pkts;
	dev->rx_pkt_burst = &rtl_recv_pkts;

	/* For secondary processes, the primary process has done all the work */
	if (rte_eal_process_type() != RTE_PROC_PRIMARY)
		return 0;

	hw->mmio_addr = (u8 *)pci_dev->mem_resource[2].addr; /* RTL8169 uses BAR2 */

	rtl_get_mac_version(hw, pci_dev);

	if (rtl_set_hw_ops(hw))
		return -ENOTSUP;

	rtl_disable_intr(hw);

	rtl_hw_initialize(hw);

	/* Read the permanent MAC address out of ROM */
	rtl_get_mac_address(hw, perm_addr);

	if (!rte_is_valid_assigned_ether_addr(perm_addr)) {
		rte_eth_random_addr(&perm_addr->addr_bytes[0]);

		rte_ether_format_addr(buf, sizeof(buf), perm_addr);

		PMD_INIT_LOG(NOTICE, "r8169: Assign randomly generated MAC address %s", buf);
	}

	/* Allocate memory for storing MAC addresses */
	dev->data->mac_addrs = rte_zmalloc("r8169", RTE_ETHER_ADDR_LEN, 0);

	if (dev->data->mac_addrs == NULL) {
		PMD_INIT_LOG(ERR, "MAC Malloc failed");
		return -ENOMEM;
	}

	/* Copy the permanent MAC address */
	rte_ether_addr_copy(perm_addr, &dev->data->mac_addrs[0]);

	rtl_rar_set(hw, &perm_addr->addr_bytes[0]);

	rte_intr_callback_register(intr_handle, rtl_dev_interrupt_handler, dev);

	/* Enable uio/vfio intr/eventfd mapping */
	rte_intr_enable(intr_handle);

	return 0;
}

static int
rtl_dev_uninit(struct rte_eth_dev *dev)
{
	if (rte_eal_process_type() != RTE_PROC_PRIMARY)
		return -EPERM;

	rtl_dev_close(dev);

	return 0;
}

static int
rtl_dev_reset(struct rte_eth_dev *dev)
{
	int ret;

	ret = rtl_dev_uninit(dev);
	if (ret)
		return ret;

	ret = rtl_dev_init(dev);

	return ret;
}

static int
rtl_pci_probe(struct rte_pci_driver *pci_drv __rte_unused,
	      struct rte_pci_device *pci_dev)
{
	return rte_eth_dev_pci_generic_probe(pci_dev, sizeof(struct rtl_adapter),
					     rtl_dev_init);
}

static int
rtl_pci_remove(struct rte_pci_device *pci_dev)
{
	return rte_eth_dev_pci_generic_remove(pci_dev, rtl_dev_uninit);
}

static struct rte_pci_driver rte_r8169_pmd = {
	.id_table  = pci_id_r8169_map,
	.drv_flags = RTE_PCI_DRV_NEED_MAPPING | RTE_PCI_DRV_INTR_LSC,
	.probe     = rtl_pci_probe,
	.remove    = rtl_pci_remove,
};

RTE_PMD_REGISTER_PCI(net_r8169, rte_r8169_pmd);
RTE_PMD_REGISTER_PCI_TABLE(net_r8169, pci_id_r8169_map);
RTE_PMD_REGISTER_KMOD_DEP(net_r8169, "* igb_uio | uio_pci_generic | vfio-pci");

RTE_LOG_REGISTER_SUFFIX(r8169_logtype_init, init, NOTICE)
RTE_LOG_REGISTER_SUFFIX(r8169_logtype_driver, driver, NOTICE)
#ifdef RTE_ETHDEV_DEBUG_RX
RTE_LOG_REGISTER_SUFFIX(r8169_logtype_rx, rx, DEBUG)
#endif
#ifdef RTE_ETHDEV_DEBUG_TX
RTE_LOG_REGISTER_SUFFIX(r8169_logtype_tx, tx, DEBUG)
#endif
