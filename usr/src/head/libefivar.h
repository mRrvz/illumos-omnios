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
 * Userland interface to UEFI variables.
 *
 * Each variable is identified by a (vendor GUID, name) pair. Names in
 * UEFI are CHAR16 strings; this library accepts and returns UTF-8 and
 * does the conversion internally. Variables whose names contain code
 * points outside the basic ASCII range are not currently supported by
 * this library: boot-manager and platform variables in practice are
 * always ASCII.
 *
 * All routines return 0 on success or -1 with errno set. Notable errno
 * values: ENOENT means the variable does not exist (or, for the
 * iterator, that there are no more entries); EACCES means /dev/efi
 * gated the request behind PRIV_SYS_CONFIG; ENOTSUP means UEFI
 * Runtime Services are not available on this system.
 */

#ifndef	_LIBEFIVAR_H
#define	_LIBEFIVAR_H

#include <sys/types.h>
#include <uuid/uuid.h>

#ifdef	__cplusplus
extern "C" {
#endif

typedef struct uuid efi_guid_t;

/*
 * UEFI 2.10 variable attribute bits used by callers that need to
 * preserve them across a get/set.
 */
#define	EFI_VARIABLE_NON_VOLATILE			0x00000001
#define	EFI_VARIABLE_BOOTSERVICE_ACCESS			0x00000002
#define	EFI_VARIABLE_RUNTIME_ACCESS			0x00000004
#define	EFI_VARIABLE_HARDWARE_ERROR_RECORD		0x00000008
#define	EFI_VARIABLE_AUTHENTICATED_WRITE_ACCESS		0x00000010
#define	EFI_VARIABLE_TIME_BASED_AUTHENTICATED_WRITE_ACCESS \
							0x00000020
#define	EFI_VARIABLE_APPEND_WRITE			0x00000040

/*
 * Quick probe: returns 0 if /dev/efi is openable and the kernel reports
 * UEFI Runtime Services as available, -1 with errno otherwise. Useful
 * for utilities that want to fail early on legacy-BIOS systems.
 */
extern int efi_variables_supported(void);

/*
 * Read a variable.  On success, *data is a malloc()ed buffer of
 * *datasize bytes that the caller must free(); *attrib (if non-NULL)
 * receives the UEFI attribute mask. On ENOENT *data is left
 * unchanged.
 */
extern int efi_get_variable(efi_guid_t vendor, const char *name,
    uint8_t **data, size_t *datasize, uint32_t *attrib);

/*
 * Write a variable. A datasize of 0 deletes the variable per UEFI
 * 8.2.3.
 */
extern int efi_set_variable(efi_guid_t vendor, const char *name,
    const uint8_t *data, size_t datasize, uint32_t attrib);

/*
 * Delete a variable. Equivalent to efi_set_variable(..., NULL, 0, 0).
 */
extern int efi_del_variable(efi_guid_t vendor, const char *name);

/*
 * Iterate the firmware's variable namespace. On the first call *name
 * must be NULL; on subsequent calls pass the values returned by the
 * previous call. The library frees the previous *name and replaces it
 * with a freshly malloc()ed UTF-8 name. End of iteration is reported
 * by -1 with errno == ENOENT, at which point *name is freed and set to
 * NULL. The caller is responsible for free()ing *name when aborting
 * iteration early.
 */
extern int efi_get_next_variable_name(efi_guid_t *vendor, char **name);

#ifdef	__cplusplus
}
#endif

#endif	/* _LIBEFIVAR_H */
