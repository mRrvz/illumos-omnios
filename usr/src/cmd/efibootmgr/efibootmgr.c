/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2026 OmniOS Community Edition (OmniOSce) Association.
 */

/*
 * efibootmgr -- read and modify the UEFI Boot Manager's NVRAM state.
 *
 * The supported subset matches the basic FreeBSD/Linux efibootmgr(8)
 * mode: print the current configuration, set BootOrder/BootNext/
 * Timeout, toggle the active bit on a single Boot#### entry.
 *
 * Creating, deleting and renaming Boot#### entries needs partition
 * lookups against the underlying disk and is handled by a future
 * commit.
 */

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <inttypes.h>
#include <libefivar.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <unistd.h>

/*
 * EFI Global Variable GUID (8be4df61-93ca-11d2-aa0d-00e098032b8c).
 * All Boot##### / BootCurrent / BootOrder / BootNext / Timeout
 * variables live here.
 */
static const efi_guid_t efi_global_guid = {
	.time_low = 0x8be4df61,
	.time_mid = 0x93ca,
	.time_hi_and_version = 0x11d2,
	.clock_seq_hi_and_reserved = 0xaa,
	.clock_seq_low = 0x0d,
	.node_addr = { 0x00, 0xe0, 0x98, 0x03, 0x2b, 0x8c }
};

/*
 * The UEFI attribute mask used for every NVRAM variable this tool
 * writes: persistent and visible to the firmware boot path.
 */
#define	EFI_VAR_NV_BS_RT \
	(EFI_VARIABLE_NON_VOLATILE | \
	EFI_VARIABLE_BOOTSERVICE_ACCESS | \
	EFI_VARIABLE_RUNTIME_ACCESS)

static int
guid_eq(const efi_guid_t *a, const efi_guid_t *b)
{
	return (memcmp(a, b, sizeof (*a)) == 0);
}

static uint16_t
le16_unpack(const uint8_t *p)
{
	uint16_t v;
	(void) memcpy(&v, p, sizeof (v));
	return (v);
}

static int
parse_bootnum(const char *s, uint16_t *out)
{
	char *end;
	unsigned long v;

	errno = 0;
	v = strtoul(s, &end, 16);
	if (errno != 0 || *end != '\0' || end == s || v > UINT16_MAX)
		return (-1);
	*out = (uint16_t)v;
	return (0);
}

static void
print_uint16_var(const char *label, const char *varname, const char *suffix)
{
	uint8_t *data = NULL;
	size_t sz = 0;

	if (efi_get_variable(efi_global_guid, varname, &data, &sz, NULL) < 0)
		return;
	if (sz == sizeof (uint16_t))
		(void) printf("%s: %04X%s\n", label, le16_unpack(data), suffix);
	free(data);
}

static void
print_timeout(void)
{
	uint8_t *data = NULL;
	size_t sz = 0;

	if (efi_get_variable(efi_global_guid, "Timeout", &data, &sz,
	    NULL) < 0)
		return;
	if (sz == sizeof (uint16_t))
		(void) printf("Timeout: %u seconds\n", le16_unpack(data));
	free(data);
}

static void
print_bootorder(void)
{
	uint8_t *data = NULL;
	size_t sz = 0;
	size_t n, i;

	if (efi_get_variable(efi_global_guid, "BootOrder", &data, &sz,
	    NULL) < 0)
		return;

	n = sz / sizeof (uint16_t);
	(void) printf("BootOrder: ");
	for (i = 0; i < n; i++) {
		if (i > 0)
			(void) printf(",");
		(void) printf("%04X",
		    le16_unpack(data + i * sizeof (uint16_t)));
	}
	(void) printf("\n");
	free(data);
}

static int
is_boot_var_name(const char *name)
{
	int i;

	if (strncmp(name, "Boot", 4) != 0 || strlen(name) != 8)
		return (0);
	for (i = 4; i < 8; i++)
		if (!isxdigit((unsigned char)name[i]))
			return (0);
	return (1);
}

static void
print_one_boot(const char *name, int verbose)
{
	uint8_t *data = NULL;
	size_t sz = 0;
	efi_load_option_t lo;
	char active;

	if (efi_get_variable(efi_global_guid, name, &data, &sz, NULL) < 0)
		return;
	if (efi_load_option_parse(data, sz, &lo) < 0) {
		free(data);
		(void) fprintf(stderr, "%s: cannot parse load option\n", name);
		return;
	}
	free(data);

	active = (lo.elo_attributes & EFI_LOAD_OPTION_ACTIVE) ? '*' : ' ';
	(void) printf("%s%c %s", name, active,
	    lo.elo_description ? lo.elo_description : "");

	if (verbose) {
		char *dpstr = NULL;
		if (efi_devpath_to_string(lo.elo_device_path,
		    lo.elo_device_path_size, &dpstr) == 0) {
			(void) printf("\t%s", dpstr);
			free(dpstr);
		}
	}
	(void) printf("\n");
	efi_load_option_clear(&lo);
}

static void
print_all_boots(int verbose)
{
	char *name = NULL;
	efi_guid_t vendor;

	while (efi_get_next_variable_name(&vendor, &name) == 0) {
		if (!guid_eq(&vendor, &efi_global_guid))
			continue;
		if (!is_boot_var_name(name))
			continue;
		print_one_boot(name, verbose);
	}
	if (errno != ENOENT) {
		warn("variable iteration");
	}
	free(name);
}

static void
do_list(int verbose)
{
	print_uint16_var("BootCurrent", "BootCurrent", "");
	print_timeout();
	print_bootorder();
	print_uint16_var("BootNext", "BootNext", "");
	print_all_boots(verbose);
}

