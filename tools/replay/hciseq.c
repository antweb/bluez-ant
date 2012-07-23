/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *  Copyright (C) 2012       Anton Weber <ant@antweb.me>
 *  Copyright (C) 2011-2012  Intel Corporation
 *  Copyright (C) 2004-2010  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include <stdlib.h>
#include <stdint.h>

#include "hciseq.h"
#include "time.h"
#include "monitor/bt.h"

void calc_rel_ts(struct hciseq_list *seq)
{
	struct timeval start;
	struct hciseq_node *tmp;

	start = seq->current->frame->ts;
	tmp = seq->current;

	/* first packet */
	tmp->attr->ts_rel.tv_sec = 0;
	tmp->attr->ts_rel.tv_usec = 0;
	tmp->attr->ts_diff.tv_sec = 0;
	tmp->attr->ts_diff.tv_usec = 0;

	while (tmp->next != NULL) {
		timeval_diff(&tmp->next->frame->ts, &start,
				&tmp->next->attr->ts_rel);
		timeval_diff(&tmp->next->frame->ts, &tmp->frame->ts,
				&tmp->next->attr->ts_diff);
		tmp = tmp->next;
	}
}
