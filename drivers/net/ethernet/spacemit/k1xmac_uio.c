// SPDX-License-Identifier: GPL-2.0
/**
 * Copyright (c) 2023, spacemit Corporation.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/of_net.h>
#include <linux/uio_driver.h>
#include <linux/list.h>

#include <linux/clk.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/ethtool.h>
#include <linux/if_ether.h>
#include <linux/crc32.h>
#include <linux/mii.h>
#include <linux/if.h>
#include <linux/if_vlan.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/prefetch.h>
#include <linux/pinctrl/consumer.h>
#ifdef CONFIG_DEBUG_FS
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#endif /* CONFIG_DEBUG_FS */
#include <linux/net_tstamp.h>
#include <linux/udp.h>
#include <net/pkt_cls.h>
#include <linux/reset.h>
#include <linux/of_mdio.h>

#include "k1x-emac.h"

#define DRIVER_NAME	"k1xmac_uio_drv"
#define DRIVER_VERSION	"0.1"

#define K1XMAC_UIO_DEVICE_NAME     "smt-k1x-uio"

#define EMAC_UIO_TX_RING_NUM		1024
#define EMAC_UIO_RX_RING_NUM		1024

#define UIO_CLK_PHASE_REVERT		180

#define UIO_TX_PHASE			1
#define UIO_RX_PHASE			0

enum uio_clk_tuning_way {
	/* fpga clk tuning register */
	UIO_CLK_TUNING_BY_REG,
	/* zebu/evb rgmii delayline register */
	UIO_CLK_TUNING_BY_DLINE,
	/* evb rmii only revert tx/rx clock for clk tuning */
	UIO_CLK_TUNING_BY_CLK_REVERT,
	UIO_CLK_TUNING_MAX,
};

/**
 * k1x_emac_uio_pdev_info
 * local information for uio module driver
 *
 * @dev:      device pointer
 * @ndev:     network device pointer
 * @name:     uio name
 * @uio:      uio information
 * @map_num:  number of uio memory regions
 */
struct k1x_emac_uio_pdev_info {
	struct device *dev;
	struct net_device *ndev;
	char name[16];
	struct uio_info uio;
	int map_num;
};

static int k1x_emac_uio_open(struct uio_info *info, struct inode *inode)
{
	return 0;
}

static int k1x_emac_uio_release(struct uio_info *info, struct inode *inode)
{
	return 0;
}

static int k1x_emac_uio_mmap(struct uio_info *info,
			     struct vm_area_struct *vma)
{
	u32 ret;
	u32 pfn;

	pfn = (info->mem[vma->vm_pgoff].addr) >> PAGE_SHIFT;

	if (vma->vm_pgoff)
		vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	else
		vma->vm_page_prot = pgprot_device(vma->vm_page_prot);

	ret = remap_pfn_range(vma, vma->vm_start, pfn,
			      vma->vm_end - vma->vm_start,
			      vma->vm_page_prot);
	if (ret) {
		/* Error Handle */
		pr_err("remap_pfn_range failed");
	}
	return ret;
}

/* Name		k1x_emac_uio_alloc_tx_resources
 * Arguments	priv : pointer to driver private data structure
 * Return	Status: 0 - Success;  non-zero - Fail
 * Description	Allocates TX resources and getting virtual & physical address.
 */
static int k1x_emac_uio_alloc_tx_resources(struct emac_priv *priv)
{
	struct emac_desc_ring *tx_ring = &priv->tx_ring;
	struct platform_device *pdev  = priv->pdev;

	tx_ring->total_size = tx_ring->total_cnt * sizeof(struct emac_tx_desc);

	EMAC_ROUNDUP(tx_ring->total_size, 1024);

	tx_ring->desc_addr = dma_alloc_coherent(&pdev->dev, tx_ring->total_size,
						&tx_ring->desc_dma_addr,
						GFP_KERNEL);
	if (!tx_ring->desc_addr) {
		pr_err("Memory allocation failed for the Transmit descriptor ring\n");
		return -ENOMEM;
	}

	memset(tx_ring->desc_addr, 0, tx_ring->total_size);

	tx_ring->head = 0;
	tx_ring->tail = 0;

	return 0;
}

/**
 * k1x_emac_uio_free_tx_resources - free TX dma desc resources
 * @priv: private structure
 */
static void k1x_emac_uio_free_tx_resources(struct emac_priv *priv)
{
	dma_free_coherent(&priv->pdev->dev, priv->tx_ring.total_size,
			  priv->tx_ring.desc_addr,
			  priv->tx_ring.desc_dma_addr);
	priv->tx_ring.desc_addr = NULL;
}

