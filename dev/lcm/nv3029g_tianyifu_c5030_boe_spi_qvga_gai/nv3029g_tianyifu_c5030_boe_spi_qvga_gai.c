/*
 * CT07 (BD6737T_35G_A_M0) LCD panel: NV3029G over GPIO bit-bang SPI.
 * Same transport as the ili9341 sibling driver; init table copied
 * verbatim from nv3029g_tianyifu_c5030_boe_spi_qvga_gai.c (3.18 tree).
 * compare_id always accepts (mirrors the kernel driver: the panel is
 * the LK-selected fallback when ili9341 ID read fails).
 *
 * No logo support: init() only inits the panel and fills it white
 * as a transport proof. update() only sets the address window.
 */

#include <string.h>
#include <stdio.h>
#include <platform/mt_gpio.h>
#include <platform/mt_reg_base.h>
#include <platform/timer.h>
#include <platform/mt_leds.h>
#include <platform/mt_typedefs.h>
#include <platform/upmu_hw.h>
#include "lcm_drv.h"

/* pmic_set_register_value lives in platform upmu_common.c. */
extern kal_uint16 pmic_set_register_value(PMU_FLAGS_LIST_ENUM flagname,
					  kal_uint32 val);

#define FRAME_WIDTH  (240)
#define FRAME_HEIGHT (320)

/* GPIO 65=CS 66=CLK 67=RS 68=DATA, MMSYS LCM_RST for panel reset. */
#define CT07_LCD_CS    65
#define CT07_LCD_CLK   66
#define CT07_LCD_RS    67
#define CT07_LCD_SDA   68

#define CT07_MMSYS_LCM_RST_REG  (MMSYS_CONFIG_BASE + 0x150)

#define REGFLAG_DELAY        0xFE
#define REGFLAG_END_OF_TABLE 0xFF

struct ct07_lcm_setting_table {
	unsigned char cmd;
	unsigned char count;
	unsigned char para_list[16];
};

static void ct07_spi_gpio_init(void)
{
	mt_set_gpio_mode(CT07_LCD_CS, GPIO_MODE_00);
	mt_set_gpio_mode(CT07_LCD_CLK, GPIO_MODE_00);
	mt_set_gpio_mode(CT07_LCD_RS, GPIO_MODE_00);
	mt_set_gpio_mode(CT07_LCD_SDA, GPIO_MODE_00);
	mt_set_gpio_dir(CT07_LCD_CS, GPIO_DIR_OUT);
	mt_set_gpio_dir(CT07_LCD_CLK, GPIO_DIR_OUT);
	mt_set_gpio_dir(CT07_LCD_RS, GPIO_DIR_OUT);
	mt_set_gpio_dir(CT07_LCD_SDA, GPIO_DIR_OUT);
	mt_set_gpio_out(CT07_LCD_CS, GPIO_OUT_ONE);
	mt_set_gpio_out(CT07_LCD_CLK, GPIO_OUT_ZERO);
}

static void ct07_spi_write_byte(unsigned char b)
{
	int i;

	for (i = 7; i >= 0; i--) {
		mt_set_gpio_out(CT07_LCD_CLK, GPIO_OUT_ZERO);
		mt_set_gpio_out(CT07_LCD_SDA, (b >> i) & 0x01);
		mt_set_gpio_out(CT07_LCD_CLK, GPIO_OUT_ONE);
	}
	mt_set_gpio_out(CT07_LCD_CLK, GPIO_OUT_ZERO);
}

static void ct07_send_ctrl_cmd(unsigned int cmd)
{
	mt_set_gpio_dir(CT07_LCD_SDA, GPIO_DIR_OUT);
	mt_set_gpio_out(CT07_LCD_RS, GPIO_OUT_ZERO);
	mt_set_gpio_out(CT07_LCD_CS, GPIO_OUT_ZERO);
	ct07_spi_write_byte(cmd & 0xFF);
	mt_set_gpio_out(CT07_LCD_CS, GPIO_OUT_ONE);
}

static void ct07_send_data_bytes(const unsigned char *data, unsigned int len)
{
	mt_set_gpio_dir(CT07_LCD_SDA, GPIO_DIR_OUT);
	mt_set_gpio_out(CT07_LCD_RS, GPIO_OUT_ONE);
	mt_set_gpio_out(CT07_LCD_CS, GPIO_OUT_ZERO);
	while (len--)
		ct07_spi_write_byte(*data++);
	mt_set_gpio_out(CT07_LCD_CS, GPIO_OUT_ONE);
}

