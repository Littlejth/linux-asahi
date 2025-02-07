// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2021 Alyssa Rosenzweig <alyssa@rosenzweig.io> */

#include <linux/bitmap.h>
#include <linux/clk.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/of_device.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/iommu.h>
#include <linux/align.h>
#include <linux/apple-mailbox.h>
#include <linux/soc/apple/rtkit.h>
#include <linux/completion.h>
#include "linux/workqueue.h"

#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

#include "dcp.h"
#include "dcp-internal.h"
#include "iomfb.h"
#include "parser.h"
#include "trace.h"

#define APPLE_DCP_COPROC_CPU_CONTROL	 0x44
#define APPLE_DCP_COPROC_CPU_CONTROL_RUN BIT(4)

#define DCP_BOOT_TIMEOUT msecs_to_jiffies(1000)

/* HACK: moved here to avoid circular dependency between apple_drv and dcp */
void dcp_drm_crtc_vblank(struct apple_crtc *crtc)
{
	unsigned long flags;

	spin_lock_irqsave(&crtc->base.dev->event_lock, flags);
	if (crtc->event) {
		drm_crtc_send_vblank_event(&crtc->base, crtc->event);
		crtc->event = NULL;
	}
	spin_unlock_irqrestore(&crtc->base.dev->event_lock, flags);
}

void dcp_set_dimensions(struct apple_dcp *dcp)
{
	int i;

	/* Set the connector info */
	if (dcp->connector) {
		struct drm_connector *connector = &dcp->connector->base;

		mutex_lock(&connector->dev->mode_config.mutex);
		connector->display_info.width_mm = dcp->width_mm;
		connector->display_info.height_mm = dcp->height_mm;
		mutex_unlock(&connector->dev->mode_config.mutex);
	}

	/*
	 * Fix up any probed modes. Modes are created when parsing
	 * TimingElements, dimensions are calculated when parsing
	 * DisplayAttributes, and TimingElements may be sent first
	 */
	for (i = 0; i < dcp->nr_modes; ++i) {
		dcp->modes[i].mode.width_mm = dcp->width_mm;
		dcp->modes[i].mode.height_mm = dcp->height_mm;
	}
}

/*
 * Helper to send a DRM vblank event. We do not know how call swap_submit_dcp
 * without surfaces. To avoid timeouts in drm_atomic_helper_wait_for_vblanks
 * send a vblank event via a workqueue.
 */
static void dcp_delayed_vblank(struct work_struct *work)
{
	struct apple_dcp *dcp;

	dcp = container_of(work, struct apple_dcp, vblank_wq);
	mdelay(5);
	dcp_drm_crtc_vblank(dcp->crtc);
}

static void dcp_recv_msg(void *cookie, u8 endpoint, u64 message)
{
	struct apple_dcp *dcp = cookie;

	trace_dcp_recv_msg(dcp, endpoint, message);

	switch (endpoint) {
	case IOMFB_ENDPOINT:
		return iomfb_recv_msg(dcp, message);
	default:
		WARN(endpoint, "unknown DCP endpoint %hhu", endpoint);
	}
}

static void dcp_rtk_crashed(void *cookie)
{
	struct apple_dcp *dcp = cookie;

	dcp->crashed = true;
	dev_err(dcp->dev, "DCP has crashed");
	if (dcp->connector) {
		dcp->connector->connected = 0;
		schedule_work(&dcp->connector->hotplug_wq);
	}
}

static int dcp_rtk_shmem_setup(void *cookie, struct apple_rtkit_shmem *bfr)
{
	struct apple_dcp *dcp = cookie;

	if (bfr->iova) {
		struct iommu_domain *domain =
			iommu_get_domain_for_dev(dcp->dev);
		phys_addr_t phy_addr;

		if (!domain)
			return -ENOMEM;

		// TODO: get map from device-tree
		phy_addr = iommu_iova_to_phys(domain,
					      bfr->iova & ~dcp->asc_dram_mask);
		if (!phy_addr)
			return -ENOMEM;

		// TODO: verify phy_addr, cache attribute
		bfr->buffer = memremap(phy_addr, bfr->size, MEMREMAP_WB);
		if (!bfr->buffer)
			return -ENOMEM;

		bfr->is_mapped = true;
		dev_info(dcp->dev,
			 "shmem_setup: iova: %lx -> pa: %lx -> iomem: %lx",
			 (uintptr_t)bfr->iova, (uintptr_t)phy_addr,
			 (uintptr_t)bfr->buffer);
	} else {
		bfr->buffer = dma_alloc_coherent(dcp->dev, bfr->size,
						 &bfr->iova, GFP_KERNEL);
		if (!bfr->buffer)
			return -ENOMEM;

		bfr->iova |= dcp->asc_dram_mask;

		dev_info(dcp->dev, "shmem_setup: iova: %lx, buffer: %lx",
			 (uintptr_t)bfr->iova, (uintptr_t)bfr->buffer);
	}

	return 0;
}

