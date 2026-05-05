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
 * EFI_DEVICE_PATH builder, parser and pretty-printer.
 *
 * EFI_DEVICE_PATH is a flat sequence of nodes; each node carries a
 * one-byte type, a one-byte sub-type, a little-endian two-byte length
 * (covering the node header and its payload) and a type-specific
 * payload.  The end of an entire device path is signalled by a node of
 * type 0x7f sub-type 0xff length 4.
 *
 * For now only the two media nodes that boot-manager entries always
 * use are supported in the builder and the printer:
 *
 *   - Hard Drive (type 4 / sub-type 1, UEFI 2.10 9.3.5.1):
 *       partition number, LBA start, LBA size, signature[16],
 *       MBR/GPT type, signature type.
 *   - File Path (type 4 / sub-type 4, UEFI 2.10 9.3.6.4):
 *       NUL-terminated UCS-2 path with backslashes as separators.
 *
 * The printer also accepts everything else and emits a generic
 * "Unk(<type>,<sub>,<len>)" stand-in.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/uuid.h>
#include <uuid/uuid.h>

#include <libefivar.h>

#include "libefivar_impl.h"

#define	EFI_DP_TYPE_END		0x7f
#define	EFI_DP_TYPE_MEDIA	0x04

#define	EFI_DP_SUB_END_ENTIRE	0xff
#define	EFI_DP_SUB_MEDIA_HD	0x01
#define	EFI_DP_SUB_MEDIA_FILE	0x04

#define	EFI_DP_END_LEN		4U

#pragma pack(1)
struct efi_dp_node_hdr {
	uint8_t		dp_type;
	uint8_t		dp_subtype;
	uint8_t		dp_length[2];	/* LE, full node */
};

struct efi_dp_hd {
	struct efi_dp_node_hdr	hd_hdr;
	uint8_t			hd_partnum[4];
	uint8_t			hd_partstart[8];
	uint8_t			hd_partsize[8];
	uint8_t			hd_signature[16];
	uint8_t			hd_mbr_type;
	uint8_t			hd_signature_type;
};
#pragma pack()

#define	EFI_DP_HD_LEN	((uint16_t)sizeof (struct efi_dp_hd))

struct efi_devpath_builder {
	uint8_t		*db_buf;
	size_t		db_len;
	size_t		db_cap;
	int		db_finalised;
};

/* Little-endian helpers (host is amd64 / little-endian). */
static uint16_t
le16r(const uint8_t *p)
{
	uint16_t v;
	(void) memcpy(&v, p, sizeof (v));
	return (v);
}

static void
le16w(uint8_t *p, uint16_t v)
{
	(void) memcpy(p, &v, sizeof (v));
}

static void
le32w(uint8_t *p, uint32_t v)
{
	(void) memcpy(p, &v, sizeof (v));
}

static void
le64w(uint8_t *p, uint64_t v)
{
	(void) memcpy(p, &v, sizeof (v));
}

static int
efi_devpath_grow(efi_devpath_builder_t *b, size_t need)
{
	size_t newcap = b->db_cap;
	uint8_t *p;

	if (b->db_len + need <= b->db_cap)
		return (0);
	if (newcap == 0)
		newcap = 64;
	while (newcap < b->db_len + need) {
		if (newcap > SIZE_MAX / 2) {
			errno = ENOMEM;
			return (-1);
		}
		newcap *= 2;
	}
	p = realloc(b->db_buf, newcap);
	if (p == NULL)
		return (-1);
	b->db_buf = p;
	b->db_cap = newcap;
	return (0);
}

efi_devpath_builder_t *
efi_devpath_new(void)
{
	efi_devpath_builder_t *b;

	b = calloc(1, sizeof (*b));
	return (b);
}

void
efi_devpath_free(efi_devpath_builder_t *b)
{
	if (b == NULL)
		return;
	free(b->db_buf);
	free(b);
}

