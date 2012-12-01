/*
 * This file is part of the sigrok project.
 *
 * Copyright (C) 2012 Uwe Hermann <uwe@hermann-uwe.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

/*
 * Metex 14-bytes ASCII protocol parser.
 *
 * This should work for various multimeters which use this kind of protocol,
 * even though there is some variation in which modes each DMM supports.
 *
 * It does _not_ work for all Metex DMMs, some use a quite different protocol.
 */

#include <string.h>
#include <ctype.h>
#include <math.h>
#include <glib.h>
#include "libsigrok.h"
#include "libsigrok-internal.h"

/* Message logging helpers with driver-specific prefix string. */
#define DRIVER_LOG_DOMAIN "metex14: "
#define sr_log(l, s, args...) sr_log(l, DRIVER_LOG_DOMAIN s, ## args)
#define sr_spew(s, args...) sr_spew(DRIVER_LOG_DOMAIN s, ## args)
#define sr_dbg(s, args...) sr_dbg(DRIVER_LOG_DOMAIN s, ## args)
#define sr_info(s, args...) sr_info(DRIVER_LOG_DOMAIN s, ## args)
#define sr_warn(s, args...) sr_warn(DRIVER_LOG_DOMAIN s, ## args)
#define sr_err(s, args...) sr_err(DRIVER_LOG_DOMAIN s, ## args)

static int parse_value(const uint8_t *buf, float *result)
{
	int i, sign, intval = 0, factor, decimal_point = 0, is_ol;
	float floatval;
	uint8_t digit;

	/* Byte 3: Sign (' ' or '-') */
	if (buf[3] == ' ') {
		sign = 1;
	} else if (buf[3] == '-') {
		sign = -1;
	} else {
		sr_err("Invalid sign byte: 0x%02x.", buf[3]);
		return SR_ERR;
	}

	/* Bytes 5-7: Over limit (various forms) */
	is_ol = 0;
	is_ol += !strncmp((char *)&buf[5], ".OL", 3);
	is_ol += !strncmp((char *)&buf[5], "O.L", 3);
	is_ol += !strncmp((char *)&buf[5], "OL.", 3);
	is_ol += !strncmp((char *)&buf[5], " OL", 3);
	if (is_ol != 0) {
		sr_spew("Over limit.");
		*result = INFINITY;
		return SR_OK;
	}

	/* Bytes 4-8: Value (up to 4 digits) and decimal point */
	factor = 1000;
	for (i = 0; i < 5; i++) {
		digit = buf[4 + i];
		if (digit == '.') {
			decimal_point = i;
		} else if (isdigit(digit)) {
			intval += (digit - '0') * factor;
			factor /= 10;
		} else {
			sr_err("Invalid digit byte: 0x%02x.", digit);
			return SR_ERR;
		}
	}

	floatval = (float)intval;

	/* Decimal point position */
	if (decimal_point == 0 || decimal_point == 4) {
		/* TODO: Doesn't happen? */
	} else if (decimal_point == 1) {
		floatval /= 1000;
	} else if (decimal_point == 2) {
		floatval /= 100;
	} else if (decimal_point == 3) {
		floatval /= 10;
	} else {
		sr_err("Invalid decimal point position: %d.", decimal_point);
		return SR_ERR;
	}

	/* Apply sign. */
	floatval *= sign;

	sr_spew("The display value is %f.", floatval);

	*result = floatval;

	return SR_OK;
}

static void parse_flags(const char *buf, struct metex14_info *info)
{
	/* Bytes 0-1: Measurement mode */
	/* Note: Protocol doesn't distinguish "resistance" from "beep" mode. */
	info->is_ac          = !strncmp(buf, "AC", 2);
	info->is_dc          = !strncmp(buf, "DC", 2);
	info->is_resistance  = !strncmp(buf, "OH", 2);
	info->is_capacity    = !strncmp(buf, "CA", 2);
	info->is_temperature = !strncmp(buf, "TE", 2);
	info->is_diode       = !strncmp(buf, "DI", 2);
	info->is_frequency   = !strncmp(buf, "FR", 2);

	if (info->is_dc || info->is_ac)
		info->is_volt = TRUE;

	/* Byte 2: Always space (0x20). */

	/* Bytes 3-8: See parse_value(). */

	/* Bytes 9-12: Unit */
	if (!strcmp(buf + 9, "   A"))
		info->is_ampere = TRUE;
	else if (!strcmp(buf + 9, "  mA"))
		info->is_milli = info->is_ampere = TRUE;
	else if (!strcmp(buf + 9, "   V"))
		info->is_volt = TRUE;
	else if (!strcmp(buf + 9, "  mV"))
		info->is_milli = info->is_volt = TRUE;
	else if (!strcmp(buf + 9, " Ohm"))
		info->is_ohm = TRUE;
	else if (!strcmp(buf + 9, "KOhm"))
		info->is_kilo = info->is_ohm = TRUE;
	else if (!strcmp(buf + 9, "MOhm"))
		info->is_mega = info->is_ohm = TRUE;
	else if (!strcmp(buf + 9, "  nF"))
		info->is_nano = info->is_farad = TRUE;
	else if (!strcmp(buf + 9, "  uF"))
		info->is_micro = info->is_farad = TRUE;
	else if (!strcmp(buf + 9, " KHz"))
		info->is_kilo = info->is_hertz = TRUE;
	else if (!strcmp(buf + 9, "   C"))
		info->is_celsius = TRUE;

	/* Byte 13: Always '\r' (carriage return, 0x0d, 13) */
}