static void ct07_set_reset_pin(unsigned int v)
{
	*(volatile unsigned int *)CT07_MMSYS_LCM_RST_REG = v;
}

static const struct ct07_lcm_setting_table lcm_initialization_setting[] = {
	{0xFD, 2, {0x06, 0x07}},
	{0x66, 1, {0x80}},
	{0x80, 1, {0x05}},
	{REGFLAG_DELAY, 5, {0x00}},
	{0x80, 1, {0x01}},
	{REGFLAG_DELAY, 100, {0x00}},
	{0xB6, 2, {0x02, 0xA2}},
	{0x60, 1, {0x26}},
	{0x63, 1, {0x08}},
	{0x64, 1, {0x0C}},
	{0x68, 1, {0x70}},
	{0x69, 1, {0x1B}},
	{0x6A, 1, {0xC4}},
	{0x6B, 1, {0x0E}},
	{0x6C, 1, {0x18}},
	{0x6D, 1, {0x77}},
	{0x6E, 1, {0x84}},
	{0x6F, 1, {0x48}},
	{0xF7, 1, {0x10}},
	{0x70, 1, {0x44}},
	{0x71, 1, {0x05}},
	{0xED, 6, {0xF9, 0xF9, 0x00, 0x00, 0x11, 0x00}},
	{0xE0, 7, {0x0E, 0x17, 0x13, 0x1B, 0x07, 0x14, 0x19}},
	{0xE1, 2, {0x28, 0x59}},
	{0xE2, 6, {0x1B, 0x25, 0x26, 0x11, 0x09, 0x17}},
	{0xE3, 7, {0x0D, 0x17, 0x05, 0x0F, 0x06, 0x10, 0x07}},
	{0xE4, 2, {0x0F, 0x52}},
	{0xE5, 6, {0x17, 0x26, 0x1F, 0x19, 0x19, 0x1F}},
	{0x66, 1, {0x9A}},
	{0x67, 1, {0x07}},
	{0xB1, 2, {0x00, 0x12}},
	{0xEC, 6, {0x33, 0x16, 0x16, 0x00, 0x18, 0x18}},
	{0xF6, 3, {0x01, 0x10, 0x00}},
	{0xFD, 2, {0xFA, 0xFB}},
	{0x11, 0, {0x00}},
	{REGFLAG_DELAY, 200, {0x00}},
	{0x36, 1, {0x08}},
	{0x3A, 1, {0x65}},
	/* Stock (and LK) end the table with DISPON + 10 ms here. */
	{0x29, 0, {0x00}},
	{REGFLAG_DELAY, 10, {0x00}},
	{REGFLAG_END_OF_TABLE, 0, {0x00}},
};

static const struct ct07_lcm_setting_table lcm_suspend_setting[] = {
	{0x28, 1, {0x00}},
	{REGFLAG_DELAY, 120, {0x00}},
	{0x10, 1, {0x00}},
	{REGFLAG_DELAY, 20, {0x00}},
	{REGFLAG_END_OF_TABLE, 0, {0x00}},
};

static void push_table(const struct ct07_lcm_setting_table *table,
		       unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; i++) {
		switch (table[i].cmd) {
		case REGFLAG_DELAY:
			mdelay(table[i].count);
			break;
		case REGFLAG_END_OF_TABLE:
			return;
		default:
			ct07_send_ctrl_cmd(table[i].cmd);
			if (table[i].count)
				ct07_send_data_bytes(table[i].para_list,
						     table[i].count);
			break;
		}
	}
}

static void lcm_set_util_funcs(const LCM_UTIL_FUNCS *util)
{
	(void)util;
}

static void lcm_get_params(LCM_PARAMS *params)
{
	/* Fake DSI params, same as the kernel driver: this LK has no DBI
	 * output path, DDP/DSI init must behave exactly as with the stock
	 * r63417 project (blind but harmless). */
	memset(params, 0, sizeof(LCM_PARAMS));

	params->type = LCM_TYPE_DSI;
	params->width = FRAME_WIDTH;
	params->height = FRAME_HEIGHT;
	params->dsi.mode = BURST_VDO_MODE;
	params->dsi.LANE_NUM = LCM_ONE_LANE;
	params->dsi.data_format.color_order = LCM_COLOR_ORDER_RGB;
	params->dsi.data_format.trans_seq = LCM_DSI_TRANS_SEQ_MSB_FIRST;
	params->dsi.data_format.padding = LCM_DSI_PADDING_ON_LSB;
	params->dsi.data_format.format = LCM_DSI_FORMAT_RGB888;
	params->dsi.PS = LCM_PACKED_PS_24BIT_RGB888;
	params->dsi.word_count = 0x870;
	params->dsi.packet_size = 0x100;
	params->dsi.vertical_sync_active = 8;
	params->dsi.vertical_backporch = 0x14;
	params->dsi.vertical_frontporch = 0x19;
	params->dsi.vertical_active_line = FRAME_HEIGHT;
	params->dsi.horizontal_sync_active = 0x0a;
	params->dsi.horizontal_backporch = 0x28;
	params->dsi.horizontal_frontporch = 0x54;
	params->dsi.horizontal_active_pixel = FRAME_WIDTH;
	params->dsi.PLL_CLOCK = 0x5a;
}

