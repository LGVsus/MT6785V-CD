// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2020 Linaro Ltd
 * Author: Sumit Semwal <sumit.semwal@linaro.org>
 *
 * This driver is for the DSI interface to panels using the NT36672A display driver IC
 * from Novatek.
 * Currently supported are the Tianma FHD+ panels found in some Xiaomi phones, including
 * some variants of the Poco F1 phone.
 *
 * Panels using the Novatek NT37762A IC should add appropriate configuration per-panel and
 * use this driver.
 */

#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>

#include <linux/gpio/consumer.h>
#include <linux/pinctrl/consumer.h>
#include <linux/regulator/consumer.h>

#include <drm/drm_device.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

#include <video/mipi_display.h>

struct nt36672a_panel_cmd {
	size_t len;
	const u8 *payload;
};

#define NT36672A_CMD(_len, ...) \
	{ .len = (_len), .payload = (const u8[]) { __VA_ARGS__ } }

static const char * const nt36672a_regulator_names[] = {
	"vddio",
	"vddpos",
	"vddneg",
};

static unsigned long const nt36672a_regulator_enable_loads[] = {
	62000,
	100000,
	100000
};

struct nt36672a_panel_reset_seq {
	int gpio_value;
	unsigned int sleep_msecs;
};

struct nt36672a_panel_desc {
	const struct drm_display_mode *display_mode;
	const char *panel_name;

	unsigned int width_mm;
	unsigned int height_mm;

	unsigned long mode_flags;
	enum mipi_dsi_pixel_format format;
	unsigned int lanes;

	unsigned int num_on_cmds_1;
	const struct nt36672a_panel_cmd *on_cmds_1;
	unsigned int num_on_cmds_2;
	const struct nt36672a_panel_cmd *on_cmds_2;

	unsigned int num_off_cmds;
	const struct nt36672a_panel_cmd *off_cmds;

	unsigned int num_reset_seq;
	const struct nt36672a_panel_reset_seq *reset_seq;
};

struct nt36672a_panel {
	struct drm_panel base;
	struct mipi_dsi_device *link;
	const struct nt36672a_panel_desc *desc;

	struct regulator_bulk_data supplies[ARRAY_SIZE(nt36672a_regulator_names)];

	struct gpio_desc *reset_gpio;
};

static inline struct nt36672a_panel *to_nt36672a_panel(struct drm_panel *panel)
{
	return container_of(panel, struct nt36672a_panel, base);
}

static void nt36672a_panel_power_off(struct drm_panel *panel)
{
	struct nt36672a_panel *pinfo = to_nt36672a_panel(panel);
	int ret;

	gpiod_set_value(pinfo->reset_gpio, 1);

	ret = regulator_bulk_disable(ARRAY_SIZE(pinfo->supplies), pinfo->supplies);
	if (ret)
		dev_err(panel->dev, "regulator_bulk_disable failed %d\n", ret);
}

static int nt36672a_panel_unprepare(struct drm_panel *panel)
{
	struct nt36672a_panel *pinfo = to_nt36672a_panel(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = pinfo->link };
	int i;

	/* send off cmds */
	for (i = 0; i < pinfo->desc->num_off_cmds; i++) {
		const struct nt36672a_panel_cmd *cmd = &pinfo->desc->off_cmds[i];
		mipi_dsi_dcs_write_buffer_multi(&dsi_ctx, cmd->payload, cmd->len);
	}

	/* Reset error to continue with display off even if send_cmds failed */
	dsi_ctx.accum_err = 0;
	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	/* Reset error to continue power-down even if display off failed */
	dsi_ctx.accum_err = 0;

	/* 120ms delay required here as per DCS spec */
	msleep(120);

	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);

	/* 0x3C = 60ms delay */
	msleep(60);

	nt36672a_panel_power_off(panel);

	return 0;
}

static int nt36672a_panel_power_on(struct nt36672a_panel *pinfo)
{
	const struct nt36672a_panel_reset_seq *reset_seq;
	int i;

	int ret = regulator_bulk_enable(ARRAY_SIZE(pinfo->supplies), pinfo->supplies);
	if (ret < 0)
		return ret;

	reset_seq = pinfo->desc->reset_seq;

	for (i = 0; i < pinfo->desc->num_reset_seq; i++) {
		gpiod_set_value(pinfo->reset_gpio, reset_seq[i].gpio_value);
		msleep(reset_seq[i].sleep_msecs);
	}

	return 0;
}

