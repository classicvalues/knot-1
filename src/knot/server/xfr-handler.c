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
#include <unistd.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <assert.h>
#include <urcu.h>

#include "knot/common.h"
#include "knot/server/xfr-handler.h"
#include "libknot/nameserver/name-server.h"
#include "knot/other/error.h"
#include "knot/server/socket.h"
#include "knot/server/udp-handler.h"
#include "knot/server/tcp-handler.h"
#include "libknot/updates/xfr-in.h"
#include "knot/server/zones.h"
#include "libknot/util/error.h"
#include "libknot/tsig-op.h"
#include "common/evsched.h"
#include "common/prng.h"

/* Constants */
#define XFR_BUFFER_SIZE 65535 /*! Do not change this - maximum value for UDP packet length. */

void xfr_interrupt(xfrhandler_t *h)
{
	for(unsigned i = 0; i < h->unit->size; ++i) {
		evqueue_write(h->workers[i]->q, "", 1);
	}
}

static void xfr_request_deinit(knot_ns_xfr_t *r)
{
	if (r) {
		free(r->msgpref);
		r->msgpref = NULL;
	}
}

/*!
 * \brief SOA query timeout handler.
 */
static int xfr_udp_timeout(event_t *e)
{
	knot_ns_xfr_t *data = (knot_ns_xfr_t *)e->data;
	if (!data) {
		return KNOTD_EINVAL;
	}
	
	/* Remove reference to this event. */
	if (data->zone != NULL) {
		zonedata_t *zd = (zonedata_t *)knot_zone_data(data->zone);
		if (zd != NULL && zd->soa_pending == e) {
			zd->soa_pending = NULL;
		}
	}

	/* Close socket. */
	knot_zone_t *z = data->zone;
	if (z && knot_zone_get_contents(z) && knot_zone_data(z)) {
		log_zone_info("%s timeout exceeded.\n",
		              data->msgpref);
	}
	
	knot_ns_xfr_t cr = {};
	cr.type = XFR_TYPE_CLOSE;
	cr.session = data->session;
	cr.data = data;
	cr.zone = data->zone;
	xfrworker_t *w = (xfrworker_t *)data->owner;
	if (w) {
		evqueue_write(w->q, &cr, sizeof(knot_ns_xfr_t));
	}

	return KNOTD_EOK;
}

/*!
 * \brief Query reponse event handler function.
 *
 * Handle single query response event.
 *
 * \param loop Associated event pool.
 * \param w Associated socket watcher.
 * \param revents Returned events.
 */
static int xfr_process_udp_query(xfrworker_t *w, int fd, knot_ns_xfr_t *data)
{
	/* Receive msg. */
	ssize_t n = recvfrom(data->session, data->wire, data->wire_size, 0, data->addr.ptr, &data->addr.len);
	size_t resp_len = data->wire_size;
	if (n > 0) {
		log_zone_info("%s Finished.\n", data->msgpref);
		udp_handle(fd, data->wire, n, &resp_len, &data->addr, w->ns);
	}
	
	if(data->type == XFR_TYPE_SOA && data->zone) {
		zonedata_t * zd = (zonedata_t*)knot_zone_data(data->zone);
		zd->soa_pending = NULL;
	}

	/* Disable timeout. */
	evsched_t *sched =
		((server_t *)knot_ns_get_data(w->ns))->sched;
	event_t *ev = (event_t *)data->data;
	if (ev) {
		dbg_xfr("xfr: cancelling UDP query timeout\n");
		evsched_cancel(sched, ev);
		ev = (event_t *)data->data;
		if (ev) {
			evsched_event_free(sched, ev);
			data->data = 0;
		}
		
		/* Close after receiving response. */
		knot_ns_xfr_t cr = {};
		cr.type = XFR_TYPE_CLOSE;
		cr.session = data->session;
		cr.data = data;
		cr.zone = data->zone;
		evqueue_write(w->q, &cr, sizeof(knot_ns_xfr_t));
	}
	
	return KNOTD_EOK;
}

/*! \todo Document me (issue #1586) */
static void xfr_free_task(knot_ns_xfr_t *task)
{
	if (!task) {
		return;
	}
	
	xfrworker_t *w = (xfrworker_t *)task->owner;
	if (!w) {
		free(task);
		return;
	}
	
	/* Remove from fdset. */
	if (w->fdset) {
		dbg_xfr("xfr_free_task: freeing fd=%d.\n", task->session);
		fdset_remove(w->fdset, task->session);
	}
	
	/* Unlock if XFR/IN.*/
	if (task->type == XFR_TYPE_AIN || task->type == XFR_TYPE_IIN) {
		knot_zone_t *zone = task->zone;
		zonedata_t *zd = (zonedata_t *)knot_zone_data(zone);
		if (zd) {
			zd->xfr_in.wrkr = 0;
			pthread_mutex_unlock(&zd->xfr_in.lock);
		}
	}

	/* Remove fd-related data. */
	xfrhandler_t *h = w->master;
	pthread_mutex_lock(&h->tasks_mx);
	skip_remove(h->tasks, (void*)((size_t)task->session), 0, 0);
	pthread_mutex_unlock(&h->tasks_mx);
	
	/* Deinitialize */
	xfr_request_deinit(task);

	close(task->session);
	free(task);
}

/*! \todo Document me (issue #1586) */
static knot_ns_xfr_t *xfr_register_task(xfrworker_t *w, knot_ns_xfr_t *req)
{
	knot_ns_xfr_t *t = malloc(sizeof(knot_ns_xfr_t));
	if (!t) {
		return NULL;
	}

	memcpy(t, req, sizeof(knot_ns_xfr_t));
	sockaddr_update(&t->addr);

	/* Update request. */
	t->wire = 0; /* Invalidate shared buffer. */
	t->wire_size = 0;
	t->data = 0; /* New zone will be built. */

	/* Register data. */
	xfrhandler_t * h = w->master;
	pthread_mutex_lock(&h->tasks_mx);
	int ret = skip_insert(h->tasks, (void*)((ssize_t)t->session), t, 0);
	pthread_mutex_unlock(&h->tasks_mx);

	/* Add to set. */
	if (ret == 0) {
		ret = fdset_add(w->fdset, t->session, OS_EV_READ);
	}
	/* Evaluate final return code. */
	if (ret == 0) {
		t->owner = w;
	} else {
		/* Attempt to remove from list anyway. */
		skip_remove(h->tasks, (void*)((ssize_t)t->session), NULL, NULL);
		free(t);
		t = NULL;
	}
	return t;
}