/**
 * k1x_emac_uio_alloc_rx_resources - alloc RX resources.
 * @priv: private structure
 * Description: according to which descriptor can be used (extend or basic)
 * this function allocates the resources for TX and RX paths. In case of
 * reception, for example, it pre-allocated the RX socket buffer in order to
 * allow zero-copy mechanism.
 */
static int k1x_emac_uio_alloc_rx_resources(struct emac_priv *priv)
{
	struct emac_desc_ring *rx_ring = &priv->rx_ring;
	struct platform_device *pdev  = priv->pdev;

	/* round up to nearest 4K */
	rx_ring->total_size = rx_ring->total_cnt * sizeof(struct emac_rx_desc);

	EMAC_ROUNDUP(rx_ring->total_size, 1024);

	rx_ring->desc_addr = dma_alloc_coherent(&pdev->dev,
						rx_ring->total_size,
						&rx_ring->desc_dma_addr,
						GFP_KERNEL);
	if (!rx_ring->desc_addr) {
		pr_err("Memory allocation failed for the Receive descriptor ring\n");
		return -ENOMEM;
	}

	memset(rx_ring->desc_addr, 0, rx_ring->total_size);

	rx_ring->head = 0;
	rx_ring->tail = 0;

	return 0;
}

/**
 * k1x_emac_uio_free_rx_resources - free RX dma desc resources
 * @priv: private structure
 */
static void k1x_emac_uio_free_rx_resources(struct emac_priv *priv)
{
	dma_free_coherent(&priv->pdev->dev, priv->rx_ring.total_size,
			  priv->rx_ring.desc_addr,
			  priv->rx_ring.desc_dma_addr);
	priv->rx_ring.desc_addr = NULL;
}

static bool uio_emac_is_rmii(struct emac_priv *priv)
{
	return priv->phy_interface == PHY_INTERFACE_MODE_RMII;
}

static void uio_phy_interface_config(struct emac_priv *priv)
{
	u32 val;

	val = readl(priv->ctrl_reg);
	if (uio_emac_is_rmii(priv)) {
		val &= ~PHY_INTF_RGMII;
		if (priv->ref_clk_frm_soc)
			val |= REF_CLK_SEL;
		else
			val &= ~REF_CLK_SEL;
	} else {
		val |= PHY_INTF_RGMII;
		if (priv->ref_clk_frm_soc)
			val |= RGMII_TX_CLK_SEL;
	}
	writel(val, priv->ctrl_reg);
}

static void uio_emac_adjust_link(struct net_device *dev)
{
	struct phy_device *phydev = dev->phydev;
	struct emac_priv *priv = netdev_priv(dev);
	bool link_changed = false;
	u32 ctrl;

	if (!phydev)
		return;

	if (phydev->link) {
		ctrl = emac_rd(priv, MAC_GLOBAL_CONTROL);

		/* Now we make sure that we can be in full duplex mode
		 * If not, we operate in half-duplex mode.
		 */
		if (phydev->duplex != priv->duplex) {
			link_changed = true;

			if (!phydev->duplex)
				ctrl &= ~MREGBIT_FULL_DUPLEX_MODE;
			else
				ctrl |= MREGBIT_FULL_DUPLEX_MODE;
			priv->duplex = phydev->duplex;
		}

		if (phydev->speed != priv->speed) {
			link_changed = true;

			ctrl &= ~MREGBIT_SPEED;

			switch (phydev->speed) {
			case SPEED_1000:
				ctrl |= MREGBIT_SPEED_1000M;
				break;
			case SPEED_100:
				ctrl |= MREGBIT_SPEED_100M;
				break;
			case SPEED_10:
				ctrl |= MREGBIT_SPEED_10M;
				break;
			default:
				pr_err("broken speed: %d\n", phydev->speed);
				phydev->speed = SPEED_UNKNOWN;
				break;
			}

			if (phydev->speed != SPEED_UNKNOWN)
				priv->speed = phydev->speed;
		}

		emac_wr(priv, MAC_GLOBAL_CONTROL, ctrl);

		if (!priv->link) {
			priv->link = true;
			link_changed = true;
		}
	} else if (priv->link) {
		priv->link = false;
		link_changed = true;
		priv->duplex = DUPLEX_UNKNOWN;
		priv->speed = SPEED_UNKNOWN;
	}

	if (link_changed)
		phy_print_status(phydev);
}

