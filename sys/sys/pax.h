/*-
 * Copyright (c) 2006 Elad Efrat <elad@NetBSD.org>
 * Copyright (c) 2013-2015, by Oliver Pinter <oliver.pinter@hardenedbsd.org>
 * Copyright (c) 2014-2015, by Shawn Webb <shawn.webb@hardenedbsd.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * $FreeBSD$
 *
 * HardenedBSD-version: f3999cdf0bf50578e038f10b952ffc1c446d8927
 *
 */

#ifndef	__SYS_PAX_H
#define	__SYS_PAX_H

#if defined(_KERNEL) || defined(_WANT_PRISON)
struct hardening_features {
	int	 hr_pax_aslr_status;		/* (p) PaX ASLR enabled */
	int	 hr_pax_aslr_mmap_len;		/* (p) Number of bits randomized with mmap */
	int	 hr_pax_aslr_stack_len;		/* (p) Number of bits randomized with stack */
	int	 hr_pax_aslr_exec_len;		/* (p) Number of bits randomized with the execbase */
	int	 hr_pax_aslr_compat_status;	/* (p) PaX ASLR enabled (compat32) */
	int	 hr_pax_aslr_compat_mmap_len;	/* (p) Number of bits randomized with mmap (compat32) */
	int	 hr_pax_aslr_compat_stack_len;	/* (p) Number of bits randomized with stack (compat32) */
	int	 hr_pax_aslr_compat_exec_len;	/* (p) Number of bits randomized with the execbase (compat32) */
};
#endif

#ifdef _KERNEL
struct image_params;
struct prison;
struct thread;
struct proc;
struct vnode;
struct vm_offset_t;

/*
 * used in sysctl handler
 */
#define	PAX_FEATURE_DISABLED		0
#define	PAX_FEATURE_OPTIN		1
#define	PAX_FEATURE_OPTOUT		2
#define	PAX_FEATURE_FORCE_ENABLED	3
#define	PAX_FEATURE_UNKNOWN_STATUS	4

extern const char *pax_status_str[];

#define PAX_FEATURE_SIMPLE_DISABLED	0
#define PAX_FEATURE_SIMPLE_ENABLED	1

extern const char *pax_status_simple_str[];

/*
 * generic pax functions
 */
int pax_elf(struct image_params *, uint32_t);
void pax_get_flags(struct proc *p, uint32_t *flags);
void pax_get_flags_td(struct thread *td, uint32_t *flags);
struct prison *pax_get_prison(struct proc *p);
struct prison *pax_get_prison_td(struct thread *td);
void pax_init_prison(struct prison *pr);
int pax_proc_is_special(struct proc *p);

/*
 * ASLR related functions
 */
int pax_aslr_active(struct proc *p);
void pax_aslr_init_vmspace(struct proc *p);
void pax_aslr_init_vmspace32(struct proc *p);
#ifdef PAX_ASLR
void pax_aslr_init_prison(struct prison *pr);
void pax_aslr_init_prison32(struct prison *pr);
#else
#define	pax_aslr_init_prison(pr)	do {} while (0)
#define	pax_aslr_init_prison32(pr)	do {} while (0)
#endif
void pax_aslr_init(struct image_params *imgp);
void pax_aslr_execbase(struct proc *p, u_long *et_dyn_addr);
void pax_aslr_mmap(struct proc *p, vm_offset_t *addr, 
    vm_offset_t orig_addr, int flags);
uint32_t pax_aslr_setup_flags(struct image_params *imgp, uint32_t mode);
void pax_aslr_stack(struct proc *p, uintptr_t *addr);
void pax_aslr_stack_adjust(struct proc *p, u_long *ssiz);
#endif /* _KERNEL */

/*
 * keep this values, to keep compatibility with HardenedBSD
 */
#define	PAX_NOTE_ASLR		0x00000040
#define	PAX_NOTE_NOASLR		0x00000080

#define	PAX_NOTE_ALL_ENABLED	(PAX_NOTE_ASLR)
#define	PAX_NOTE_ALL_DISABLED	(PAX_NOTE_NOASLR)
#define	PAX_NOTE_ALL	(PAX_NOTE_ALL_ENABLED | PAX_NOTE_ALL_DISABLED)

#endif /* __SYS_PAX_H */