/*!
 * \brief Clean pending transfer data.
 */
static int xfr_xfrin_cleanup(xfrworker_t *w, knot_ns_xfr_t *data)
{
	int ret = KNOTD_EOK;
	knot_changesets_t *chs = 0;
	
	switch(data->type) {
	case XFR_TYPE_AIN:
		if (data->data) {
			if (data->flags & XFR_FLAG_AXFR_FINISHED) {
				knot_zone_contents_deep_free(
					(knot_zone_contents_t **)&data->data, 0);
			} else {
				xfrin_constructed_zone_t *constr_zone =
					(xfrin_constructed_zone_t *)data->data;
				knot_zone_contents_deep_free(
						&(constr_zone->contents), 0);
				xfrin_free_orphan_rrsigs(&(constr_zone->rrsigs));
				free(data->data);
				data->data = 0;
			}
		}
		break;
	case XFR_TYPE_IIN:
		if (data->data) {
			chs = (knot_changesets_t *)data->data;
			knot_free_changesets(&chs);
		}
		break;
	}
	
	return ret;
}

/*!
 * \brief Finalize XFR/IN transfer.
 *
 * \param w XFR worker.
 * \param data Associated data.
 *
 * \retval KNOTD_EOK on success.
 * \retval KNOTD_ERROR
 */
static int xfr_xfrin_finalize(xfrworker_t *w, knot_ns_xfr_t *data)
{
	/* CLEANUP */
//	// get the zone name from Question
//	dbg_xfr_verb("Query: %p, response: %p\n", data->query, data->response);
//	const knot_dname_t *qname = knot_packet_qname(data->query);
//	char *zorigin = "(unknown)";
//	if (qname != NULL) {
//		zorigin = knot_dname_to_str(qname);
//	}
	
	int ret = KNOTD_EOK;
	knot_changesets_t *chs = NULL;
	
	switch(data->type) {
	case XFR_TYPE_AIN:
		dbg_xfr("xfr: AXFR/IN saving new zone\n");
		ret = zones_save_zone(data);
		if (ret != KNOTD_EOK) {
			xfr_xfrin_cleanup(w, data);
			log_zone_error("%s Failed to save transferred zone - %s\n",
			               data->msgpref, knotd_strerror(ret));
		} else {
			dbg_xfr("xfr: AXFR/IN new zone saved.\n");
			ret = knot_ns_switch_zone(w->ns, data);
			if (ret != KNOT_EOK) {
				log_zone_error("%s Failed to switch in-memory "
				               "zone - %s\n",  data->msgpref,
				               knot_strerror(ret));
			}
		}
		
		break;
	case XFR_TYPE_IIN:
		/* Save changesets. */
		dbg_xfr("xfr: IXFR/IN saving changesets\n");
		ret = zones_store_changesets(data);
		if (ret != KNOTD_EOK) {
			log_zone_error("%s Failed to apply changesets - %s\n",
			               data->msgpref,
			               knot_strerror(ret));
		} else {
			/* Update zone. */
			ret = zones_apply_changesets(data);
			if (ret != KNOT_EOK) {
				log_zone_error("%s Failed to save "
					       "transferred changesets - %s\n",
					       data->msgpref, knotd_strerror(ret));
			}
		}
		/* Free changesets, but not the data. */
		chs = (knot_changesets_t *)data->data;
		knot_free_changesets(&chs);
		/* CLEANUP */
//		free(chs->sets);
//		free(chs);
		data->data = 0;
		log_zone_info("%s %s.\n", data->msgpref,
		              ret == KNOTD_EOK ? "Finished" : "Failed");
		break;
	default:
		ret = KNOTD_EINVAL;
		break;
	}

	/* CLEANUP */
//	if (qname != NULL) {
//		free(zorigin);
//	}
	
	return ret;
}

/*!
 * \brief Prepare TSIG for XFR.
 */
static int xfr_prepare_tsig(knot_ns_xfr_t *xfr, knot_key_t *key)
{
	int ret = KNOT_EOK;
	xfr->tsig_key = key;
	xfr->tsig_size = tsig_wire_maxsize(key);
	xfr->digest_max_size = tsig_alg_digest_length(
				key->algorithm);
	xfr->digest = malloc(xfr->digest_max_size);
	memset(xfr->digest, 0 , xfr->digest_max_size);
	dbg_xfr("xfr: found TSIG key (MAC len=%zu), adding to transfer\n",
		xfr->digest_max_size);
	
	return ret;
}

/*!
 * \brief Check TSIG if exists.
 */