static void dcp_rtk_shmem_destroy(void *cookie, struct apple_rtkit_shmem *bfr)
{
	struct apple_dcp *dcp = cookie;

	if (bfr->is_mapped)
		memunmap(bfr->buffer);
	else
		dma_free_coherent(dcp->dev, bfr->size, bfr->buffer,
				  bfr->iova & ~dcp->asc_dram_mask);
}

static struct apple_rtkit_ops rtkit_ops = {
	.crashed = dcp_rtk_crashed,
	.recv_message = dcp_recv_msg,
	.shmem_setup = dcp_rtk_shmem_setup,
	.shmem_destroy = dcp_rtk_shmem_destroy,
};

void dcp_send_message(struct apple_dcp *dcp, u8 endpoint, u64 message)
{
	trace_dcp_send_msg(dcp, endpoint, message);
	apple_rtkit_send_message(dcp->rtk, endpoint, message, NULL,
				 false);
}

int dcp_crtc_atomic_check(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	struct platform_device *pdev = to_apple_crtc(crtc)->dcp;
	struct apple_dcp *dcp = platform_get_drvdata(pdev);
	struct drm_plane_state *new_state, *old_state;
	struct drm_plane *plane;
	struct drm_crtc_state *crtc_state;
	int plane_idx, plane_count = 0;
	bool needs_modeset;

	if (dcp->crashed)
		return -EINVAL;

	crtc_state = drm_atomic_get_new_crtc_state(state, crtc);

	needs_modeset = drm_atomic_crtc_needs_modeset(crtc_state) || !dcp->valid_mode;
	if (!needs_modeset && !dcp->connector->connected) {
		dev_err(dcp->dev, "crtc_atomic_check: disconnected but no modeset");
		return -EINVAL;
	}

	for_each_oldnew_plane_in_state(state, plane, old_state, new_state, plane_idx) {
		/* skip planes not for this crtc */
		if (new_state->crtc != crtc)
			continue;

		plane_count += 1;
	}

	if (plane_count > DCP_MAX_PLANES) {
		dev_err(dcp->dev, "crtc_atomic_check: Blend supports only 2 layers!");
		return -EINVAL;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(dcp_crtc_atomic_check);

int dcp_get_connector_type(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	return (dcp->connector_type);
}
EXPORT_SYMBOL_GPL(dcp_get_connector_type);

void dcp_link(struct platform_device *pdev, struct apple_crtc *crtc,
	      struct apple_connector *connector)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	dcp->crtc = crtc;
	dcp->connector = connector;
}
EXPORT_SYMBOL_GPL(dcp_link);

int dcp_start(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);
	int ret;

	/* start RTKit endpoints */
	ret = iomfb_start_rtkit(dcp);
	if (ret)
		dev_err(dcp->dev, "Failed to start IOMFB endpoint: %d", ret);

	return ret;
}
EXPORT_SYMBOL(dcp_start);

static void dcp_work_register_backlight(struct work_struct *work)
{
	int ret;
	struct apple_dcp *dcp;

	dcp = container_of(work, struct apple_dcp, bl_register_wq);

	mutex_lock(&dcp->bl_register_mutex);
	if (dcp->brightness.bl_dev)
		goto out_unlock;

	/* try to register backlight device, */
	ret = dcp_backlight_register(dcp);
	if (ret) {
		dev_err(dcp->dev, "Unable to register backlight device\n");
		dcp->brightness.maximum = 0;
	}

out_unlock:
	mutex_unlock(&dcp->bl_register_mutex);
}

