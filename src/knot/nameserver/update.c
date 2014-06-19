/*  Copyright (C) 2013 CZ.NIC, z.s.p.o. <knot-dns@labs.nic.cz>

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

#include "knot/nameserver/update.h"
#include "knot/nameserver/internet.h"
#include "knot/nameserver/process_query.h"
#include "knot/nameserver/requestor.h"
#include "knot/updates/apply.h"
#include "knot/dnssec/zone-sign.h"
#include "common/debug.h"
#include "knot/dnssec/zone-events.h"
#include "knot/updates/ddns.h"
#include "common/descriptor.h"
#include "libknot/tsig-op.h"
#include "knot/zone/zone.h"
#include "knot/zone/events.h"

/* UPDATE-specific logging (internal, expects 'qdata' variable set). */
#define UPDATE_LOG(severity, msg...) \
	QUERY_LOG(severity, qdata, "UPDATE", msg)

static int update_forward(knot_pkt_t *pkt, struct query_data *qdata)
{
	/*! \todo ref #244 This will be reimplemented later. */
	qdata->rcode = KNOT_RCODE_NOTIMPL;
	return NS_PROC_FAIL;
}

int update_answer(knot_pkt_t *pkt, struct query_data *qdata)
{
	/* RFC1996 require SOA question. */
	NS_NEED_QTYPE(qdata, KNOT_RRTYPE_SOA, KNOT_RCODE_FORMERR);

	/* Check valid zone. */
	NS_NEED_ZONE(qdata, KNOT_RCODE_NOTAUTH);

	/* Allow pass-through of an unknown TSIG in DDNS forwarding
	   (must have zone). */
	zone_t *zone = (zone_t *)qdata->zone;
	if (zone_master(zone) != NULL) {
		return update_forward(pkt, qdata);
	}

	/* Need valid transaction security. */
	NS_NEED_AUTH(&zone->conf->acl.update_in, qdata);
	/* Check expiration. */
	NS_NEED_ZONE_CONTENTS(qdata, KNOT_RCODE_SERVFAIL);

	/* Store update into DDNS queue. */
	int ret = zone_update_enqueue(zone, qdata->query, qdata->param);
	if (ret != KNOT_EOK) {
		return NS_PROC_FAIL;
	}

	/* No immediate response. */
	pkt->size = 0;
	return NS_PROC_DONE;
}

static bool apex_rr_changed(const zone_contents_t *old_contents,
                            const zone_contents_t *new_contents,
                            uint16_t type)
{
	knot_rrset_t old_rr = node_rrset(old_contents->apex, type);
	knot_rrset_t new_rr = node_rrset(new_contents->apex, type);

	return !knot_rrset_equal(&old_rr, &new_rr, KNOT_RRSET_COMPARE_WHOLE);
}

static bool zones_dnskey_changed(const zone_contents_t *old_contents,
                                 const zone_contents_t *new_contents)
{
	return apex_rr_changed(old_contents, new_contents, KNOT_RRTYPE_DNSKEY);
}

static bool zones_nsec3param_changed(const zone_contents_t *old_contents,
                                     const zone_contents_t *new_contents)
{
	return apex_rr_changed(old_contents, new_contents,
	                       KNOT_RRTYPE_NSEC3PARAM);
}

static int sign_update(zone_t *zone, const zone_contents_t *old_contents,
                       zone_contents_t *new_contents, changeset_t *ddns_ch)
{
	assert(zone != NULL);
	assert(old_contents != NULL);
	assert(new_contents != NULL);
	assert(ddns_ch != NULL);

	changesets_t *sec_chs = changesets_create(1);
	if (sec_chs == NULL) {
		return KNOT_ENOMEM;
	}
	changeset_t *sec_ch = changesets_get_last(sec_chs);

	/*
	 * Check if the UPDATE changed DNSKEYs or NSEC3PARAM.
	 * If yes, signing just the changes is insufficient, we have to sign
	 * the whole zone.
	 */
	int ret = KNOT_EOK;
	uint32_t refresh_at = 0;
	if (zones_dnskey_changed(old_contents, new_contents) ||
	    zones_nsec3param_changed(old_contents, new_contents)) {
		ret = knot_dnssec_zone_sign(new_contents, zone->conf,
		                            sec_ch, KNOT_SOA_SERIAL_KEEP,
		                            &refresh_at);
	} else {
		// Sign the created changeset
		ret = knot_dnssec_sign_changeset(new_contents, zone->conf,
		                                 ddns_ch, sec_ch,
		                                 &refresh_at);
	}
	if (ret != KNOT_EOK) {
		changesets_free(&sec_chs, NULL);
		return ret;
	}

	// Apply DNSSEC changeset
	ret = apply_changesets_directly(new_contents, sec_chs);
	if (ret != KNOT_EOK) {
		changesets_free(&sec_chs, NULL);
		return ret;
	}

	// Merge changesets
	ret = changeset_merge(ddns_ch, sec_ch);
	if (ret != KNOT_EOK) {
		changesets_free(&sec_chs, NULL);
		return ret;
	}

	// Free the DNSSEC changeset's SOA from (not used anymore)
	knot_rrset_free(&sec_ch->soa_from, NULL);
	// Shallow free DNSSEC changesets
	free(sec_chs);

	// Plan next zone resign.
	const time_t resign_time = zone_events_get_time(zone, ZONE_EVENT_DNSSEC);
	if (time(NULL) + refresh_at < resign_time) {
		zone_events_schedule(zone, ZONE_EVENT_DNSSEC, refresh_at);
	}
	return ret;
}

struct changeset_state {
	knot_rrset_t *soa_from;
	knot_rrset_t *soa_to;
};