static int xfr_check_tsig(knot_ns_xfr_t *xfr, knot_rcode_t *rcode, char **tag)
{
	/* Parse rest of the packet. */
	int ret = KNOT_EOK;
	knot_packet_t *qry = xfr->query;
	knot_key_t *key = 0;
	const knot_rrset_t *tsig_rr = 0;
	ret = knot_packet_parse_rest(qry);
	if (ret == KNOT_EOK) {
		
		/* Find TSIG key name from query. */
		const knot_dname_t* kname = 0;
		int tsig_pos = knot_packet_additional_rrset_count(qry) - 1;
		if (tsig_pos >= 0) {
			tsig_rr = knot_packet_additional_rrset(qry, tsig_pos);
			if (knot_rrset_type(tsig_rr) == KNOT_RRTYPE_TSIG) {
				dbg_xfr("xfr: found TSIG in AR\n");
				kname = knot_rrset_owner(tsig_rr);
				if (tag) {
					*tag = knot_dname_to_str(kname);
					
				}
			} else {
				tsig_rr = 0;
			}
		}
		if (!kname) {
			dbg_xfr("xfr: TSIG not found in AR\n");
			char *name = knot_dname_to_str(
						knot_zone_name(xfr->zone));
			free(name);

			// return REFUSED
			xfr->tsig_key = 0;
			*rcode = KNOT_RCODE_REFUSED;
			return KNOT_EXFRREFUSED;
		}
		if (tsig_rr) {
			tsig_algorithm_t alg = tsig_rdata_alg(tsig_rr);
			if (tsig_alg_digest_length(alg) == 0) {
				log_server_info("%s Unsupported digest algorithm "
				                "requested, treating as "
				                "bad key.\n",
				                xfr->msgpref);
				*rcode = KNOT_RCODE_NOTAUTH;
				xfr->tsig_key = NULL;
				xfr->tsig_rcode = KNOT_TSIG_RCODE_BADKEY;
				xfr->tsig_prev_time_signed =
				                tsig_rdata_time_signed(tsig_rr);
				return KNOT_TSIG_EBADKEY;
			}
		}

		/* Evaluate configured key for claimed key name.*/
		key = xfr->tsig_key; /* Expects already set key (check_zone) */
		xfr->tsig_key = 0;
		if (key && kname && knot_dname_compare(key->name, kname) == 0) {
			dbg_xfr("xfr: found claimed TSIG key for comparison\n");
		} else {
			/*! \todo These ifs are redundant (issue #1586) */
			*rcode = KNOT_RCODE_NOTAUTH;
			/* TSIG is mandatory if configured for interface. */
			if (key && !kname) {
				dbg_xfr("xfr: TSIG key is mandatory for "
				        "this interface\n");
				ret = KNOT_TSIG_EBADKEY;
				xfr->tsig_rcode = KNOT_TSIG_RCODE_BADKEY;
			}
			
			/* Configured, but doesn't match. */
			if (kname) {
				dbg_xfr("xfr: no claimed key configured, "
				        "treating as bad key\n");
				ret = KNOT_TSIG_EBADKEY;
				xfr->tsig_rcode = KNOT_TSIG_RCODE_BADKEY;
			}
			
			key = 0; /* Invalidate, ret already set to BADKEY */
		}

		/* Validate with TSIG. */
		if (key) {
			/* Prepare variables for TSIG */
			xfr_prepare_tsig(xfr, key);
			
			/* Copy MAC from query. */
			dbg_xfr("xfr: validating TSIG from query\n");
			const uint8_t* mac = tsig_rdata_mac(tsig_rr);
			size_t mac_len = tsig_rdata_mac_length(tsig_rr);
			if (mac_len > xfr->digest_max_size) {
				ret = KNOT_EMALF;
				dbg_xfr("xfr: MAC length %zu exceeds digest "
					"maximum size %zu\n",
					mac_len, xfr->digest_max_size);
			} else {
				memcpy(xfr->digest, mac, mac_len);
				xfr->digest_size = mac_len;
				
				/* Check query TSIG. */
				ret = knot_tsig_server_check(
						tsig_rr,
						knot_packet_wireformat(qry),
						knot_packet_size(qry),
						key);
				dbg_xfr("knot_tsig_server_check() returned %s\n",
					knot_strerror(ret));
			}
			
			/* Evaluate TSIG check results. */
			switch(ret) {
			case KNOT_EOK:
				*rcode = KNOT_RCODE_NOERROR;
				break;
			case KNOT_TSIG_EBADKEY:
				xfr->tsig_rcode = KNOT_TSIG_RCODE_BADKEY;
				xfr->tsig_key = NULL;
				*rcode = KNOT_RCODE_NOTAUTH;
				break;
			case KNOT_TSIG_EBADSIG:
				xfr->tsig_rcode = KNOT_TSIG_RCODE_BADSIG;
				xfr->tsig_key = NULL;
				*rcode = KNOT_RCODE_NOTAUTH;
				break;
			case KNOT_TSIG_EBADTIME:
				xfr->tsig_rcode = KNOT_TSIG_RCODE_BADTIME;
				// store the time signed from the query
				assert(tsig_rr != NULL);
				xfr->tsig_prev_time_signed =
				                tsig_rdata_time_signed(tsig_rr);
				*rcode = KNOT_RCODE_NOTAUTH;
				break;
			case KNOT_EMALF:
				*rcode = KNOT_RCODE_FORMERR;
				break;
			default:
				*rcode = KNOT_RCODE_SERVFAIL;
			}
		}
	} else {
		dbg_xfr("xfr: failed to parse rest of the packet\n");
		*rcode = KNOT_RCODE_FORMERR;
	}
	
	return ret;
}

/*!
 * \brief XFR-IN event handler function.
 *
 * Handle single XFR client event.
 *
 * \param w Associated XFR worker.
 * \param fd Associated file descriptor.
 * \param data Transfer data.
 */
