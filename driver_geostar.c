/*
 * This is the gpsd driver for GeoStar Navigation receivers
 * operating in binary mode.
 *
 * Tested with GeoS-1M GPS/GLONASS receiver.
 *
 * By Viktar Palstsiuk, viktar.palstsiuk@promwad.com
 *
 * This file is Copyright (c) 2010 by the GPSD project
 * BSD terms apply: see the file COPYING in the distribution root for details.
 */

#include <sys/types.h>
#include <math.h>

#ifndef S_SPLINT_S
#include <unistd.h>
#endif /* S_SPLINT_S */
#include "gpsd_config.h"

#if defined (HAVE_SYS_SELECT_H)
#include <sys/select.h>
#endif
#if defined(HAVE_SYS_TIME_H)
#include <sys/time.h>
#endif

#include <string.h>

#include "bits.h"
#include "gpsd.h"

#ifdef GEOSTAR_ENABLE
#define GEOSTAR_CHANNELS	24

#define JAN_2008		0x47798280 /* 1199145600 = 2008 - 1970 in seconds */

#define OFFSET(n)		((n)*4+4)

static int decode_channel_id (unsigned long ch_id) {
	int num = 0;
	num = ch_id & 0x1F;			/* SV ID */
	if((ch_id & (1<<30)) == 0) num += 32;	/* GLONASS SV */
	else if (num == 0 ) num = 32;		/* GPS SV */
	return num;
}

static int geostar_write(struct gps_device_t *session,
			 unsigned int id,/*@null@*/ unsigned char *data, size_t len)
{
    int i;
    unsigned long cs = 0;

    putbyte(session->msgbuf, 0, 'P');
    putbyte(session->msgbuf, 1, 'S');
    putbyte(session->msgbuf, 2, 'G');
    putbyte(session->msgbuf, 3, 'G');

    putbeword(session->msgbuf, 4, id);
    putbeword(session->msgbuf, 6, len);

    /* Copy content */
    memcpy(session->msgbuf + 8, data, len * sizeof(long));

    len += 2; /* PSGG + id + len */

    /* Calculate checksum */
    for (i = 0; (size_t)i < len; i++) {
	cs ^= getleul(session->msgbuf, i * sizeof(long));
    }

    putlelong(session->msgbuf, len * sizeof(long), cs);

    len += 1; /* Checksum */

    session->msgbuflen = len * sizeof(long);

    gpsd_report(LOG_IO, "Sent GeoStar packet id 0x%x: %s\n", id,
		gpsd_hexdump_wrapper(session->msgbuf, session->msgbuflen, LOG_IO));

    if (gpsd_write(session, session->msgbuf, session->msgbuflen) !=
	(ssize_t) session->msgbuflen)
	return -1;

    return 0;
}

/* geostar_detect()
 *
 * see if it looks like a GeoStar device is listening and
 * return 1 if found, 0 if not
 */
static bool geostar_detect(struct gps_device_t *session)
{
    unsigned char buf[1 * sizeof(long)];
    unsigned int n;
    bool ret = false;
    int myfd;
    fd_set fdset;
    struct timeval to;

    gpsd_report(LOG_PROG, "Probing GeoStar\n");

    myfd = session->gpsdata.gps_fd;

    /* request firmware revision and look for a valid response */
    /*@+ignoresigns@*/
    putbelong(buf, 0, 0);
    /*@+ignoresigns@*/
    if (geostar_write(session, 0xc1, buf, 1) == 0) {
	for (n = 0; n < 3; n++) {
	    FD_ZERO(&fdset);
	    FD_SET(myfd, &fdset);
	    to.tv_sec = 1;
	    to.tv_usec = 0;
	    if (select(myfd + 1, &fdset, NULL, NULL, &to) != 1)
		break;
	    if (generic_get(session) >= 0) {
		if (session->packet.type == GEOSTAR_PACKET) {
		    gpsd_report(LOG_RAW, "geostar_detect found\n");
		    ret = true;
		    break;
		}
	    }
	}
    }

    return ret;
}

