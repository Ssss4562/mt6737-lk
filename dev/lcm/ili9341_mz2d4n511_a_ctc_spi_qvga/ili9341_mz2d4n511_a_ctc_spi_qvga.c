/*
 * CT07 (BD6737T_35G_A_M0) LCD panel: ILI9341 over GPIO bit-bang SPI.
 *
 * The panel is NOT on DSI/DBI hardware: stock drives it through the
 * SoC SPI controller (see ct07_lcm_spi in the 3.18 tree). LK has no SPI
 * master driver, so this driver bit-bangs the same 4-wire bus
 * (CS/CLK/RS/DATA) with plain GPIOs. Init table is copied verbatim
 * from ili9341_mz2d4n511_a_ctc_spi_qvga.c (3.18 tree).
 *
 * No logo support: init() only inits the panel and fills it white
 * as a transport proof. update() only sets the address window.
 */

#include <string.h>
#include <stdio.h>
#include <platform/mt_gpio.h>
#include <platform/mt_reg_base.h>
#include <platform/timer.h>
#include <platform/mt_typedefs.h>
#include <platform/upmu_hw.h>
#include "lcm_drv.h"
#include <platform/mt_leds.h>
extern void *mt_get_fb_addr(void);
void ct07_draw_text(unsigned int row, const char *s,
		    unsigned short fg, unsigned short bg);
extern void mt_disp_update(unsigned int x, unsigned int y, unsigned int w, unsigned int h);

#define font5x12 ct07_font5x12
#include "../../fbcon/font5x12.h"
#undef font5x12

/* pmic_set_register_value lives in platform upmu_common.c (no header
 * declaration in this tree). */
extern kal_uint16 pmic_set_register_value(PMU_FLAGS_LIST_ENUM flagname,
					  kal_uint32 val);
extern void mtk_wdt_restart(void);

#define FRAME_WIDTH  (240)
#define FRAME_HEIGHT (320)

/* From vz6737t_35g_a_m0 DTS pinctrl (pins 0x41..0x44 = GPIO 65..68):
 * CS=65 CLK=66 RS=67 DATA=68. Function 1 = SPI peripheral in DTS,
 * here we force GPIO mode (0) for bit-banging. */
#define CT07_LCD_CS    65
#define CT07_LCD_CLK   66
#define CT07_LCD_RS    67
#define CT07_LCD_SDA   68

/* Panel reset is the MMSYS LCM_RST bit, same as the 3.18 kernel
 * (lcm_set_reset_pin: MMSYS_CONFIG_BASE + 0x150). */
#define CT07_MMSYS_LCM_RST_REG  (MMSYS_CONFIG_BASE + 0x150)

#define LCM_ID        (0x9341)
#define LCM_ID_MASK   (0xFF)

/* Set by panel init; push/text are no-ops before it. */
int ct07_ready = 0;

/* Last D3 read from compare_id (for on-screen ID). */
unsigned int ct07_last_id = 0xFFFF;

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