int xfr_process_event(xfrworker_t *w, int fd, knot_ns_xfr_t *data, uint8_t *buf, size_t buflen)
{
	/* Update xfer state. */
	knot_zone_t *zone = (knot_zone_t *)data->zone;
	data->wire = buf;
	data->wire_size = buflen;

	/* Handle SOA/NOTIFY responses. */
	if (data->type == XFR_TYPE_NOTIFY || data->type == XFR_TYPE_SOA) {
		return xfr_process_udp_query(w, fd, data);
	}

	/* Read DNS/TCP packet. */
	int ret = 0;
	int rcvd = tcp_recv(fd, buf, buflen, 0);
	data->wire_size = rcvd;
	if (rcvd <= 0) {
		data->wire_size = 0;
		ret = KNOT_ECONN;
	} else {

		/* Process incoming packet. */
		switch(data->type) {
		case XFR_TYPE_AIN:
			ret = knot_ns_process_axfrin(w->ns, data);
			break;
		case XFR_TYPE_IIN:
			ret = knot_ns_process_ixfrin(w->ns, data);
			break;
		default:
			ret = KNOT_EBADARG;
			break;
		}
	}

	/* AXFR-style IXFR. */
	if (ret == KNOT_ENOIXFR) {
		assert(data->type == XFR_TYPE_IIN);
		log_server_notice("%s Fallback to AXFR/IN.\n", data->msgpref);
		data->type = XFR_TYPE_AIN;
		data->msgpref[0] = 'A';
		ret = knot_ns_process_axfrin(w->ns, data);
	}

	/* Check return code for errors. */
	dbg_xfr_verb("xfr: processed incoming XFR packet (res =  %d)\n", ret);
	
	/* Finished xfers. */
	int xfer_finished = 0;
	if (ret != KNOT_EOK) {
		xfer_finished = 1;
	}
	
	/* IXFR refused, try again with AXFR. */
	if (zone && data->type == XFR_TYPE_IIN && ret == KNOT_EXFRREFUSED) {
		log_server_notice("%s Transfer failed, fallback to AXFR/IN.\n",
				  data->msgpref);
		size_t bufsize = buflen;
		data->wire_size = buflen; /* Reset maximum bufsize */
		ret = xfrin_create_axfr_query(zone->name, data,
		                              &bufsize, 1);
		/* Send AXFR/IN query. */
		if (ret == KNOT_EOK) {
			ret = data->send(data->session, &data->addr,
			                 data->wire, bufsize);
			/* Switch to AIN type XFR and return now. */
			if (ret == bufsize) {
				data->type = XFR_TYPE_AIN;
				data->msgpref[0] = 'A';
				return KNOTD_EOK;
			}
		}
	}

	/* Handle errors. */
	if (ret == KNOT_ENOXFR) {
		log_server_warning("%s Finished, %s\n",
		                   data->msgpref, knot_strerror(ret));
	} else if (ret < 0) {
		log_server_error("%s %s\n",
		                 data->msgpref, knot_strerror(ret));
	}


	/* Check finished zone. */
	int result = KNOTD_EOK;
	if (xfer_finished) {
		
		knot_zone_t *zone = (knot_zone_t *)data->zone;
		zonedata_t *zd = (zonedata_t *)knot_zone_data(zone);

		/* Only for successful xfers. */
		if (ret > 0) {
			ret = xfr_xfrin_finalize(w, data);
			
			/* AXFR bootstrap timeout. */
			rcu_read_lock();
			if (ret != KNOTD_EOK && !knot_zone_contents(zone)) {
				/* Schedule request (60 - 90s random delay). */
				int tmr_s = AXFR_BOOTSTRAP_RETRY;
				tmr_s += (30.0 * 1000) * (tls_rand());
				zd->xfr_in.bootstrap_retry = tmr_s;
				log_zone_info("%s Next attempt to bootstrap "
				              "in %d seconds.\n",
				              data->msgpref, tmr_s / 1000);
			}
			rcu_read_unlock();

			/* Update timers. */
			server_t *server = (server_t *)knot_ns_get_data(w->ns);
			zones_timers_update(zone, zd->conf, server->sched);
			
		} else {
			/* Cleanup */
			xfr_xfrin_cleanup(w, data);
		}
		
		/* Free TSIG buffers. */
		if (data->digest) {
			free(data->digest);
			data->digest = 0;
			data->digest_size = 0;
		}
		if (data->tsig_data) {
			free(data->tsig_data);
			data->tsig_data = 0;
			data->tsig_data_size = 0;
		}
		
		/* Disconnect. */
		result = KNOTD_ECONNREFUSED; /* Make it disconnect. */
	}

	return result;
}

/*! \todo Document me (issue #1586)
 */
