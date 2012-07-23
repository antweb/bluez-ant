/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *  Copyright (C) 2012       Anton Weber <ant@antweb.me>
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

#include "hciseq.h"

struct hciseq_type_cfg {
	struct hciseq_attr *cmd[9216]; /*
					 * opcodes 0x0000 - 0x23FF
					 * (OGF 0x01 - 0x08)
					 */
	struct hciseq_attr *evt[256]; /* events 0x00 - 0xFF */
	struct hciseq_attr *acl;
};

int parse_config(char *path, struct hciseq_list *_seq,
		 struct hciseq_type_cfg *_type_cfg, bool _verbose);