static int nt36672a_panel_prepare(struct drm_panel *panel)
{
	struct nt36672a_panel *pinfo = to_nt36672a_panel(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = pinfo->link };
	int i;

	dsi_ctx.accum_err = nt36672a_panel_power_on(pinfo);

	/* send first part of init cmds */
	for (i = 0; i < pinfo->desc->num_on_cmds_1; i++) {
		const struct nt36672a_panel_cmd *cmd = &pinfo->desc->on_cmds_1[i];
		mipi_dsi_dcs_write_buffer_multi(&dsi_ctx, cmd->payload, cmd->len);
	}

	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);

	/* 0x46 = 70 ms delay */
	mipi_dsi_msleep(&dsi_ctx, 70);

	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);

	/* Send rest of the init cmds */
	for (i = 0; i < pinfo->desc->num_on_cmds_2; i++) {
		const struct nt36672a_panel_cmd *cmd = &pinfo->desc->on_cmds_2[i];
		mipi_dsi_dcs_write_buffer_multi(&dsi_ctx, cmd->payload, cmd->len);
	}

	mipi_dsi_msleep(&dsi_ctx, 120);

	if (dsi_ctx.accum_err < 0)
		gpiod_set_value(pinfo->reset_gpio, 0);

	return dsi_ctx.accum_err;
}

static int nt36672a_panel_get_modes(struct drm_panel *panel,
				    struct drm_connector *connector)
{
	struct nt36672a_panel *pinfo = to_nt36672a_panel(panel);
	const struct drm_display_mode *m = pinfo->desc->display_mode;
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, m);
	if (!mode) {
		dev_err(panel->dev, "failed to add mode %ux%u@%u\n", m->hdisplay,
			m->vdisplay, drm_mode_vrefresh(m));
		return -ENOMEM;
	}

	connector->display_info.width_mm = pinfo->desc->width_mm;
	connector->display_info.height_mm = pinfo->desc->height_mm;

	drm_mode_set_name(mode);
	drm_mode_probed_add(connector, mode);

	return 1;
}

static const struct drm_panel_funcs panel_funcs = {
	.unprepare = nt36672a_panel_unprepare,
	.prepare = nt36672a_panel_prepare,
	.get_modes = nt36672a_panel_get_modes,
};

