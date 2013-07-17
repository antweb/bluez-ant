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

#include <errno.h>
#include <unistd.h>
#include <sys/time.h>

struct frame {
	void		*data;
	uint32_t	data_len;
	void		*ptr;
	uint32_t	len;
	uint16_t	dev_id;
	uint8_t		in;
	uint8_t		master;
	uint16_t	handle;
	uint16_t	cid;
	uint16_t	num;
	uint8_t		dlci;
	uint8_t		channel;
	unsigned long	flags;
	struct timeval	ts;
	int		pppdump_fd;
	int		audio_fd;
};

struct btsnoop_hdr {
	uint8_t id[8];		/* Identification Pattern */
	uint32_t version;	/* Version Number = 1 */
	uint32_t type;		/* Datalink Type */
} __attribute__ ((packed));
#define BTSNOOP_HDR_SIZE (sizeof(struct btsnoop_hdr))

struct btsnoop_pkt {
	uint32_t size;		/* Original Length */
	uint32_t len;		/* Included Length */
	uint32_t flags;		/* Packet Flags */
	uint32_t drops;		/* Cumulative Drops */
	uint64_t ts;		/* Timestamp microseconds */
	uint8_t data[0];	/* Packet Data */
} __attribute__ ((packed));
#define BTSNOOP_PKT_SIZE (sizeof(struct btsnoop_pkt))

struct hcidump_hdr {
	uint16_t	len;
	uint8_t		in;
	uint8_t		pad;
	uint32_t	ts_sec;
	uint32_t	ts_usec;
} __attribute__ ((packed));
#define HCIDUMP_HDR_SIZE (sizeof(struct hcidump_hdr))

struct pktlog_hdr {
	uint32_t	len;
	uint64_t	ts;
	uint8_t		type;
} __attribute__ ((packed));
#define PKTLOG_HDR_SIZE (sizeof(struct pktlog_hdr))

static const uint8_t btsnoop_id[] = { 0x62, 0x74, 0x73, 0x6e,
				      0x6f, 0x6f, 0x70, 0x00 };

static inline int read_n(int fd, char *buf, int len)
{
	int t = 0, w;

	while (len > 0) {
		if ((w = read(fd, buf, len)) < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			return -1;
		}
		if (!w)
			return 0;
		len -= w; buf += w; t += w;
	}
	return t;
}

static inline int write_n(int fd, char *buf, int len)
{
	int t = 0, w;

	while (len > 0) {
		if ((w = write(fd, buf, len)) < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			return -1;
		}
		if (!w)
			return 0;
		len -= w; buf += w; t += w;
	}
	return t;
}

int parse_pktlog (int fd, struct frame *frm, struct pktlog_hdr *hdr);
int parse_btsnoop (int fd, struct frame *frm, struct btsnoop_hdr *hdr);
int parse_hcidump (int fd, struct frame *frm, struct hcidump_hdr *hdr);