static int uio_clk_phase_rgmii_set(struct emac_priv *priv, bool is_tx)
{
	u32 val;

	switch (priv->clk_tuning_way) {
	case UIO_CLK_TUNING_BY_REG:
		val = readl(priv->ctrl_reg);
		if (is_tx) {
			val &= ~RGMII_TX_PHASE_MASK;
			val |= (priv->tx_clk_phase & 0x7) << RGMII_TX_PHASE_OFFSET;
		} else {
			val &= ~RGMII_RX_PHASE_MASK;
			val |= (priv->rx_clk_phase & 0x7) << RGMII_RX_PHASE_OFFSET;
		}
		writel(val, priv->ctrl_reg);
		break;
	case UIO_CLK_TUNING_BY_DLINE:
		val = readl(priv->dline_reg);
		if (is_tx) {
			val &= ~EMAC_TX_DLINE_CODE_MASK;
			val |= priv->tx_clk_phase << EMAC_TX_DLINE_CODE_OFFSET;
			val |= EMAC_TX_DLINE_EN;
		} else {
			val &= ~EMAC_RX_DLINE_CODE_MASK;
			val |= priv->rx_clk_phase << EMAC_RX_DLINE_CODE_OFFSET;
			val |= EMAC_RX_DLINE_EN;
		}
		writel(val, priv->dline_reg);
		break;
	default:
		pr_err("wrong clk tuning way:%d !!\n", priv->clk_tuning_way);
		return -1;
	}
	pr_debug("%s tx phase:%d rx phase:%d\n",
		 __func__, priv->tx_clk_phase, priv->rx_clk_phase);
	return 0;
}

static int uio_clk_phase_rmii_set(struct emac_priv *priv, bool is_tx)
{
	u32 val;

	switch (priv->clk_tuning_way) {
	case UIO_CLK_TUNING_BY_REG:
		val = readl(priv->ctrl_reg);
		if (is_tx) {
			val &= ~RMII_TX_PHASE_MASK;
			val |= (priv->tx_clk_phase & 0x7) << RMII_TX_PHASE_OFFSET;
		} else {
			val &= ~RMII_RX_PHASE_MASK;
			val |= (priv->rx_clk_phase & 0x7) << RMII_RX_PHASE_OFFSET;
		}
		writel(val, priv->ctrl_reg);
		break;
	case UIO_CLK_TUNING_BY_CLK_REVERT:
		val = readl(priv->ctrl_reg);
		if (is_tx) {
			if (priv->tx_clk_phase == UIO_CLK_PHASE_REVERT)
				val |= RMII_TX_CLK_SEL;
			else
				val &= ~RMII_TX_CLK_SEL;
		} else {
			if (priv->rx_clk_phase == UIO_CLK_PHASE_REVERT)
				val |= RMII_RX_CLK_SEL;
			else
				val &= ~RMII_RX_CLK_SEL;
		}
		writel(val, priv->ctrl_reg);
		break;
	default:
		pr_err("wrong clk tuning way:%d !!\n", priv->clk_tuning_way);
		return -1;
	}
	pr_debug("%s tx phase:%d rx phase:%d\n",
		 __func__, priv->tx_clk_phase, priv->rx_clk_phase);
	return 0;
}

static int uio_clk_phase_set(struct emac_priv *priv, bool is_tx)
{
	if (priv->clk_tuning_enable) {
		if (uio_emac_is_rmii(priv))
			uio_clk_phase_rmii_set(priv, is_tx);
		else
			uio_clk_phase_rgmii_set(priv, is_tx);
	}

	return 0;
}

/**
 * k1x_emac_uio_phy_connect - PHY initialization
 * @dev: net device structure
 * Description: it initializes the driver's PHY state, and attaches the PHY
 * to the mac driver.
 *  Return value:
 *  0 on success
 */
