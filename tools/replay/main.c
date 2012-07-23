/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *  Copyright (C) 2012       Anton Weber <ant@antweb.me>
 *  Copyright (C) 2011-2012  Intel Corporation
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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>

#include "main.h"
#include "lib/bluetooth.h"
#include "lib/hci.h"
#include "monitor/bt.h"
#include "monitor/btsnoop.h"
#include "monitor/control.h"
#include "monitor/packet.h"

#define MAX_EPOLL_EVENTS 1
#define MAX_MSG 128

static struct hciseq_list dumpseq;

static int fd;
static int pos = 1;
static int skipped = 0;

static int epoll_fd;
static struct epoll_event epoll_event;

static int timeout = -1;
static bool verbose = false;

static inline int read_n(int fd, char *buf, int len)
{
	int t = 0, w;

	while (len > 0) {
		w = read(fd, buf, len);
		if (w < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			return -1;
		} else if (w == 0) {
			return 0;
		}

		len -= w;
		buf += w;
		t += w;
	}

	return t;
}

static int
parse_btsnoop(int fd, struct frame *frm, struct btsnoop_hdr *hdr)
{
	struct btsnoop_pkt pkt;
	uint8_t pkt_type;
	uint64_t ts;
	int n;

	n = read_n(fd, (void *) &pkt, BTSNOOP_PKT_SIZE);
	if (n < 0)
		return -1;
	else if (n == 0)
		return 0;

	switch (ntohl(hdr->type)) {
	case 1001:
		if (ntohl(pkt.flags) & 0x02) {
			if (ntohl(pkt.flags) & 0x01)
				pkt_type = HCI_EVENT_PKT;
			else
				pkt_type = HCI_COMMAND_PKT;
		} else
			pkt_type = HCI_ACLDATA_PKT;

		((uint8_t *) frm->data)[0] = pkt_type;

		frm->data_len = ntohl(pkt.len) + 1;
		n = read_n(fd, frm->data + 1, frm->data_len - 1);
		break;

	case 1002:
		frm->data_len = ntohl(pkt.len);
		n = read_n(fd, frm->data, frm->data_len);
		break;
	}

	frm->in = ntohl(pkt.flags) & 0x01;
	ts = ntoh64(pkt.ts) - 0x00E03AB44A676000ll;
	frm->ts.tv_sec = (ts / 1000000ll) + 946684800ll;
	frm->ts.tv_usec = ts % 1000000ll;

	return n;
}

static int parse_dump(int fd, struct hciseq_list *seq)
{
	struct frame *frm;
	struct btsnoop_hdr bh;
	int n, count;
	struct hciseq_node *nodeptr, *last;

	last = seq->current;

	/* read BTSnoop header once */
	if (read_n(fd, (void *) &bh, BTSNOOP_HDR_SIZE) != BTSNOOP_HDR_SIZE)
		return -1;

	/* check for "btsnoop" string in header */
	if (memcmp(bh.id, btsnoop_id, sizeof(btsnoop_id)) != 0)
		return -1;

	count = seq->len;
	while (1) {
		frm = malloc(sizeof(*frm));
		frm->data = malloc(HCI_MAX_FRAME_SIZE);

		n = parse_btsnoop(fd, frm, &bh);
		if (n <= 0) {
			free(frm->data);
			free(frm);
			return n;
		}

		frm->ptr = frm->data;
		frm->len = frm->data_len;

		nodeptr = malloc(sizeof(*nodeptr));
		nodeptr->frame = frm;
		nodeptr->attr = malloc(sizeof(*nodeptr->attr));
		nodeptr->attr->action = HCISEQ_ACTION_REPLAY;

		if (last == NULL)
			seq->frames = nodeptr;
		else
			last->next = nodeptr;

		last = nodeptr;
		nodeptr->next = NULL;
		seq->len = ++count;
	}

	return 0;
}