static struct platform_device *dcp_get_dev(struct device *dev, const char *name)
{
	struct platform_device *pdev;
	struct device_node *node = of_parse_phandle(dev->of_node, name, 0);

	if (!node)
		return NULL;

	pdev = of_find_device_by_node(node);
	of_node_put(node);
	return pdev;
}

static int dcp_get_disp_regs(struct apple_dcp *dcp)
{
	struct platform_device *pdev = to_platform_device(dcp->dev);
	int count = pdev->num_resources - 1;
	int i;

	if (count <= 0 || count > MAX_DISP_REGISTERS)
		return -EINVAL;

	for (i = 0; i < count; ++i) {
		dcp->disp_registers[i] =
			platform_get_resource(pdev, IORESOURCE_MEM, 1 + i);
	}

	dcp->nr_disp_registers = count;
	return 0;
}

static enum dcp_firmware_version dcp_get_firmware_version(struct device *dev,
							  const char *name)
{
	u32 fw_version[3];
	int ret;

	ret = of_property_read_u32_array(dev->of_node, name, fw_version, 3);
	if (ret != 0) {
		dev_warn(dev, "Could not read '%s': %d\n", name, ret);
		return DCP_FIRMWARE_UNKNOWN;
	}

	if (fw_version[0] == 12) {
		switch (fw_version[1]) {
		case 3:
			return DCP_FIRMWARE_V_12_3;
		case 4:
			return DCP_FIRMWARE_V_12_4;
		default:
			break;
		}
	}

	dev_err(dev, "'%s' %u.%u.%u is not supported\n", name, fw_version[0],
		fw_version[1], fw_version[2]);

	return DCP_FIRMWARE_UNKNOWN;
}