static int k1x_emac_uio_phy_connect(struct net_device *dev)
{
	struct phy_device *phydev;
	struct device_node *np;
	struct emac_priv *priv = netdev_priv(dev);

	np = of_parse_phandle(priv->pdev->dev.of_node, "phy-handle", 0);
	if (!np && of_phy_is_fixed_link(priv->pdev->dev.of_node))
		np = of_node_get(priv->pdev->dev.of_node);
	if (!np)
		return -ENODEV;

	of_get_phy_mode(np, &priv->phy_interface);
	pr_info("priv phy_interface = %d\n", priv->phy_interface);

	uio_phy_interface_config(priv);

	phydev = of_phy_connect(dev, np,
				&uio_emac_adjust_link, 0, priv->phy_interface);
	if (IS_ERR_OR_NULL(phydev)) {
		pr_err("Could not attach to PHY\n");
		if (!phydev)
			return -ENODEV;
		return PTR_ERR(phydev);
	}

	pr_info("%s:  %s: attached to PHY (UID 0x%x) Link = %d\n",
		__func__, dev->name, phydev->phy_id, phydev->link);

	/* Indicate that the MAC is responsible for PHY PM */
	phydev->mac_managed_pm = true;
	dev->phydev = phydev;

	uio_clk_phase_set(priv, UIO_TX_PHASE);
	uio_clk_phase_set(priv, UIO_RX_PHASE);
	return 0;
}

void uio_enable_axi_single_id_mode(struct emac_priv *priv, int en)
{
	u32 val;

	val = readl(priv->ctrl_reg);
	if (en)
		val |= AXI_SINGLE_ID;
	else
		val &= ~AXI_SINGLE_ID;
	writel(val, priv->ctrl_reg);
}

static int k1x_emac_uio_up(struct emac_priv *priv)
{
	struct net_device *ndev = priv->ndev;
	int ret;

	ret = k1x_emac_uio_phy_connect(ndev);
	if (ret) {
		pr_err("%s  phy_connet failed\n", __func__);
		goto err;
	}

	/* enable axi mode */
	uio_enable_axi_single_id_mode(priv, 1);

	/* emac hw initiation in the dpdk driver */

	if (ndev->phydev)
		phy_start(ndev->phydev);

	return 0;

err:
	return ret;
}

/* Name		k1x_emac_uio_down
 * Arguments	priv : pointer to driver private data structure
 * Return	Status: 0 - Success;  non-zero - Fail
 * Description	This function is called from emac_close and
 *		performs the things when net interface is about to down.
 *		It frees the irq, removes the various timers.
 *		It sets the net interface off and
 *		resets the hardware. Cleans the Tx and Rx
 *		ring descriptor.
 */
int k1x_emac_uio_down(struct emac_priv *priv)
{
	struct net_device *ndev = priv->ndev;

	/* Stop and disconnect the PHY */
	if (ndev->phydev) {
		phy_stop(ndev->phydev);
		phy_disconnect(ndev->phydev);
	}

	priv->link = false;
	priv->duplex = DUPLEX_UNKNOWN;
	priv->speed = SPEED_UNKNOWN;

	return 0;
}

/**
 *  uio_open - open entry point of the driver
 *  @dev : pointer to the device structure.
 *  Description:
 *  This function is the open entry point of the driver.
 *  Return value:
 *  0 on success and an appropriate (-)ve integer as defined in errno.h
 *  file on failure.
 */
static int uio_open(struct net_device *ndev)
{
	struct emac_priv *priv = netdev_priv(ndev);
	int ret;

	priv->tx_ring.total_cnt = EMAC_UIO_TX_RING_NUM;
	priv->rx_ring.total_cnt = EMAC_UIO_RX_RING_NUM;

	ret = k1x_emac_uio_alloc_tx_resources(priv);
	if (ret) {
		pr_err("Error in setting up the Tx resources\n");
		goto emac_alloc_tx_resource_fail;
	}

	ret = k1x_emac_uio_alloc_rx_resources(priv);
	if (ret) {
		pr_err("Error in setting up the Rx resources\n");
		goto emac_alloc_rx_resource_fail;
	}

	ret = k1x_emac_uio_up(priv);
	if (ret) {
		pr_err("Error in making the net intrface up\n");
		goto emac_up_fail;
	}
	return 0;

emac_up_fail:
	k1x_emac_uio_free_rx_resources(priv);
emac_alloc_rx_resource_fail:
	k1x_emac_uio_free_tx_resources(priv);
emac_alloc_tx_resource_fail:
	return ret;
}

/**
 *  uio_release - close entry point of the driver
 *  @dev : device pointer.
 *  Description:
 *  This is the stop entry point of the driver.
 */
static int uio_release(struct net_device *ndev)
{
	struct emac_priv *priv = netdev_priv(ndev);

	dev_info(&ndev->dev, "Closing the EMAC interface\n");

	k1x_emac_uio_down(priv);
	k1x_emac_uio_free_tx_resources(priv);
	k1x_emac_uio_free_rx_resources(priv);

	return 0;
}

/**
 * k1x_emac_uio_probe() platform driver probe routine
 * - register uio devices filled with memory maps retrieved
 * from device tree
 */