static int xfr_client_start(xfrworker_t *w, knot_ns_xfr_t *data)
{
	/* Fetch associated zone. */
	knot_zone_t *zone = (knot_zone_t *)data->zone;
	if (!zone) {
		return KNOTD_EINVAL;
	}
	
	/* Check if not already processing. */
	zonedata_t *zd = (zonedata_t *)knot_zone_data(zone);
	if (!zd) {
		return KNOTD_EINVAL;
	}
	
	/* Enqueue to worker that has zone locked for XFR/IN. */
	int ret = pthread_mutex_trylock(&zd->xfr_in.lock);
	if (ret != 0) {
		dbg_xfr_verb("xfr: XFR/IN switching to another thread, "
		             "zone '%s' is already in transfer\n",
		             zd->conf->name);
		xfrworker_t *nextw = (xfrworker_t *)zd->xfr_in.wrkr;
		if (nextw == 0) {
			nextw = w;
		}
		evqueue_write(nextw->q, data, sizeof(knot_ns_xfr_t));
		return KNOTD_EOK;
	} else {
		zd->xfr_in.wrkr = w;
	}

	/* Connect to remote. */
	if (data->session <= 0) {
		int fd = socket_create(data->addr.family, SOCK_STREAM);
		if (fd >= 0) {
			/* Bind to specific address - if set. */
			sockaddr_update(&data->saddr);
			if (data->saddr.len > 0) {
				/* Presume port is already preset. */
				ret = bind(fd, data->saddr.ptr, data->saddr.len);
			}
		} else {
			pthread_mutex_unlock(&zd->xfr_in.lock);
			log_server_warning("%s Failed to create socket "
			                   "(type=%s, family=%s).\n",
			                   "SOCK_STREAM",
			                   data->msgpref,
			                   data->addr.family == AF_INET ?
			                   "AF_INET" : "AF_INET6");
			return KNOTD_ERROR;
		}
		
		ret = connect(fd, data->addr.ptr, data->addr.len);
		if (ret < 0) {
			pthread_mutex_unlock(&zd->xfr_in.lock);
			if (!knot_zone_contents(zone)) {
				/* Reschedule request (120 - 240s random delay). */
				int tmr_s = AXFR_BOOTSTRAP_RETRY * 2; /* Malus x2 */
				tmr_s += (int)((120.0 * 1000) * tls_rand());
				event_t *ev = zd->xfr_in.timer;
				if (ev) {
					evsched_cancel(ev->parent, ev);
					evsched_schedule(ev->parent, ev, tmr_s);
				}
				log_zone_notice("%s Bootstrap failed, next "
				                "attempt in %d seconds.\n",
				                data->msgpref, tmr_s / 1000);
			}
			return KNOTD_ECONNREFUSED;
		}

		/* Store new socket descriptor. */
		data->session = fd;
	} else {
		/* Duplicate existing socket descriptor. */
		data->session = dup(data->session);
	}

	/* Fetch zone contents. */
	rcu_read_lock();
	const knot_zone_contents_t *contents = knot_zone_contents(zone);
	if (!contents && data->type == XFR_TYPE_IIN) {
		pthread_mutex_unlock(&zd->xfr_in.lock);
		rcu_read_unlock();
		log_server_warning("%s Refusing to start IXFR/IN on zone with no "
				   "contents.\n", data->msgpref);
		return KNOTD_EINVAL;
	}

	/* Prepare TSIG key if set. */
	int add_tsig = 0;
	if (data->tsig_key) {
		if (xfr_prepare_tsig(data, data->tsig_key) == KNOT_EOK) {
			size_t data_bufsize = KNOT_NS_TSIG_DATA_MAX_SIZE;
			data->tsig_data = malloc(data_bufsize);
			if (data->tsig_data) {
				dbg_xfr("xfr: using TSIG for XFR/IN\n");
				add_tsig = 1;
				data->tsig_data_size = 0;
			} else {
				dbg_xfr("xfr: failed to allocate TSIG data "
					"buffer (%zu kB)\n",
					data_bufsize / 1024);
			}
		}
	}

	/* Create XFR query. */
	size_t bufsize = data->wire_size;
	switch(data->type) {
	case XFR_TYPE_AIN:
		ret = xfrin_create_axfr_query(zone->name, data, &bufsize, add_tsig);
		break;
	case XFR_TYPE_IIN:
		ret = xfrin_create_ixfr_query(contents, data, &bufsize, add_tsig);
		break;
	default:
		ret = KNOT_EBADARG;
		break;
	}

	/* Handle errors. */
	if (ret != KNOT_EOK) {
		pthread_mutex_unlock(&zd->xfr_in.lock);
		fprintf(stderr, "xfr: failed to create XFR query type %d: %s\n",
		        data->type, knot_strerror(ret));
		return KNOTD_ERROR;
	}

	/* Unlock zone contents. */
	rcu_read_unlock();
	
	/* Add to pending transfers. */
	knot_ns_xfr_t *task = xfr_register_task(w, data);
	
	ret = data->send(data->session, &data->addr, data->wire, bufsize);
	if (ret != bufsize) {
		pthread_mutex_unlock(&zd->xfr_in.lock);
		task->msgpref = NULL; /* Prevent doublefree. */
		xfr_free_task(task);
		return KNOTD_ECONNREFUSED;
	}
	
	/* Send XFR query. */
	log_server_info("%s Started.\n", data->msgpref);
	return KNOTD_EOK;
}

static int xfr_fd_compare(void *k1, void *k2)
{
	if (k1 < k2) {
		return -1;
	}
	
	if (k1 > k2) {
		return 1;
	}
	
	return 0;
}

static inline char xfr_strtype(knot_ns_xfr_t *xfr) {
	if (xfr->type == XFR_TYPE_IOUT) {
		return 'I';
	} else {
		return 'A';
	}
}

static int xfr_answer_axfr(knot_nameserver_t *ns, knot_ns_xfr_t *xfr)
{
	int ret = knot_ns_answer_axfr(ns, xfr);
	dbg_xfr("xfr: ns_answer_axfr() = %d.\n", ret);
	return ret;
}

static int xfr_answer_ixfr(knot_nameserver_t *ns, knot_ns_xfr_t *xfr)
{
	/* Check serial differeces. */
	int ret = KNOT_EOK;
	uint32_t serial_from = 0;
	uint32_t serial_to = 0;
	dbg_xfr_verb("Loading serials for IXFR.\n");
	ret = ns_ixfr_load_serials(xfr, &serial_from, &serial_to);
	dbg_xfr_detail("Loaded serials: from: %u, to: %u\n",
	               serial_from, serial_to);
	if (ret != KNOT_EOK) {
		return ret;
	}
	
	/* Load changesets from journal. */
	dbg_xfr_verb("Loading changesets from journal.\n");
	int chsload = zones_xfr_load_changesets(xfr, serial_from, serial_to);
	if (chsload != KNOTD_EOK) {
		/* History cannot be reconstructed, fallback to AXFR. */
		if (chsload == KNOTD_ERANGE || chsload == KNOTD_ENOENT) {
			log_server_info("%s Failed to load data from journal: "
			                " Incomplete history. "
			                "Fallback to AXFR.\n",
			                xfr->msgpref);
			xfr->type = XFR_TYPE_AOUT;
			xfr->msgpref[0] = 'A';
			return xfr_answer_axfr(ns, xfr);
		} else if (chsload == KNOTD_EMALF) {
			xfr->rcode = KNOT_RCODE_FORMERR;
		} else {
			xfr->rcode = KNOT_RCODE_SERVFAIL;
		}
		
		/* Mark all as generic error. */
		ret = KNOT_ERROR;
	}
	
	/* Finally, answer. */
	if (chsload == KNOTD_EOK) {
		ret = knot_ns_answer_ixfr(ns, xfr);
		dbg_xfr("xfr: ns_answer_ixfr() = %d.\n", ret);
	}
	
	return ret;
}

