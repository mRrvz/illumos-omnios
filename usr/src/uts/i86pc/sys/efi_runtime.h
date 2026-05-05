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
 * Private kernel-internal API for the efi(7D) driver.
 *
 * efi_pt_t is a freestanding amd64 4-level page table whose high half
 * is a snapshot of the kernel's PML4 (so kernel virtual addresses
 * keep working after CR3 is loaded with this table) and whose low
 * half holds 1:1 identity mappings for the EFI runtime ranges.
 * Switching CR3 to efi_pt_root_pa() lets the CPU dispatch through
 * Runtime Services function pointers that hold physical addresses,
 * while leaving kernel code, data and the current thread's stack
 * still reachable.
 */

#ifndef	_SYS_EFI_RUNTIME_H
#define	_SYS_EFI_RUNTIME_H

#include <sys/types.h>

#ifdef	__cplusplus
extern "C" {
#endif

#ifdef	_KERNEL

typedef struct efi_pt efi_pt_t;

/*
 * Page table lifecycle.
 *
 * efi_pt_create() allocates a new PML4, copies the kernel half of the
 * current CR3 into entries 256..511 and returns 0.  efi_pt_destroy()
 * releases the PML4 along with every PDPT/PD/PT page that was added
 * via efi_pt_map().
 */
extern int efi_pt_create(efi_pt_t **);
extern void efi_pt_destroy(efi_pt_t *);

/*
 * Identity-map [pa, pa + size) into the table with the supplied PTE
 * permission flags (PT_WRITABLE, PT_NX, ...).  PT_VALID is always
 * applied; pa and size are rounded to PAGESIZE.  Returns 0 or an errno.
 */
extern int efi_pt_map(efi_pt_t *, paddr_t pa, size_t size, uint64_t flags);

/*
 * Walk the EFI memory map exposed by fakebop on the root node
 * (properties "efi-mmap" / "efi-mmap-descsize") and add an identity
 * mapping for every descriptor that carries the EFI_MEMORY_RUNTIME
 * attribute.  Returns 0 on success, ENOENT if the property is missing,
 * or EINVAL/ENOMEM as appropriate.  *nrangesp is set to the number of
 * ranges that were actually mapped.
 */
extern int efi_pt_apply_mmap(efi_pt_t *, uint_t *nrangesp);

/*
 * Physical address suitable for setcr3().
 */
extern paddr_t efi_pt_root_pa(const efi_pt_t *);

#endif	/* _KERNEL */

#ifdef	__cplusplus
}
#endif

#endif	/* _SYS_EFI_RUNTIME_H */