static void dump_frame(struct frame *frm)
{
	struct timeval tv;
	uint8_t pkt_type;

	gettimeofday(&tv, NULL);

	pkt_type = ((const uint8_t *) frm->data)[0];
	switch (pkt_type) {
	case BT_H4_CMD_PKT:
		packet_hci_command(&tv, 0x00, frm->data + 1,
							frm->data_len - 1);
		break;
	case BT_H4_EVT_PKT:
		packet_hci_event(&tv, 0x00, frm->data + 1,
							frm->data_len - 1);
		break;
	case BT_H4_ACL_PKT:
		if (frm->in)
			packet_hci_acldata(&tv, 0x00, 0x01,
							frm->data + 1,
							frm->data_len - 1);
		else
			packet_hci_acldata(&tv, 0x00, 0x00,
							frm->data + 1,
							frm->data_len - 1);
		break;
	default:
		/* TODO: raw dump */
		break;
	}
}

static int send_frm(struct frame *frm)
{
	return write(fd, frm->data, frm->data_len);
}

static int recv_frm(int fd, struct frame *frm)
{
	int i, n;
	int nevs;
	uint8_t buf[HCI_MAX_FRAME_SIZE];
	struct epoll_event ev[MAX_EPOLL_EVENTS];

	nevs = epoll_wait(epoll_fd, ev, MAX_EPOLL_EVENTS, timeout);
	if (nevs < 0)
		return -1;
	else if (nevs == 0)
		return 0;

	for (i = 0; i < nevs; i++) {
		if (ev[i].events & (EPOLLERR | EPOLLHUP))
			return -1;

		n = read(fd, (void *) &buf, HCI_MAX_FRAME_SIZE);
		if (n > 0) {
			memcpy(frm->data, buf, n);
			frm->data_len = n;
		}
	}

	return n;
}

static bool check_match(struct frame *l, struct frame *r, char *msg)
{
	uint8_t type_l = ((const uint8_t *) l->data)[0];
	uint8_t type_r = ((const uint8_t *) r->data)[0];
	uint16_t opcode_l, opcode_r;
	uint8_t evt_l, evt_r;

	if (type_l != type_r) {
		snprintf(msg, MAX_MSG,
			 "! Wrong packet type - expected (0x%2.2x), was (0x%2.2x)",
			 type_l, type_r);
		return false;
	}

	switch (type_l) {
	case BT_H4_CMD_PKT:
		opcode_l = *((uint16_t *) (l->data + 1));
		opcode_r = *((uint16_t *) (r->data + 1));
		if (opcode_l != opcode_r) {
			snprintf(msg, MAX_MSG,
				"! Wrong opcode - expected (0x%2.2x|0x%4.4x), was (0x%2.2x|0x%4.4x)",
				cmd_opcode_ogf(opcode_l),
				cmd_opcode_ocf(opcode_l),
				cmd_opcode_ogf(opcode_r),
				cmd_opcode_ocf(opcode_r));
			return false;
		} else {
			return true;
		}
	case BT_H4_EVT_PKT:
		evt_l = *((uint8_t *) (l->data + 1));
		evt_r = *((uint8_t *) (r->data + 1));
		if (evt_l != evt_r) {
			snprintf(msg, MAX_MSG,
				"! Wrong event type - expected (0x%2.2x), was (0x%2.2x)",
				evt_l, evt_r);
			return false;
		} else {
			return true;
		}
	case BT_H4_ACL_PKT:
		if (l->data_len != r->data_len)
			return false;

		return memcmp(l->data, r->data, l->data_len) == 0;
	default:
		snprintf(msg, MAX_MSG, "! Unknown packet type (0x%2.2x)",
								type_l);

		if (l->data_len != r->data_len)
			return false;

		return memcmp(l->data, r->data, l->data_len) == 0;
	}
}

static bool process_in()
{
	static struct frame frm;
	static uint8_t data[HCI_MAX_FRAME_SIZE];
	int n;
	bool match;
	char msg[MAX_MSG];

	frm.data = &data;
	frm.ptr = frm.data;

	n = recv_frm(fd, &frm);
	if (n < 0) {
		perror("Could not receive\n");
		return false;
	}

	/* is this the packet in the sequence? */
	msg[0] = '\0';
	match = check_match(dumpseq.current->frame, &frm, msg);

	/* process packet if match */
	if (match)
		printf("[%4d/%4d] ", pos, dumpseq.len);
	else
		printf("[ Unknown ] %s\n            ", msg);

	dump_frame(&frm);

	return match;
}