static void store_changeset_state(struct changeset_state *state, changeset_t *ch)
{
	state->soa_from = knot_rrset_copy(ch->soa_from, NULL);
	state->soa_to = knot_rrset_copy(ch->soa_to, NULL);
}

void changeset_rollback(struct changeset_state *from, changeset_t *ch)
{
#warning complete this
	init_list(&ch->add);
	init_list(&ch->remove);

	if (from->soa_from != ch->soa_from) {
		knot_rrset_free(&ch->soa_from, NULL);
		ch->soa_from = from->soa_from;
	}

	if (from->soa_to != ch->soa_to) {
		knot_rrset_free(&ch->soa_to, NULL);
		ch->soa_to = from->soa_to;
	}
}

static int process_single_update(struct request_data *request, const zone_t *zone,
                                 changeset_t *ch)
{
	uint16_t rcode = KNOT_RCODE_NOERROR;
	int ret = ddns_process_prereqs(request->query, zone->contents, &rcode);
	if (ret != KNOT_EOK) {
		assert(rcode != KNOT_RCODE_NOERROR);
		knot_wire_set_rcode(request->resp->wire, rcode);
		return ret;
	}

	struct changeset_state st;
	store_changeset_state(&st, ch);

	ret = ddns_process_update(zone, request->query, ch, &rcode);
	if (ret != KNOT_EOK) {
		assert(rcode != KNOT_RCODE_NOERROR);
		knot_wire_set_rcode(request->resp->wire, rcode);
		changeset_rollback(&st, ch);
	} else {
		knot_rrset_free(&st.soa_from, NULL);
		knot_rrset_free(&st.soa_to, NULL);
		return KNOT_EOK;
	}
}

static void set_rcodes(list_t *queries, const uint16_t rcode)
{
	struct request_data *query;
	WALK_LIST(query, *queries) {
		if (knot_wire_get_rcode(query->resp->wire) == KNOT_RCODE_NOERROR) {
			knot_wire_set_rcode(query->resp->wire, rcode);
		}
	}
}

static int process_queries(zone_t *zone, list_t *queries)
{
#warning TODO proper logging
	assert(queries);

	// Create DDNS changesets
	changesets_t *ddns_chs = changesets_create(1);
	if (ddns_chs == NULL) {
		set_rcodes(queries, KNOT_RCODE_SERVFAIL);
		return KNOT_ENOMEM;
	}
	changeset_t *ddns_ch = changesets_get_last(ddns_chs);

	struct request_data *query;
	WALK_LIST(query, *queries) {
		process_single_update(query, zone, ddns_ch);
	}

	zone_contents_t *new_contents = NULL;
	const bool change_made = !changeset_is_empty(ddns_ch);
	if (change_made) {
		int ret = apply_changesets(zone, ddns_chs, &new_contents);
		if (ret != KNOT_EOK) {
			if (ret == KNOT_ETTL) {
				set_rcodes(queries, KNOT_RCODE_REFUSED);
			} else {
				set_rcodes(queries, KNOT_RCODE_SERVFAIL);
			}
			changesets_free(&ddns_chs, NULL);
			return ret;
		}
	} else {
		changesets_free(&ddns_chs, NULL);
		return KNOT_EOK;
	}
	assert(new_contents);

	if (zone->conf->dnssec_enable) {
		int ret = sign_update(zone, zone->contents, new_contents, ddns_ch);
		if (ret != KNOT_EOK) {
			update_rollback(ddns_chs, &new_contents);
			changesets_free(&ddns_chs, NULL);
			set_rcodes(queries, KNOT_RCODE_SERVFAIL);
			return ret;
		}
	}

	// Write changes to journal if all went well. (DNSSEC merged)
	int ret = zone_change_store(zone, ddns_chs);
	if (ret != KNOT_EOK) {
		update_rollback(ddns_chs, &new_contents);
		changesets_free(&ddns_chs, NULL);
		set_rcodes(queries, KNOT_RCODE_SERVFAIL);
		return ret;
	}

	// Switch zone contents.
	zone_contents_t *old_contents = zone_switch_contents(zone, new_contents);
	synchronize_rcu();
	update_free_old_zone(&old_contents);

	update_cleanup(ddns_chs);
	changesets_free(&ddns_chs, NULL);

	// Sync zonefile immediately if configured.
	if (zone->conf->dbsync_timeout == 0) {
		zone_events_schedule(zone, ZONE_EVENT_FLUSH, ZONE_EVENT_NOW);
	}

	return ret;
}


int update_process_queries(zone_t *zone, list_t *queries)
{
	if (zone == NULL || queries == NULL) {
		return KNOT_EINVAL;
	}

//	UPDATE_LOG(LOG_INFO, "Started.");

	/* Keep original state. */
	struct timeval t_start, t_end;
	gettimeofday(&t_start, NULL);
	const uint32_t old_serial = zone_contents_serial(zone->contents);

	/* Process authenticated packet. */
	int ret = process_queries(zone, queries);
	if (ret != KNOT_EOK) {
//		UPDATE_LOG(LOG_ERR, "%s", knot_strerror(ret));
		return ret;
	}

	/* Evaluate response. */
	const uint32_t new_serial = zone_contents_serial(zone->contents);
	if (new_serial == old_serial) {
//		UPDATE_LOG(LOG_NOTICE, "No change to zone made.");
		return KNOT_EOK;
	}

	gettimeofday(&t_end, NULL);
//	UPDATE_LOG(LOG_INFO, "Serial %u -> %u", old_serial, new_serial);
	printf("Update finished in %.02fs.\n",
	           time_diff(&t_start, &t_end) / 1000.0);
	
	zone_events_schedule(zone, ZONE_EVENT_NOTIFY, ZONE_EVENT_NOW);

	return KNOT_EOK;
}

#undef UPDATE_LOG