static gps_mask_t geostar_analyze(struct gps_device_t *session)
{
    int i, j, len;
    gps_mask_t mask = 0;
    unsigned int id;
    int16_t s1, s2, s3;
    uint16_t uw1, uw2;
    uint32_t ul1, ul2, ul3, ul4, ul5;
    double d1, d2, d3, d4, d5;
    union long_double l_d;
    unsigned char buf[BUFSIZ];
    char buf2[BUFSIZ];

    if (session->packet.type != GEOSTAR_PACKET) {
	gpsd_report(LOG_INF, "geostar_analyze packet type %d\n",
		    session->packet.type);
	return 0;
    }

    /*@ +charint @*/
    if (session->packet.outbuflen < 12 || session->packet.outbuffer[0] != 'P')
	return 0;

    /* put data part of message in buf */

    memset(buf, 0, sizeof(buf));
    memcpy(buf, session->packet.outbuffer, session->packet.outbuflen);

    buf2[len = 0] = '\0';
    for (i = 0; i < (int)session->packet.outbuflen; i++) {
	(void)snprintf(buf2 + strlen(buf2),
		       sizeof(buf2) - strlen(buf2),
		       "%02x", buf[len++] = session->packet.outbuffer[i]);
    }
    /*@ -charint @*/

	id = getleuw(session->packet.outbuffer, OFFSET(0));

    (void)snprintf(session->gpsdata.tag, sizeof(session->gpsdata.tag), "ID%02x", id);

    gpsd_report(LOG_IO, "GeoStar packet id 0x%02x length %d: %s\n", id, len, buf2);

    session->cycle_end_reliable = true;

    switch (id) {
    case 0x10:
	gpsd_report(LOG_INF, "Raw measurements\n");
	break;
    case 0x11:
	gpsd_report(LOG_INF, "GPS sub-frame data\n");
	break;
    case 0x12:
	gpsd_report(LOG_INF, "GLONASS sub-frame data\n");
	break;
    case 0x13:
	d1 = getled(buf, OFFSET(1));
	d2 = getled(buf, OFFSET(3));
	d3 = getled(buf, OFFSET(5));
	d4 = getled(buf, OFFSET(29)); /* GPS time */
	d5 = getled(buf, OFFSET(31)); /* GLONASS time */
	gpsd_report(LOG_INF, "ECEF coordinates %g %g %g %f %f\n", d1, d2, d3, d4, d5);
	break;
    case 0x20:
	d1 = getled(buf, OFFSET(1)); /* time */

	session->newdata.time = d1 + JAN_2008;
	session->newdata.latitude = getled(buf, OFFSET(3)) * RAD_2_DEG;
	session->newdata.longitude = getled(buf, OFFSET(5)) * RAD_2_DEG;
	session->newdata.altitude = getled(buf, OFFSET(7));
	session->gpsdata.separation = getled(buf, OFFSET(9));
	session->gpsdata.satellites_used = getlesl(buf, OFFSET(11));
	session->gpsdata.dop.gdop = getled(buf, OFFSET(13));
	session->gpsdata.dop.pdop = getled(buf, OFFSET(15));
	session->gpsdata.dop.tdop = getled(buf, OFFSET(17));
	session->gpsdata.dop.hdop = getled(buf, OFFSET(19));
	session->gpsdata.dop.vdop = getled(buf, OFFSET(21));
	session->newdata.speed = getled(buf, OFFSET(31));
	session->newdata.track = getled(buf, OFFSET(33)) * RAD_2_DEG;

	ul1 = getleul(buf, OFFSET(29)); /* status */

	if (ul1 != (uint8_t) 0) {
	    session->gpsdata.status = STATUS_NO_FIX;
	    mask |= STATUS_IS;
	} else {
	    if (session->gpsdata.status < STATUS_FIX) {
		session->gpsdata.status = STATUS_FIX;
		mask |= STATUS_IS;
	    }
	}
	mask |= TIME_IS | LATLON_IS | ALTITUDE_IS | SPEED_IS | TRACK_IS | DOP_IS | USED_IS | REPORT_IS;

	gpsd_report(LOG_INF, "Geographic coordinates %f %g %g %g %g %g\n",
		d1,
		session->newdata.latitude,
		session->newdata.longitude,
		session->newdata.altitude,
		session->newdata.speed,
		session->newdata.track);
	gpsd_report(LOG_INF, "Dilution of precision %g %g %g %g %g\n",
		session->gpsdata.dop.gdop,
		session->gpsdata.dop.pdop,
		session->gpsdata.dop.tdop,
		session->gpsdata.dop.hdop,
		session->gpsdata.dop.vdop);
	break;
    case 0x21:
	ul1 = getleul(buf, OFFSET(1));
	ul2 = getleul(buf, OFFSET(2));
	uw1 = getleuw(buf, OFFSET(3));
	uw2 = getleuw(buf, OFFSET(3) + 2);
	gpsd_report(LOG_INF, "Current receiver telemetry %x %d %d %d\n", ul1, ul2, uw1, uw2);
	if(ul1 & (1<<3)) {
	    session->newdata.mode = MODE_2D;
	}
	else {
	    session->newdata.mode = MODE_3D;
	}
	if(ul1 & (1<<2)) {
	    session->gpsdata.status = STATUS_FIX;
	}
	else {
	    session->gpsdata.status = STATUS_NO_FIX;
	    session->newdata.mode = MODE_NO_FIX;
	}

	mask |= MODE_IS | STATUS_IS;
	break;
    case 0x22:
	ul1 = getleul(buf, OFFSET(1));
	gpsd_report(LOG_INF, "SVs in view %d\n", ul1);
	session->gpsdata.satellites_visible = ul1;
	if(ul1 > GEOSTAR_CHANNELS) ul1 = GEOSTAR_CHANNELS;
	for(i = 0, j = 0; (unsigned int)i < ul1; i++) {
	    ul2 = getleul(buf, OFFSET(2) + i * 3 * sizeof(long));
	    s1 = getlesw(buf, OFFSET(3) + i * 3 * sizeof(long));
	    s2 = getlesw(buf, OFFSET(3) + 2 + i * 3 * sizeof(long));
	    s3 = getlesw(buf, OFFSET(4) + 2 + i * 3 * sizeof(long));
	    gpsd_report(LOG_INF, "ID %d Az %g El %g SNR %g\n",
			decode_channel_id(ul2), s1*0.001*RAD_2_DEG, s2*0.001*RAD_2_DEG, s3*0.1);
	    session->gpsdata.PRN[i] = decode_channel_id(ul2);
	    session->gpsdata.azimuth[i] = (int)round(s1*0.001 * RAD_2_DEG);
	    session->gpsdata.elevation[i] = (int)round(s2*0.001 * RAD_2_DEG);
	    session->gpsdata.ss[i] = s3*0.1;
	    if(ul2 & (1<<27)) {
		session->gpsdata.used[j++] = decode_channel_id(ul2);
	    }
	}
	session->gpsdata.skyview_time = NAN;
	mask |= SATELLITE_IS | USED_IS;
	break;
    case 0x3e:
	ul1 = getleul(buf, OFFSET(1));
	ul2 = getleul(buf, OFFSET(2));
	ul3 = getleul(buf, OFFSET(3));
	gpsd_report(LOG_INF, "Receiver power-up message %d %d %d\n", ul1, ul2, ul3);
	break;
    case 0x3f:
	ul1 = getleul(buf, OFFSET(1));
	ul2 = getleul(buf, OFFSET(2));
	gpsd_report(LOG_WARN, "Negative acknowledge %x %d\n", ul1, ul2);
	break;
    case 0x40:
	gpsd_report(LOG_INF, "Responce to Set initial parameters\n");
	break;
    case 0x41:
	gpsd_report(LOG_INF, "Responce to Set serial ports parameters\n");
	break;
    case 0x42:
	ul1 = getleul(buf, OFFSET(1));
	ul2 = getleul(buf, OFFSET(2));
	ul3 = getleul(buf, OFFSET(3));
	gpsd_report(LOG_INF, "Responce to Set receiver operation mode %d %d %d\n",
		    ul1, ul2, ul3);
	break;
    case 0x43:
	gpsd_report(LOG_INF, "Responce to Set navigation task solution parameters\n");
	break;
    case 0x44:
	gpsd_report(LOG_INF, "Responce to Set output data rate\n");
	break;
    case 0x46:
	gpsd_report(LOG_INF, "Responce to Assign data protocol to communication port\n");
	break;
    case 0x48:
	gpsd_report(LOG_INF, "Responce to Set GPS almanac\n");
	break;
    case 0x49:
	gpsd_report(LOG_INF, "Responce to Set GLONASS almanac\n");
	break;
    case 0x4a:
	gpsd_report(LOG_INF, "Responce to Set GPS ephemeris\n");
	break;
	case 0x4b:
	gpsd_report(LOG_INF, "Responce to Set GLONASS ephemeris\n");
	break;
    case 0x4c:
	ul1 = getleul(buf, OFFSET(1));
	ul2 = getleul(buf, OFFSET(2));
	ul3 = getleul(buf, OFFSET(3));
	ul4 = getleul(buf, OFFSET(4));
	ul5 = getleul(buf, OFFSET(5));
	gpsd_report(LOG_INF, "Responce to Set PPS paramters %d %d %d %d %d\n",
		    ul1, ul2, ul3, ul4, ul5);
	break;
    case 0x4d:
	gpsd_report(LOG_INF, "Response to Enable/disable SV in position fix\n");
	break;
    case 0x4e:
	gpsd_report(LOG_INF, "Response to Enable/disable NMEA messages\n");
	break;
    case 0x4f:
	ul1 = getleul(buf, OFFSET(1));
	ul2 = getleul(buf, OFFSET(2));
	gpsd_report(LOG_INF, "Response to Enable/disable binary messages %x %x\n",
		    ul1, ul2);
	break;
    case 0x80:
	gpsd_report(LOG_INF, "Response to Query initial parameters\n");
	break;
    case 0x81:
	gpsd_report(LOG_INF, "Response to Query serial ports parameters\n");
	break;
    case 0x82:
	ul1 = getleul(buf, OFFSET(1));
	ul2 = getleul(buf, OFFSET(2));
	ul3 = getleul(buf, OFFSET(3));
	gpsd_report(LOG_INF, "Response to Query receiver operation mode %d %d %d\n",
		    ul1, ul2, ul3);
	break;
    case 0x83:
	gpsd_report(LOG_INF, "Response to Query navigation task solution parameters\n");
	break;
    case 0x84:
	gpsd_report(LOG_INF, "Response to Query output data rate\n");
	break;
    case 0x86:
	session->driver.geostar.physical_port = getleul(buf, OFFSET(1));
	gpsd_report(LOG_INF, "Response to Query data protocol assignment to communication port\n");
	gpsd_report(LOG_INF, "Connected to physical port %d\n",
		    session->driver.geostar.physical_port);
	break;
    case 0x88:
	gpsd_report(LOG_INF, "Response to Query GPS almanac\n");
	break;
    case 0x89:
	gpsd_report(LOG_INF, "Response to Query GLONASS almanac\n");
	break;
    case 0x8a:
	gpsd_report(LOG_INF, "Response to Query GPS ephemerides\n");
	break;
    case 0x8b:
	d1 = getled(buf, OFFSET(23));
	d2 = getled(buf, OFFSET(25));
	d3 = getled(buf, OFFSET(27));
	gpsd_report(LOG_INF, "Response to Query GLONASS ephemerides %g %g %g\n",
		    d1, d2, d3);
	break;
    case 0x8c:
	ul1 = getleul(buf, OFFSET(1));
	ul2 = getleul(buf, OFFSET(2));
	ul3 = getleul(buf, OFFSET(3));
	ul4 = getleul(buf, OFFSET(4));
	ul5 = getleul(buf, OFFSET(5));
	gpsd_report(LOG_INF, "Response to Query PPS parameters %d %d %d %d %d\n",
		    ul1, ul2, ul3, ul4, ul5);
	break;
    case 0x8d:
	gpsd_report(LOG_INF, "Response to Query enable/disable status of the SV in position fix\n");
	break;
    case 0x8e:
	gpsd_report(LOG_INF, "Response to Query enable NMEA messages\n");
	break;
    case 0x8f:
	ul1 = getleul(buf, OFFSET(1));
	ul2 = getleul(buf, OFFSET(2));
	gpsd_report(LOG_INF, "Response to Query enable binary messages %x %x\n",
		    ul1, ul2);
	break;
    case 0xc0:
	gpsd_report(LOG_INF, "Response to Change operation mode command\n");
	break;
    case 0xc1:
	ul4 = getleul(buf, OFFSET(1));
	ul1 = getleul(buf, OFFSET(2));
	ul2 = getleul(buf, OFFSET(3));
	ul3 = getleul(buf, OFFSET(4));
	/*@ -formattype @*/
	(void)snprintf(session->subtype, sizeof(session->subtype), "%d.%d %d.%d.%d %x %c-%d\n",
		ul4>>16, ul4&0xFFFF, ul1>>9, (ul1>>5)&0xF, ul1&0x1F, ul2, ul3>>24, ul3&0x00FFFFFF);
	/*@ +formattype @*/
	gpsd_report(LOG_INF, "Response to Request FW version command: %s\n",
		    session->subtype);
	mask |= DEVICEID_IS;
	break;
    case 0xc2:
	gpsd_report(LOG_INF, "Response to Restart receiver command\n");
	break;
    case 0xc3:
	gpsd_report(LOG_INF, "Response to Store parameters to Flash command\n");
	break;
    case 0xd0:
	gpsd_report(LOG_INF, "Response to Erase Flash sector command\n");
	break;
    case 0xd1:
	gpsd_report(LOG_INF, "Response to Write data to Flash command\n");
	break;
    case 0xd2:
	gpsd_report(LOG_INF, "Response to Store Serial Number command\n");
	break;
    default:
	gpsd_report(LOG_WARN, "Unhandled GeoStar packet type 0x%02x\n", id);
	break;
    }

    return mask;
}

