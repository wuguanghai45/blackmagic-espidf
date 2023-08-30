/*
 * This file is part of the Black Magic Debug project.
 *
 * Copyright (C) 2011  Black Sphere Technologies Ltd.
 * Written by Gareth McMullin <gareth@blacksphere.co.nz>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
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

/* This file implements the GDB Remote Serial Debugging protocol packet
 * reception and transmission as well as some convenience functions.
 */

extern "C" {
#include "general.h"
#include "gdb_if.h"
#include "gdb_packet.h"
#include "hex_utils.h"
#include "remote.h"
}

#include <stdarg.h>

#include "gdb_if.hpp"


int GDB::gdb_getpacket(char *packet, int size)
{
	unsigned char c;
	unsigned char csum;
	char recv_csum[3];
	int i;

	while(1) {
	    /* Wait for packet start */
		do {
			/* Spin waiting for a start of packet character - either a gdb
             * start ('$') or a BMP remote packet start ('!').
			 */
			do {
				packet[0] = gdb_if_getchar_to(20);
				if(packet[0] == 0xFF) return 0;

				if (packet[0]==0x04) return 1;
				if (packet[0]==0x03) return 1;
			} while ((packet[0] != '$') && (packet[0] != REMOTE_SOM));
#if PC_HOSTED == 0
			if (packet[0]==REMOTE_SOM) {
				/* This is probably a remote control packet
				 * - get and handle it */
				i=0;
				bool gettingRemotePacket=true;
				while (gettingRemotePacket) {
					c=gdb_if_getchar();
					switch (c) {
					case REMOTE_SOM: /* Oh dear, packet restarts */
						i=0;
						break;

					case REMOTE_EOM:{ /* Complete packet for processing */
						packet[i]=0;
						GDB_LOCK();
						remote_packet_process(i,packet);
						gettingRemotePacket=false;
						break;
					}
					case '$': /* A 'real' gdb packet, best stop squatting now */
						packet[0]='$';
						gettingRemotePacket=false;
						break;

					default:
						if (i<size) {
							packet[i++]=c;
						} else {
							/* Who knows what is going on...return to normality */
							gettingRemotePacket=false;
						}
						break;
					}
				}
				/* Reset the packet buffer start character to zero, because function
				 * 'remotePacketProcess()' above overwrites this buffer, and
				 * an arbitrary character may have been placed there. If this is a '$'
				 * character, this will cause this loop to be terminated, which is wrong.
				 */
				packet[0] = 0;
			}
#endif
	    } while (packet[0] != '$');

		i = 0; csum = 0;
		/* Capture packet data into buffer */
		while((c = gdb_if_getchar()) != '#') {

			if(i == size) break; /* Oh shit */

			if(c == '$') { /* Restart capture */
				i = 0;
				csum = 0;
				continue;
			}
			if(c == '}') { /* escaped char */
				c = gdb_if_getchar();
				csum += c + '}';
				packet[i++] = c ^ 0x20;
				continue;
			}
			csum += c;
			packet[i++] = c;
		}
		recv_csum[0] = gdb_if_getchar();
		recv_csum[1] = gdb_if_getchar();
		recv_csum[2] = 0;

		/* return packet if checksum matches */
		if(csum == strtol(recv_csum, NULL, 16)) break;

		/* get here if checksum fails */
		if(!no_ack_mode)
			gdb_if_putchar('-', 1); /* send nack */
	}
	if(!no_ack_mode)
		gdb_if_putchar('+', 1); /* send ack */
	packet[i] = 0;

#if PC_HOSTED == 1
	DEBUG_GDB_WIRE("%s : ", __func__);
	for(int j = 0; j < i; j++) {
		c = packet[j];
		if ((c >= 32) && (c < 127))
			DEBUG_GDB_WIRE("%c", c);
		else
			DEBUG_GDB_WIRE("\\x%02X", c);
	}
	DEBUG_GDB_WIRE("\n");
#endif
	return i;
}

void GDB::gdb_next_char(const char c, uint8_t *const csum)
{
#if PC_HOSTED == 1
	if (c >= 32 && c < 127)
		DEBUG_GDB_WIRE("%c", c);
	else
		DEBUG_GDB_WIRE("\\x%02X", c);
#endif
	if (c == '$' || c == '#' || c == '}' || c == '*') {
		gdb_if_putchar('}', 0);
		gdb_if_putchar(c ^ 0x20U, 0);
		*csum += '}' + (c ^ 0x20U);
	} else {
		gdb_if_putchar(c, 0);
		*csum += c;
	}
}

