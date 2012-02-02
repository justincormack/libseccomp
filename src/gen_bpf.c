/**
 * Seccomp BPF Translator
 * 
 * Copyright (c) 2012 Red Hat <pmoore@redhat.com>
 * Author: Paul Moore <pmoore@redhat.com>
 */

/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/* XXX - this file is a quick n' dirty hack that hasn't really been verified,
 *       it is almost certainly broken; even if it does work, it needs some
 *       serious cleanup */

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <seccomp.h>

#include "gen_bpf.h"
#include "bpf_helper.h"
#include "db.h"

#if 1
/* XXX - provide our own macros for the following BPF statements, we probably
 *       want to just fixup the stuff in bpf_helper.h */
#define _JUMP(labels, label) \
	BPF_JUMP(BPF_JMP+BPF_JA, _FIND_LABEL(labels, label), JUMP_JT, JUMP_JF)
#define _LABEL(labels, label) \
	BPF_JUMP(BPF_JMP+BPF_JA, _FIND_LABEL(labels, label), LABEL_JT, LABEL_JF)
#define _FIND_LABEL(labels, label) seccomp_bpf_label(labels, label)
#define _SYSCALL(nr, jt) \
	BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, nr, 1, 0), \
	jt
#endif

/* allocation increment size */
#define BPF_INCR	(128 * sizeof(struct seccomp_filter_block))
#define LBL_INCR	16
#define LBL_LEN		256

struct bpf_filter {
	struct seccomp_fprog *prog;
	size_t prog_alen;

	char **lbl_store;
	size_t lbl_len;
	size_t lbl_alen;

	struct bpf_labels lbls;
};

#define _max(x,y) \
	((x) > (y) ? (x) : (y))

#define _bpf_blk_len(x) \
	(sizeof(x) / sizeof(x[0]))

#define _bpf_next(x) \
	(&((x)->prog->filter[(x)->prog->len]))

/**
 * Append BPF instructions to the main BPF program
 * @param bpf the bpf_filter struct
 * @param blk the new BPF instruction block
 * @param blk_len length of the new BPF instruction block
 * 
 * This function appends the given set of BPF instructions to the main BPF
 * program, growing the allocated size if necessary.  Returns zero on success,
 * negative values on error.
 *
 */
static int _gen_bpf_append(struct bpf_filter *bpf,
			   const struct seccomp_filter_block *blk,
			   size_t blk_len)
{
	void *buf;
	size_t buf_len;

	if (bpf->prog->len + blk_len > bpf->prog_alen) {
		/* XXX - check to make sure the size isn't getting too large */
		/* XXX - is there a size limit? */
		buf_len = bpf->prog_alen + _max(BPF_INCR, blk_len);
		buf = realloc(bpf->prog->filter, buf_len);
		if (buf == NULL) {
			free(bpf->prog->filter);
			bpf->prog->filter = NULL;
			bpf->prog->len = 0;
			bpf->prog_alen = 0;
			return -ENOMEM;
		}
		bpf->prog->filter = buf;
		bpf->prog_alen = buf_len;
	}
	memcpy(_bpf_next(bpf), blk, blk_len * sizeof(*blk));
	bpf->prog->len += blk_len;

	return 0;
}

/**
 * Append a new BPF jump label to the label store
 * @param bpf the bpf_filter struct
 * @param lbl the jump label
 * 
 * This function adds a new label to the BPF label store.  Returns a pointer
 * to the label string zero on success, NULL on error.
 *
 */
static char *_gen_bpf_lbl_append(struct bpf_filter *bpf, const char *lbl)
{
	int iter;
	char *lbl_new;
	char **array;
	size_t array_len;

	if (bpf->lbl_len >= bpf->lbl_alen) {
		/* XXX - note the current limit on labels in bpf_helper.{c,h} */
		array_len = bpf->lbl_alen + _max(LBL_INCR, bpf->lbl_len + 1);
		array = realloc(bpf->lbl_store, array_len * sizeof(char *));
		if (array == NULL)
			goto label_append_failure;
		bpf->lbl_store = array;
		bpf->lbl_alen = array_len;
	}
	lbl_new = strdup(lbl);
	if (lbl_new == NULL)
		goto label_append_failure;
	bpf->lbl_store[bpf->lbl_len++] = lbl_new;

	return lbl_new;

label_append_failure:
	for (iter = 0; iter < bpf->lbl_len; iter++)
		free(bpf->lbl_store[iter]);
	free(bpf->lbl_store);
	bpf->lbl_store = NULL;
	bpf->lbl_len = 0;
	bpf->lbl_alen = 0;
	return NULL;
}

/**
 * Generate BPF for the syscall argument chain
 * @param act the filter action
 * @param sys_num the syscall number
 * @param chain_num the chain number
 * @param arg the syscall argument chain
 * @param lbl_next_sys the jump label for the next syscall
 * @param bpf the bpf_filter struct
 *
 * This function generates BPF for the given syscall argument chain. Returns
 * zero on success, negative values on failure.
 *
 */