static gps_mask_t geostar_parse_input(struct gps_device_t *session)
{
    gps_mask_t st;

    if (session->packet.type == GEOSTAR_PACKET) {
	st = geostar_analyze(session);
	session->gpsdata.dev.driver_mode = MODE_BINARY;
	return st;
    } else
	return 0;
}

#ifdef ALLOW_CONTROLSEND
static ssize_t geostar_control_send(struct gps_device_t *session,
				 char *buf, size_t buflen)
/* not used by the daemon, it's for gpsctl and friends */
{
    return (ssize_t) geostar_write(session,
				(unsigned int)buf[0],
				(unsigned char *)buf + 1, (buflen - 1)/4);
}
#endif /* ALLOW_CONTROLSEND */

static void geostar_event_hook(struct gps_device_t *session, event_t event)
{
    unsigned char buf[2 * sizeof(long)];

    if (event == event_identified && event == event_reactivate) {
	/* Select binary packets */
	putbelong(buf, 0, 0xffff0000);
	putbelong(buf, 4, 0);
	(void)geostar_write(session, 0x4f, buf, 2);

	/* Poll Ports params */
	putbelong(buf, 0, 1);
	(void)geostar_write(session, 0x81, buf, 1);
	putbelong(buf, 0, 0);
	(void)geostar_write(session, 0x81, buf, 1);
	/* Poll Init params */
	(void)geostar_write(session, 0x80, buf, 1);
	/* Poll Mode */
	(void)geostar_write(session, 0x82, buf, 1);
	/* Poll Solution params */
	(void)geostar_write(session, 0x83, buf, 1);
	/* Poll Output rate */
	(void)geostar_write(session, 0x84, buf, 1);
	/* Poll Protocols assignment */
	(void)geostar_write(session, 0x86, buf, 1);
	/* Poll PPS params */
	(void)geostar_write(session, 0x8c, buf, 1);
	/* Poll NMEA packets selected */
	(void)geostar_write(session, 0x8e, buf, 1);
	/* Poll binary packets selected */
	(void)geostar_write(session, 0x8f, buf, 1);
	/* Poll Software Version */
	(void)geostar_write(session, 0xc1, buf, 1);
    }

    if (event == event_deactivate) {
	/* Perform cold restart */
	putbelong(buf, 0, 3);
	(void)geostar_write(session, 0xc2, buf, 1);
    }
}

