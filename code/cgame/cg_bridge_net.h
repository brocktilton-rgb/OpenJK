/*
===========================================================================
Copyright (C) 1999 - 2005, Id Software, Inc.
Copyright (C) 2000 - 2013, Raven Software, Inc.
Copyright (C) 2001 - 2013, Activision, Inc.
Copyright (C) 2013 - 2015, OpenJK contributors

This file is part of the OpenJK source code.

OpenJK is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License version 2 as
published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, see <http://www.gnu.org/licenses/>.
===========================================================================
*/

// Non-blocking IPv4 UDP sender. No game headers, so it cannot stall inside cgame.
// host must be a numeric IPv4 address (no DNS).

#ifndef CG_BRIDGE_NET_H
#define CG_BRIDGE_NET_H

// Opens or reuses a datagram socket aimed at host:port.
// On failure, *errorOut is a short static string. Never blocks.
bool BridgeNet_Open( const char *host, int port, const char **errorOut );

void BridgeNet_Close( void );

// 0 = queued, 1 = dropped (socket buffer full / would block), -1 = hard error.
// A drop is not a stall: sendto is non-blocking and returns immediately.
int BridgeNet_Send( const void *data, int length );

#endif
