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
#include <fcntl.h>
#include <inttypes.h>
#include <libefivar.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/efi_partition.h>
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
print_osindications(void)
{
	uint8_t *data = NULL;
	size_t sz = 0;
	uint64_t v = 0;

	if (efi_get_variable(efi_global_guid, "OsIndications", &data, &sz,
	    NULL) < 0)
		return;
	if (sz == sizeof (uint64_t)) {
		(void) memcpy(&v, data, sizeof (v));
		(void) printf("OsIndications: 0x%016" PRIx64, v);
		if (v & EFI_OS_INDICATIONS_BOOT_TO_FW_UI)
			(void) printf(" BOOT_TO_FW_UI");
		(void) printf("\n");
	}
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
	print_osindications();
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

/*
 * Open a raw disk for GPT inspection. The argument is either an
 * absolute path that we open as is, or a controller-style name that
 * we translate to /dev/rdsk/<arg>p0 (the conventional whole-disk
 * raw node on i86pc).
 */
static int
open_raw_disk(const char *spec)
{
	char path[PATH_MAX];
	int fd;

	if (spec[0] == '/')
		return (open(spec, O_RDONLY));
	(void) snprintf(path, sizeof (path), "/dev/rdsk/%sp0", spec);
	fd = open(path, O_RDONLY);
	if (fd < 0 && errno == ENOENT) {
		(void) snprintf(path, sizeof (path), "/dev/rdsk/%s", spec);
		fd = open(path, O_RDONLY);
	}
	return (fd);
}

/*
 * Walk the variables in the global namespace and report the lowest
 * BootNNNN slot that is currently free, returning it in *out. Returns
 * 0 on success or -1 with errno set.
 */
static int
find_free_bootnum(uint16_t *out)
{
	uint8_t taken[0x10000 / 8];
	char *name = NULL;
	efi_guid_t vendor;
	uint32_t i;

	bzero(taken, sizeof (taken));

	while (efi_get_next_variable_name(&vendor, &name) == 0) {
		unsigned long v;
		if (!guid_eq(&vendor, &efi_global_guid))
			continue;
		if (!is_boot_var_name(name))
			continue;
		v = strtoul(name + 4, NULL, 16);
		taken[v / 8] |= 1u << (v % 8);
	}
	if (errno != ENOENT)
		return (-1);

	for (i = 0; i < 0x10000; i++) {
		if ((taken[i / 8] & (1u << (i % 8))) == 0) {
			*out = (uint16_t)i;
			return (0);
		}
	}
	errno = ENOSPC;
	return (-1);
}

/*
 * Insert a bootnum at the head of the BootOrder array (creating it if
 * absent). Existing duplicates of the same number are removed first.
 */
static void
bootorder_prepend(uint16_t bootnum)
{
	uint8_t *data = NULL;
	size_t sz = 0;
	uint16_t *cur;
	size_t ncur, i;
	uint16_t *new;
	size_t nnew = 0;

	if (efi_get_variable(efi_global_guid, "BootOrder", &data, &sz,
	    NULL) < 0) {
		if (errno != ENOENT)
			err(1, "read BootOrder");
		sz = 0;
	}
	cur = (uint16_t *)data;
	ncur = sz / sizeof (uint16_t);

	new = malloc((ncur + 1) * sizeof (uint16_t));
	if (new == NULL)
		err(1, "malloc");
	new[nnew++] = bootnum;
	for (i = 0; i < ncur; i++) {
		if (cur[i] != bootnum)
			new[nnew++] = cur[i];
	}
	free(data);

	if (efi_set_variable(efi_global_guid, "BootOrder", (uint8_t *)new,
	    nnew * sizeof (uint16_t), EFI_VAR_NV_BS_RT) < 0) {
		free(new);
		err(1, "write BootOrder");
	}
	free(new);
}

/*
 * Remove a bootnum from BootOrder. No-op if BootOrder is absent or
 * the number is not present.
 */
static void
bootorder_remove(uint16_t bootnum)
{
	uint8_t *data = NULL;
	size_t sz = 0;
	uint16_t *cur;
	size_t ncur, i, nnew = 0;
	uint16_t *new;

	if (efi_get_variable(efi_global_guid, "BootOrder", &data, &sz,
	    NULL) < 0) {
		if (errno == ENOENT)
			return;
		err(1, "read BootOrder");
	}
	cur = (uint16_t *)data;
	ncur = sz / sizeof (uint16_t);

	new = malloc(ncur * sizeof (uint16_t));
	if (new == NULL)
		err(1, "malloc");
	for (i = 0; i < ncur; i++) {
		if (cur[i] != bootnum)
			new[nnew++] = cur[i];
	}
	free(data);

	if (nnew == ncur) {
		free(new);
		return;
	}

	if (efi_set_variable(efi_global_guid, "BootOrder", (uint8_t *)new,
	    nnew * sizeof (uint16_t), EFI_VAR_NV_BS_RT) < 0) {
		free(new);
		err(1, "write BootOrder");
	}
	free(new);
}

static void
do_create(const char *disk, int partnum, const char *label,
    const char *loader)
{
	int fd;
	struct dk_gpt *gpt = NULL;
	struct dk_part *part;
	uint8_t signature[16];
	efi_devpath_builder_t *b;
	uint8_t *devpath = NULL;
	size_t devpath_size = 0;
	efi_load_option_t lo;
	uint8_t *blob = NULL;
	size_t blob_size = 0;
	uint16_t bootnum;
	char varname[16];

	if (disk == NULL || loader == NULL || partnum <= 0)
		errx(2, "-c needs -d, -p and -l");
	if (label == NULL)
		label = "OmniOS";

	fd = open_raw_disk(disk);
	if (fd < 0)
		err(1, "open %s", disk);
	if (efi_alloc_and_read(fd, &gpt) < 0) {
		(void) close(fd);
		errx(1, "%s does not have a GPT label", disk);
	}
	(void) close(fd);

	if ((uint_t)(partnum - 1) >= gpt->efi_nparts) {
		efi_free(gpt);
		errx(1, "partition %d is out of range (1..%u)", partnum,
		    gpt->efi_nparts);
	}
	part = &gpt->efi_parts[partnum - 1];

	(void) memcpy(signature, &part->p_uguid, sizeof (signature));

	b = efi_devpath_new();
	if (b == NULL)
		err(1, "devpath builder");
	if (efi_devpath_append_hd(b, (uint32_t)partnum,
	    (uint64_t)part->p_start, (uint64_t)part->p_size, signature,
	    EFI_DEVPATH_HD_MBR_TYPE_GPT,
	    EFI_DEVPATH_HD_SIG_TYPE_GUID) < 0) {
		efi_devpath_free(b);
		efi_free(gpt);
		err(1, "devpath HD");
	}
	if (efi_devpath_append_file(b, loader) < 0) {
		efi_devpath_free(b);
		efi_free(gpt);
		err(1, "devpath File");
	}
	if (efi_devpath_finalize(b, &devpath, &devpath_size) < 0) {
		efi_devpath_free(b);
		efi_free(gpt);
		err(1, "devpath finalize");
	}
	efi_devpath_free(b);
	efi_free(gpt);

	bzero(&lo, sizeof (lo));
	lo.elo_attributes = EFI_LOAD_OPTION_ACTIVE;
	lo.elo_description = (char *)label;
	lo.elo_device_path = devpath;
	lo.elo_device_path_size = devpath_size;

	if (efi_load_option_build(&lo, &blob, &blob_size) < 0) {
		free(devpath);
		err(1, "build load option");
	}
	free(devpath);

	if (find_free_bootnum(&bootnum) < 0) {
		free(blob);
		err(1, "find free Boot####");
	}
	(void) snprintf(varname, sizeof (varname), "Boot%04X", bootnum);

	if (efi_set_variable(efi_global_guid, varname, blob, blob_size,
	    EFI_VAR_NV_BS_RT) < 0) {
		free(blob);
		err(1, "write %s", varname);
	}
	free(blob);

	bootorder_prepend(bootnum);
}

static void
do_delete(uint16_t bootnum)
{
	char name[16];

	(void) snprintf(name, sizeof (name), "Boot%04X", bootnum);
	if (efi_del_variable(efi_global_guid, name) < 0 && errno != ENOENT)
		err(1, "delete %s", name);
	bootorder_remove(bootnum);
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
	    "       efibootmgr -c -d disk -p partnum -l loader [-L label]\n"
	    "       efibootmgr -B -b NNNN\n"
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
	int create = 0;
	int del = 0;
	const char *disk = NULL;
	const char *loader = NULL;
	const char *label = NULL;
	int partnum = 0;
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

	while ((ch = getopt(argc, argv, "vcBd:p:l:L:o:On:Nt:Tab:A")) != -1) {
		switch (ch) {
		case 'v':
			verbose = 1;
			break;
		case 'c':
			create = 1;
			break;
		case 'B':
			del = 1;
			break;
		case 'd':
			disk = optarg;
			break;
		case 'p': {
			char *end;
			unsigned long v;
			errno = 0;
			v = strtoul(optarg, &end, 0);
			if (errno != 0 || *end != '\0' || v == 0 ||
			    v > INT_MAX)
				errx(2, "invalid -p: %s", optarg);
			partnum = (int)v;
			break;
		}
		case 'l':
			loader = optarg;
			break;
		case 'L':
			label = optarg;
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
	if (create && del) {
		usage(stderr);
		return (2);
	}

	if (efi_variables_supported() < 0)
		err(1, "/dev/efi");

	if (create)
		do_create(disk, partnum, label, loader);
	if (del) {
		if (!bootnum_seen)
			errx(2, "-B requires -b NNNN");
		do_delete(bootnum);
	}
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
