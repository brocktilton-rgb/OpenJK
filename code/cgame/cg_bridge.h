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

// Opt-in UDP camera / player-pose stream. See docs/BRIDGE.md for the byte layout.
// Included after the cgame headers that provide stereoFrame_t.

#ifndef CG_BRIDGE_H
#define CG_BRIDGE_H

#define BRIDGE_VERSION				1
#define BRIDGE_PACKET_POSE			1
#define BRIDGE_PACKET_MAP			2

// Common header is 20 bytes. Sizes include that header.
#define BRIDGE_POSE_PACKET_SIZE		160
#define BRIDGE_MAP_PACKET_SIZE		84
#define BRIDGE_MAP_NAME_LEN			64

#define BRIDGE_DEFAULT_PORT			"27420"

#define BRIDGE_FLAG_THIRDPERSON		0x00000001u
#define BRIDGE_FLAG_CINEMATIC		0x00000002u
#define BRIDGE_FLAG_ZOOM			0x00000004u
#define BRIDGE_FLAG_INTERMISSION	0x00000008u
#define BRIDGE_FLAG_PLAYER_VALID	0x00000010u

void Bridge_Init( void );
void Bridge_Shutdown( void );
void Bridge_Frame( int stereoView );

#endif
