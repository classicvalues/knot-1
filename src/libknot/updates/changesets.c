/*  Copyright (C) 2011 CZ.NIC, z.s.p.o. <knot-dns@labs.nic.cz>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <config.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

#include "updates/changesets.h"
#include "libknot/common.h"
#include "common/descriptor.h"
#include "common/mempattern.h"
#include "common/mempool.h"
#include "rrset.h"
#include "util/debug.h"

/*----------------------------------------------------------------------------*/

static int knot_changeset_rrsets_match(const knot_rrset_t *rrset1,
                                         const knot_rrset_t *rrset2)
{
	return knot_rrset_equal(rrset1, rrset2, KNOT_RRSET_COMPARE_HEADER)
	       && (knot_rrset_type(rrset1) != KNOT_RRTYPE_RRSIG
	           || knot_rrset_rdata_rrsig_type_covered(rrset1)
	              == knot_rrset_rdata_rrsig_type_covered(rrset2));
}

/*----------------------------------------------------------------------------*/

int knot_changesets_init(knot_changesets_t **changesets, uint32_t flags)
{
	// Create new changesets structure
	*changesets = xmalloc(sizeof(knot_changesets_t));
	memset(*changesets, 0, sizeof(knot_changesets_t));
	(*changesets)->flags = flags;

	// Initialize memory context (xmalloc'd)
	struct mempool *pool = mp_new(sizeof(knot_changeset_t));
	(*changesets)->mem_ctx.ctx = pool;
	(*changesets)->mem_ctx.alloc = mp_alloc_mm_ctx_wrap;
	(*changesets)->mem_ctx.free = NULL;

	// Init list with changesets
	init_list(&(*changesets)->sets);

	return KNOT_EOK;
}

knot_changesets_t *knot_changesets_create(uint32_t flags)
{
	knot_changesets_t *ch = NULL;
	int ret = knot_changesets_init(&ch, flags);
	if (ret != KNOT_EOK) {
		return NULL;
	} else {
		return ch;
	}
}

knot_changeset_t *knot_changesets_create_changeset(knot_changesets_t *ch)
{
	if (ch == NULL) {
		return NULL;
	}

	// Create set using mempool
	knot_changeset_t *set = ch->mem_ctx.alloc(ch->mem_ctx.ctx,
	                                          sizeof(knot_changeset_t));
	if (set == NULL) {
		ERR_ALLOC_FAILED;
		return NULL;
	}
	memset(set, 0, sizeof(knot_changeset_t));
	set->flags = ch->flags;

	// Init set's memory context (xmalloc'd)
	struct mempool *pool = mp_new(sizeof(knot_rrset_t *));
	set->mem_ctx.ctx = pool;
	set->mem_ctx.alloc = mp_alloc_mm_ctx_wrap;
	set->mem_ctx.free = NULL;

	// Insert into list of sets
	add_tail(&ch->sets, (node *)set);

	return set;
}

knot_changeset_t *knot_changesets_get_last(knot_changesets_t *chs)
{
	if (chs == NULL) {
		return NULL;
	}

	return (knot_changeset_t *)(chs->sets.tail);
}

/*----------------------------------------------------------------------------*/

int knot_changeset_add_rrset(knot_changeset_t *chgs, knot_rrset_t *rrset,
                             knot_changeset_part_t part)
{
	// Create wrapper node for list
	knot_rr_node_t *rr_node =
		chgs->mem_ctx.alloc(chgs->mem_ctx.ctx, sizeof(knot_rr_node_t));
	if (rr_node == NULL) {
		// This will not happen with mp_alloc, but allocator can change
		ERR_ALLOC_FAILED;
		return KNOT_ENOMEM;
	}
	rr_node->rr = rrset;

	if (part == KNOT_CHANGESET_ADD) {
		add_tail(&chgs->add, (node *)rr_node);
	} else {
		add_tail(&chgs->remove, (node *)rr_node);
	}

	return KNOT_EOK;
}

/*----------------------------------------------------------------------------*/