static int _gen_bpf_syscall_chain(enum scmp_flt_action act,
				  unsigned int sys_num,
				  unsigned int chain_num,
				  struct db_syscall_arg_chain_list *chain,
				  const char *lbl_next_sys;
				  struct bpf_filter *bpf)
{
	int rc;
	char *lbl_end;
	char lbl_next_chain[LBL_LEN]; /* XXX - ungh */
	struct db_syscall_arg_list *a_iter;

	/* set the next jump point */
	if (chain->next != NULL) {
		/* XXX - note the current limit on labels in bpf_helper.{c,h} */
		rc = snprintf(lbl_next_chain, LBL_LEN,
			      "syscall_%d_c%d_next", sys_num, chain_num);
		if (rc >= LBL_LEN)
			return -E2BIG;
		lbl_end = _gen_bpf_lbl_append(bpf, lbl_next_chain);
		if (lbl_end == NULL)
			return -ENOMEM;
	} else
		/* we are the last argument chain so just jump to the end of
		 * the syscall filter block */
		lbl_end = lbl_next_sys;

	/* run through the argument chains */
	db_list_foreach(a_iter, chain->args) {
		/* load the argument */
		{
			struct seccomp_filter_block blk[] = {
				ARG(a_iter->num),
			};
			rc = _gen_bpf_append(bpf, blk, _bpf_blk_len(blk));
			if (rc < 0)
				return rc;
		}

		/* do the comparison */
		if (a_iter->op == SCMP_CMP_NE) {
			struct seccomp_filter_block blk[] = {
				JEQ(a_iter->datum, _JUMP(&bpf->lbls, lbl_end)),
			};
			rc = _gen_bpf_append(bpf, blk, _bpf_blk_len(blk));
			if (rc < 0)
				return rc;
		} else if (a_iter->op == SCMP_CMP_LT) {
			struct seccomp_filter_block blk[] = {
				JGE(a_iter->datum, _JUMP(&bpf->lbls, lbl_end)),
			};
			rc = _gen_bpf_append(bpf, blk, _bpf_blk_len(blk));
			if (rc < 0)
				return rc;
		} else if (a_iter->op == SCMP_CMP_LE) {
			struct seccomp_filter_block blk[] = {
				JGT(a_iter->datum, _JUMP(&bpf->lbls, lbl_end)),
			};
			rc = _gen_bpf_append(bpf, blk, _bpf_blk_len(blk));
			if (rc < 0)
				return rc;
		} else if (a_iter->op == SCMP_CMP_EQ) {
			struct seccomp_filter_block blk[] = {
				JNE(a_iter->datum, _JUMP(&bpf->lbls, lbl_end)),
			};
			rc = _gen_bpf_append(bpf, blk, _bpf_blk_len(blk));
			if (rc < 0)
				return rc;
		} else if (a_iter->op == SCMP_CMP_GE) {
			struct seccomp_filter_block blk[] = {
				JLT(a_iter->datum, _JUMP(&bpf->lbls, lbl_end)),
			};
			rc = _gen_bpf_append(bpf, blk, _bpf_blk_len(blk));
			if (rc < 0)
				return rc;
		} else if (a_iter->op == SCMP_CMP_GT) {
			struct seccomp_filter_block blk[] = {
				JLE(a_iter->datum, _JUMP(&bpf->lbls, lbl_end)),
			};
			rc = _gen_bpf_append(bpf, blk, _bpf_blk_len(blk));
			if (rc < 0)
				return rc;
		}
	}
	
	/* matching action and jump label for next chain */
	if (act == SCMP_ACT_ALLOW) {
		struct seccomp_filter_block blk[] = {
			ALLOW,
			_LABEL(&bpf->lbls, lbl_end),
		};
		rc = _gen_bpf_append(bpf, blk, _bpf_blk_len(blk));
		if (rc < 0)
			return rc;
	} else {
		struct seccomp_filter_block blk[] = {
			DENY,
			_LABEL(&bpf->lbls, lbl_end),
		};
		rc = _gen_bpf_append(bpf, blk, _bpf_blk_len(blk));
		if (rc < 0)
			return rc;
	}

	return 0;
}

/**
 * Generate BPF for a syscall filter
 * @param act the filter action
 * @param sys the system call filter
 * @param bpf the bpf_filter struct
 * 
 * This function generates BPF for the given syscall filter and action.
 * Returns zero on success, negative values on failure.
 *
 */
static int _gen_bpf_syscall(enum scmp_flt_action act,
			    const struct db_syscall_list *sys,
			    struct bpf_filter *bpf)
{
	int rc;
	char *lbl_end;
	char lbl_next_sys[LBL_LEN]; /* XXX - ungh^2 */
	struct db_syscall_arg_chain_list *c_iter;
	unsigned int c_count = 0;

