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

// cg_bridge.cpp -- opt-in UDP stream of the rendered camera and the player pose.
// Byte layout: docs/BRIDGE.md. Sockets live in cg_bridge_net.cpp.

#include "cg_headers.h"

#include "cg_media.h"
#include "cg_bridge.h"
#include "cg_bridge_net.h"

#include <cstdint>
#include <cstring>

static vmCvar_t		bridge_enable;
static vmCvar_t		bridge_host;
static vmCvar_t		bridge_port;
static vmCvar_t		bridge_rate;

static qboolean		s_inited = qfalse;
static qboolean		s_streaming = qfalse;
static uint32_t		s_sequence = 1;
static int			s_lastPoseMs = 0;
static int			s_lastMapMs = 0;
static char			s_lastMap[BRIDGE_MAP_NAME_LEN];
static char			s_lastComplain[256];

static uint8_t *Bridge_WriteU16( uint8_t *p, unsigned int value )
{
	p[0] = (uint8_t)( value & 0xffu );
	p[1] = (uint8_t)( ( value >> 8 ) & 0xffu );
	return p + 2;
}

static uint8_t *Bridge_WriteU32( uint8_t *p, uint32_t value )
{
	p[0] = (uint8_t)( value & 0xffu );
	p[1] = (uint8_t)( ( value >> 8 ) & 0xffu );
	p[2] = (uint8_t)( ( value >> 16 ) & 0xffu );
	p[3] = (uint8_t)( ( value >> 24 ) & 0xffu );
	return p + 4;
}

static uint8_t *Bridge_WriteI32( uint8_t *p, int value )
{
	return Bridge_WriteU32( p, (uint32_t)value );
}

// Host float bits, stored little-endian. Correct on both endiannesses.
static uint8_t *Bridge_WriteF32( uint8_t *p, float value )
{
	uint32_t bits;
	static_assert( sizeof( float ) == 4, "bridge packets require IEEE754 binary32" );
	memcpy( &bits, &value, sizeof( bits ) );
	return Bridge_WriteU32( p, bits );
}

static uint8_t *Bridge_WriteVec3( uint8_t *p, const vec3_t v )
{
	p = Bridge_WriteF32( p, v[0] );
	p = Bridge_WriteF32( p, v[1] );
	p = Bridge_WriteF32( p, v[2] );
	return p;
}

static uint8_t *Bridge_WriteHeader( uint8_t *p, unsigned int type, uint32_t sequence, uint32_t frame, int serverTime )
{
	p[0] = 'O';
	p[1] = 'J';
	p[2] = 'B';
	p[3] = '1';
	p += 4;
	p = Bridge_WriteU16( p, BRIDGE_VERSION );
	p = Bridge_WriteU16( p, type );
	p = Bridge_WriteU32( p, sequence );
	p = Bridge_WriteU32( p, frame );
	p = Bridge_WriteI32( p, serverTime );
	return p;
}

static void Bridge_Complain( const char *msg )
{
	if ( !msg || !msg[0] ) {
		return;
	}
	if ( !Q_stricmp( s_lastComplain, msg ) ) {
		return;
	}
	Q_strncpyz( s_lastComplain, msg, sizeof( s_lastComplain ) );
	CG_Printf( "Bridge: %s\n", msg );
}

static void Bridge_ClearComplain( void )
{
	s_lastComplain[0] = '\0';
}

static float Bridge_BlendedFrame( const lerpFrame_t *lf )
{
	if ( !lf ) {
		return 0.0f;
	}
	return (float)lf->oldFrame * lf->backlerp + (float)lf->frame * ( 1.0f - lf->backlerp );
}

// Fractional Ghoul2 bone frame when the player model has one, otherwise the
// legacy lerp frame. Read-only; the same query cgame already uses for anim events.
static float Bridge_RenderedFrame( centity_t *cent, int bone, const lerpFrame_t *fallback )
{
	float current;
	int startFrame;
	int endFrame;
	int flags;
	float animSpeed;
	int model;

	if ( !cent || !cent->gent || !cent->gent->client ) {
		return Bridge_BlendedFrame( fallback );
	}
	model = cent->gent->playerModel;
	if ( bone >= 0 && model >= 0 && cent->gent->ghoul2.size() > model ) {
		current = 0.0f;
		startFrame = 0;
		endFrame = 0;
		flags = 0;
		animSpeed = 0.0f;
		if ( gi.G2API_GetBoneAnimIndex( &cent->gent->ghoul2[model], bone, cg.time,
				&current, &startFrame, &endFrame, &flags, &animSpeed, cgs.model_draw ) ) {
			return current;
		}
	}
	return Bridge_BlendedFrame( fallback );
}