static int dcp_platform_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *panel_np;
	struct apple_dcp *dcp;
	enum dcp_firmware_version fw_version, fw_compat;
	u32 cpu_ctrl;
	int ret;

	fw_version = dcp_get_firmware_version(dev, "apple,firmware-version");
	if (fw_version == DCP_FIRMWARE_UNKNOWN)
		return -ENODEV;

	fw_compat = dcp_get_firmware_version(dev, "apple,firmware-compat");
	if (fw_compat == DCP_FIRMWARE_UNKNOWN)
		return -ENODEV;

	dcp = devm_kzalloc(dev, sizeof(*dcp), GFP_KERNEL);
	if (!dcp)
		return -ENOMEM;

	dcp->fw_version = fw_version;
	dcp->fw_compat = fw_compat;

	platform_set_drvdata(pdev, dcp);
	dcp->dev = dev;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(36));
	if (ret)
		return ret;

	dcp->coproc_reg = devm_platform_ioremap_resource_byname(pdev, "coproc");
	if (IS_ERR(dcp->coproc_reg))
		return PTR_ERR(dcp->coproc_reg);

	of_platform_default_populate(dev->of_node, NULL, dev);

	ret = of_property_read_u32(dev->of_node, "apple,notch-height",
				   &dcp->notch_height);
	if (dcp->notch_height > MAX_NOTCH_HEIGHT)
		dcp->notch_height = MAX_NOTCH_HEIGHT;
	if (dcp->notch_height > 0)
		dev_info(dev, "Detected display with notch of %u pixel\n", dcp->notch_height);

	/* intialize brightness scale to a sensible default to avoid divide by 0*/
	dcp->brightness.scale = 65536;
	panel_np = of_get_compatible_child(dev->of_node, "apple,panel-mini-led");
	if (panel_np)
		dcp->has_mini_led = true;
	else
		panel_np = of_get_compatible_child(dev->of_node, "apple,panel");

	if (panel_np) {
		const char height_prop[2][16] = { "adj-height-mm", "height-mm" };

		if (of_device_is_available(panel_np)) {
			ret = of_property_read_u32(panel_np, "apple,max-brightness",
						   &dcp->brightness.maximum);
			if (ret)
				dev_err(dev, "Missing property 'apple,max-brightness'\n");
		}

		of_property_read_u32(panel_np, "width-mm", &dcp->width_mm);
		/* use adjusted height as long as the notch is hidden */
		of_property_read_u32(panel_np, height_prop[!dcp->notch_height],
				     &dcp->height_mm);

		of_node_put(panel_np);
		dcp->connector_type = DRM_MODE_CONNECTOR_eDP;
		INIT_WORK(&dcp->bl_register_wq, dcp_work_register_backlight);
		mutex_init(&dcp->bl_register_mutex);
	} else if (of_property_match_string(dev->of_node, "apple,connector-type", "HDMI-A") >= 0)
		dcp->connector_type = DRM_MODE_CONNECTOR_HDMIA;
	else if (of_property_match_string(dev->of_node, "apple,connector-type", "USB-C") >= 0)
		dcp->connector_type = DRM_MODE_CONNECTOR_USB;
	else
		dcp->connector_type = DRM_MODE_CONNECTOR_Unknown;

	dcp->piodma = dcp_get_dev(dev, "apple,piodma-mapper");
	if (!dcp->piodma) {
		dev_err(dev, "failed to find piodma\n");
		return -ENODEV;
	}

	dcp->piodma_link = device_link_add(dev, &dcp->piodma->dev,
					   DL_FLAG_AUTOREMOVE_CONSUMER);
	if (!dcp->piodma_link) {
		dev_err(dev, "Failed to link to piodma device");
		return -EINVAL;
	}

	if (dcp->piodma_link->supplier->links.status != DL_DEV_DRIVER_BOUND)
		return -EPROBE_DEFER;

	ret = dcp_get_disp_regs(dcp);
	if (ret) {
		dev_err(dev, "failed to find display registers\n");
		return ret;
	}

	dcp->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(dcp->clk))
		return dev_err_probe(dev, PTR_ERR(dcp->clk),
				     "Unable to find clock\n");

	ret = of_property_read_u64(dev->of_node, "apple,asc-dram-mask",
				   &dcp->asc_dram_mask);
	if (ret)
		dev_warn(dev, "failed read 'apple,asc-dram-mask': %d\n", ret);
	dev_dbg(dev, "'apple,asc-dram-mask': 0x%011llx\n", dcp->asc_dram_mask);

	bitmap_zero(dcp->memdesc_map, DCP_MAX_MAPPINGS);
	// TDOD: mem_desc IDs start at 1, for simplicity just skip '0' entry
	set_bit(0, dcp->memdesc_map);

	INIT_WORK(&dcp->vblank_wq, dcp_delayed_vblank);

	dcp->swapped_out_fbs =
		(struct list_head)LIST_HEAD_INIT(dcp->swapped_out_fbs);

	cpu_ctrl =
		readl_relaxed(dcp->coproc_reg + APPLE_DCP_COPROC_CPU_CONTROL);
	writel_relaxed(cpu_ctrl | APPLE_DCP_COPROC_CPU_CONTROL_RUN,
		       dcp->coproc_reg + APPLE_DCP_COPROC_CPU_CONTROL);

	dcp->rtk = devm_apple_rtkit_init(dev, dcp, "mbox", 0, &rtkit_ops);
	if (IS_ERR(dcp->rtk))
		return dev_err_probe(dev, PTR_ERR(dcp->rtk),
				     "Failed to intialize RTKit");

	ret = apple_rtkit_wake(dcp->rtk);
	if (ret)
		return dev_err_probe(dev, PTR_ERR(dcp->rtk),
				     "Failed to boot RTKit: %d", ret);

	return ret;
}

/*
 * We need to shutdown DCP before tearing down the display subsystem. Otherwise
 * the DCP will crash and briefly flash a green screen of death.
 */
static void dcp_platform_shutdown(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	if (dcp->shmem)
		iomfb_shutdown(dcp);
}

static const struct of_device_id of_match[] = {
	{ .compatible = "apple,dcp" },
	{}
};
MODULE_DEVICE_TABLE(of, of_match);

static struct platform_driver apple_platform_driver = {
	.probe		= dcp_platform_probe,
	.shutdown	= dcp_platform_shutdown,
	.driver	= {
		.name = "apple-dcp",
		.of_match_table	= of_match,
	},
};

module_platform_driver(apple_platform_driver);

MODULE_AUTHOR("Alyssa Rosenzweig <alyssa@rosenzweig.io>");
MODULE_DESCRIPTION("Apple Display Controller DRM driver");
MODULE_LICENSE("Dual MIT/GPL");