void GDB::gdb_putpacket(const char *packet, int size, char pktstart)
{
	int i;
	unsigned char csum;
	unsigned char c;
	char xmit_csum[3];
	int tries = 0;

	DEBUG_GDB("gdb_putpacket %s", packet);

	do {
		DEBUG_GDB_WIRE("%s : ", __func__);
		csum = 0;
		gdb_if_putchar(pktstart, 0);
		for(i = 0; i < size; i++) {
			c = packet[i];
#if PC_HOSTED == 1
			if ((c >= 32) && (c < 127))
				DEBUG_GDB_WIRE("%c", c);
			else
				DEBUG_GDB_WIRE("\\x%02X", c);
#endif
			if((c == '$') || (c == '#') || (c == '}') || (c == '*')) {
				gdb_if_putchar('}', 0);
				gdb_if_putchar(c ^ 0x20, 0);
				csum += '}' + (c ^ 0x20);
			} else {
				gdb_if_putchar(c, 0);
				csum += c;
			}
		}
		gdb_if_putchar('#', 0);
		snprintf(xmit_csum, sizeof(xmit_csum), "%02X", csum);
		gdb_if_putchar(xmit_csum[0], 0);
		gdb_if_putchar(xmit_csum[1], 1);
		DEBUG_GDB_WIRE("\n");
	} while(!no_ack_mode && (gdb_if_getchar_to(2000) != '+') && (tries++ < 3));
}

void GDB::gdb_putpacket_f(const char *fmt, ...)
{
	va_list ap;
	char *buf;
	int size;

	va_start(ap, fmt);
	size = vasprintf(&buf, fmt, ap);
	gdb_putpacket(buf, size);
	free(buf);
	va_end(ap);
}
void GDB::gdb_putnotifpacket_f(const char *fmt, ...)
{
	va_list ap;
	char *buf;
	int size;

	va_start(ap, fmt);
	size = vasprintf(&buf, fmt, ap);
	gdb_putpacket(buf, size, '%');
	free(buf);
	va_end(ap);
}

void GDB::gdb_out(const char *buf)
{
	char *hexdata;
	int i;

	hexdata = (char*)alloca((i = strlen(buf)*2 + 1) + 1);
	hexdata[0] = 'O';
	hexify(hexdata+1, buf, strlen(buf));
	gdb_putpacket(hexdata, i);
}

void GDB::gdb_put_notification(const char *const packet, const size_t size)
{
	char xmit_csum[3];

	DEBUG_GDB_WIRE("%s: ", __func__);
	uint8_t csum = 0;
	gdb_if_putchar('%', 0);
	for (size_t i = 0; i < size; ++i)
		gdb_next_char(packet[i], &csum);
	gdb_if_putchar('#', 0);
	snprintf(xmit_csum, sizeof(xmit_csum), "%02X", csum);
	gdb_if_putchar(xmit_csum[0], 0);
	gdb_if_putchar(xmit_csum[1], 1);
	DEBUG_GDB_WIRE("\n");
}


void GDB::gdb_voutf(const char *fmt, va_list ap)
{
	char *buf;

	if (vasprintf(&buf, fmt, ap) < 0)
		return;
	gdb_out(buf);
	free(buf);
}

void GDB::gdb_outf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	gdb_voutf(fmt, ap);
	va_end(ap);
}


void GDB::gdb_putpacket2(const char *const packet1, const size_t size1, const char *const packet2, const size_t size2)
{
	char xmit_csum[3];
	size_t tries = 0;

	DEBUG_GDB("gdb_putpacket2 %s", packet1);
	DEBUG_GDB("gdb_putpacket2 %s", packet2);

	do {
		DEBUG_GDB_WIRE("%s: ", __func__);
		uint8_t csum = 0;
		gdb_if_putchar('$', 0);

		for (size_t i = 0; i < size1; ++i)
			gdb_next_char(packet1[i], &csum);
		for (size_t i = 0; i < size2; ++i)
			gdb_next_char(packet2[i], &csum);

		gdb_if_putchar('#', 0);
		snprintf(xmit_csum, sizeof(xmit_csum), "%02X", csum);
		gdb_if_putchar(xmit_csum[0], 0);
		gdb_if_putchar(xmit_csum[1], 1);
		DEBUG_GDB_WIRE("\n");
	} while (gdb_if_getchar_to(2000) != '+' && tries++ < 3U);
}