static int xfr_update_msgpref(knot_ns_xfr_t *req, const char *keytag)
{
	/* Check */
	if (req == NULL) {
		return KNOTD_EINVAL;
	}

	/* Update address. */
	if (req->msgpref) {
		free(req->msgpref);
		req->msgpref = NULL;
	}
	
	char r_addr[SOCKADDR_STRLEN];
	char *r_key = NULL;
	int r_port = sockaddr_portnum(&req->addr);
	sockaddr_tostr(&req->addr, r_addr, sizeof(r_addr));
	char *tag = NULL;
	if (keytag) {
		tag = strdup(keytag);
	} else if (req->tsig_key) {
		tag = knot_dname_to_str(req->tsig_key->name);
	}
	if (tag) {
		/* Allocate: " key '$key' " (7 extra bytes + \0)  */
		size_t dnlen = strlen(tag);
		r_key = malloc(dnlen + 7 + 1);
		if (r_key) {
			char *kp = r_key;
			memcpy(kp, " key '", 6); kp += 6;
			/* Trim trailing '.' */
			memcpy(kp, tag, dnlen); kp += dnlen - 1;
			memcpy(kp, "'", 2); /* 1 + '\0' */
		}
		free(tag);
		tag = NULL;
	}

	/* Prepare log message. */
	conf_read_lock();
	const char *zname = req->zname;
	if (zname == NULL) {
		zonedata_t *zd = NULL;
		if(req->zone != NULL) {
			zd = (zonedata_t *)knot_zone_data(req->zone);
			if (zd == NULL) {
				free(r_key);
				return KNOTD_EINVAL;
			}
		}
		
		zname = zd->conf->name;
	}
	const char *pformat = NULL;
	switch (req->type) {
	case XFR_TYPE_AIN:
		pformat = "AXFR transfer of '%s/IN' with %s:%d%s:";
		break;
	case XFR_TYPE_IIN:
		pformat = "IXFR transfer of '%s/IN' with %s:%d%s:";
		break;
	case XFR_TYPE_AOUT:
		pformat = "AXFR transfer of '%s/OUT' to %s:%d%s:";
		break;
	case XFR_TYPE_IOUT:
		pformat = "IXFR transfer of '%s/OUT' to %s:%d%s:";
		break;
	case XFR_TYPE_NOTIFY:
		pformat = "NOTIFY query of '%s' to %s:%d%s:";
		break;
	case XFR_TYPE_SOA:
		pformat = "SOA query of '%s' to %s:%d%s:";
		break;
	default:
		pformat = "";
		break;
	}

	int len = 512;
	char *msg = malloc(len + 1);
	if (msg) {
		memset(msg, 0, len + 1);
		len = snprintf(msg, len + 1, pformat, zname, r_addr, r_port,
		               r_key ? r_key : ""); 
		/* Shorten as some implementations (<C99) don't allow
		 * printing to NULL to estimate size. */
		if (len > 0) {
			msg = realloc(msg, len + 1);
		}
		
		req->msgpref = msg;
	}
	
	conf_read_unlock();
	free(r_key);
	return KNOTD_EOK;
}

/*
 * Public APIs.
 */

static xfrworker_t* xfr_worker_create(xfrhandler_t *h, knot_nameserver_t *ns)
{
	xfrworker_t *w = malloc(sizeof(xfrworker_t));
	if(!w) {
		return 0;
	}
	
	/* Set nameserver and master. */
	w->ns = ns;
	w->master = h;
	
	/* Create event queue. */
	w->q = evqueue_new();
	if (!w->q) {
		free(w);
		return 0;
	}
	
	/* Create fdset. */
	w->fdset = fdset_new();
	if (!w->fdset) {
		evqueue_free(&w->q);
		free(w);
		return 0;
	}
	
	/* Add evqueue to fdset. */
	fdset_add(w->fdset, evqueue_pollfd(w->q), OS_EV_READ);
	
	return w;
}

static void xfr_worker_free(xfrworker_t *w) {
	if (w) {
		evqueue_free(&w->q);
		fdset_destroy(w->fdset);
		free(w);
	}
}

xfrhandler_t *xfr_create(size_t thrcount, knot_nameserver_t *ns)
{
	/* Create XFR handler data. */
	xfrhandler_t *data = malloc(sizeof(xfrhandler_t));
	if (data == NULL) {
		return NULL;
	}
	memset(data, 0, sizeof(xfrhandler_t));
	
	/* Create RR mutex. */
	pthread_mutex_init(&data->rr_mx, 0);

	/* Create tasks structure and mutex. */
	pthread_mutex_init(&data->tasks_mx, 0);
	data->tasks = skip_create_list(xfr_fd_compare);
	
	/* Initialize threads. */
	data->workers = malloc(thrcount * sizeof(xfrhandler_t*));
	if(data->workers == NULL) {
		pthread_mutex_destroy(&data->rr_mx);
		free(data);
		return NULL;
	}
	
	/* Create threading unit. */
	dt_unit_t *unit = dt_create(thrcount);
	if (unit == NULL) {
		pthread_mutex_destroy(&data->rr_mx);
		free(data->workers);
		free(data);
		return NULL;
	}
	data->unit = unit;
	
	/* Create worker threads. */
	unsigned initialized = 0;
	for (unsigned i = 0; i < thrcount; ++i) {
		data->workers[i] = xfr_worker_create(data, ns);
		if(data->workers[i] == 0) {
			break;
		}
		++initialized;
	}
	
	/* Check for initialized. */
	if (initialized != thrcount) {
		for (unsigned i = 0; i < initialized; ++i) {
			xfr_worker_free(data->workers[i]);
		}
		pthread_mutex_destroy(&data->rr_mx);
		free(data->workers);
		free(data->unit);
		free(data);
		return NULL;
	}
	
	/* Assign worker threads. */
	for (unsigned i = 0; i < thrcount; ++i) {
		dt_repurpose(unit->threads[i], xfr_worker, data->workers[i]);
	}
	
	data->interrupt = xfr_interrupt;

	return data;
}