int
efi_devpath_append_hd(efi_devpath_builder_t *b, uint32_t partnum,
    uint64_t partstart, uint64_t partsize, const uint8_t signature[16],
    uint8_t mbr_type, uint8_t signature_type)
{
	struct efi_dp_hd *n;

	if (b == NULL || b->db_finalised) {
		errno = EINVAL;
		return (-1);
	}
	if (efi_devpath_grow(b, sizeof (*n)) != 0)
		return (-1);

	n = (struct efi_dp_hd *)(b->db_buf + b->db_len);
	n->hd_hdr.dp_type = EFI_DP_TYPE_MEDIA;
	n->hd_hdr.dp_subtype = EFI_DP_SUB_MEDIA_HD;
	le16w(n->hd_hdr.dp_length, EFI_DP_HD_LEN);
	le32w(n->hd_partnum, partnum);
	le64w(n->hd_partstart, partstart);
	le64w(n->hd_partsize, partsize);
	if (signature != NULL)
		(void) memcpy(n->hd_signature, signature, 16);
	else
		bzero(n->hd_signature, 16);
	n->hd_mbr_type = mbr_type;
	n->hd_signature_type = signature_type;

	b->db_len += sizeof (*n);
	return (0);
}

int
efi_devpath_append_file(efi_devpath_builder_t *b, const char *utf8_path)
{
	uint16_t *path16 = NULL;
	size_t path16_size = 0;
	size_t node_size;
	uint8_t *p;
	int saved;

	if (b == NULL || b->db_finalised || utf8_path == NULL) {
		errno = EINVAL;
		return (-1);
	}

	if (_efivar_ascii_to_ucs2(utf8_path, &path16, &path16_size) < 0)
		return (-1);

	if (path16_size > UINT16_MAX - sizeof (struct efi_dp_node_hdr)) {
		free(path16);
		errno = EINVAL;
		return (-1);
	}
	node_size = sizeof (struct efi_dp_node_hdr) + path16_size;

	if (efi_devpath_grow(b, node_size) != 0) {
		saved = errno;
		free(path16);
		errno = saved;
		return (-1);
	}

	p = b->db_buf + b->db_len;
	*p++ = EFI_DP_TYPE_MEDIA;
	*p++ = EFI_DP_SUB_MEDIA_FILE;
	le16w(p, (uint16_t)node_size);
	p += sizeof (uint16_t);
	(void) memcpy(p, path16, path16_size);

	b->db_len += node_size;
	free(path16);
	return (0);
}

int
efi_devpath_finalize(efi_devpath_builder_t *b, uint8_t **out_p,
    size_t *outsize_p)
{
	uint8_t end[EFI_DP_END_LEN];
	uint8_t *out;

	if (b == NULL || b->db_finalised) {
		errno = EINVAL;
		return (-1);
	}

	end[0] = EFI_DP_TYPE_END;
	end[1] = EFI_DP_SUB_END_ENTIRE;
	end[2] = EFI_DP_END_LEN;
	end[3] = 0;

	if (efi_devpath_grow(b, sizeof (end)) != 0)
		return (-1);
	(void) memcpy(b->db_buf + b->db_len, end, sizeof (end));
	b->db_len += sizeof (end);

	out = realloc(b->db_buf, b->db_len);
	if (out == NULL) {
		/* keep the original buffer; caller can still free us */
		return (-1);
	}
	*out_p = out;
	*outsize_p = b->db_len;
	b->db_buf = NULL;
	b->db_len = 0;
	b->db_cap = 0;
	b->db_finalised = 1;
	return (0);
}

/*
 * Pretty-printer. Walks each node and concatenates its rendering into
 * a malloc()ed dynamic buffer. Unknown nodes use a generic shape.
 */

struct sbuf {
	char	*s_buf;
	size_t	s_len;
	size_t	s_cap;
};

static int
sbuf_reserve(struct sbuf *s, size_t need)
{
	size_t newcap = s->s_cap;
	char *p;

	if (s->s_len + need + 1 <= s->s_cap)
		return (0);
	if (newcap == 0)
		newcap = 64;
	while (newcap < s->s_len + need + 1) {
		if (newcap > SIZE_MAX / 2) {
			errno = ENOMEM;
			return (-1);
		}
		newcap *= 2;
	}
	p = realloc(s->s_buf, newcap);
	if (p == NULL)
		return (-1);
	s->s_buf = p;
	s->s_cap = newcap;
	return (0);
}