static int k1x_emac_uio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node, *mac_node;
	struct k1x_emac_uio_pdev_info *pdev_info;
	struct net_device *netdev;
	struct emac_priv *priv;
	struct uio_info *uio;
	struct resource *res;
	int err = 0;

	pdev_info = devm_kzalloc(dev, sizeof(struct k1x_emac_uio_pdev_info),
				 GFP_KERNEL);
	if (!pdev_info)
		return -ENOMEM;

	uio = &pdev_info->uio;
	pdev_info->dev = dev;
	mac_node = of_parse_phandle(np, "k1x,ethernet", 0);
	if (!mac_node)
		return -ENODEV;

	if (of_device_is_available(mac_node)) {
		netdev = of_find_net_device_by_node(mac_node);
		of_node_put(mac_node);
		if (!netdev)
			return -ENODEV;
	} else {
		of_node_put(mac_node);
		return -EINVAL;
	}

	pdev_info->ndev = netdev;
	rtnl_lock();
	dev_close(netdev);
	rtnl_unlock();

	rtnl_lock();
	err = uio_open(netdev);
	if (err) {
		rtnl_unlock();
		dev_err(dev, "Failed to open k1 emac resource: %d\n", err);
		return err;
	}
	rtnl_unlock();

	priv = netdev_priv(netdev);
	snprintf(pdev_info->name, sizeof(pdev_info->name), "%s",
		 K1XMAC_UIO_DEVICE_NAME);
	uio->name = pdev_info->name;
	uio->version = DRIVER_VERSION;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;

	uio->mem[0].name = "eth_regs";
	uio->mem[0].addr = res->start & PAGE_MASK;
	uio->mem[0].size = PAGE_ALIGN(resource_size(res));
	uio->mem[0].memtype = UIO_MEM_PHYS;

	uio->mem[1].name = "eth_rx_bd";
	uio->mem[1].addr = priv->rx_ring.desc_dma_addr;
	uio->mem[1].size = priv->rx_ring.total_cnt * sizeof(struct emac_rx_desc);
	uio->mem[1].memtype = UIO_MEM_PHYS;

	uio->mem[2].name = "eth_tx_bd";
	uio->mem[2].addr = priv->tx_ring.desc_dma_addr;
	uio->mem[2].size = priv->tx_ring.total_cnt * sizeof(struct emac_tx_desc);
	uio->mem[2].memtype = UIO_MEM_PHYS;

	uio->open = k1x_emac_uio_open;
	uio->release = k1x_emac_uio_release;
	/* Custom mmap function. */
	uio->mmap = k1x_emac_uio_mmap;
	uio->priv = pdev_info;

	err = uio_register_device(dev, uio);
	if (err) {
		dev_err(dev, "Failed to register uio device: %d\n", err);
		return err;
	}

	pdev_info->map_num = 3;

	dev_info(dev, "Registered %s uio devices, %d register maps attached\n",
		 pdev_info->name, pdev_info->map_num);

	platform_set_drvdata(pdev, pdev_info);

	return 0;
}

/**
 * k1x_emac_uio_remove() - Spacemit k1 ETH UIO platform driver release
 * routine - unregister uio devices
 */
static int k1x_emac_uio_remove(struct platform_device *pdev)
{
	struct k1x_emac_uio_pdev_info *pdev_info =
					platform_get_drvdata(pdev);
	struct net_device *netdev;

	if (!pdev_info)
		return -EINVAL;

	netdev = pdev_info->ndev;

	uio_unregister_device(&pdev_info->uio);

	if (netdev) {
		rtnl_lock();
		uio_release(netdev);
		rtnl_unlock();
	}

	platform_set_drvdata(pdev, NULL);

	if (netdev) {
		rtnl_lock();
		dev_open(netdev, NULL);
		rtnl_unlock();
	}

	return 0;
}

static const struct of_device_id k1x_emac_uio_of_match[] = {
	{ .compatible	= "k1x,uio-emac", },
	{ }
};

static struct platform_driver k1x_emac_uio_driver = {
	.driver = {
		.owner		= THIS_MODULE,
		.name		= DRIVER_NAME,
		.of_match_table	= k1x_emac_uio_of_match,
	},
	.probe	= k1x_emac_uio_probe,
	.remove	= k1x_emac_uio_remove,
};

module_platform_driver(k1x_emac_uio_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Spacemit emac uio driver");
MODULE_AUTHOR("Spacemit");