int xfr_free(xfrhandler_t *handler)
{
	if (!handler) {
		return KNOTD_EINVAL;
	}
	
	/* Free RR mutex. */
	pthread_mutex_destroy(&handler->rr_mx);

	/* Free tasks and mutex. */
	skip_destroy_list(&handler->tasks, 0,
	                  (void(*)(void*))xfr_free_task);
	pthread_mutex_destroy(&handler->tasks_mx);
	
	/* Free workers. */
	for (unsigned i = 0; i < handler->unit->size; ++i) {
		xfr_worker_free(handler->workers[i]);
	}
	free(handler->workers);

	/* Delete unit. */
	dt_delete(&handler->unit);
	free(handler);

	return KNOTD_EOK;
}

int xfr_stop(xfrhandler_t *handler)
{
	/* Break loop. */
	dt_stop(handler->unit);
	return KNOTD_EOK;
}

int xfr_join(xfrhandler_t *handler) {
	return dt_join(handler->unit);
}

int xfr_request_init(knot_ns_xfr_t *r, int type, int flags, knot_packet_t *pkt)
{
	if (!r || type < 0 || flags < 0) {
		return KNOTD_EINVAL;
	}
	
	/* Blank and init. */
	memset(r, 0, sizeof(knot_ns_xfr_t));
	r->type = type;
	r->flags = flags;
	
	/* Copy packet if applicable. */
	if (pkt != 0) {
		uint8_t *wire_copy = malloc(sizeof(uint8_t) * pkt->size);
		if (!wire_copy) {
			ERR_ALLOC_FAILED;
			return KNOTD_ENOMEM;
		}
		memcpy(wire_copy, pkt->wireformat, pkt->size);
		pkt->wireformat = wire_copy;
		r->query = pkt;
	}
	
	return KNOTD_EOK;
}

int xfr_request(xfrhandler_t *handler, knot_ns_xfr_t *req)
{
	if (!handler || !req) {
		return KNOTD_EINVAL;
	}
	
	/* Get next worker in RR fashion */
	pthread_mutex_lock(&handler->rr_mx);
	evqueue_t *q = handler->workers[handler->rr]->q;
	handler->rr = get_next_rr(handler->rr, handler->unit->size);
	pthread_mutex_unlock(&handler->rr_mx);
	
	/* Update XFR message prefix. */
	xfr_update_msgpref(req, NULL);
	
	/* Delegate request. */
	int ret = evqueue_write(q, req, sizeof(knot_ns_xfr_t));
	if (ret < 0) {
		return KNOTD_ERROR;
	}

	return KNOTD_EOK;
}

int xfr_answer(knot_nameserver_t *ns, knot_ns_xfr_t *xfr)
{
	if (ns == NULL || xfr == NULL) {
		return KNOTD_EINVAL;
	}
	
	int ret = knot_ns_init_xfr(ns, xfr);
	int xfr_failed = (ret != KNOT_EOK);
	const char * errstr = knot_strerror(ret);
	
	// use the QNAME as the zone name to get names also for
	// zones that are not in the server
	/*! \todo Update msgpref with zname from query. */
	const knot_dname_t *qname = knot_packet_qname(xfr->query);
	if (qname != NULL) {
		xfr->zname = knot_dname_to_str(qname);
	} else {
		xfr->zname = strdup("(unknown)");
	}

	/* Check requested zone. */
	if (!xfr_failed) {
		ret = zones_xfr_check_zone(xfr, &xfr->rcode);
		xfr_failed = (ret != KNOTD_EOK);
		errstr = knotd_strerror(ret);
	}

	/* Check TSIG. */
	char *keytag = NULL;
	if (!xfr_failed && xfr->tsig_key != NULL) {
		ret = xfr_check_tsig(xfr, &xfr->rcode, &keytag);
		xfr_failed = (ret != KNOT_EOK);
		errstr = knot_strerror(ret);
	}
	
	ret = xfr_update_msgpref(xfr, keytag);
	free(keytag);
	if (ret != KNOTD_EOK) {
		xfr->msgpref = strdup("XFR:");
	}
	
	/* Prepare place for TSIG data */
	xfr->tsig_data = malloc(KNOT_NS_TSIG_DATA_MAX_SIZE);
	if (xfr->tsig_data) {
		dbg_xfr("xfr: TSIG data allocated: %zu.\n",
			KNOT_NS_TSIG_DATA_MAX_SIZE);
		xfr->tsig_data_size = 0;
	} else {
		dbg_xfr("xfr: failed to allocate TSIG data "
			"buffer (%zu kB)\n",
			KNOT_NS_TSIG_DATA_MAX_SIZE / 1024);
	}
	
	/* Finally, answer AXFR/IXFR. */
	int io_error = 0;
	if (!xfr_failed) {
		switch(xfr->type) {
		case XFR_TYPE_AOUT:
			ret = xfr_answer_axfr(ns, xfr);
			break;
		case XFR_TYPE_IOUT:
			ret = xfr_answer_ixfr(ns, xfr);
			break;
		default: ret = KNOTD_ENOTSUP; break;
		}
		
		xfr_failed = (ret != KNOT_EOK);
		errstr = knot_strerror(ret);
		io_error = (ret == KNOT_ECONN);
	}
	
	/* Check results. */
	if (xfr_failed) {
		if (!io_error) {
			knot_ns_xfr_send_error(ns, xfr, xfr->rcode);
		}
		log_server_notice("%s %s\n", xfr->msgpref, errstr);
		ret = KNOTD_ERROR;
	} else {
		log_server_info("%s Finished.\n", xfr->msgpref);
		ret = KNOTD_EOK;
	}
	
	/* Free allocated data. */
	free(xfr->tsig_data);
	xfr->tsig_data = NULL;
	xfr_request_deinit(xfr);
	
	/* Cleanup. */
	free(xfr->digest);
	free(xfr->query->wireformat);   /* Free wireformat. */
	knot_packet_free(&xfr->query);  /* Free query. */
	knot_packet_free(&xfr->response);  /* Free response. */
	knot_free_changesets((knot_changesets_t **)(&xfr->data));
	free(xfr->zname);
	return ret;
}