static void handle_flags(struct sr_datafeed_analog *analog, float *floatval,
			 const struct metex14_info *info)
{
	/* Factors */
	if (info->is_nano)
		*floatval /= 1000000000;
	if (info->is_micro)
		*floatval /= 1000000;
	if (info->is_milli)
		*floatval /= 1000;
	if (info->is_kilo)
		*floatval *= 1000;
	if (info->is_mega)
		*floatval *= 1000000;

	/* Measurement modes */
	if (info->is_volt) {
		analog->mq = SR_MQ_VOLTAGE;
		analog->unit = SR_UNIT_VOLT;
	}
	if (info->is_ampere) {
		analog->mq = SR_MQ_CURRENT;
		analog->unit = SR_UNIT_AMPERE;
	}
	if (info->is_ohm) {
		analog->mq = SR_MQ_RESISTANCE;
		analog->unit = SR_UNIT_OHM;
	}
	if (info->is_hertz) {
		analog->mq = SR_MQ_FREQUENCY;
		analog->unit = SR_UNIT_HERTZ;
	}
	if (info->is_farad) {
		analog->mq = SR_MQ_CAPACITANCE;
		analog->unit = SR_UNIT_FARAD;
	}
	if (info->is_celsius) {
		analog->mq = SR_MQ_TEMPERATURE;
		analog->unit = SR_UNIT_CELSIUS;
	}
	if (info->is_diode) {
		analog->mq = SR_MQ_VOLTAGE;
		analog->unit = SR_UNIT_VOLT;
	}

	/* Measurement related flags */
	if (info->is_ac)
		analog->mqflags |= SR_MQFLAG_AC;
	if (info->is_dc)
		analog->mqflags |= SR_MQFLAG_DC;
}

static gboolean flags_valid(const struct metex14_info *info)
{
	int count;

	/* Does the packet have more than one multiplier? */
	count = 0;
	count += (info->is_nano) ? 1 : 0;
	count += (info->is_micro) ? 1 : 0;
	count += (info->is_milli) ? 1 : 0;
	count += (info->is_kilo) ? 1 : 0;
	count += (info->is_mega) ? 1 : 0;
	if (count > 1) {
		sr_err("More than one multiplier detected in packet.");
		return FALSE;
	}

	/* Does the packet "measure" more than one type of value? */
	count = 0;
	count += (info->is_ac) ? 1 : 0;
	count += (info->is_dc) ? 1 : 0;
	count += (info->is_resistance) ? 1 : 0;
	count += (info->is_capacity) ? 1 : 0;
	count += (info->is_temperature) ? 1 : 0;
	count += (info->is_diode) ? 1 : 0;
	count += (info->is_frequency) ? 1 : 0;
	if (count > 1) {
		sr_err("More than one measurement type detected in packet.");
		return FALSE;
	}

	/* Both AC and DC set? */
	if (info->is_ac && info->is_dc) {
		sr_err("Both AC and DC flags detected in packet.");
		return FALSE;
	}

	return TRUE;
}

SR_PRIV gboolean sr_metex14_packet_valid(const uint8_t *buf)
{
	struct metex14_info info;

	memset(&info, 0x00, sizeof(struct metex14_info));
	parse_flags((const char *)buf, &info);

	if (!flags_valid(&info))
		return FALSE;

	if (buf[13] != '\r')
		return FALSE;

	return TRUE;
}

/**
 * Parse a protocol packet.
 *
 * @param buf Buffer containing the protocol packet. Must not be NULL.
 * @param floatval Pointer to a float variable. That variable will be modified
 *                 in-place depending on the protocol packet. Must not be NULL.
 * @param analog Pointer to a struct sr_datafeed_analog. The struct will be
 *               filled with data according to the protocol packet.
 *               Must not be NULL.
 * @param info Pointer to a struct metex14_info. The struct will be filled
 *             with data according to the protocol packet. Must not be NULL.
 *
 * @return SR_OK upon success, SR_ERR upon failure. Upon errors, the
 *         'analog' variable contents are undefined and should not be used.
 */
SR_PRIV int sr_metex14_parse(const uint8_t *buf, float *floatval,
			     struct sr_datafeed_analog *analog, void *info)
{
	int ret;
	struct metex14_info *info_local;

	info_local = (struct metex14_info *)info;

	if ((ret = parse_value(buf, floatval)) != SR_OK) {
		sr_err("Error parsing value: %d.", ret);
		return ret;
	}

	memset(info_local, 0x00, sizeof(struct metex14_info));
	parse_flags((const char *)buf, info_local);
	handle_flags(analog, floatval, info_local);

	return SR_OK;
}