int knot_changeset_add_rr(knot_changeset_t *chgs, knot_rrset_t *rr,
                          knot_changeset_part_t part)
{
	// Just check the last RRSet. If the RR belongs to it, merge it,
	// otherwise just add the RR to the end of the list
	list *l = part == KNOT_CHANGESET_ADD ? &chgs->add : &chgs->remove;
	knot_rrset_t *tail_rr = l ? ((knot_rr_node_t *)(l->tail))->rr : NULL;

	if (tail_rr && knot_changeset_rrsets_match(tail_rr, rr)) {
		// Create changesets exactly as they came, with possibly
		// duplicate records
		if (knot_rrset_merge(tail_rr, rr) != KNOT_EOK) {
			return KNOT_ERROR;
		}

		knot_rrset_deep_free(&rr, 1, 0);
		return KNOT_EOK;
	} else {
		return knot_changeset_add_rrset(chgs, rr, part);
	}
}

int knot_changes_add_rrset(knot_changes_t *ch, knot_rrset_t *rrset,
                           knot_changes_part_t part)
{
	if (ch == NULL || rrset == NULL) {
		return KNOT_EINVAL;
	}

	knot_rr_node_t *rr_node =
		ch->mem_ctx.alloc(ch->mem_ctx.ctx, sizeof(knot_rr_node_t));
	if (rr_node == NULL) {
		// This will not happen with mp_alloc, but allocator can change
		ERR_ALLOC_FAILED;
		return KNOT_ENOMEM;
	}
	rr_node->rr = rrset;

	if (part == KNOT_CHANGES_NEW) {
		add_tail(&ch->new_rrsets, (node *)rr_node);
	} else {
		assert(part == KNOT_CHANGES_OLD);
		add_tail(&ch->old_rrsets, (node *)rr_node);
	}

	return KNOT_EOK;
}

int knot_changes_add_node(knot_changes_t *ch, knot_node_t *kn_node,
                          knot_changes_part_t part)
{
	if (ch == NULL || kn_node == NULL) {
		return KNOT_EINVAL;
	}

	// Using the same allocator for node and rr's, sizes are equal.
	knot_node_list_t *list_node =
		ch->mem_ctx.alloc(ch->mem_ctx.ctx, sizeof(knot_node_list_t));
	if (list_node == NULL) {
		// This will not happen with mp_alloc, but allocator can change
		ERR_ALLOC_FAILED;
		return KNOT_ENOMEM;
	}
	list_node->node = kn_node;

	if (part == KNOT_CHANGES_NORMAL_NODE) {
		add_tail(&ch->old_nodes, (node *)list_node);
	} else {
		assert(part == KNOT_CHANGES_NSEC3_NODE);
		add_tail(&ch->old_nsec3, (node *)list_node);
	}
}

/*----------------------------------------------------------------------------*/

void knot_changeset_store_soa(knot_rrset_t **chg_soa,
                               uint32_t *chg_serial, knot_rrset_t *soa)
{
	*chg_soa = soa;
	*chg_serial = knot_rrset_rdata_soa_serial(soa);
}

/*----------------------------------------------------------------------------*/

int knot_changeset_add_soa(knot_changeset_t *changeset, knot_rrset_t *soa,
                           knot_changeset_part_t part)
{
	switch (part) {
	case KNOT_CHANGESET_ADD:
		knot_changeset_store_soa(&changeset->soa_to,
		                          &changeset->serial_to, soa);
		break;
	case KNOT_CHANGESET_REMOVE:
		knot_changeset_store_soa(&changeset->soa_from,
		                          &changeset->serial_from, soa);
		break;
	default:
		assert(0);
	}

	/*! \todo Remove return value? */
	return KNOT_EOK;
}

/*---------------------------------------------------------------------------*/

void knot_changeset_set_flags(knot_changeset_t *changeset,
                             uint32_t flags)
{
	changeset->flags = flags;
}

/*----------------------------------------------------------------------------*/

uint32_t knot_changeset_flags(knot_changeset_t *changeset)
{
	return changeset->flags;
}

/*----------------------------------------------------------------------------*/

int knot_changeset_is_empty(const knot_changeset_t *changeset)
{
	if (changeset == NULL) {
		return 0;
	}

	return list_is_empty(&changeset->add) &&
	       list_is_empty(&changeset->remove) == 0;
}

static void knot_free_changeset(knot_changeset_t *changeset)
{
	if (changeset == NULL) {
		return;
	}

	// Delete mempool with RRs, cannot be done via, mem_ctx function, sadly.
	mp_delete(changeset->mem_ctx.ctx);

	// Delete binary data
	free(changeset->data);
}