/* SPI mode 0 (CPOL=0/CPHA=0, MSB first), same as the kernel mt_spi conf. */
static void ct07_spi_write_byte(unsigned char b)
{
	int i;

	for (i = 7; i >= 0; i--) {
		mt_set_gpio_out(CT07_LCD_SDA, (b >> i) & 0x01);
		mt_set_gpio_out(CT07_LCD_CLK, GPIO_OUT_ONE);
		mt_set_gpio_out(CT07_LCD_CLK, GPIO_OUT_ZERO);
	}
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

static __inline void ct07_send_data_cmd(unsigned int data)
{
	unsigned char b = data & 0xFF;

	ct07_send_data_bytes(&b, 1);
}

static unsigned char ct07_spi_read_byte(void)
{
	int i;
	unsigned char v = 0;

	mt_set_gpio_dir(CT07_LCD_SDA, GPIO_DIR_IN);
	for (i = 7; i >= 0; i--) {
		mt_set_gpio_out(CT07_LCD_CLK, GPIO_OUT_ZERO);
		mt_set_gpio_out(CT07_LCD_CLK, GPIO_OUT_ONE);
		v |= ((mt_get_gpio_in(CT07_LCD_SDA) & 0x01) << i);
	}
	mt_set_gpio_out(CT07_LCD_CLK, GPIO_OUT_ZERO);
	mt_set_gpio_dir(CT07_LCD_SDA, GPIO_DIR_OUT);
	return v;
}

/* Command phase with RS low, then RS high and clock in n bytes. */
static void ct07_read_bytes(unsigned char cmd, unsigned char *buf, unsigned int n)
{
	mt_set_gpio_dir(CT07_LCD_SDA, GPIO_DIR_OUT);
	mt_set_gpio_out(CT07_LCD_RS, GPIO_OUT_ZERO);
	mt_set_gpio_out(CT07_LCD_CS, GPIO_OUT_ZERO);
	ct07_spi_write_byte(cmd);
	mt_set_gpio_out(CT07_LCD_RS, GPIO_OUT_ONE);
	while (n--)
		*buf++ = ct07_spi_read_byte();
	mt_set_gpio_out(CT07_LCD_CS, GPIO_OUT_ONE);
}

static void ct07_set_reset_pin(unsigned int v)
{
	*(volatile unsigned int *)CT07_MMSYS_LCM_RST_REG = v;
}

static void init_lcm_registers(void)
{
	ct07_send_ctrl_cmd(0xCF);
	ct07_send_data_cmd(0x00);
	ct07_send_data_cmd(0x99);
	ct07_send_data_cmd(0x30);

	ct07_send_ctrl_cmd(0xED);
	ct07_send_data_cmd(0x67);
	ct07_send_data_cmd(0x03);
	ct07_send_data_cmd(0x12);
	ct07_send_data_cmd(0x81);

	ct07_send_ctrl_cmd(0xE8);
	ct07_send_data_cmd(0x8A);
	ct07_send_data_cmd(0x0A);
	ct07_send_data_cmd(0x78);

	ct07_send_ctrl_cmd(0xCB);
	ct07_send_data_cmd(0x39);
	ct07_send_data_cmd(0x2C);
	ct07_send_data_cmd(0x00);
	ct07_send_data_cmd(0x34);
	ct07_send_data_cmd(0x02);

	ct07_send_ctrl_cmd(0xF7);
	ct07_send_data_cmd(0x20);

	ct07_send_ctrl_cmd(0xEA);
	ct07_send_data_cmd(0x00);
	ct07_send_data_cmd(0x00);

	ct07_send_ctrl_cmd(0xC0);
	ct07_send_data_cmd(0x26);

	ct07_send_ctrl_cmd(0xC1);
	ct07_send_data_cmd(0x11);

	ct07_send_ctrl_cmd(0xC5);
	ct07_send_data_cmd(0x2B);
	ct07_send_data_cmd(0x2C);

	ct07_send_ctrl_cmd(0xC7);
	ct07_send_data_cmd(0xD4);

	ct07_send_ctrl_cmd(0xF6);
	ct07_send_data_cmd(0x01);
	ct07_send_data_cmd(0x30);
	ct07_send_data_cmd(0x00);

	ct07_send_ctrl_cmd(0xB6);
	ct07_send_data_cmd(0x0A);
	ct07_send_data_cmd(0xA2);

	ct07_send_ctrl_cmd(0x36);
	ct07_send_data_cmd(0x08);

	ct07_send_ctrl_cmd(0x3A);
	ct07_send_data_cmd(0x66);

	ct07_send_ctrl_cmd(0xB1);
	ct07_send_data_cmd(0x00);
	ct07_send_data_cmd(0x19);

	ct07_send_ctrl_cmd(0xF2);
	ct07_send_data_cmd(0x00);

	ct07_send_ctrl_cmd(0x26);
	ct07_send_data_cmd(0x01);

	ct07_send_ctrl_cmd(0xE0);
	ct07_send_data_cmd(0x0F);
	ct07_send_data_cmd(0x22);
	ct07_send_data_cmd(0x20);
	ct07_send_data_cmd(0x09);
	ct07_send_data_cmd(0x0D);
	ct07_send_data_cmd(0x08);
	ct07_send_data_cmd(0x4E);
	ct07_send_data_cmd(0xA7);
	ct07_send_data_cmd(0x3F);
	ct07_send_data_cmd(0x05);
	ct07_send_data_cmd(0x0B);
	ct07_send_data_cmd(0x03);
	ct07_send_data_cmd(0x20);
	ct07_send_data_cmd(0x23);
	ct07_send_data_cmd(0x00);

	ct07_send_ctrl_cmd(0xE1);
	ct07_send_data_cmd(0x00);
	ct07_send_data_cmd(0x1D);
	ct07_send_data_cmd(0x1F);
	ct07_send_data_cmd(0x06);
	ct07_send_data_cmd(0x12);
	ct07_send_data_cmd(0x07);
	ct07_send_data_cmd(0x31);
	ct07_send_data_cmd(0x58);
	ct07_send_data_cmd(0x40);
	ct07_send_data_cmd(0x0A);
	ct07_send_data_cmd(0x14);
	ct07_send_data_cmd(0x0C);
	ct07_send_data_cmd(0x1F);
	ct07_send_data_cmd(0x1C);
	ct07_send_data_cmd(0x0F);

	ct07_send_ctrl_cmd(0x2A);
	ct07_send_data_cmd(0x00);
	ct07_send_data_cmd(0x00);
	ct07_send_data_cmd(0x00);
	ct07_send_data_cmd(0xEF);

	ct07_send_ctrl_cmd(0x2B);
	ct07_send_data_cmd(0x00);
	ct07_send_data_cmd(0x00);
	ct07_send_data_cmd(0x01);
	ct07_send_data_cmd(0x3F);

	ct07_send_ctrl_cmd(0x11);
	mdelay(120);

	ct07_send_ctrl_cmd(0x29);
	mdelay(10);

	ct07_send_ctrl_cmd(0x2C);


	/* Selection proof: this driver ran. GREEN 24x24 at (0,0). */
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
			ct07_spi_write_byte(0x07);
			ct07_spi_write_byte(0xE0);
		}
		mt_set_gpio_out(CT07_LCD_CS, GPIO_OUT_ONE);
	}
}