static const struct nt36672a_panel_cmd tianma_fhd_video_on_cmds_1[] = {
	/* skin enhancement mode */
	NT36672A_CMD(2, 0xff, 0x22),
	NT36672A_CMD(2, 0x00, 0x40),
	NT36672A_CMD(2, 0x01, 0xc0),
	NT36672A_CMD(2, 0x02, 0x40),
	NT36672A_CMD(2, 0x03, 0x40),
	NT36672A_CMD(2, 0x04, 0x40),
	NT36672A_CMD(2, 0x05, 0x40),
	NT36672A_CMD(2, 0x06, 0x40),
	NT36672A_CMD(2, 0x07, 0x40),
	NT36672A_CMD(2, 0x08, 0x40),
	NT36672A_CMD(2, 0x09, 0x40),
	NT36672A_CMD(2, 0x0a, 0x40),
	NT36672A_CMD(2, 0x0b, 0x40),
	NT36672A_CMD(2, 0x0c, 0x40),
	NT36672A_CMD(2, 0x0d, 0x40),
	NT36672A_CMD(2, 0x0e, 0x40),
	NT36672A_CMD(2, 0x0f, 0x40),
	NT36672A_CMD(2, 0x10, 0x40),
	NT36672A_CMD(2, 0x11, 0x50),
	NT36672A_CMD(2, 0x12, 0x60),
	NT36672A_CMD(2, 0x13, 0x70),
	NT36672A_CMD(2, 0x14, 0x58),
	NT36672A_CMD(2, 0x15, 0x68),
	NT36672A_CMD(2, 0x16, 0x78),
	NT36672A_CMD(2, 0x17, 0x77),
	NT36672A_CMD(2, 0x18, 0x39),
	NT36672A_CMD(2, 0x19, 0x2d),
	NT36672A_CMD(2, 0x1a, 0x2e),
	NT36672A_CMD(2, 0x1b, 0x32),
	NT36672A_CMD(2, 0x1c, 0x37),
	NT36672A_CMD(2, 0x1d, 0x3a),
	NT36672A_CMD(2, 0x1e, 0x40),
	NT36672A_CMD(2, 0x1f, 0x40),
	NT36672A_CMD(2, 0x20, 0x40),
	NT36672A_CMD(2, 0x21, 0x40),
	NT36672A_CMD(2, 0x22, 0x40),
	NT36672A_CMD(2, 0x23, 0x40),
	NT36672A_CMD(2, 0x24, 0x40),
	NT36672A_CMD(2, 0x25, 0x40),
	NT36672A_CMD(2, 0x26, 0x40),
	NT36672A_CMD(2, 0x27, 0x40),
	NT36672A_CMD(2, 0x28, 0x40),
	NT36672A_CMD(2, 0x2d, 0x00),
	NT36672A_CMD(2, 0x2f, 0x40),
	NT36672A_CMD(2, 0x30, 0x40),
	NT36672A_CMD(2, 0x31, 0x40),
	NT36672A_CMD(2, 0x32, 0x40),
	NT36672A_CMD(2, 0x33, 0x40),
	NT36672A_CMD(2, 0x34, 0x40),
	NT36672A_CMD(2, 0x35, 0x40),
	NT36672A_CMD(2, 0x36, 0x40),
	NT36672A_CMD(2, 0x37, 0x40),
	NT36672A_CMD(2, 0x38, 0x40),
	NT36672A_CMD(2, 0x39, 0x40),
	NT36672A_CMD(2, 0x3a, 0x40),
	NT36672A_CMD(2, 0x3b, 0x40),
	NT36672A_CMD(2, 0x3d, 0x40),
	NT36672A_CMD(2, 0x3f, 0x40),
	NT36672A_CMD(2, 0x40, 0x40),
	NT36672A_CMD(2, 0x41, 0x40),
	NT36672A_CMD(2, 0x42, 0x40),
	NT36672A_CMD(2, 0x43, 0x40),
	NT36672A_CMD(2, 0x44, 0x40),
	NT36672A_CMD(2, 0x45, 0x40),
	NT36672A_CMD(2, 0x46, 0x40),
	NT36672A_CMD(2, 0x47, 0x40),
	NT36672A_CMD(2, 0x48, 0x40),
	NT36672A_CMD(2, 0x49, 0x40),
	NT36672A_CMD(2, 0x4a, 0x40),
	NT36672A_CMD(2, 0x4b, 0x40),
	NT36672A_CMD(2, 0x4c, 0x40),
	NT36672A_CMD(2, 0x4d, 0x40),
	NT36672A_CMD(2, 0x4e, 0x40),
	NT36672A_CMD(2, 0x4f, 0x40),
	NT36672A_CMD(2, 0x50, 0x40),
	NT36672A_CMD(2, 0x51, 0x40),
	NT36672A_CMD(2, 0x52, 0x40),
	NT36672A_CMD(2, 0x53, 0x01),
	NT36672A_CMD(2, 0x54, 0x01),
	NT36672A_CMD(2, 0x55, 0xfe),
	NT36672A_CMD(2, 0x56, 0x77),
	NT36672A_CMD(2, 0x58, 0xcd),
	NT36672A_CMD(2, 0x59, 0xd0),
	NT36672A_CMD(2, 0x5a, 0xd0),
	NT36672A_CMD(2, 0x5b, 0x50),
	NT36672A_CMD(2, 0x5c, 0x50),
	NT36672A_CMD(2, 0x5d, 0x50),
	NT36672A_CMD(2, 0x5e, 0x50),
	NT36672A_CMD(2, 0x5f, 0x50),
	NT36672A_CMD(2, 0x60, 0x50),
	NT36672A_CMD(2, 0x61, 0x50),
	NT36672A_CMD(2, 0x62, 0x50),
	NT36672A_CMD(2, 0x63, 0x50),
	NT36672A_CMD(2, 0x64, 0x50),
	NT36672A_CMD(2, 0x65, 0x50),
	NT36672A_CMD(2, 0x66, 0x50),
	NT36672A_CMD(2, 0x67, 0x50),
	NT36672A_CMD(2, 0x68, 0x50),
	NT36672A_CMD(2, 0x69, 0x50),
	NT36672A_CMD(2, 0x6a, 0x50),
	NT36672A_CMD(2, 0x6b, 0x50),
	NT36672A_CMD(2, 0x6c, 0x50),
	NT36672A_CMD(2, 0x6d, 0x50),
	NT36672A_CMD(2, 0x6e, 0x50),
	NT36672A_CMD(2, 0x6f, 0x50),
	NT36672A_CMD(2, 0x70, 0x07),
	NT36672A_CMD(2, 0x71, 0x00),
	NT36672A_CMD(2, 0x72, 0x00),
	NT36672A_CMD(2, 0x73, 0x00),
	NT36672A_CMD(2, 0x74, 0x06),
	NT36672A_CMD(2, 0x75, 0x0c),
	NT36672A_CMD(2, 0x76, 0x03),
	NT36672A_CMD(2, 0x77, 0x09),
	NT36672A_CMD(2, 0x78, 0x0f),
	NT36672A_CMD(2, 0x79, 0x68),
	NT36672A_CMD(2, 0x7a, 0x88),
	NT36672A_CMD(2, 0x7c, 0x80),
	NT36672A_CMD(2, 0x7d, 0x80),
	NT36672A_CMD(2, 0x7e, 0x80),
	NT36672A_CMD(2, 0x7f, 0x00),
	NT36672A_CMD(2, 0x80, 0x00),
	NT36672A_CMD(2, 0x81, 0x00),
	NT36672A_CMD(2, 0x83, 0x01),
	NT36672A_CMD(2, 0x84, 0x00),
	NT36672A_CMD(2, 0x85, 0x80),
	NT36672A_CMD(2, 0x86, 0x80),
	NT36672A_CMD(2, 0x87, 0x80),
	NT36672A_CMD(2, 0x88, 0x40),
	NT36672A_CMD(2, 0x89, 0x91),
	NT36672A_CMD(2, 0x8a, 0x98),
	NT36672A_CMD(2, 0x8b, 0x80),
	NT36672A_CMD(2, 0x8c, 0x80),
	NT36672A_CMD(2, 0x8d, 0x80),
	NT36672A_CMD(2, 0x8e, 0x80),
	NT36672A_CMD(2, 0x8f, 0x80),
	NT36672A_CMD(2, 0x90, 0x80),
	NT36672A_CMD(2, 0x91, 0x80),
	NT36672A_CMD(2, 0x92, 0x80),
	NT36672A_CMD(2, 0x93, 0x80),
	NT36672A_CMD(2, 0x94, 0x80),
	NT36672A_CMD(2, 0x95, 0x80),
	NT36672A_CMD(2, 0x96, 0x80),
	NT36672A_CMD(2, 0x97, 0x80),
	NT36672A_CMD(2, 0x98, 0x80),
	NT36672A_CMD(2, 0x99, 0x80),
	NT36672A_CMD(2, 0x9a, 0x80),
	NT36672A_CMD(2, 0x9b, 0x80),
	NT36672A_CMD(2, 0x9c, 0x80),
	NT36672A_CMD(2, 0x9d, 0x80),
	NT36672A_CMD(2, 0x9e, 0x80),
	NT36672A_CMD(2, 0x9f, 0x80),
	NT36672A_CMD(2, 0xa0, 0x8a),
	NT36672A_CMD(2, 0xa2, 0x80),
	NT36672A_CMD(2, 0xa6, 0x80),
	NT36672A_CMD(2, 0xa7, 0x80),
	NT36672A_CMD(2, 0xa9, 0x80),
	NT36672A_CMD(2, 0xaa, 0x80),
	NT36672A_CMD(2, 0xab, 0x80),
	NT36672A_CMD(2, 0xac, 0x80),
	NT36672A_CMD(2, 0xad, 0x80),
	NT36672A_CMD(2, 0xae, 0x80),
	NT36672A_CMD(2, 0xaf, 0x80),
	NT36672A_CMD(2, 0xb7, 0x76),
	NT36672A_CMD(2, 0xb8, 0x76),
	NT36672A_CMD(2, 0xb9, 0x05),
	NT36672A_CMD(2, 0xba, 0x0d),
	NT36672A_CMD(2, 0xbb, 0x14),
	NT36672A_CMD(2, 0xbc, 0x0f),
	NT36672A_CMD(2, 0xbd, 0x18),
	NT36672A_CMD(2, 0xbe, 0x1f),
	NT36672A_CMD(2, 0xbf, 0x05),
	NT36672A_CMD(2, 0xc0, 0x0d),
	NT36672A_CMD(2, 0xc1, 0x14),
	NT36672A_CMD(2, 0xc2, 0x03),
	NT36672A_CMD(2, 0xc3, 0x07),
	NT36672A_CMD(2, 0xc4, 0x0a),
	NT36672A_CMD(2, 0xc5, 0xa0),
	NT36672A_CMD(2, 0xc6, 0x55),
	NT36672A_CMD(2, 0xc7, 0xff),
	NT36672A_CMD(2, 0xc8, 0x39),
	NT36672A_CMD(2, 0xc9, 0x44),
	NT36672A_CMD(2, 0xca, 0x12),
	NT36672A_CMD(2, 0xcd, 0x80),
	NT36672A_CMD(2, 0xdb, 0x80),
	NT36672A_CMD(2, 0xdc, 0x80),
	NT36672A_CMD(2, 0xdd, 0x80),
	NT36672A_CMD(2, 0xe0, 0x80),
	NT36672A_CMD(2, 0xe1, 0x80),
	NT36672A_CMD(2, 0xe2, 0x80),
	NT36672A_CMD(2, 0xe3, 0x80),
	NT36672A_CMD(2, 0xe4, 0x80),
	NT36672A_CMD(2, 0xe5, 0x40),
	NT36672A_CMD(2, 0xe6, 0x40),
	NT36672A_CMD(2, 0xe7, 0x40),
	NT36672A_CMD(2, 0xe8, 0x40),
	NT36672A_CMD(2, 0xe9, 0x40),
	NT36672A_CMD(2, 0xea, 0x40),
	NT36672A_CMD(2, 0xeb, 0x40),
	NT36672A_CMD(2, 0xec, 0x40),
	NT36672A_CMD(2, 0xed, 0x40),
	NT36672A_CMD(2, 0xee, 0x40),
	NT36672A_CMD(2, 0xef, 0x40),
	NT36672A_CMD(2, 0xf0, 0x40),
	NT36672A_CMD(2, 0xf1, 0x40),
	NT36672A_CMD(2, 0xf2, 0x40),
	NT36672A_CMD(2, 0xf3, 0x40),
	NT36672A_CMD(2, 0xf4, 0x40),
	NT36672A_CMD(2, 0xf5, 0x40),
	NT36672A_CMD(2, 0xf6, 0x40),
	NT36672A_CMD(2, 0xfb, 0x01),
	NT36672A_CMD(2, 0xff, 0x23),
	NT36672A_CMD(2, 0xfb, 0x01),
	/* dimming enable */
	NT36672A_CMD(2, 0x01, 0x84),
	NT36672A_CMD(2, 0x05, 0x2d),
	NT36672A_CMD(2, 0x06, 0x00),
	/* resolution 1080*2246 */
	NT36672A_CMD(2, 0x11, 0x01),
	NT36672A_CMD(2, 0x12, 0x7b),
	NT36672A_CMD(2, 0x15, 0x6f),
	NT36672A_CMD(2, 0x16, 0x0b),
	/* UI mode */
	NT36672A_CMD(2, 0x29, 0x0a),
	NT36672A_CMD(2, 0x30, 0xff),
	NT36672A_CMD(2, 0x31, 0xff),
	NT36672A_CMD(2, 0x32, 0xff),
	NT36672A_CMD(2, 0x33, 0xff),
	NT36672A_CMD(2, 0x34, 0xff),
	NT36672A_CMD(2, 0x35, 0xff),
	NT36672A_CMD(2, 0x36, 0xff),
	NT36672A_CMD(2, 0x37, 0xff),
	NT36672A_CMD(2, 0x38, 0xfc),
	NT36672A_CMD(2, 0x39, 0xf8),
	NT36672A_CMD(2, 0x3a, 0xf4),
	NT36672A_CMD(2, 0x3b, 0xf1),
	NT36672A_CMD(2, 0x3d, 0xee),
	NT36672A_CMD(2, 0x3f, 0xeb),
	NT36672A_CMD(2, 0x40, 0xe8),
	NT36672A_CMD(2, 0x41, 0xe5),
	/* STILL mode */
	NT36672A_CMD(2, 0x2a, 0x13),
	NT36672A_CMD(2, 0x45, 0xff),
	NT36672A_CMD(2, 0x46, 0xff),
	NT36672A_CMD(2, 0x47, 0xff),
	NT36672A_CMD(2, 0x48, 0xff),
	NT36672A_CMD(2, 0x49, 0xff),
	NT36672A_CMD(2, 0x4a, 0xff),
	NT36672A_CMD(2, 0x4b, 0xff),
	NT36672A_CMD(2, 0x4c, 0xff),
	NT36672A_CMD(2, 0x4d, 0xed),
	NT36672A_CMD(2, 0x4e, 0xd5),
	NT36672A_CMD(2, 0x4f, 0xbf),
	NT36672A_CMD(2, 0x50, 0xa6),
	NT36672A_CMD(2, 0x51, 0x96),
	NT36672A_CMD(2, 0x52, 0x86),
	NT36672A_CMD(2, 0x53, 0x76),
	NT36672A_CMD(2, 0x54, 0x66),
	/* MOVING mode */
	NT36672A_CMD(2, 0x2b, 0x0e),
	NT36672A_CMD(2, 0x58, 0xff),
	NT36672A_CMD(2, 0x59, 0xff),
	NT36672A_CMD(2, 0x5a, 0xff),
	NT36672A_CMD(2, 0x5b, 0xff),
	NT36672A_CMD(2, 0x5c, 0xff),
	NT36672A_CMD(2, 0x5d, 0xff),
	NT36672A_CMD(2, 0x5e, 0xff),
	NT36672A_CMD(2, 0x5f, 0xff),
	NT36672A_CMD(2, 0x60, 0xf6),
	NT36672A_CMD(2, 0x61, 0xea),
	NT36672A_CMD(2, 0x62, 0xe1),
	NT36672A_CMD(2, 0x63, 0xd8),
	NT36672A_CMD(2, 0x64, 0xce),
	NT36672A_CMD(2, 0x65, 0xc3),
	NT36672A_CMD(2, 0x66, 0xba),
	NT36672A_CMD(2, 0x67, 0xb3),
	NT36672A_CMD(2, 0xff, 0x25),
	NT36672A_CMD(2, 0xfb, 0x01),
	NT36672A_CMD(2, 0x05, 0x04),
	NT36672A_CMD(2, 0xff, 0x26),
	NT36672A_CMD(2, 0xfb, 0x01),
	NT36672A_CMD(2, 0x1c, 0xaf),
	NT36672A_CMD(2, 0xff, 0x10),
	NT36672A_CMD(2, 0xfb, 0x01),
	NT36672A_CMD(2, 0x51, 0xff),
	NT36672A_CMD(2, 0x53, 0x24),
	NT36672A_CMD(2, 0x55, 0x00),
};