#ifdef ALLOW_RECONFIGURE
static bool geostar_speed_switch(struct gps_device_t *session,
			      speed_t speed, char parity, int stopbits)
{
    unsigned char buf[4 * sizeof(long)];

    switch (parity) {
    case 'E':
    case 2:
	parity = (char)2;
	break;
    case 'O':
    case 1:
	parity = (char)1;
	break;
    case 'N':
    case 0:
    default:
	parity = (char)0;
	break;
    }

    putbelong(buf, 0, session->driver.geostar.physical_port);
    putbelong(buf, 4, speed);
    putbelong(buf, 8, stopbits);
    putbelong(buf, 12, parity);
    (void)geostar_write(session, 0x41, buf, 4);

    return true;	/* it would be nice to error-check this */
}

static void geostar_mode(struct gps_device_t *session, int mode)
{
    unsigned char buf[1 * sizeof(long)];

    if (mode == MODE_NMEA) {
	/* Switch to NMEA mode */
	putbelong(buf, 0, (session->driver.geostar.physical_port == 0) ? 1 : 0);
	geostar_write(session, 0x46, buf, 1);
    } else if (mode == MODE_BINARY) {
	/* Switch to binary mode */
	(void)nmea_send(session, "$GPSGG,SWPROT");
    } else {
	gpsd_report(LOG_ERROR, "unknown mode %i requested\n", mode);
    }
}
#endif /* ALLOW_RECONFIGURE */