static bool process_out()
{
	uint8_t pkt_type;

	pkt_type = ((const uint8_t *) dumpseq.current->frame->data)[0];

	switch (pkt_type) {
	case BT_H4_EVT_PKT:
	case BT_H4_ACL_PKT:
		printf("[%4d/%4d] ", pos, dumpseq.len);
		dump_frame(dumpseq.current->frame);
		send_frm(dumpseq.current->frame);
		break;
	default:
		printf("Unsupported packet 0x%2.2x\n", pkt_type);
		break;
	}

	return true;
}

static void process()
{
	bool processed;

	do {
		if (dumpseq.current->frame->in == 1)
			processed = process_out();
		else
			processed = process_in();

		if (processed) {
			dumpseq.current = dumpseq.current->next;
			pos++;
		}
	} while (dumpseq.current != NULL);

	printf("Done\n");
	printf("Processed %d out of %d\n", dumpseq.len - skipped,
							dumpseq.len);
}

static int vhci_open()
{
	fd = open("/dev/vhci", O_RDWR | O_NONBLOCK);
	epoll_fd = epoll_create1(EPOLL_CLOEXEC);
	if (epoll_fd < 0)
		return -1;

	epoll_event.events = EPOLLIN;
	epoll_event.data.fd = fd;

	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD,
			epoll_event.data.fd, &epoll_event) < 0) {
		return -1;
	}

	return fd;
}

static int vhci_close()
{
	epoll_ctl(epoll_fd, EPOLL_CTL_DEL, epoll_event.data.fd, NULL);
	return close(fd);
}

static void delete_list()
{
	struct hciseq_node *node, *tmp;

	node = dumpseq.frames;
	while (node != NULL) {
		tmp = node;
		node = node->next;

		free(tmp->frame->data);
		free(tmp->frame);
		free(tmp->attr);
		free(tmp);
	}
}

static void usage(void)
{
	printf("hcireplay - Bluetooth replayer\n"
	       "Usage:\thcireplay-client [options] file...\n"
	       "options:\n"
	       "\t-v, --verbose                Enable verbose output\n"
	       "\t    --version                Give version information\n"
	       "\t    --help                   Give a short usage message\n");
}

static const struct option main_options[] = {
	{"verbose", no_argument, NULL, 'v'},
	{"version", no_argument, NULL, 'V'},
	{"help", no_argument, NULL, 'H'},
	{}
};

int main(int argc, char *argv[])
{
	int dumpfd;
	int i;

	while (1) {
		int opt;

		opt = getopt_long(argc, argv, "v",
						main_options, NULL);
		if (opt < 0)
			break;

		switch (opt) {
		case 'v':
			verbose = true;
			break;
		case 'V':
			printf("%s\n", VERSION);
			return EXIT_SUCCESS;
		case 'H':
			usage();
			return EXIT_SUCCESS;
		default:
			return EXIT_FAILURE;
		}
	}

	if (optind >= argc) {
		usage();
		return EXIT_FAILURE;
	}

	dumpseq.current = NULL;
	dumpseq.frames = NULL;
	for (i = optind; i < argc; i++) {
		dumpfd = open(argv[i], O_RDONLY);
		if (dumpfd < 0) {
			perror("Failed to open dump file");
			return EXIT_FAILURE;
		}

		if (parse_dump(dumpfd, &dumpseq) < 0) {
			fprintf(stderr, "Error parsing dump file\n");
			vhci_close();
			return EXIT_FAILURE;
		}
	}
	dumpseq.current = dumpseq.frames;

	/*
	 * make sure we open the interface after parsing
	 * through all files so we can start without delay
	 */
	fd = vhci_open();
	if (fd < 0) {
		perror("Failed to open VHCI interface");
		return EXIT_FAILURE;
	}

	printf("Running\n");

	process();

	vhci_close();
	delete_list();
	printf("Terminating\n");

	return EXIT_SUCCESS;
}
