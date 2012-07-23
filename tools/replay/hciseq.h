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

struct frame {
	void *data;
	uint32_t data_len;
	void *ptr;
	uint32_t len;
	uint16_t dev_id;
	uint8_t in;
	uint8_t master;
	uint16_t handle;
	uint16_t cid;
	uint16_t num;
	uint8_t dlci;
	uint8_t channel;
	unsigned long flags;
	struct timeval ts;
	int pppdump_fd;
	int audio_fd;
};

enum hciseq_action {
	HCISEQ_ACTION_REPLAY = 0,
};

struct hciseq_list {
	struct hciseq_node *frames;
	struct hciseq_node *current;
	int len;
};

struct hciseq_attr {
	struct timeval ts_rel;
	struct timeval ts_diff;
	enum hciseq_action action;
};

struct hciseq_node {
	struct frame *frame;
	struct hciseq_node *next;
	struct hciseq_attr *attr;
};

void calc_rel_ts(struct hciseq_list *seq);