static const struct nt36672a_panel_cmd tianma_fhd_video_on_cmds_2[] = {
	NT36672A_CMD(2, 0xff, 0x24),
	NT36672A_CMD(2, 0xfb, 0x01),
	NT36672A_CMD(2, 0xc3, 0x01),
	NT36672A_CMD(2, 0xc4, 0x54),
	NT36672A_CMD(2, 0xff, 0x10),
};

static const struct nt36672a_panel_cmd tianma_fhd_video_off_cmds[] = {
	NT36672A_CMD(2, 0xff, 0x24),
	NT36672A_CMD(2, 0xfb, 0x01),
	NT36672A_CMD(2, 0xc3, 0x01),
	NT36672A_CMD(2, 0xff, 0x10),
};

/*
 * As per downstream kernel, Reset sequence of Tianma FHD panel requires the panel to
 * be out of reset for 10ms, followed by being held in reset for 10ms. But for Android
 * AOSP, we needed to bump it up to 200ms otherwise we get white screen sometimes.
 * FIXME: Try to reduce this 200ms to a lesser value.
 */
static const struct nt36672a_panel_reset_seq tianma_fhd_video_reset_seq[] = {
	{ .gpio_value = 1, .sleep_msecs = 200 },
	{ .gpio_value = 0, .sleep_msecs = 200 },
};