static int
sbuf_appendf(struct sbuf *s, const char *fmt, ...)
{
	va_list ap;
	int n;
	size_t avail;

	for (;;) {
		avail = s->s_cap - s->s_len;
		va_start(ap, fmt);
		n = vsnprintf(s->s_buf + s->s_len, avail, fmt, ap);
		va_end(ap);
		if (n < 0)
			return (-1);
		if ((size_t)n < avail) {
			s->s_len += n;
			return (0);
		}
		if (sbuf_reserve(s, (size_t)n) != 0)
			return (-1);
	}
}

static int
sbuf_append(struct sbuf *s, const char *str)
{
	size_t len = strlen(str);

	if (sbuf_reserve(s, len) != 0)
		return (-1);
	(void) memcpy(s->s_buf + s->s_len, str, len);
	s->s_len += len;
	s->s_buf[s->s_len] = '\0';
	return (0);
}

static int
print_hd(struct sbuf *s, const uint8_t *node)
{
	const struct efi_dp_hd *hd = (const struct efi_dp_hd *)node;
	uint32_t partnum;
	uint64_t partstart, partsize;
	char guid[UUID_PRINTABLE_STRING_LENGTH];
	uuid_t u;

	(void) memcpy(&partnum, hd->hd_partnum, sizeof (partnum));
	(void) memcpy(&partstart, hd->hd_partstart, sizeof (partstart));
	(void) memcpy(&partsize, hd->hd_partsize, sizeof (partsize));

	if (hd->hd_signature_type == EFI_DEVPATH_HD_SIG_TYPE_GUID) {
		(void) memcpy(u, hd->hd_signature, sizeof (u));
		uuid_unparse_lower(u, guid);
		return (sbuf_appendf(s,
		    "HD(%u,GPT,%s,0x%" PRIx64 ",0x%" PRIx64 ")",
		    partnum, guid, partstart, partsize));
	}
	return (sbuf_appendf(s,
	    "HD(%u,MBR,0x%02x%02x%02x%02x,0x%" PRIx64 ",0x%" PRIx64 ")",
	    partnum, hd->hd_signature[3], hd->hd_signature[2],
	    hd->hd_signature[1], hd->hd_signature[0],
	    partstart, partsize));
}

static int
print_file(struct sbuf *s, const uint8_t *node, uint16_t length)
{
	const uint8_t *payload = node + sizeof (struct efi_dp_node_hdr);
	size_t plen;
	char *path = NULL;
	int rc;

	if (length < sizeof (struct efi_dp_node_hdr)) {
		errno = EINVAL;
		return (-1);
	}
	plen = length - sizeof (struct efi_dp_node_hdr);
	if (_efivar_ucs2_to_ascii((const uint16_t *)payload, plen, &path) < 0)
		return (-1);
	rc = sbuf_appendf(s, "File(%s)", path);
	free(path);
	return (rc);
}

int
efi_devpath_to_string(const uint8_t *path, size_t pathsize, char **out)
{
	struct sbuf s;
	size_t off = 0;
	int first = 1;
	int rc = -1;

	bzero(&s, sizeof (s));
	if (sbuf_reserve(&s, 1) != 0)
		goto done;
	s.s_buf[0] = '\0';

	while (off + sizeof (struct efi_dp_node_hdr) <= pathsize) {
		const uint8_t *node = path + off;
		uint8_t type = node[0];
		uint8_t sub = node[1];
		uint16_t len = le16r(node + 2);

		if (len < sizeof (struct efi_dp_node_hdr) ||
		    off + len > pathsize) {
			errno = EINVAL;
			goto done;
		}

		if (type == EFI_DP_TYPE_END)
			break;

		if (!first && sbuf_append(&s, "/") != 0)
			goto done;
		first = 0;

		if (type == EFI_DP_TYPE_MEDIA && sub == EFI_DP_SUB_MEDIA_HD &&
		    len >= EFI_DP_HD_LEN) {
			if (print_hd(&s, node) != 0)
				goto done;
		} else if (type == EFI_DP_TYPE_MEDIA &&
		    sub == EFI_DP_SUB_MEDIA_FILE) {
			if (print_file(&s, node, len) != 0)
				goto done;
		} else {
			if (sbuf_appendf(&s, "Unk(%u,%u,%u)", type, sub,
			    len) != 0)
				goto done;
		}

		off += len;
	}

	*out = s.s_buf;
	s.s_buf = NULL;
	rc = 0;
done:
	free(s.s_buf);
	return (rc);
}
