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

#include <string.h>
#include <stdio.h>
#include <getopt.h>
#include <stdlib.h>

#include "utils/nsupdate/nsupdate_params.h"
#include "utils/common/msg.h"
#include "libknot/util/descriptor.h"
#include "common/errcode.h"

#define DEFAULT_RETRIES 3

static void nsupdate_params_init(params_t *params)
{
	memset(params, 0, sizeof(*params));
	
	// Lists
	init_list(&params->qfiles);

	// Default values.
	params->port = DEFAULT_PORT;
	params->protocol = PROTO_ALL;
	params->udp_size = DEFAULT_UDP_SIZE;
	params->retries = DEFAULT_RETRIES;
	params->wait = DEFAULT_WAIT_INTERVAL;
	params->verbose = false;
}

void nsupdate_params_clean(params_t *params)
{
	if (params == NULL) {
		return;
	}

	// Clean up the structure.
	memset(params, 0, sizeof(*params));
}

static void nsupdate_params_help(int argc, char *argv[])
{
	printf("Usage: %s [-d] [-v] [-p port] [-t timeout] [-r retries] "
	       "[filename]\n", argv[0]);
}

int nsupdate_params_parse(params_t *params, int argc, char *argv[])
{
	int opt = 0;

	if (params == NULL || argv == NULL) {
		return KNOT_EINVAL;
	}

	nsupdate_params_init(params);

	/* Command line options processing. */
	while ((opt = getopt(argc, argv, "dvp:t:r:")) != -1) {
		switch (opt) {
		case 'd':
			params_flag_verbose(params);
			break;
		case 'v':
			params_flag_tcp(params);
			break;
		case 'r':
			if (params_parse_num(optarg, &params->retries)
			                != KNOT_EOK) {
				return KNOT_EINVAL;
			}
			break;
		case 't':
			if (params_parse_interval(optarg, &params->wait)
			                != KNOT_EOK) {
				return KNOT_EINVAL;
			}
			break;
		default:
			nsupdate_params_help(argc, argv);
			return KNOT_ENOTSUP;
		}
	}

	/* Process non-option parameters. */
	for (; optind < argc; ++optind) {
		strnode_t *n = malloc(sizeof(strnode_t));
		if (!n) { /* Params will be cleaned on exit. */
			return KNOT_ENOMEM;
		}
		n->str = argv[optind];
		add_tail(&params->qfiles, &n->n);
	}

	return KNOT_EOK;
}

