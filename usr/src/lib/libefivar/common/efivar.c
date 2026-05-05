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
 * libefivar -- thin userland wrapper around the efi(7D) ioctls.
 *
 * The kernel side speaks the FreeBSD-style EFIIOC_VAR_GET / SET / NEXT
 * interface (see <sys/efiio.h>) which carries a UCS-2 variable name.
 * Boot-manager and platform variables are always ASCII in practice,
 * so this library converts to and from UTF-8 along the ASCII fast
 * path: any code point above 0x7f is rejected on input (EILSEQ) and
 * rendered as '?' on output. A future commit can extend the helpers
 * to the full BMP without touching the public API.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/efiio.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>

#include <libefivar.h>

/* UEFI 2.10 caps a name at 1024 CHAR16; allow that much. */
#define	EFI_NAME_BUF_CHARS	1024

/*
 * One file descriptor per process is enough: every ioctl is
 * stateless and the driver serialises calls internally. Not
 * thread-safe at present.
 */
static int efivar_fd = -1;

static int
efivar_open(void)
{
	if (efivar_fd >= 0)
		return (efivar_fd);
	efivar_fd = open("/dev/efi", O_RDWR);
	return (efivar_fd);
}

int
efi_variables_supported(void)
{
	if (efivar_open() < 0)
		return (-1);
	return (0);
}

/*
 * Convert a NUL-terminated ASCII string into a freshly malloc()ed
 * NUL-terminated UCS-2 buffer. *dstsize_p is the byte count (including
 * the trailing NUL).
 */
static int
ascii_to_ucs2(const char *src, uint16_t **dst_p, size_t *dstsize_p)
{
	size_t len = strlen(src);
	size_t i;
	uint16_t *p;

	p = malloc((len + 1) * sizeof (uint16_t));
	if (p == NULL)
		return (-1);
	for (i = 0; i < len; i++) {
		unsigned char c = (unsigned char)src[i];
		if (c >= 0x80) {
			free(p);
			errno = EILSEQ;
			return (-1);
		}
		p[i] = c;
	}
	p[len] = 0;
	*dst_p = p;
	*dstsize_p = (len + 1) * sizeof (uint16_t);
	return (0);
}

/*
 * Convert a UCS-2 buffer into a freshly malloc()ed UTF-8 string.
 * srcsize is the number of bytes in src (which may or may not include
 * a trailing NUL). Code points above 0x7f are replaced by '?'.
 */
static int
ucs2_to_ascii(const uint16_t *src, size_t srcsize, char **dst_p)
{
	size_t nchar = srcsize / sizeof (uint16_t);
	size_t i;
	char *d;

	if (nchar > 0 && src[nchar - 1] == 0)
		nchar--;

	d = malloc(nchar + 1);
	if (d == NULL)
		return (-1);
	for (i = 0; i < nchar; i++) {
		uint16_t c = src[i];
		if (c == 0) {
			free(d);
			errno = EILSEQ;
			return (-1);
		}
		d[i] = (c < 0x80) ? (char)c : '?';
	}
	d[nchar] = '\0';
	*dst_p = d;
	return (0);
}

int
efi_get_variable(efi_guid_t vendor, const char *name, uint8_t **data_p,
    size_t *datasize_p, uint32_t *attrib_p)
{
	int fd;
	uint16_t *name16 = NULL;
	size_t name16size;
	struct efi_var_ioc v;
	uint8_t *buf = NULL;
	int saved;

	if ((fd = efivar_open()) < 0)
		return (-1);
	if (ascii_to_ucs2(name, &name16, &name16size) < 0)
		return (-1);

	bzero(&v, sizeof (v));
	v.name = name16;
	v.namesize = name16size;
	v.vendor = vendor;

	/*
	 * Probe with a zero-length output buffer first. The driver
	 * returns EOVERFLOW with v.datasize set to the required size;
	 * any other error is fatal. A zero-length variable yields
	 * success on the first call.
	 */
	if (ioctl(fd, EFIIOC_VAR_GET, &v) < 0) {
		if (errno != EOVERFLOW) {
			saved = errno;
			free(name16);
			errno = saved;
			return (-1);
		}
		buf = malloc(v.datasize);
		if (buf == NULL) {
			saved = errno;
			free(name16);
			errno = saved;
			return (-1);
		}
		v.data = buf;
		if (ioctl(fd, EFIIOC_VAR_GET, &v) < 0) {
			saved = errno;
			free(buf);
			free(name16);
			errno = saved;
			return (-1);
		}
		*data_p = buf;
		*datasize_p = v.datasize;
	} else {
		*data_p = NULL;
		*datasize_p = 0;
	}
	if (attrib_p != NULL)
		*attrib_p = v.attrib;
	free(name16);
	return (0);
}

int
efi_set_variable(efi_guid_t vendor, const char *name, const uint8_t *data,
    size_t datasize, uint32_t attrib)
{
	int fd;
	uint16_t *name16;
	size_t name16size;
	struct efi_var_ioc v;
	int rc, saved;

	if ((fd = efivar_open()) < 0)
		return (-1);
	if (ascii_to_ucs2(name, &name16, &name16size) < 0)
		return (-1);

	bzero(&v, sizeof (v));
	v.name = name16;
	v.namesize = name16size;
	v.vendor = vendor;
	v.attrib = attrib;
	v.data = (void *)(uintptr_t)data;
	v.datasize = datasize;

	rc = ioctl(fd, EFIIOC_VAR_SET, &v);
	saved = errno;
	free(name16);
	errno = saved;
	return (rc);
}

int
efi_del_variable(efi_guid_t vendor, const char *name)
{
	return (efi_set_variable(vendor, name, NULL, 0, 0));
}

int
efi_get_next_variable_name(efi_guid_t *vendor, char **name_p)
{
	int fd;
	uint16_t name16[EFI_NAME_BUF_CHARS];
	struct efi_var_ioc v;
	char *new_name = NULL;
	int saved;

	if ((fd = efivar_open()) < 0)
		return (-1);

	bzero(&v, sizeof (v));
	bzero(name16, sizeof (name16));

	/*
	 * If the caller is continuing iteration we seed the buffer
	 * with the previous name so the firmware returns the next
	 * one; on the first call *name_p is NULL and the empty
	 * buffer asks for "the first variable".
	 */
	if (*name_p != NULL) {
		uint16_t *seed;
		size_t seedsize;

		if (ascii_to_ucs2(*name_p, &seed, &seedsize) < 0)
			return (-1);
		if (seedsize > sizeof (name16)) {
			free(seed);
			errno = ENAMETOOLONG;
			return (-1);
		}
		(void) memcpy(name16, seed, seedsize);
		free(seed);
		v.vendor = *vendor;
	}

	v.name = name16;
	v.namesize = sizeof (name16);

	if (ioctl(fd, EFIIOC_VAR_NEXT, &v) < 0) {
		if (errno == ENOENT) {
			free(*name_p);
			*name_p = NULL;
		}
		return (-1);
	}

	if (ucs2_to_ascii(name16, v.namesize, &new_name) < 0) {
		saved = errno;
		errno = saved;
		return (-1);
	}
	free(*name_p);
	*name_p = new_name;
	*vendor = v.vendor;
	return (0);
}