static void Bridge_Send( const uint8_t *packet, int size )
{
	int result = BridgeNet_Send( packet, size );
	if ( result == 0 ) {
		Bridge_ClearComplain();
		return;
	}
	if ( result > 0 ) {
		Bridge_Complain( "UDP send dropped (socket buffer full); frame skipped" );
		return;
	}
	Bridge_Complain( "UDP send failed" );
}

static void Bridge_MaybeSendMap( uint32_t frame, int serverTime )
{
	uint8_t packet[BRIDGE_MAP_PACKET_SIZE];
	uint8_t *p;
	int now;

	if ( !cgs.mapname[0] ) {
		return;
	}

	now = cgi_Milliseconds();
	if ( !Q_stricmp( s_lastMap, cgs.mapname ) ) {
		int elapsed = now - s_lastMapMs;
		if ( s_lastMapMs != 0 && elapsed >= 0 && elapsed < 2000 ) {
			return;
		}
	}

	p = Bridge_WriteHeader( packet, BRIDGE_PACKET_MAP, s_sequence++, frame, serverTime );
	memset( p, 0, BRIDGE_MAP_NAME_LEN );
	Q_strncpyz( (char *)p, cgs.mapname, BRIDGE_MAP_NAME_LEN );
	p += BRIDGE_MAP_NAME_LEN;
	if ( (int)( p - packet ) != BRIDGE_MAP_PACKET_SIZE ) {
		Bridge_Complain( "map packet size mismatch" );
		return;
	}

	Bridge_Send( packet, BRIDGE_MAP_PACKET_SIZE );
	Q_strncpyz( s_lastMap, cgs.mapname, sizeof( s_lastMap ) );
	s_lastMapMs = now;
}

static void Bridge_SendPose( uint32_t frame, int serverTime )
{
	uint8_t packet[BRIDGE_POSE_PACKET_SIZE];
	uint8_t *p;
	const playerState_t *ps;
	centity_t *cent;
	int clientNum;
	vec3_t playerOrigin;
	float legsFrame;
	float torsoFrame;
	uint32_t flags;
	int axis;

	ps = &cg.predicted_player_state;
	clientNum = ps->clientNum;
	if ( clientNum < 0 || clientNum >= MAX_GENTITIES ) {
		clientNum = 0;
	}
	cent = &cg_entities[clientNum];

	flags = 0;
	if ( cg.renderingThirdPerson ) {
		flags |= BRIDGE_FLAG_THIRDPERSON;
	}
	if ( in_camera ) {
		flags |= BRIDGE_FLAG_CINEMATIC;
	}
	if ( cg.zoomMode ) {
		flags |= BRIDGE_FLAG_ZOOM;
	}
	if ( ps->pm_type == PM_INTERMISSION ) {
		flags |= BRIDGE_FLAG_INTERMISSION;
	}

	if ( cent->currentValid ) {
		flags |= BRIDGE_FLAG_PLAYER_VALID;
		VectorCopy( cent->lerpOrigin, playerOrigin );
		legsFrame = Bridge_RenderedFrame( cent, cent->gent ? cent->gent->rootBone : -1, &cent->pe.legs );
		torsoFrame = Bridge_RenderedFrame( cent, cent->gent ? cent->gent->lowerLumbarBone : -1, &cent->pe.torso );
	} else {
		VectorCopy( ps->origin, playerOrigin );
		legsFrame = 0.0f;
		torsoFrame = 0.0f;
	}

	p = Bridge_WriteHeader( packet, BRIDGE_PACKET_POSE, s_sequence++, frame, serverTime );
	p = Bridge_WriteVec3( p, cg.refdef.vieworg );
	p = Bridge_WriteVec3( p, cg.refdefViewAngles );
	for ( axis = 0; axis < 3; ++axis ) {
		p = Bridge_WriteVec3( p, cg.refdef.viewaxis[axis] );
	}
	p = Bridge_WriteF32( p, cg.refdef.fov_x );
	p = Bridge_WriteF32( p, cg.refdef.fov_y );
	p = Bridge_WriteVec3( p, playerOrigin );
	p = Bridge_WriteVec3( p, ps->viewangles );
	p = Bridge_WriteVec3( p, ps->velocity );
	p = Bridge_WriteI32( p, ps->legsAnim );
	p = Bridge_WriteI32( p, ps->legsAnimTimer );
	p = Bridge_WriteI32( p, ps->torsoAnim );
	p = Bridge_WriteI32( p, ps->torsoAnimTimer );
	p = Bridge_WriteF32( p, legsFrame );
	p = Bridge_WriteF32( p, torsoFrame );
	p = Bridge_WriteU32( p, flags );
	p = Bridge_WriteI32( p, ps->weapon );
	p = Bridge_WriteI32( p, ps->pm_type );

	if ( (int)( p - packet ) != BRIDGE_POSE_PACKET_SIZE ) {
		Bridge_Complain( "pose packet size mismatch" );
		return;
	}

	Bridge_Send( packet, BRIDGE_POSE_PACKET_SIZE );
}