static void lcm_set_util_funcs(const LCM_UTIL_FUNCS *util)
{
	(void)util;
}

static void lcm_get_params(LCM_PARAMS *params)
{
	/* Fake DSI params (same as the kernel nv3029 driver): this LK has
	 * no DBI output path, and the DDP/DSI init must behave exactly as
	 * with the stock r63417 project (blind but harmless). The panel is
	 * driven through the bit-bang SPI above, not through DSI. */
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
	/* Panel logic rails (kernel keeps VAUD28+VAUX18 boot-on; LK enables
	 * nothing by itself). Without these the panel is dead in LK. */
	pmic_set_register_value(PMIC_RG_VAUD28_EN, 1);
	pmic_set_register_value(PMIC_RG_VAUX18_EN, 1);
	mdelay(10);

	ct07_spi_gpio_init();
	mt65xx_backlight_on();

	ct07_set_reset_pin(1);
	mdelay(20);
	ct07_set_reset_pin(0);
	mdelay(20);
	ct07_set_reset_pin(1);
	mdelay(100);

	init_lcm_registers();
	ct07_ready = 1;
	/* Show the D3 read result on screen (panel truth). */
	{
		char s[8] = "ID=....";
		unsigned int v = ct07_last_id, k;
		for (k = 0; k < 4; k++) {
			unsigned int nib = (v >> 12) & 0xF;
			s[3 + k] = nib < 10 ? '0' + nib : 'A' + nib - 10;
			v <<= 4;
		}
		ct07_draw_text(5, s, 0xFFFF, 0x0000);
	}
	printf("[CT07_LCM] ili9341 init done\n");
}

static void lcm_suspend(void)
{
	ct07_send_ctrl_cmd(0x28);
	mdelay(50);
	ct07_send_ctrl_cmd(0x10);
	mdelay(120);
}

static void lcm_resume(void)
{
	ct07_send_ctrl_cmd(0x11);
	mdelay(150);
	ct07_send_ctrl_cmd(0x29);
	mdelay(50);
	ct07_send_ctrl_cmd(0x2c);
}