static void
do_set_bootorder(const char *s)
{
	uint16_t buf[256];
	size_t n = 0;
	const char *p = s;

	while (*p != '\0') {
		char *end;
		unsigned long v;

		errno = 0;
		v = strtoul(p, &end, 16);
		if (errno != 0 || end == p || v > UINT16_MAX)
			errx(1, "BootOrder: bad element near \"%s\"", p);
		if (n >= sizeof (buf) / sizeof (buf[0]))
			errx(1, "BootOrder: too many entries");
		buf[n++] = (uint16_t)v;
		p = end;
		if (*p == ',')
			p++;
		else if (*p != '\0')
			errx(1, "BootOrder: unexpected character '%c'", *p);
	}

	if (efi_set_variable(efi_global_guid, "BootOrder", (uint8_t *)buf,
	    n * sizeof (uint16_t), EFI_VAR_NV_BS_RT) < 0)
		err(1, "set BootOrder");
}

static void
do_set_uint16(const char *varname, uint16_t value)
{
	if (efi_set_variable(efi_global_guid, varname, (uint8_t *)&value,
	    sizeof (value), EFI_VAR_NV_BS_RT) < 0)
		err(1, "set %s", varname);
}

static void
do_del(const char *varname)
{
	if (efi_del_variable(efi_global_guid, varname) < 0) {
		if (errno == ENOENT)
			return;
		err(1, "delete %s", varname);
	}
}

static void
do_set_active(uint16_t bootnum, int active)
{
	char name[16];
	uint8_t *data = NULL;
	size_t sz = 0;
	efi_load_option_t lo;
	uint8_t *newbuf = NULL;
	size_t newsz = 0;

	(void) snprintf(name, sizeof (name), "Boot%04X", bootnum);

	if (efi_get_variable(efi_global_guid, name, &data, &sz, NULL) < 0)
		err(1, "%s", name);
	if (efi_load_option_parse(data, sz, &lo) < 0) {
		free(data);
		err(1, "parse %s", name);
	}
	free(data);

	if (active)
		lo.elo_attributes |= EFI_LOAD_OPTION_ACTIVE;
	else
		lo.elo_attributes &= ~EFI_LOAD_OPTION_ACTIVE;

	if (efi_load_option_build(&lo, &newbuf, &newsz) < 0) {
		efi_load_option_clear(&lo);
		err(1, "rebuild %s", name);
	}
	efi_load_option_clear(&lo);

	if (efi_set_variable(efi_global_guid, name, newbuf, newsz,
	    EFI_VAR_NV_BS_RT) < 0) {
		free(newbuf);
		err(1, "write %s", name);
	}
	free(newbuf);
}

static void
usage(FILE *fp)
{
	(void) fprintf(fp,
	    "usage: efibootmgr [-v]\n"
	    "       efibootmgr -o NNNN[,NNNN...] | -O\n"
	    "       efibootmgr -n NNNN | -N\n"
	    "       efibootmgr -t SECONDS | -T\n"
	    "       efibootmgr {-a | -A} -b NNNN\n"
	    "Boot entry numbers are four hexadecimal digits.\n");
}

int
main(int argc, char **argv)
{
	int ch;
	int verbose = 0;
	const char *new_bootorder = NULL;
	int clear_bootorder = 0;
	int new_bootnext = -1;
	int clear_bootnext = 0;
	int new_timeout = -1;
	int clear_timeout = 0;
	int active_op = 0;
	uint16_t bootnum = 0;
	int bootnum_seen = 0;
	uint16_t parsed;

	while ((ch = getopt(argc, argv, "vo:On:Nt:Tab:A")) != -1) {
		switch (ch) {
		case 'v':
			verbose = 1;
			break;
		case 'o':
			new_bootorder = optarg;
			break;
		case 'O':
			clear_bootorder = 1;
			break;
		case 'n':
			if (parse_bootnum(optarg, &parsed) < 0)
				errx(2, "invalid -n: %s", optarg);
			new_bootnext = parsed;
			break;
		case 'N':
			clear_bootnext = 1;
			break;
		case 't': {
			char *end;
			unsigned long v;
			errno = 0;
			v = strtoul(optarg, &end, 0);
			if (errno != 0 || *end != '\0' || v > UINT16_MAX)
				errx(2, "invalid -t: %s", optarg);
			new_timeout = (int)v;
			break;
		}
		case 'T':
			clear_timeout = 1;
			break;
		case 'a':
			active_op = 1;
			break;
		case 'A':
			active_op = -1;
			break;
		case 'b':
			if (parse_bootnum(optarg, &parsed) < 0)
				errx(2, "invalid -b: %s", optarg);
			bootnum = parsed;
			bootnum_seen = 1;
			break;
		default:
			usage(stderr);
			return (2);
		}
	}
	if (optind != argc) {
		usage(stderr);
		return (2);
	}

	if (efi_variables_supported() < 0)
		err(1, "/dev/efi");

	if (new_bootorder != NULL)
		do_set_bootorder(new_bootorder);
	if (clear_bootorder)
		do_del("BootOrder");
	if (new_bootnext >= 0)
		do_set_uint16("BootNext", (uint16_t)new_bootnext);
	if (clear_bootnext)
		do_del("BootNext");
	if (new_timeout >= 0)
		do_set_uint16("Timeout", (uint16_t)new_timeout);
	if (clear_timeout)
		do_del("Timeout");
	if (active_op != 0) {
		if (!bootnum_seen)
			errx(2, "-a / -A require -b NNNN");
		do_set_active(bootnum, active_op > 0);
	}

	do_list(verbose);
	return (0);
}
