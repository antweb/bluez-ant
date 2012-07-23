/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *  Copyright (C) 2012       Anton Weber <ant@antweb.me>
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

void timeval_adjust_usec(struct timeval *l, struct timeval *r);

__useconds_t
timeval_diff(struct timeval *l, struct timeval *r, struct timeval *diff);

int timeval_cmp(struct timeval *l, struct timeval *r);

inline __useconds_t
	get_timeval_passed(struct timeval *since, struct timeval *diff);
