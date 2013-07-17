/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *  Copyright (C) 2012       Anton Weber <ant@antweb.me>
 *  Copyright (C) 2000-2002  Maxim Krasnyansky <maxk@qualcomm.com>
 *  Copyright (C) 2003-2011  Marcel Holtmann <marcel@holtmann.org>
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

#include <stdint.h>

#include "hcidump-common.h"
#include "lib/hci.c"

int parse_pktlog (int fd, struct frame *frm, struct pktlog_hdr *hdr)
{
	int err;
	uint64_t ts;

	switch (hdr->type) {
	case 0x00:
		((uint8_t *) frm->data)[0] = HCI_COMMAND_PKT;
		frm->in = 0;
		break;
	case 0x01:
		((uint8_t *) frm->data)[0] = HCI_EVENT_PKT;
		frm->in = 1;
		break;
	case 0x02:
		((uint8_t *) frm->data)[0] = HCI_ACLDATA_PKT;
		frm->in = 0;
		break;
	case 0x03:
		((uint8_t *) frm->data)[0] = HCI_ACLDATA_PKT;
		frm->in = 1;
		break;
	default:
		lseek(fd, ntohl(hdr->len) - 9, SEEK_CUR);
		return 0;
	}

	frm->data_len = ntohl(hdr->len) - 8;
	err = read_n(fd, frm->data + 1, frm->data_len - 1);

	ts = ntoh64(hdr->ts);
	frm->ts.tv_sec = ts >> 32;
	frm->ts.tv_usec = ts & 0xffffffff;

	return err;
}

int parse_btsnoop (int fd, struct frame *frm, struct btsnoop_hdr *hdr) {
	uint32_t opcode;
	uint8_t pkt_type;
	struct btsnoop_pkt dp;
	int err;
	uint64_t ts;

	uint32_t btsnoop_type = ntohl(hdr->type);

	err = read_n(fd, (void *) &dp, BTSNOOP_PKT_SIZE);
	if (err < 0)
		return err;
	else if (err == 0)
		return 0;

	switch (btsnoop_type) {
	case 1001:
		if (ntohl(dp.flags) & 0x02) {
			if (ntohl(dp.flags) & 0x01)
				pkt_type = HCI_EVENT_PKT;
			else
				pkt_type = HCI_COMMAND_PKT;
		} else
			pkt_type = HCI_ACLDATA_PKT;

		((uint8_t *) frm->data)[0] = pkt_type;

		frm->data_len = ntohl(dp.len) + 1;
		err = read_n(fd, frm->data + 1, frm->data_len - 1);
		break;

	case 1002:
		frm->data_len = ntohl(dp.len);
		err = read_n(fd, frm->data, frm->data_len);
		break;

	case 2001:
		opcode = ntohl(dp.flags) & 0xffff;

		switch (opcode) {
		case 2:
			pkt_type = HCI_COMMAND_PKT;
			frm->in = 0;
			break;
		case 3:
			pkt_type = HCI_EVENT_PKT;
			frm->in = 1;
			break;
		case 4:
			pkt_type = HCI_ACLDATA_PKT;
			frm->in = 0;
			break;
		case 5:
			pkt_type = HCI_ACLDATA_PKT;
			frm->in = 1;
			break;
		case 6:
			pkt_type = HCI_SCODATA_PKT;
			frm->in = 0;
			break;
		case 7:
			pkt_type = HCI_SCODATA_PKT;
			frm->in = 1;
			break;
		default:
			pkt_type = 0xff;
			break;
		}

		((uint8_t *) frm->data)[0] = pkt_type;

		frm->data_len = ntohl(dp.len) + 1;
		err = read_n(fd, frm->data + 1, frm->data_len - 1);
	}

	frm->in = ntohl(dp.flags) & 0x01;
	ts = ntoh64(dp.ts) - 0x00E03AB44A676000ll;
	frm->ts.tv_sec = (ts / 1000000ll) + 946684800ll;
	frm->ts.tv_usec = ts % 1000000ll;

	return err;
}

int parse_hcidump (int fd, struct frame *frm, struct hcidump_hdr *hdr)
{
	int err;

	frm->data_len = btohs(hdr->len);
	err = read_n(fd, frm->data, frm->data_len);

	frm->in = hdr->in;
	frm->ts.tv_sec  = btohl(hdr->ts_sec);
	frm->ts.tv_usec = btohl(hdr->ts_usec);

	return err;
}