void knot_changes_free(knot_changes_t **changes)
{
	// Destroy mempool's data
	mp_delete((struct mempool *)((*changes)->mem_ctx.ctx));
	free(*changes);
	*changes = NULL;
}

/*----------------------------------------------------------------------------*/

void knot_free_changesets(knot_changesets_t **changesets)
{
	if (changesets == NULL || *changesets == NULL) {
		return;
	}

	// Free each changeset's mempool
	knot_changeset_t *chg = NULL;
	WALK_LIST(chg, (*changesets)->sets) {
		knot_free_changeset(chg);
	}

	// Free pool with sets themselves
	mp_delete((*changesets)->mem_ctx.ctx);

	knot_rrset_deep_free(&(*changesets)->first_soa, 1, 1);

	assert((*changesets)->changes == NULL);

	free(*changesets);
	*changesets = NULL;
}

/*----------------------------------------------------------------------------*/
/* knot_changes_t manipulation                                                */
/*----------------------------------------------------------------------------*/

int knot_changes_add_old_rrsets(knot_rrset_t **rrsets, size_t count,
                                knot_changes_t *changes)
{
	if (rrsets == NULL || changes == NULL) {
		return KNOT_EINVAL;
	}

//	/* Mark RRsets and RDATA for removal. */
//	for (size_t i = 0; i < count; ++i) {
//		assert(rrsets[i]);

//		knot_rrset_t *rrsigs = knot_rrset_get_rrsigs(rrsets[i]);

//			/* RDATA count in the RRSet. */
//			int rdata_count = 1;

//			if (rrsigs != NULL) {
//				/* Increment the RDATA count by the count of
//				 * RRSIGs. */
//				rdata_count += 1;
//			}

//			/* Remove old RDATA. */
//			ret = knot_changes_rdata_reserve(&changes->old_rdata,
//			                          changes->old_rdata_count,
//			                          &changes->old_rdata_allocated,
//			                          rdata_count);
//			if (ret != KNOT_EOK) {
////				dbg_xfrin("Failed to reserve changes rdata.\n");
//				return ret;
//			}

//			knot_changes_add_rdata(changes->old_rdata,
//			                       &changes->old_rdata_count,
//			                       rrsets[i]);

//			knot_changes_add_rdata(changes->old_rdata,
//			                       &changes->old_rdata_count,
//			                       rrsigs);
//		}

//		/* Disconnect RRsigs from rrset. */
//		knot_rrset_set_rrsigs(rrsets[i], NULL);
//		changes->old_rrsets[changes->old_rrsets_count++] = rrsets[i];
//		if (rrsigs) {
//			changes->old_rrsets[changes->old_rrsets_count++] = rrsigs;
//		}
//	}

//	return KNOT_EOK;
}

/*----------------------------------------------------------------------------*/

int knot_changes_add_new_rrsets(knot_rrset_t **rrsets, int count,
                                knot_changes_t *changes, int add_rdata)
{
//	if (rrsets == NULL || changes == NULL) {
//		return KNOT_EINVAL;
//	}

//	if (count == 0) {
//		return KNOT_EOK;
//	}

//	int ret = knot_changes_rrsets_reserve(&changes->new_rrsets,
//	                                      &changes->new_rrsets_count,
//	                                      &changes->new_rrsets_allocated,
//	                                      count);
//	if (ret != KNOT_EOK) {
//		return ret;
//	}

//	/* Mark RRsets and RDATA for removal. */
//	for (unsigned i = 0; i < count; ++i) {
//		if (rrsets[i] == NULL) {
//			continue;
//		}

//		if (add_rdata) {
//			ret = knot_changes_rdata_reserve(&changes->new_rdata,
//			                          changes->new_rdata_count,
//			                          &changes->new_rdata_allocated,
//			                          1);
//			if (ret != KNOT_EOK) {
//				return ret;
//			}

//			knot_changes_add_rdata(changes->new_rdata,
//			                       &changes->new_rdata_count,
//			                       rrsets[i]);
//		}

//		changes->new_rrsets[changes->new_rrsets_count++] = rrsets[i];
//	}

//	return KNOT_EOK;
}