void Bridge_Init( void )
{
	if ( s_inited ) {
		return;
	}

	cgi_Cvar_Register( &bridge_enable, "bridge_enable", "0", CVAR_ARCHIVE );
	cgi_Cvar_Register( &bridge_host, "bridge_host", "127.0.0.1", CVAR_ARCHIVE );
	cgi_Cvar_Register( &bridge_port, "bridge_port", BRIDGE_DEFAULT_PORT, CVAR_ARCHIVE );
	cgi_Cvar_Register( &bridge_rate, "bridge_rate", "0", CVAR_ARCHIVE );
	s_inited = qtrue;
}

void Bridge_Shutdown( void )
{
	BridgeNet_Close();
	s_streaming = qfalse;
	s_sequence = 1;
	s_lastPoseMs = 0;
	s_lastMapMs = 0;
	s_lastMap[0] = '\0';
	s_lastComplain[0] = '\0';
}

void Bridge_Frame( int stereoView )
{
	const char *error = NULL;
	int rate;
	int interval;
	int now;
	int elapsed;

	if ( !s_inited ) {
		Bridge_Init();
	}

	cgi_Cvar_Update( &bridge_enable );
	if ( !bridge_enable.integer ) {
		if ( s_streaming ) {
			Bridge_Shutdown();
			CG_Printf( "Bridge: UDP pose stream stopped\n" );
		}
		return;
	}

	// Stereo draws the same pose twice. The right eye is a small lateral
	// offset applied after this hook, so one center/left packet is enough.
	if ( stereoView == STEREO_RIGHT ) {
		return;
	}

	cgi_Cvar_Update( &bridge_host );
	cgi_Cvar_Update( &bridge_port );
	cgi_Cvar_Update( &bridge_rate );

	if ( !BridgeNet_Open( bridge_host.string, bridge_port.integer, &error ) ) {
		if ( s_streaming ) {
			BridgeNet_Close();
			s_streaming = qfalse;
		}
		Bridge_Complain( error ? error : "could not open UDP socket" );
		return;
	}

	if ( !s_streaming ) {
		s_streaming = qtrue;
		CG_Printf( "Bridge: UDP pose stream -> %s:%d\n", bridge_host.string, bridge_port.integer );
	}

	rate = bridge_rate.integer;
	if ( rate < 0 ) {
		rate = 0;
	} else if ( rate > 480 ) {
		rate = 480;
	}
	if ( rate > 0 ) {
		interval = 1000 / rate;
		if ( interval < 1 ) {
			interval = 1;
		}
		now = cgi_Milliseconds();
		elapsed = now - s_lastPoseMs;
		if ( s_lastPoseMs != 0 && elapsed >= 0 && elapsed < interval ) {
			return;
		}
		s_lastPoseMs = now;
	}

	Bridge_MaybeSendMap( (uint32_t)cg.clientFrame, cg.time );
	Bridge_SendPose( (uint32_t)cg.clientFrame, cg.time );
}