static int xfr_process_request(xfrworker_t *w, uint8_t *buf, size_t buflen)
{
	/* Read single request. */
	knot_ns_xfr_t xfr = {};
	int ret = evqueue_read(w->q, &xfr, sizeof(knot_ns_xfr_t));
	if (ret != sizeof(knot_ns_xfr_t)) {
		dbg_xfr_verb("xfr: evqueue_read() returned %d.\n", ret);
		return KNOTD_ENOTRUNNING;
	}
	
	/* Update request. */
	xfr.wire = buf;
	xfr.wire_size = buflen;

	conf_read_lock();

	/* Handle request. */
	zonedata_t *zd = NULL;
	if(xfr.zone != NULL) {
		zd = (zonedata_t *)knot_zone_data(xfr.zone);
	}
	knot_ns_xfr_t *task = NULL;
	evsched_t *sch = NULL;
	dbg_xfr_verb("xfr: processing request type '%d'\n", xfr.type);
	dbg_xfr_verb("xfr: query ptr: %p\n", xfr.query);
	switch(xfr.type) {
	case XFR_TYPE_AIN:
	case XFR_TYPE_IIN:
		ret = xfr_client_start(w, &xfr);
		
		/* Report. */
		if (ret != KNOTD_EOK && ret != KNOTD_EACCES) {
			log_server_error("%s %s\n",
			                 xfr.msgpref, knotd_strerror(ret));
			xfr_request_deinit(&xfr);
		}
		break;
	case XFR_TYPE_SOA:
	case XFR_TYPE_NOTIFY:
		/* Register task. */
		task = xfr_register_task(w, &xfr);
		if (!task) {
			xfr_request_deinit(&xfr);
			ret = KNOTD_ENOMEM;
			break;
		}
		
		/* Add timeout. */
		sch = ((server_t *)knot_ns_get_data(w->ns))->sched;
		task->data = evsched_schedule_cb(sch, xfr_udp_timeout,
						 task, SOA_QRY_TIMEOUT);
		if (zd && xfr.type == XFR_TYPE_SOA) {
			zd->soa_pending = (event_t*)task->data;
		}
		log_server_info("%s: query issued.\n", xfr.msgpref);
		ret = KNOTD_EOK;
		break;
	/* Socket close event. */
	case XFR_TYPE_CLOSE:
		xfr_free_task((knot_ns_xfr_t *)xfr.data);
		ret = KNOTD_EOK;
		break;
	default:
		log_server_error("Unknown XFR request type (%d).\n", xfr.type);
		break;
	}

	conf_read_unlock();
	
	return ret;
}

int xfr_worker(dthread_t *thread)
{
	xfrworker_t *w = (xfrworker_t *)thread->data;	

	/* Check data. */
	if (w < 0) {
		dbg_xfr("xfr: NULL worker data, worker cancelled\n");
		return KNOTD_EINVAL;
	}

	/* Buffer for answering. */
	size_t buflen = XFR_BUFFER_SIZE;
	uint8_t* buf = malloc(buflen);
	if (buf == NULL) {
		dbg_xfr("xfr: failed to allocate buffer for XFR worker\n");
		return KNOTD_ENOMEM;
	}
	

	/* Accept requests. */
	int ret = 0;
	dbg_xfr_verb("xfr: worker=%p starting\n", w);
	for (;;) {
		
		/* Check for cancellation. */
		if (dt_is_cancelled(thread)) {
			break;
		}
		
		/* Poll fdset. */
		int nfds = fdset_wait(w->fdset, OS_EV_FOREVER);
		if (nfds <= 0) {
			continue;
		}
		
		/* Check for cancellation. */
		if (dt_is_cancelled(thread)) {
			break;
		}
		
		/* Iterate fdset. */
		xfrhandler_t *h = w->master;
		knot_ns_xfr_t *data = 0;
		int rfd = evqueue_pollfd(w->q);
		fdset_it_t it;
		fdset_begin(w->fdset, &it);
		while(1) {
			
			/* Check if it request. */
			if (it.fd == rfd) {
				dbg_xfr_verb("xfr: worker=%p processing request\n",
				             w);
				ret = xfr_process_request(w, buf, buflen);
				if (ret == KNOTD_ENOTRUNNING) {
					break;
				}
			} else {
				/* Find data. */
				pthread_mutex_lock(&h->tasks_mx);
				data = skip_find(h->tasks, (void*)((size_t)it.fd));
				pthread_mutex_unlock(&h->tasks_mx);
				if (data == NULL) {
					dbg_xfr_verb("xfr: worker=%p processing event on "
						     "fd=%d got empty data.\n",
						     w, it.fd);
					fdset_remove(w->fdset, it.fd);
					close(it.fd); /* Always dup()'d or created. */
					
					/* Next fd. */
					if (fdset_next(w->fdset, &it) < 0) {
						break;
					} else {
						continue;
					}
				}
				dbg_xfr_verb("xfr: worker=%p processing event on "
				             "fd=%d data=%p.\n",
				             w, it.fd, data);
				ret = xfr_process_event(w, it.fd, data, buf, buflen);
				if (ret != KNOTD_EOK) {
					xfr_free_task(data);
				}
			}
			
			/* Next fd. */
			if (fdset_next(w->fdset, &it) < 0) {
				break;
			}
		}
	}


	/* Stop whole unit. */
	free(buf);
	dbg_xfr_verb("xfr: worker=%p finished.\n", w);
	thread->data = 0;
	return KNOTD_EOK;
}
