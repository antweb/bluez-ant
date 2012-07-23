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

#include <stdlib.h>
#include <sys/time.h>

#include "time.h"

/*
 * Adjust timeval structs to make sure usec difference is not negative
 * see http://www.gnu.org/software/libc/manual/html_node/Elapsed-Time.html
 */
void timeval_adjust_usec(struct timeval *l, struct timeval *r)
{
	int tmpsec;

	if (r->tv_usec > l->tv_usec) {
		tmpsec = (r->tv_usec - l->tv_usec) / 1000000 + 1;
		r->tv_sec += tmpsec;
		r->tv_usec -= 1000000 * tmpsec;
	}

	if ((l->tv_usec - r->tv_usec) > 1000000) {
		tmpsec = (l->tv_usec - r->tv_usec) / 1000000;
		r->tv_sec -= tmpsec;
		r->tv_usec += 1000000 * tmpsec;
	}
}

__useconds_t
timeval_diff(struct timeval *l, struct timeval *r, struct timeval *diff)
{
	static struct timeval tmp;

	timeval_adjust_usec(l, r);

	/* use local variable if we only need return value */
	if (diff == NULL)
		diff = &tmp;

	diff->tv_sec = l->tv_sec - r->tv_sec;
	diff->tv_usec = l->tv_usec - r->tv_usec;

	return (diff->tv_sec * 1000000) + diff->tv_usec;
}

int timeval_cmp(struct timeval *l, struct timeval *r)
{
	timeval_adjust_usec(l, r);

	if (l->tv_sec > r->tv_sec) {
		return 1;
	} else if (l->tv_sec < r->tv_sec) {
		return -1;
	} else {
		if (l->tv_usec > r->tv_usec)
			return 1;
		else if (l->tv_usec < r->tv_usec)
			return -1;
		else
			return 0;
	}
}

inline __useconds_t
get_timeval_passed(struct timeval *since, struct timeval *diff)
{
	struct timeval now;

	gettimeofday(&now, NULL);

	return timeval_diff(&now, since, diff);
}