static void lcm_init(void)
{
	/* Panel logic rails (kernel keeps VAUD28+VAUX18 boot-on). */
	pmic_set_register_value(PMIC_RG_VAUD28_EN, 1);
	pmic_set_register_value(PMIC_RG_VAUX18_EN, 1);
	mdelay(10);

	ct07_spi_gpio_init();
	mt65xx_backlight_on();

	ct07_set_reset_pin(1);
	mdelay(20);
	ct07_set_reset_pin(0);
	mdelay(150);
	ct07_set_reset_pin(1);
	mdelay(120);

	push_table(lcm_initialization_setting,
		   sizeof(lcm_initialization_setting) /
		   sizeof(struct ct07_lcm_setting_table));

	/* Selection proof: YELLOW 24x24 at (0,0). */
	{
		unsigned char win[4] = {0x00, 0x00, 0x00, 0x17};
		unsigned long n = 24UL * 24UL;
		ct07_send_ctrl_cmd(0x2A);
		ct07_send_data_bytes(win, 4);
		ct07_send_ctrl_cmd(0x2B);
		ct07_send_data_bytes(win, 4);
		ct07_send_ctrl_cmd(0x2C);
		mt_set_gpio_dir(CT07_LCD_SDA, GPIO_DIR_OUT);
		mt_set_gpio_out(CT07_LCD_RS, GPIO_OUT_ONE);
		mt_set_gpio_out(CT07_LCD_CS, GPIO_OUT_ZERO);
		while (n--) {
			ct07_spi_write_byte(0xFF);
			ct07_spi_write_byte(0xE0);
			}
		mt_set_gpio_out(CT07_LCD_CS, GPIO_OUT_ONE);
	}
	extern int ct07_ready;
	ct07_ready = 1;
	printf("[CT07_LCM] nv3029 init done\n");
}

static void lcm_suspend(void)
{
	push_table(lcm_suspend_setting,
		   sizeof(lcm_suspend_setting) /
		   sizeof(struct ct07_lcm_setting_table));
}

static void lcm_resume(void)
{
	lcm_init();
}

static void lcm_update(unsigned int x, unsigned int y,
		       unsigned int width, unsigned int height)
{
	unsigned int x0 = x, y0 = y;
	unsigned int x1 = x0 + width - 1, y1 = y0 + height - 1;
	unsigned char data[4];

	/* Window only; frame push (logo) is intentionally not supported. */
	data[0] = (x0 >> 8) & 0xFF;
	data[1] = x0 & 0xFF;
	data[2] = (x1 >> 8) & 0xFF;
	data[3] = x1 & 0xFF;
	ct07_send_ctrl_cmd(0x2A);
	ct07_send_data_bytes(data, sizeof(data));

	data[0] = (y0 >> 8) & 0xFF;
	data[1] = y0 & 0xFF;
	data[2] = (y1 >> 8) & 0xFF;
	data[3] = y1 & 0xFF;
	ct07_send_ctrl_cmd(0x2B);
	ct07_send_data_bytes(data, sizeof(data));

	ct07_send_ctrl_cmd(0x2C);
}

static unsigned int lcm_compare_id(void)
{
	/* Mirrors the kernel driver: accept, this is the LK-selected
	 * fallback panel when ili9341 ID read fails. */
	printf("[CT07_LCM] nv3029 compare_id accept\n");
	return 1;
}

LCM_DRIVER nv3029g_tianyifu_c5030_boe_spi_qvga_gai_lcm_drv = {
	.name           = "NV3029G_TIANYIFU_C5030_BOE_SPI_QVGA_GAI",
	.set_util_funcs = lcm_set_util_funcs,
	.get_params     = lcm_get_params,
	.init           = lcm_init,
	.suspend        = lcm_suspend,
	.resume         = lcm_resume,
	.update         = lcm_update,
	.compare_id     = lcm_compare_id,
};
