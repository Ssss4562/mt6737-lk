#include <debug.h>
#include <reg.h>
#include <target.h>
#include <printf.h>
#include <string.h>
#include <platform/mt_reg_base.h>
#include <platform/mt_typedefs.h>
#include <platform/timer.h>

/* From app/mt_boot/fastboot.h (not on this target's include path). */
void fastboot_register(const char *prefix,
		       void (*handle)(const char *arg, void *data, unsigned sz),
		       unsigned char security_enabled);
void fastboot_okay(const char *info);
void fastboot_fail(const char *reason);
extern void fastboot_info(const char *reason);

/* CT07 debug over USB: keypad + panel state without UART. */

extern unsigned int ct07_last_id;

static void ct07_oem_kpdump(const char *arg, void *data, unsigned sz)
{
	char msg[80];
	unsigned short m[5];
	int i;

	(void)arg;
	(void)data;
	(void)sz;

	/* Make sure the controller scans (same as boot KP bring-up). */
	*(volatile unsigned int *)0x10000044 = (1U << 16);
	*(volatile unsigned short *)0x10003018 = 0x400;
	*(volatile unsigned short *)0x10003024 = 0x1;

	for (i = 0; i < 5; i++)
		m[i] = DRV_Reg16(0x10003004 + (i << 2));

	snprintf(msg, sizeof(msg), "KPMEM %04x %04x %04x %04x %04x",
		 m[0], m[1], m[2], m[3], m[4]);
	fastboot_info(msg);
	fastboot_okay("");
}

/* Survey mode: N samples ~300ms apart in one command. Press keys one by
 * one in a known order while it runs, paste the whole output. */
static void ct07_oem_kploop(const char *arg, void *data, unsigned sz)
{
	char msg[80];
	int i, n;

	(void)data;
	(void)sz;

	n = 25;
	if (arg && *arg >= '0' && *arg <= '9') {
		n = 0;
		while (*arg >= '0' && *arg <= '9') {
			n = n * 10 + (*arg - '0');
			arg++;
		}
		if (n < 5)
			n = 5;
		if (n > 100)
			n = 100;
	}

	*(volatile unsigned int *)0x10000044 = (1U << 16);
	*(volatile unsigned short *)0x10003018 = 0x400;
	*(volatile unsigned short *)0x10003024 = 0x1;

	for (i = 0; i < n; i++) {
		unsigned short m[5];
		int k;
		for (k = 0; k < 5; k++)
			m[k] = DRV_Reg16(0x10003004 + (k << 2));
		snprintf(msg, sizeof(msg), "KP %02d: %04x %04x %04x %04x %04x",
			 i, m[0], m[1], m[2], m[3], m[4]);
		fastboot_info(msg);
		mdelay(300);
	}
	fastboot_okay("");
}

static void ct07_oem_lcdid(const char *arg, void *data, unsigned sz)
{
	char msg[32];

	(void)arg;
	(void)data;
	(void)sz;

	snprintf(msg, sizeof(msg), "LCDID %04x", ct07_last_id & 0xFFFF);
	fastboot_info(msg);
	fastboot_okay("");
}

void fastboot_oem_register()
{
	fastboot_register("oem kpdump", ct07_oem_kpdump, TRUE);
	fastboot_register("oem kploop", ct07_oem_kploop, TRUE);
	fastboot_register("oem lcdid", ct07_oem_lcdid, TRUE);
}