static void lcm_update(unsigned int x, unsigned int y,
		       unsigned int width, unsigned int height)
{
	unsigned int x0 = x, y0 = y;
	unsigned int x1 = x0 + width - 1, y1 = y0 + height - 1;

	/* Window only; frame push (logo) is intentionally not supported. */
	ct07_send_ctrl_cmd(0x2A);
	ct07_send_data_cmd((x0 >> 8) & 0xFF);
	ct07_send_data_cmd(x0 & 0xFF);
	ct07_send_data_cmd((x1 >> 8) & 0xFF);
	ct07_send_data_cmd(x1 & 0xFF);

	ct07_send_ctrl_cmd(0x2B);
	ct07_send_data_cmd((y0 >> 8) & 0xFF);
	ct07_send_data_cmd(y0 & 0xFF);
	ct07_send_data_cmd((y1 >> 8) & 0xFF);
	ct07_send_data_cmd(y1 & 0xFF);

	ct07_send_ctrl_cmd(0x2C);
}

static unsigned int lcm_compare_id(void)
{
	unsigned char buf[4];
	unsigned int id;

	ct07_spi_gpio_init();
	ct07_read_bytes(0xD3, buf, 4);
	ct07_last_id = id = ((buf[2] & LCM_ID_MASK) << 8) | buf[3];
	printf("[CT07_LCM] ili9341 id=%04x\n", id);

	return (LCM_ID == id) ? 1 : 0;
}

/* Solid rect for LK debug output (no text engine): sets the address
 * window and streams one color. Used by boot_mode keypad scan. */
void ct07_dbg_rect(unsigned int x, unsigned int y, unsigned int w,
		   unsigned int h, unsigned short color)
{
	unsigned int x1 = x + w - 1, y1 = y + h - 1;
	unsigned long n = (unsigned long)w * h;
	unsigned char hi = (color >> 8) & 0xFF, lo = color & 0xFF;

	ct07_send_ctrl_cmd(0x2A);
	ct07_send_data_cmd((x >> 8) & 0xFF);
	ct07_send_data_cmd(x & 0xFF);
	ct07_send_data_cmd((x1 >> 8) & 0xFF);
	ct07_send_data_cmd(x1 & 0xFF);

	ct07_send_ctrl_cmd(0x2B);
	ct07_send_data_cmd((y >> 8) & 0xFF);
	ct07_send_data_cmd(y & 0xFF);
	ct07_send_data_cmd((y1 >> 8) & 0xFF);
	ct07_send_data_cmd(y1 & 0xFF);

	ct07_send_ctrl_cmd(0x2C);

	mt_set_gpio_dir(CT07_LCD_SDA, GPIO_DIR_OUT);
	mt_set_gpio_out(CT07_LCD_RS, GPIO_OUT_ONE);
	mt_set_gpio_out(CT07_LCD_CS, GPIO_OUT_ZERO);
	while (n--) {
		ct07_spi_write_byte(hi);
		ct07_spi_write_byte(lo);
	}
	mt_set_gpio_out(CT07_LCD_CS, GPIO_OUT_ONE);
}

LCM_DRIVER ili9341_mz2d4n511_a_ctc_spi_qvga_lcm_drv = {
	.name           = "ili9341_mz2d4n511_a_ctc_spi_qvga",
	.set_util_funcs = lcm_set_util_funcs,
	.get_params     = lcm_get_params,
	.init           = lcm_init,
	.suspend        = lcm_suspend,
	.resume         = lcm_resume,
	.update         = lcm_update,
	.compare_id     = lcm_compare_id,
};

/* Push a 32-bit BGRA (redoffset=1: byte0=B) DRAM framebuffer region to
 * the panel, converting to RGB565. pitch_words = full row stride. */