#ifdef NTPSHM_ENABLE
static double geostar_ntp_offset(struct gps_device_t *session)
{
    return 0.31;
}
#endif /* NTPSHM_ENABLE */

/* this is everything we export */
/* *INDENT-OFF* */
const struct gps_type_t geostar_binary =
{
    .type_name      = "GeoStar binary",	/* full name of type */
    .packet_type    = GEOSTAR_PACKET,	/* associated lexer packet type */
    .trigger        = NULL,		/* no trigger */
    .channels       = GEOSTAR_CHANNELS,	/* consumer-grade GPS/GLONASS */
    .probe_detect   = geostar_detect,	/* probe for device */
    .get_packet     = generic_get,	/* use the generic packet getter */
    .parse_packet   = geostar_parse_input,	/* parse message packets */
    .rtcm_writer    = NULL,		/* doesn't accept DGPS corrections */
    .event_hook     = geostar_event_hook,	/* ifire on various lifetime events */
#ifdef ALLOW_RECONFIGURE
    .speed_switcher = geostar_speed_switch,/* change baud rate */
    .mode_switcher  = geostar_mode,	/* there is a mode switcher */
    .rate_switcher  = NULL,		/* no rate switcher */
    .min_cycle      = 1,		/* not relevant, no rate switcher */
#endif /* ALLOW_RECONFIGURE */
#ifdef ALLOW_CONTROLSEND
    .control_send   = geostar_control_send,/* how to send commands */
#endif /* ALLOW_CONTROLSEND */
#ifdef NTPSHM_ENABLE
    .ntp_offset     = geostar_ntp_offset,
#endif /* NTPSHM_ENABLE */
};
/* *INDENT-ON* */

#endif /* GEOSTAR_ENABLE */