static const struct drm_display_mode tianma_fhd_video_panel_default_mode = {
	.clock		= 161331,

	.hdisplay	= 1080,
	.hsync_start	= 1080 + 40,
	.hsync_end	= 1080 + 40 + 20,
	.htotal		= 1080 + 40 + 20 + 44,

	.vdisplay	= 2246,
	.vsync_start	= 2246 + 15,
	.vsync_end	= 2246 + 15 + 2,
	.vtotal		= 2246 + 15 + 2 + 8,

	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

static const struct nt36672a_panel_desc tianma_fhd_video_panel_desc = {
	.display_mode = &tianma_fhd_video_panel_default_mode,

	.width_mm = 68,
	.height_mm = 136,

	.mode_flags = MIPI_DSI_MODE_LPM | MIPI_DSI_MODE_VIDEO
			| MIPI_DSI_MODE_VIDEO_HSE
			| MIPI_DSI_CLOCK_NON_CONTINUOUS
			| MIPI_DSI_MODE_VIDEO_BURST,
	.format = MIPI_DSI_FMT_RGB888,
	.lanes = 4,
	.on_cmds_1 = tianma_fhd_video_on_cmds_1,
	.num_on_cmds_1 = ARRAY_SIZE(tianma_fhd_video_on_cmds_1),
	.on_cmds_2 = tianma_fhd_video_on_cmds_2,
	.num_on_cmds_2 = ARRAY_SIZE(tianma_fhd_video_on_cmds_2),
	.off_cmds = tianma_fhd_video_off_cmds,
	.num_off_cmds = ARRAY_SIZE(tianma_fhd_video_off_cmds),
	.reset_seq = tianma_fhd_video_reset_seq,
	.num_reset_seq = ARRAY_SIZE(tianma_fhd_video_reset_seq),
};

static const struct nt36672a_panel_cmd begonia_nt36672a_video_on_cmds_1[] = {
	{ .data = {0xFF, 0x25} },
	{ .data = {0xFB, 0x01} },
	{ .data = {0x05, 0x04} },
	{ .data = {0x13, 0x04} },
	{ .data = {0xFF, 0x24} },
	{ .data = {0xFB, 0x01} },
	{ .data = {0xC3, 0x00} },
	{ .data = {0xC4, 0x20} },
	{ .data = {0xC2, 0x8E} },
	{ .data = {0xFF, 0x10} },
	{ .data = {0x35, 0x00} },
	{ .data = {0x51, 0xFF} },
	{ .data = {0x53, 0x24} },
	{ .data = {0x55, 0x00} },
};

static const struct nt36672a_panel_cmd begonia_csot_nt36672a_video_on_cmds_2[] = {
	{ .data = {0xFF, 0x24} },
	{ .data = {0xFB, 0x01} },
	{ .data = {0xC3, 0x01} },
	{ .data = {0xC4, 0x05} },
	{ .data = {0xFF, 0x10} },
};

static const struct nt36672a_panel_cmd begonia_tianma_nt36672a_video_on_cmds_2[] = {
	{ .data = {0xFF, 0xF0} },
	{ .data = {0xFB, 0x01} },
	{ .data = {0xA2, 0x00} },
	{ .data = {0xFF, 0x24} },
	{ .data = {0xFB, 0x01} },
	{ .data = {0xC3, 0x01} },
	{ .data = {0xC4, 0x25} },
	{ .data = {0xFF, 0x10} },
};

static const struct nt36672a_panel_cmd begonia_nt36672a_video_off_cmds[] = {
	{ .data = {0xFF, 0x24} },
	{ .data = {0xFB, 0x01} },
	{ .data = {0xC3, 0x00} },
	{ .data = {0xFF, 0x10} },
};

static const struct nt36672a_panel_reset_seq begonia_nt36672a_reset_seq[] = {
	{ .gpio_value = 1, .sleep_msecs = 5 },
	{ .gpio_value = 0, .sleep_msecs = 5 },
};

static const struct drm_display_mode begonia_nt36672a_panel_default_mode = {
	.clock		= (1080 + 64 + 16 + 56) * (2340 + 14 + 2 + 10) * 60 / 1000,

	.hdisplay	= 1080,
	.hsync_start	= 1080 + 64,
	.hsync_end	= 1080 + 64 + 16,
	.htotal		= 1080 + 64 + 16 + 56,

	.vdisplay	= 2340,
	.vsync_start	= 2340 + 14,
	.vsync_end	= 2340 + 14 + 2,
	.vtotal		= 2340 + 14 + 2 + 10,

	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

static const struct nt36672a_panel_desc begonia_csot_nt36672a_panel_desc = {
	.display_mode = &begonia_nt36672a_panel_default_mode,

	.width_mm = 69,
	.height_mm = 150,

	.mode_flags = MIPI_DSI_MODE_VIDEO |
		      MIPI_DSI_MODE_VIDEO_SYNC_PULSE |
		      MIPI_DSI_CLOCK_NON_CONTINUOUS,
	.format = MIPI_DSI_FMT_RGB888,
	.lanes = 4,
	.on_cmds_1 = begonia_nt36672a_video_on_cmds_1,
	.num_on_cmds_1 = ARRAY_SIZE(begonia_nt36672a_video_on_cmds_1),
	.on_cmds_2 = begonia_csot_nt36672a_video_on_cmds_2,
	.num_on_cmds_2 = ARRAY_SIZE(begonia_csot_nt36672a_video_on_cmds_2),
	.off_cmds = begonia_nt36672a_video_off_cmds,
	.num_off_cmds = ARRAY_SIZE(begonia_nt36672a_video_off_cmds),
	.reset_seq = begonia_nt36672a_reset_seq,
	.num_reset_seq = ARRAY_SIZE(begonia_nt36672a_reset_seq),
};

static const struct nt36672a_panel_desc begonia_tianma_nt36672a_panel_desc = {
	.display_mode = &begonia_nt36672a_panel_default_mode,

	.width_mm = 69,
	.height_mm = 150,

	.mode_flags = MIPI_DSI_MODE_VIDEO |
		      MIPI_DSI_MODE_VIDEO_SYNC_PULSE |
		      MIPI_DSI_CLOCK_NON_CONTINUOUS,
	.format = MIPI_DSI_FMT_RGB888,
	.lanes = 4,
	.on_cmds_1 = begonia_nt36672a_video_on_cmds_1,
	.num_on_cmds_1 = ARRAY_SIZE(begonia_nt36672a_video_on_cmds_1) - 3,
	.on_cmds_2 = begonia_tianma_nt36672a_video_on_cmds_2,
	.num_on_cmds_2 = ARRAY_SIZE(begonia_tianma_nt36672a_video_on_cmds_2),
	.off_cmds = begonia_nt36672a_video_off_cmds,
	.num_off_cmds = ARRAY_SIZE(begonia_nt36672a_video_off_cmds),
	.reset_seq = begonia_nt36672a_reset_seq,
	.num_reset_seq = ARRAY_SIZE(begonia_nt36672a_reset_seq),
};

static int nt36672a_panel_add(struct nt36672a_panel *pinfo)
{
	struct device *dev = &pinfo->link->dev;
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(pinfo->supplies); i++) {
		pinfo->supplies[i].supply = nt36672a_regulator_names[i];
		pinfo->supplies[i].init_load_uA = nt36672a_regulator_enable_loads[i];
	}

	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(pinfo->supplies),
				      pinfo->supplies);
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to get regulators\n");

	pinfo->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(pinfo->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(pinfo->reset_gpio),
				     "failed to get reset gpio from DT\n");

	ret = drm_panel_of_backlight(&pinfo->base);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get backlight\n");

	drm_panel_add(&pinfo->base);

	return 0;
}