void ct07_push_fb(unsigned int *fb, unsigned int pitch_words,
		  unsigned int x, unsigned int y,
		  unsigned int w, unsigned int h)
{
	unsigned int r, c;

	if (!ct07_ready || !fb)
		return;
	if (x >= 240 || y >= 320)
		return;
	if (x + w > 240)
		w = 240 - x;
	if (y + h > 320)
		h = 320 - y;
	if (!w || !h)
		return;

	ct07_send_ctrl_cmd(0x2A);
	ct07_send_data_cmd((x >> 8) & 0xFF);
	ct07_send_data_cmd(x & 0xFF);
	ct07_send_data_cmd(((x + w - 1) >> 8) & 0xFF);
	ct07_send_data_cmd((x + w - 1) & 0xFF);

	ct07_send_ctrl_cmd(0x2B);
	ct07_send_data_cmd((y >> 8) & 0xFF);
	ct07_send_data_cmd(y & 0xFF);
	ct07_send_data_cmd(((y + h - 1) >> 8) & 0xFF);
	ct07_send_data_cmd((y + h - 1) & 0xFF);

	ct07_send_ctrl_cmd(0x2C);

	mt_set_gpio_dir(CT07_LCD_SDA, GPIO_DIR_OUT);
	mt_set_gpio_out(CT07_LCD_RS, GPIO_OUT_ONE);
	mt_set_gpio_out(CT07_LCD_CS, GPIO_OUT_ZERO);
	for (r = 0; r < h; r++) {
		unsigned int *rowp = fb + (y + r) * pitch_words + x;
		if ((r & 0xF) == 0)
			mtk_wdt_restart();
		for (c = 0; c < w; c++) {
			unsigned int px = rowp[c];
			unsigned int rr = (px >> 16) & 0xFF;
			unsigned int gg = (px >> 8) & 0xFF;
			unsigned int bb = px & 0xFF;
			unsigned int v = ((rr >> 3) << 11) | ((gg >> 2) << 5) | (bb >> 3);
			ct07_spi_write_byte((v >> 8) & 0xFF);
			ct07_spi_write_byte(v & 0xFF);
		}
	}
	mt_set_gpio_out(CT07_LCD_CS, GPIO_OUT_ONE);
}

#define CT07_FONT_W 5
#define CT07_FONT_H 12
#define CT07_TEXT_COLS 40

/* Text row into the DRAM framebuffer + band push, so it survives later
 * pushes (direct panel draws get erased by the next mt_disp_update).
 * fg/bg are RGB565, expanded to 32-bit BGRA to match the LK fb. */
void ct07_draw_text(unsigned int row, const char *s,
		    unsigned short fg, unsigned short bg)
{
	unsigned int *fb;
	unsigned int pitch = 256; /* ALIGN_TO(240, MTK_FB_ALIGNMENT=32) */
	unsigned int x, y, col = 0;
	unsigned int fg32, bg32;

	if (row > 26)
		return;
	fg32 = 0xFF000000 |
	       (((((fg >> 11) & 0x1F) << 3) | ((fg >> 13) & 0x07)) << 16) |
	       (((((fg >> 5) & 0x3F) << 2) | ((fg >> 9) & 0x03)) << 8) |
	       (((fg & 0x1F) << 3) | ((fg >> 2) & 0x07));
	bg32 = 0xFF000000 |
	       (((((bg >> 11) & 0x1F) << 3) | ((bg >> 13) & 0x07)) << 16) |
	       (((((bg >> 5) & 0x3F) << 2) | ((bg >> 9) & 0x03)) << 8) |
	       (((bg & 0x1F) << 3) | ((bg >> 2) & 0x07));

	fb = (unsigned int *)mt_get_fb_addr();
	if (!fb)
		return;
	for (y = 0; y < 12; y++)
		for (x = 0; x < 240; x++)
			fb[(row * 12 + y) * pitch + x] = bg32;
	while (*s && col < 40) {
		unsigned char c = *s++;
		unsigned int *glyph;
		unsigned int data;
		if (c < 32 || c > 126)
			c = '?';
		glyph = ct07_font5x12 + (c - 32) * 2;
		data = glyph[0];
		for (y = 0; y < 6; y++) {
			for (x = 0; x < 5; x++) {
				if (data & 1)
					fb[(row * 12 + y) * pitch + col * 6 + x] = fg32;
				data >>= 1;
			}
		}
		data = glyph[1];
		for (y = 6; y < 12; y++) {
			for (x = 0; x < 5; x++) {
				if (data & 1)
					fb[(row * 12 + y) * pitch + col * 6 + x] = fg32;
				data >>= 1;
			}
		}
		col++;
	}
	mt_disp_update(0, row * 12, 240, 12);
}