	if (sys->chains == NULL) {
		if (act == SCMP_ACT_ALLOW) {
			struct seccomp_filter_block blk[] = {
				SYSCALL(sys->num, ALLOW),
			};
			rc = _gen_bpf_append(bpf, blk, _bpf_blk_len(blk));
			if (rc < 0)
				return rc;
		} else {
			struct seccomp_filter_block blk[] = {
				SYSCALL(sys->num, DENY),
			};
			rc = _gen_bpf_append(bpf, blk, _bpf_blk_len(blk));
			if (rc < 0)
				return rc;
		}
	} else {
		/* XXX - note the current limit on labels in bpf_helper.{c,h} */
		rc = snprintf(lbl_next_sys, LBL_LEN, "syscall_%d_end",
			      sys->num);
		if (rc >= LBL_LEN)
			return -E2BIG;
		lbl_end = _gen_bpf_lbl_append(bpf, lbl_next_sys);
		if (lbl_end == NULL)
			return -ENOMEM;

		/* jump past this syscall block if it isn't the right one */
		{
			struct seccomp_filter_block blk[] = {
				_SYSCALL(sys->num, _JUMP(&bpf->lbls, lbl_end)),
			};
			rc = _gen_bpf_append(bpf, blk, _bpf_blk_len(blk));
			if (rc < 0)
				return rc;
		}

		/* iterate over the argument chains */
		db_list_foreach(c_iter, sys->chains) {
			rc = _gen_bpf_syscall_chain(act, sys->num,
						    c_count++, c_iter,
						    lbl_end, bpf);
			if (rc < 0)
				return rc;
		}

		/* jump label for next syscall block */
		{
			struct seccomp_filter_block blk[] = {
				_LABEL(&bpf->lbls, lbl_end),
			};
			rc = _gen_bpf_append(bpf, blk, _bpf_blk_len(blk));
			if (rc < 0)
				return rc;
		}
	}

	return 0;
}

/**
 * Generate a BPF representation of the filter DB
 * @param db the seccomp filter DB
 * 
 * This function generates a BPF representation of the given filter DB.
 * Returns a pointer to a valid seccomp_fprog on success, NULL on failure.
 *
 */
struct seccomp_fprog *gen_bpf_generate(const struct db_filter *db)
{
	int rc, iter;
	struct bpf_filter bpf;
	struct db_syscall_list *sys_iter;

	memset(&bpf, 0, sizeof(bpf));

	bpf.prog = malloc(sizeof(*bpf.prog));
	if (bpf.prog == NULL)
		return NULL;
	memset(bpf.prog, 0, sizeof(*bpf.prog));

	/* XXX - assume we can get away with LOAD_SYSCALL_NR just at the top,
	 *       which may be wrong */
	/* load the syscall */
	{
		struct seccomp_filter_block blk[] = {
			LOAD_SYSCALL_NR,
		};
		rc = _gen_bpf_append(&bpf, blk, _bpf_blk_len(blk));
		if (rc < 0)
			goto generate_failure;
	}

	db_list_foreach(sys_iter, db->syscalls) {
		rc = _gen_bpf_syscall((db->def_action == SCMP_ACT_DENY ?
				       SCMP_ACT_ALLOW : SCMP_ACT_DENY),
				      sys_iter, &bpf);
		if (rc < 0)
			goto generate_failure;
	}

	/* default action */
	if (db->def_action == SCMP_ACT_ALLOW) {
		struct seccomp_filter_block blk[] = {
			ALLOW,
		};
		rc = _gen_bpf_append(&bpf, blk, _bpf_blk_len(blk));
		if (rc < 0)
			goto generate_failure;
	} else {
		struct seccomp_filter_block blk[] = {
			DENY,
		};
		rc = _gen_bpf_append(&bpf, blk, _bpf_blk_len(blk));
		if (rc < 0)
			goto generate_failure;
	}

	/* resolve the labels/jumps */
	rc = bpf_resolve_jumps(&bpf.lbls, bpf.prog->filter, bpf.prog->len);
	if (rc != 0)
		goto generate_failure;

	/* we don't need to keep the labels after we have resolved the jumps */
	for (iter = 0; iter < bpf.lbl_len; iter++)
		free(bpf.lbl_store[iter]);
	free(bpf.lbl_store);

	return bpf.prog;

generate_failure:
	gen_bpf_destroy(bpf.prog);
	for (iter = 0; iter < bpf.lbl_len; iter++)
		free(bpf.lbl_store[iter]);
	free(bpf.lbl_store);
	return NULL;
}

/**
 * Free memory associated with a BPF representation
 * @param fprog the BPF representation
 * 
 * Free the memory associated with a BPF representation generated by the
 * gen_bpf_generate().
 *
 */
void gen_bpf_destroy(struct seccomp_fprog *fprog)
{
	if (fprog == NULL)
		return;
	if (fprog->filter != NULL)
		free(fprog->filter);
	free(fprog);
}