static int nt36672a_panel_probe(struct mipi_dsi_device *dsi)
{
	struct nt36672a_panel *pinfo;
	const struct nt36672a_panel_desc *desc;
	int err;

	pinfo = devm_drm_panel_alloc(&dsi->dev, __typeof(*pinfo), base,
				     &panel_funcs, DRM_MODE_CONNECTOR_DSI);

	if (IS_ERR(pinfo))
		return PTR_ERR(pinfo);

	desc = of_device_get_match_data(&dsi->dev);
	dsi->mode_flags = desc->mode_flags;
	dsi->format = desc->format;
	dsi->lanes = desc->lanes;
	pinfo->desc = desc;
	pinfo->link = dsi;

	mipi_dsi_set_drvdata(dsi, pinfo);

	err = nt36672a_panel_add(pinfo);
	if (err < 0)
		return err;

	err = mipi_dsi_attach(dsi);
	if (err < 0) {
		drm_panel_remove(&pinfo->base);
		return err;
	}

	return 0;
}

static void nt36672a_panel_remove(struct mipi_dsi_device *dsi)
{
	struct nt36672a_panel *pinfo = mipi_dsi_get_drvdata(dsi);
	int err;

	err = mipi_dsi_detach(dsi);
	if (err < 0)
		dev_err(&dsi->dev, "failed to detach from DSI host: %d\n", err);

	drm_panel_remove(&pinfo->base);
}

static const struct of_device_id nt36672a_of_match[] = {
	{
		.compatible = "tianma,fhd-video",
		.data = &tianma_fhd_video_panel_desc,
	},
	{
		.compatible = "xiaomi,beryllium-tianma-nt36672a",
		.data = &tianma_fhd_video_panel_desc,
	},
	{
		.compatible = "xiaomi,begonia-csot-nt36672a",
		.data = &begonia_csot_nt36672a_panel_desc,
	},
	{
		.compatible = "xiaomi,begonia-tianma-nt36672a",
		.data = &begonia_tianma_nt36672a_panel_desc,
	},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, nt36672a_of_match);

static struct mipi_dsi_driver nt36672a_panel_driver = {
	.driver = {
		.name = "panel-novatek-nt36672a",
		.of_match_table = nt36672a_of_match,
	},
	.probe = nt36672a_panel_probe,
	.remove = nt36672a_panel_remove,
};
module_mipi_dsi_driver(nt36672a_panel_driver);

MODULE_AUTHOR("Sumit Semwal <sumit.semwal@linaro.org>");
MODULE_DESCRIPTION("NOVATEK NT36672A based MIPI-DSI LCD panel driver");
MODULE_LICENSE("GPL");
