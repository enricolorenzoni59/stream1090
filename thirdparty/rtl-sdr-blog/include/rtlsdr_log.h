/*
 * rtl-sdr, turns your Realtek RTL2832 based DVB dongle into a SDR receiver
 * Copyright (C) 2012-2013 by Steve Markgraf <steve@steve-m.de>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __RTLSDR_LOG_H
#define __RTLSDR_LOG_H

/*
 * stream1090 patch: library-internal logging shim.
 *
 * Every message the library used to write straight to stderr goes through
 * rtlsdr_log(), so a host can install rtlsdr_set_log_callback() (declared in
 * rtl-sdr.h) and route it through its own logger. Without a callback the
 * message still goes to stderr, so the rtl_* command line tools keep their
 * original behaviour.
 *
 * This header deliberately does not include rtl-sdr.h: the tuner sources
 * include rtlsdr_i2c.h, whose prototypes use void* and would conflict with
 * the rtlsdr_dev_t* ones in rtl-sdr.h.
 */

typedef enum {
	RTLSDR_LOG_ERROR = 0,
	RTLSDR_LOG_WARN = 1,
	RTLSDR_LOG_INFO = 2,
	RTLSDR_LOG_DEBUG = 3
} rtlsdr_log_level_t;

typedef void (*rtlsdr_log_callback_t)(rtlsdr_log_level_t level, const char *message);

void rtlsdr_log(rtlsdr_log_level_t level, const char *format, ...);

#endif /* __RTLSDR_LOG_H */
