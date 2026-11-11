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
static uint32_t		s_sentPose = 0;
static uint32_t		s_sentMap = 0;
static uint32_t		s_sentSaber = 0;
static uint32_t		s_sentRender = 0;
static uint32_t		s_sentNpc = 0;
static uint32_t		s_sentHud = 0;
static uint32_t		s_sentMover = 0;
static vmCvar_t		bridge_npc_radius;
static unsigned char	s_npcWasHere[MAX_GENTITIES];
static int			s_npcLastFlags[MAX_GENTITIES];
static unsigned char	s_npcFireCount[MAX_GENTITIES];
static unsigned char	s_npcFireBits[MAX_GENTITIES];
static int			s_npcLastFireTime[MAX_GENTITIES];
static unsigned char	s_moverWasHere[MAX_GENTITIES];
static int			s_moverLastFlags[MAX_GENTITIES];
static uint32_t		s_sendDrops = 0;
static uint32_t		s_sendFails = 0;
static int			s_lastPoseMs = 0;
static int			s_lastMapMs = 0;
static char			s_lastMap[BRIDGE_MAP_NAME_LEN];
static char			s_lastComplain[256];
static char			s_lastSendError[160];

// Last two simulation positions. SP usually has no future snapshot, so
// lerpOrigin only moves on the server tick (sv_fps, default 20). Type 4
// plays that step across the following client frames.
static qboolean		s_haveOrigin = qfalse;
static int			s_originSnapTime = 0;
static int			s_originSpan = 50;
static int			s_originSampleMs = 0;
static vec3_t		s_originFrom;
static vec3_t		s_originTo;

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

static uint8_t *Bridge_WriteHeaderVersion( uint8_t *p, unsigned int version, unsigned int type, uint32_t sequence, uint32_t frame, int serverTime )
{
	p[0] = 'O';
	p[1] = 'J';
	p[2] = 'B';
	p[3] = '1';
	p += 4;
	p = Bridge_WriteU16( p, version );
	p = Bridge_WriteU16( p, type );
	p = Bridge_WriteU32( p, sequence );
	p = Bridge_WriteU32( p, frame );
	p = Bridge_WriteI32( p, serverTime );
	return p;
}

static uint8_t *Bridge_WriteHeader( uint8_t *p, unsigned int type, uint32_t sequence, uint32_t frame, int serverTime )
{
	return Bridge_WriteHeaderVersion( p, BRIDGE_VERSION, type, sequence, frame, serverTime );
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

static void Bridge_NoteSendError( const char *msg )
{
	Q_strncpyz( s_lastSendError, msg, sizeof( s_lastSendError ) );
	Bridge_Complain( msg );
}

static void Bridge_Send( const uint8_t *packet, int size )
{
	int result = BridgeNet_Send( packet, size );
	unsigned int type = 0;

	if ( size >= 8 ) {
		type = (unsigned int)packet[6] | ( (unsigned int)packet[7] << 8 );
	}
	if ( result == 0 ) {
		if ( type == BRIDGE_PACKET_POSE ) {
			s_sentPose++;
		} else if ( type == BRIDGE_PACKET_MAP ) {
			s_sentMap++;
		} else if ( type == BRIDGE_PACKET_SABER ) {
			s_sentSaber++;
		} else if ( type == BRIDGE_PACKET_RENDER ) {
			s_sentRender++;
		} else if ( type == BRIDGE_PACKET_NPC ) {
			s_sentNpc++;
		} else if ( type == BRIDGE_PACKET_HUD ) {
			s_sentHud++;
		} else if ( type == BRIDGE_PACKET_MOVER ) {
			s_sentMover++;
		}
		s_lastSendError[0] = '\0';
		Bridge_ClearComplain();
		return;
	}
	if ( result > 0 ) {
		s_sendDrops++;
		Bridge_NoteSendError( "UDP send dropped (socket buffer full); frame skipped" );
		return;
	}
	s_sendFails++;
	Bridge_NoteSendError( "UDP send failed" );
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

static uint8_t *Bridge_WriteU8( uint8_t *p, unsigned int value )
{
	p[0] = (uint8_t)( value & 0xffu );
	return p + 1;
}

static void Bridge_WriteHiltName( uint8_t *dst, const char *name )
{
	memset( dst, 0, BRIDGE_HILT_NAME_LEN );
	if ( name && name[0] ) {
		Q_strncpyz( (char *)dst, name, BRIDGE_HILT_NAME_LEN );
	}
}

static int Bridge_ClampBladeCount( int numBlades )
{
	if ( numBlades < 0 ) {
		return 0;
	}
	if ( numBlades > MAX_BLADES ) {
		return MAX_BLADES;
	}
	return numBlades;
}

// tip = muzzlePoint + muzzleDir * length. CG_AddSaberBladeGo writes both
// vectors while drawing the blade, earlier in this same frame, so type 3
// matches the segment the renderer just used.
static uint8_t *Bridge_WriteBlade( uint8_t *p, int saberIndex, int bladeIndex, const bladeInfo_t *blade )
{
	vec3_t tip;

	p = Bridge_WriteU8( p, (unsigned int)saberIndex );
	p = Bridge_WriteU8( p, (unsigned int)bladeIndex );
	p = Bridge_WriteU8( p, blade->active ? 1u : 0u );
	p = Bridge_WriteU8( p, (unsigned int)blade->color );
	p = Bridge_WriteF32( p, blade->length );
	p = Bridge_WriteVec3( p, blade->muzzlePoint );
	VectorMA( blade->muzzlePoint, blade->length, blade->muzzleDir, tip );
	p = Bridge_WriteVec3( p, tip );
	return p;
}

// Sent on the same ticks as the pose packet, only while a lightsaber is the
// equipped weapon or is in flight. Pose and map bytes are not touched.
static void Bridge_MaybeSendSaber( const playerState_t *ps, const centity_t *cent, uint32_t frame, int serverTime )
{
	const playerState_t *saberPs;
	uint8_t packet[BRIDGE_SABER_PACKET_SIZE];
	uint8_t *p;
	uint8_t *bladeOut;
	int saberSlots;
	int saberIndex;
	int bladeCount;
	int slot;
	unsigned int flags;

	static_assert( BRIDGE_SABER_PACKET_SIZE == 672, "saber packet size drifted" );
	static_assert( BRIDGE_SABER_BLADE_BYTES * BRIDGE_SABER_BLADE_SLOTS == 512, "saber blade table drifted" );

	saberPs = ps;
	if ( cent && cent->gent && cent->gent->client ) {
		saberPs = &cent->gent->client->ps;
	}
	if ( !saberPs ) {
		return;
	}
	if ( saberPs->weapon != WP_SABER && !saberPs->saberInFlight ) {
		return;
	}

	memset( packet, 0, sizeof( packet ) );
	p = Bridge_WriteHeader( packet, BRIDGE_PACKET_SABER, s_sequence++, frame, serverTime );

	saberSlots = saberPs->dualSabers ? 2 : 1;
	flags = 0;
	if ( saberPs->dualSabers ) {
		flags |= BRIDGE_SABER_DUAL;
	}
	if ( saberPs->saberInFlight ) {
		flags |= BRIDGE_SABER_IN_FLIGHT;
	}

	bladeCount = 0;
	for ( saberIndex = 0; saberIndex < saberSlots; ++saberIndex ) {
		bladeCount += Bridge_ClampBladeCount( saberPs->saber[saberIndex].numBlades );
	}
	if ( bladeCount > BRIDGE_SABER_BLADE_SLOTS ) {
		bladeCount = BRIDGE_SABER_BLADE_SLOTS;
	}

	p = Bridge_WriteU16( p, (unsigned int)bladeCount );
	p = Bridge_WriteU8( p, (unsigned int)saberSlots );
	p = Bridge_WriteU8( p, flags );
	Bridge_WriteHiltName( p, saberPs->saber[0].name );
	p += BRIDGE_HILT_NAME_LEN;
	p = Bridge_WriteI32( p, (int)saberPs->saber[0].type );
	if ( saberSlots > 1 ) {
		Bridge_WriteHiltName( p, saberPs->saber[1].name );
		p += BRIDGE_HILT_NAME_LEN;
		p = Bridge_WriteI32( p, (int)saberPs->saber[1].type );
	} else {
		p += BRIDGE_HILT_NAME_LEN;
		p = Bridge_WriteI32( p, 0 );
	}

	bladeOut = p;
	slot = 0;
	for ( saberIndex = 0; saberIndex < saberSlots && slot < bladeCount; ++saberIndex ) {
		int blades = Bridge_ClampBladeCount( saberPs->saber[saberIndex].numBlades );
		int bladeIndex;

		for ( bladeIndex = 0; bladeIndex < blades && slot < bladeCount; ++bladeIndex, ++slot ) {
			bladeOut = Bridge_WriteBlade( bladeOut, saberIndex, bladeIndex, &saberPs->saber[saberIndex].blade[bladeIndex] );
		}
	}

	if ( (int)( p - packet ) != 160 || slot != bladeCount ) {
		Bridge_Complain( "saber packet size mismatch" );
		return;
	}
	if ( (int)( bladeOut - packet ) + ( BRIDGE_SABER_BLADE_SLOTS - slot ) * BRIDGE_SABER_BLADE_BYTES != BRIDGE_SABER_PACKET_SIZE ) {
		Bridge_Complain( "saber packet size mismatch" );
		return;
	}

	Bridge_Send( packet, BRIDGE_SABER_PACKET_SIZE );
}

// Spread one sv_fps step across the following client frames. The result is
// one simulation step behind the raw origin, and it moves every render frame.
static void Bridge_SmoothOrigin( const vec3_t raw, vec3_t out, float *outFrac )
{
	int now = cgi_Milliseconds();
	int snapTime = ( cg.snap ) ? cg.snap->serverTime : 0;
	vec3_t delta;
	float dist;
	float frac;
	int i;

	VectorSubtract( raw, s_originTo, delta );
	dist = VectorLength( delta );
	if ( !s_haveOrigin || dist > 200.0f ) {
		VectorCopy( raw, s_originFrom );
		VectorCopy( raw, s_originTo );
		s_originSampleMs = now;
		s_originSpan = 50;
		s_originSnapTime = snapTime;
		s_haveOrigin = qtrue;
		VectorCopy( raw, out );
		*outFrac = 1.0f;
		return;
	}

	if ( snapTime != s_originSnapTime ) {
		int span = now - s_originSampleMs;
		vec3_t shown;

		if ( span < 1 ) {
			span = 1;
		} else if ( span > 200 ) {
			span = 200;
		}
		frac = (float)( now - s_originSampleMs ) / (float)s_originSpan;
		if ( frac < 0.0f ) {
			frac = 0.0f;
		} else if ( frac > 1.0f ) {
			frac = 1.0f;
		}
		for ( i = 0; i < 3; ++i ) {
			shown[i] = s_originFrom[i] + frac * ( s_originTo[i] - s_originFrom[i] );
		}
		VectorCopy( shown, s_originFrom );
		VectorCopy( raw, s_originTo );
		s_originSpan = span;
		s_originSampleMs = now;
		s_originSnapTime = snapTime;
	}

	frac = (float)( now - s_originSampleMs ) / (float)s_originSpan;
	if ( frac < 0.0f ) {
		frac = 0.0f;
	} else if ( frac > 1.0f ) {
		frac = 1.0f;
	}
	for ( i = 0; i < 3; ++i ) {
		out[i] = s_originFrom[i] + frac * ( s_originTo[i] - s_originFrom[i] );
	}
	*outFrac = frac;
}

// Ghoul2 bolt matrix is 3 rows by 4 columns. Columns are +X, +Y, +Z, origin.
static qboolean Bridge_ReadHandBolt( centity_t *cent, int bolt, const vec3_t angles, const vec3_t origin, vec3_t outOrg, vec3_t outX, vec3_t outY, vec3_t outZ )
{
	mdxaBone_t boltMatrix;
	int model;

	if ( !cent || !cent->gent || bolt < 0 ) {
		return qfalse;
	}
	model = cent->gent->playerModel;
	if ( model < 0 || cent->gent->ghoul2.size() <= model ) {
		return qfalse;
	}
	if ( !gi.G2API_GetBoltMatrix( cent->gent->ghoul2, model, bolt, &boltMatrix, angles, origin,
			cg.time, cgs.model_draw, cent->currentState.modelScale ) ) {
		return qfalse;
	}
	outX[0] = boltMatrix.matrix[0][0];
	outX[1] = boltMatrix.matrix[1][0];
	outX[2] = boltMatrix.matrix[2][0];
	outY[0] = boltMatrix.matrix[0][1];
	outY[1] = boltMatrix.matrix[1][1];
	outY[2] = boltMatrix.matrix[2][1];
	outZ[0] = boltMatrix.matrix[0][2];
	outZ[1] = boltMatrix.matrix[1][2];
	outZ[2] = boltMatrix.matrix[2][2];
	outOrg[0] = boltMatrix.matrix[0][3];
	outOrg[1] = boltMatrix.matrix[1][3];
	outOrg[2] = boltMatrix.matrix[2][3];
	return qtrue;
}

static uint8_t *Bridge_WriteHand( uint8_t *p, const vec3_t org, const vec3_t axisX, const vec3_t axisY, const vec3_t axisZ )
{
	p = Bridge_WriteVec3( p, org );
	p = Bridge_WriteVec3( p, axisX );
	p = Bridge_WriteVec3( p, axisY );
	p = Bridge_WriteVec3( p, axisZ );
	return p;
}

static void Bridge_SendRender( const playerState_t *ps, centity_t *cent, uint32_t frame, int serverTime )
{
	uint8_t packet[BRIDGE_RENDER_PACKET_SIZE];
	uint8_t *p;
	uint8_t *flagByte;
	vec3_t rawOrigin;
	vec3_t smoothOrigin;
	vec3_t playerAngles;
	vec3_t camOrigin;
	vec3_t boltAngles;
	vec3_t boltOrigin;
	vec3_t handOrg;
	vec3_t handX;
	vec3_t handY;
	vec3_t handZ;
	int rightBolt = -1;
	int leftBolt = -1;
	float originLerp = 1.0f;
	float legsFrame = 0.0f;
	float torsoFrame = 0.0f;
	unsigned int handFlags = 0;

	static_assert( BRIDGE_RENDER_PACKET_SIZE == 228, "render packet size drifted" );

	VectorClear( rawOrigin );
	VectorClear( playerAngles );
	if ( cent && cent->currentValid ) {
		VectorCopy( cent->lerpOrigin, rawOrigin );
		VectorCopy( cent->lerpAngles, playerAngles );
		legsFrame = Bridge_RenderedFrame( cent, cent->gent ? cent->gent->rootBone : -1, &cent->pe.legs );
		torsoFrame = Bridge_RenderedFrame( cent, cent->gent ? cent->gent->lowerLumbarBone : -1, &cent->pe.torso );
	} else if ( ps ) {
		VectorCopy( ps->origin, rawOrigin );
		VectorCopy( ps->viewangles, playerAngles );
	}
	Bridge_SmoothOrigin( rawOrigin, smoothOrigin, &originLerp );
	VectorSubtract( smoothOrigin, rawOrigin, camOrigin );
	VectorAdd( cg.refdef.vieworg, camOrigin, camOrigin );

	memset( packet, 0, sizeof( packet ) );
	p = Bridge_WriteHeader( packet, BRIDGE_PACKET_RENDER, s_sequence++, frame, serverTime );
	p = Bridge_WriteI32( p, cgi_Milliseconds() );
	p = Bridge_WriteVec3( p, smoothOrigin );
	p = Bridge_WriteVec3( p, playerAngles );
	p = Bridge_WriteVec3( p, camOrigin );
	p = Bridge_WriteVec3( p, cg.refdefViewAngles );
	p = Bridge_WriteVec3( p, cg.refdef.viewaxis[0] );
	p = Bridge_WriteVec3( p, cg.refdef.viewaxis[1] );
	p = Bridge_WriteVec3( p, cg.refdef.viewaxis[2] );
	p = Bridge_WriteF32( p, cg.refdef.fov_x );
	p = Bridge_WriteF32( p, cg.refdef.fov_y );
	p = Bridge_WriteF32( p, legsFrame );
	p = Bridge_WriteF32( p, torsoFrame );
	p = Bridge_WriteF32( p, originLerp );

	VectorClear( boltAngles );
	VectorCopy( rawOrigin, boltOrigin );
	if ( cent && cent->gent && cent->currentValid ) {
		boltAngles[YAW] = cent->renderAngles[YAW];
		rightBolt = cent->gent->handRBolt;
		leftBolt = cent->gent->handLBolt;
		if ( cent->currentState.modelScale[2] && cent->currentState.modelScale[2] != 1.0f ) {
			boltOrigin[2] += 24.0f * ( cent->currentState.modelScale[2] - 1.0f );
		}
	}

	if ( Bridge_ReadHandBolt( cent, rightBolt, boltAngles, boltOrigin, handOrg, handX, handY, handZ ) ) {
		handFlags |= BRIDGE_RENDER_HAND_R;
	} else {
		VectorClear( handOrg );
		VectorClear( handX );
		VectorClear( handY );
		VectorClear( handZ );
	}
	flagByte = p;
	p = Bridge_WriteU8( p, (unsigned int)handFlags );
	p = Bridge_WriteU8( p, 0 );
	p = Bridge_WriteU16( p, 0 );
	p = Bridge_WriteHand( p, handOrg, handX, handY, handZ );

	if ( Bridge_ReadHandBolt( cent, leftBolt, boltAngles, boltOrigin, handOrg, handX, handY, handZ ) ) {
		*flagByte = (uint8_t)( *flagByte | BRIDGE_RENDER_HAND_L );
	} else {
		VectorClear( handOrg );
		VectorClear( handX );
		VectorClear( handY );
		VectorClear( handZ );
	}
	p = Bridge_WriteHand( p, handOrg, handX, handY, handZ );

	if ( (int)( p - packet ) != BRIDGE_RENDER_PACKET_SIZE ) {
		Bridge_Complain( "render packet size mismatch" );
		return;
	}
	Bridge_Send( packet, BRIDGE_RENDER_PACKET_SIZE );
}

#ifndef ANIM_TOGGLEBIT
#define ANIM_TOGGLEBIT 2048
#endif

static uint32_t Bridge_Fnv1a32Lower( const char *text )
{
	uint32_t hash = 2166136261u;
	const unsigned char *s;

	if ( !text ) {
		return hash;
	}
	for ( s = (const unsigned char *)text; *s; ++s ) {
		unsigned char c = *s;
		if ( c >= 'A' && c <= 'Z' ) {
			c = (unsigned char)( c + ( 'a' - 'A' ) );
		}
		hash ^= c;
		hash *= 16777619u;
	}
	return hash;
}

static int Bridge_NpcState( int health, int legsAnim )
{
	if ( health > 0 ) {
		return BRIDGE_NPC_ALIVE;
	}
	if ( legsAnim >= BOTH_DEATH1 && legsAnim < BOTH_DEAD1 ) {
		return BRIDGE_NPC_DYING;
	}
	return BRIDGE_NPC_DEAD;
}

static uint8_t *Bridge_WriteNpc( uint8_t *p, int entityNum )
{
	gentity_t *gent = &g_entities[entityNum];
	centity_t *cent = &cg_entities[entityNum];
	int weapon = cent->currentState.weapon;
	int legsAnim = cent->currentState.legsAnim;
	int torsoAnim = cent->currentState.torsoAnim;
	int health = gent->health;
	unsigned int flags = 0;
	uint8_t *start = p;
	char name[BRIDGE_NPC_NAME_LEN];

	if ( gent->client ) {
		weapon = gent->client->ps.weapon;
		legsAnim = gent->client->ps.legsAnim;
		torsoAnim = gent->client->ps.torsoAnim;
	}
	legsAnim &= ~ANIM_TOGGLEBIT;
	torsoAnim &= ~ANIM_TOGGLEBIT;

	if ( !s_npcWasHere[entityNum] || ( ( cent->currentState.eFlags ^ s_npcLastFlags[entityNum] ) & EF_TELEPORT_BIT ) ) {
		flags |= BRIDGE_NPC_NEW;
	}
	if ( weapon != WP_NONE ) {
		flags |= BRIDGE_NPC_WEAPON;
	}
	if ( cent->currentState.saberActive || ( gent->client && gent->client->ps.SaberActive() ) ) {
		flags |= BRIDGE_NPC_SABER;
	}

	memset( name, 0, sizeof( name ) );
	if ( gent->NPC_type && gent->NPC_type[0] ) {
		Q_strncpyz( name, gent->NPC_type, sizeof( name ) );
	}

	p = Bridge_WriteU16( p, (unsigned int)entityNum );
	p = Bridge_WriteU8( p, (unsigned int)Bridge_NpcState( health, legsAnim ) );
	p = Bridge_WriteU8( p, flags );
	p = Bridge_WriteU32( p, Bridge_Fnv1a32Lower( name ) );
	p = Bridge_WriteVec3( p, cent->lerpOrigin );
	p = Bridge_WriteVec3( p, cent->lerpAngles );
	p = Bridge_WriteI32( p, legsAnim );
	p = Bridge_WriteI32( p, torsoAnim );
	p = Bridge_WriteF32( p, Bridge_RenderedFrame( cent, gent->rootBone, &cent->pe.legs ) );
	p = Bridge_WriteF32( p, Bridge_RenderedFrame( cent, gent->lowerLumbarBone, &cent->pe.torso ) );
	p = Bridge_WriteI32( p, weapon );
	p = Bridge_WriteI32( p, health );
	p = Bridge_WriteVec3( p, cent->currentState.pos.trDelta );
	memcpy( p, name, BRIDGE_NPC_NAME_LEN );
	p += BRIDGE_NPC_NAME_LEN;

	{
		vec3_t boltAngles;
		vec3_t boltOrigin;
		vec3_t handOrg;
		vec3_t handX;
		vec3_t handY;
		vec3_t handZ;
		unsigned int handFlags = 0;
		unsigned int fireFlags = s_npcFireBits[entityNum];
		unsigned int saberFlags = 0;
		float bladeLen[2];
		unsigned char bladeColor[2];
		char hilt[BRIDGE_NPC_NAME_LEN];
		int fireCount = s_npcFireCount[entityNum];

		VectorClear( boltAngles );
		VectorCopy( cent->lerpOrigin, boltOrigin );
		boltAngles[YAW] = cent->renderAngles[YAW];
		if ( cent->currentState.modelScale[2] && cent->currentState.modelScale[2] != 1.0f ) {
			boltOrigin[2] += 24.0f * ( cent->currentState.modelScale[2] - 1.0f );
		}
		if ( cent->currentState.eFlags & EF_FIRING ) {
			fireFlags |= BRIDGE_NPC_FIRE_EF;
		}
		if ( cent->currentState.eFlags & EF_ALT_FIRING ) {
			fireFlags |= BRIDGE_NPC_FIRE_ALT_EF;
		}
		bladeLen[0] = 0.0f;
		bladeLen[1] = 0.0f;
		bladeColor[0] = 0;
		bladeColor[1] = 0;
		memset( hilt, 0, sizeof( hilt ) );
		if ( gent->client ) {
			playerState_t *ps = &gent->client->ps;
			saberInfo_t *saber = &ps->saber[0];

			bladeLen[0] = saber->blade[0].length;
			bladeColor[0] = (unsigned char)saber->blade[0].color;
			if ( saber->blade[0].active ) {
				saberFlags |= BRIDGE_NPC_SABER_BLADE1;
			}
			if ( ps->dualSabers ) {
				saberFlags |= BRIDGE_NPC_SABER_DUAL;
				bladeLen[1] = ps->saber[1].blade[0].length;
				bladeColor[1] = (unsigned char)ps->saber[1].blade[0].color;
				if ( ps->saber[1].blade[0].active ) {
					saberFlags |= BRIDGE_NPC_SABER_BLADE2;
				}
			} else {
				bladeLen[1] = saber->blade[1].length;
				bladeColor[1] = (unsigned char)saber->blade[1].color;
				if ( saber->blade[1].active ) {
					saberFlags |= BRIDGE_NPC_SABER_BLADE2;
				}
			}
			if ( saber->type == SABER_STAFF ) {
				saberFlags |= BRIDGE_NPC_SABER_STAFF;
			}
			if ( saber->name && saber->name[0] ) {
				Q_strncpyz( hilt, saber->name, sizeof( hilt ) );
			}
		}
		if ( Bridge_ReadHandBolt( cent, gent->handRBolt, boltAngles, boltOrigin, handOrg, handX, handY, handZ ) ) {
			handFlags |= BRIDGE_NPC_HAND_R;
		} else {
			VectorClear( handOrg );
			VectorClear( handX );
			VectorClear( handY );
			VectorClear( handZ );
		}
		p = Bridge_WriteU8( p, handFlags );
		p = Bridge_WriteU8( p, (unsigned int)fireCount );
		p = Bridge_WriteU8( p, fireFlags );
		p = Bridge_WriteU8( p, saberFlags );
		p = Bridge_WriteHand( p, handOrg, handX, handY, handZ );
		if ( Bridge_ReadHandBolt( cent, gent->handLBolt, boltAngles, boltOrigin, handOrg, handX, handY, handZ ) ) {
			handFlags |= BRIDGE_NPC_HAND_L;
			start[100] = (uint8_t)handFlags;
		} else {
			VectorClear( handOrg );
			VectorClear( handX );
			VectorClear( handY );
			VectorClear( handZ );
		}
		p = Bridge_WriteHand( p, handOrg, handX, handY, handZ );
		p = Bridge_WriteF32( p, bladeLen[0] );
		p = Bridge_WriteF32( p, bladeLen[1] );
		p = Bridge_WriteU8( p, bladeColor[0] );
		p = Bridge_WriteU8( p, bladeColor[1] );
		p = Bridge_WriteU16( p, 0 );
		memcpy( p, hilt, BRIDGE_NPC_NAME_LEN );
		p += BRIDGE_NPC_NAME_LEN;
		p = Bridge_WriteI32( p, s_npcLastFireTime[entityNum] );
		s_npcFireCount[entityNum] = 0;
		s_npcFireBits[entityNum] = 0;
	}

	if ( (int)( p - start ) != BRIDGE_NPC_ENTRY_SIZE ) {
		return start;
	}
	return p;
}

static void Bridge_SendNpcs( uint32_t frame, int serverTime )
{
	int ids[MAX_GENTITIES];
	unsigned char here[MAX_GENTITIES];
	int total = 0;
	int i;
	int playerNum;
	float radius;
	float radiusSq;
	int packetCount;
	int packetIndex;
	uint32_t seq;

	static_assert( BRIDGE_HEADER_SIZE == 20, "bridge header size" );
	static_assert( BRIDGE_NPC_ENTRY_SIZE == 248, "npc entry size" );
	static_assert( BRIDGE_NPC_PACKET_MAX == 1268, "npc packet max" );
	static_assert( BRIDGE_HEADER_SIZE + 8 + BRIDGE_NPC_ENTRY_SIZE * BRIDGE_NPC_MAX_PER_PACKET == BRIDGE_NPC_PACKET_MAX, "npc packet math" );

	if ( !cg.snap ) {
		return;
	}
	cgi_Cvar_Update( &bridge_npc_radius );
	radius = bridge_npc_radius.value;
	if ( radius < 0.0f ) {
		radius = 0.0f;
	}
	radiusSq = radius * radius;
	playerNum = cg.snap->ps.clientNum;
	memset( here, 0, sizeof( here ) );

	for ( i = 0; i < cg.snap->numEntities; ++i ) {
		int num = cg.snap->entities[i].number;
		gentity_t *gent;
		centity_t *cent;
		vec3_t delta;

		if ( num < 0 || num >= MAX_GENTITIES || num == playerNum ) {
			continue;
		}
		gent = &g_entities[num];
		if ( !gent->inuse || ( !gent->NPC && !gent->client ) ) {
			continue;
		}
		cent = &cg_entities[num];
		if ( radius > 0.0f ) {
			VectorSubtract( cent->lerpOrigin, cg.refdef.vieworg, delta );
			if ( VectorLengthSquared( delta ) > radiusSq ) {
				continue;
			}
		}
		ids[total++] = num;
		here[num] = 1;
	}

	packetCount = total > 0 ? ( total + BRIDGE_NPC_MAX_PER_PACKET - 1 ) / BRIDGE_NPC_MAX_PER_PACKET : 1;
	seq = s_sequence++;
	for ( packetIndex = 0; packetIndex < packetCount; ++packetIndex ) {
		uint8_t packet[BRIDGE_NPC_PACKET_MAX];
		uint8_t *p;
		int begin = packetIndex * BRIDGE_NPC_MAX_PER_PACKET;
		int count = total - begin;
		int n;

		if ( count > BRIDGE_NPC_MAX_PER_PACKET ) {
			count = BRIDGE_NPC_MAX_PER_PACKET;
		}
		if ( count < 0 ) {
			count = 0;
		}
		memset( packet, 0, sizeof( packet ) );
		p = Bridge_WriteHeaderVersion( packet, BRIDGE_NPC_VERSION, BRIDGE_PACKET_NPC, seq, frame, serverTime );
		p = Bridge_WriteU16( p, (unsigned int)count );
		p = Bridge_WriteU16( p, (unsigned int)total );
		p = Bridge_WriteU8( p, (unsigned int)packetIndex );
		p = Bridge_WriteU8( p, (unsigned int)packetCount );
		p = Bridge_WriteU16( p, 0 );
		for ( n = 0; n < count; ++n ) {
			uint8_t *next = Bridge_WriteNpc( p, ids[begin + n] );
			if ( next == p ) {
				Bridge_Complain( "npc entry size mismatch" );
				return;
			}
			p = next;
		}
		if ( (int)( p - packet ) != BRIDGE_HEADER_SIZE + 8 + count * BRIDGE_NPC_ENTRY_SIZE ) {
			Bridge_Complain( "npc packet size mismatch" );
			return;
		}
		Bridge_Send( packet, (int)( p - packet ) );
	}

	for ( i = 0; i < total; ++i ) {
		int num = ids[i];
		s_npcLastFlags[num] = cg_entities[num].currentState.eFlags;
	}
	memcpy( s_npcWasHere, here, sizeof( s_npcWasHere ) );
}

static void Bridge_SendHud( uint32_t frame, int serverTime )
{
	const playerState_t *ps;
	uint8_t packet[BRIDGE_HUD_PACKET_SIZE];
	uint8_t *p;
	int weapon;
	int ammo = -1;
	int ammoMax = 0;
	int ammoIndex;

	static_assert( BRIDGE_HUD_PACKET_SIZE == 56, "hud packet size" );

	if ( !cg.snap ) {
		return;
	}
	ps = &cg.snap->ps;
	weapon = ps->weapon;
	if ( weapon >= 0 && weapon < WP_NUM_WEAPONS ) {
		ammoIndex = weaponData[weapon].ammoIndex;
		if ( ammoIndex != AMMO_NONE && ammoIndex >= 0 && ammoIndex < AMMO_MAX && ammoIndex < MAX_AMMO ) {
			ammo = ps->ammo[ammoIndex];
			ammoMax = ammoData[ammoIndex].max;
		}
	}

	memset( packet, 0, sizeof( packet ) );
	p = Bridge_WriteHeader( packet, BRIDGE_PACKET_HUD, s_sequence++, frame, serverTime );
	p = Bridge_WriteI32( p, ps->stats[STAT_HEALTH] );
	p = Bridge_WriteI32( p, ps->stats[STAT_MAX_HEALTH] );
	p = Bridge_WriteI32( p, ps->stats[STAT_ARMOR] );
	p = Bridge_WriteI32( p, ps->forcePower );
	p = Bridge_WriteI32( p, ps->forcePowerMax );
	p = Bridge_WriteI32( p, ps->saberAnimLevel );
	p = Bridge_WriteI32( p, weapon );
	p = Bridge_WriteI32( p, ammo );
	p = Bridge_WriteI32( p, ammoMax );
	if ( (int)( p - packet ) != BRIDGE_HUD_PACKET_SIZE ) {
		Bridge_Complain( "hud packet size mismatch" );
		return;
	}
	Bridge_Send( packet, BRIDGE_HUD_PACKET_SIZE );
}

// "*N" with N in 1..65535. "*0" is the world model and is not a mover.
static qboolean Bridge_ParseInlineModel( const char *name, unsigned int *outN )
{
	const char *p;
	unsigned int n = 0;

	if ( !name || name[0] != '*' || !name[1] ) {
		return qfalse;
	}
	for ( p = name + 1; *p; ++p ) {
		if ( *p < '0' || *p > '9' ) {
			return qfalse;
		}
		n = n * 10u + (unsigned int)( *p - '0' );
		if ( n > 65535u ) {
			return qfalse;
		}
	}
	if ( n == 0 ) {
		return qfalse;
	}
	*outN = n;
	return qtrue;
}

// SOLID_BMODEL stores the inline index in modelindex (SV_SetBrushModel).
// CG_Mover draws cgs.inlineDrawModel[modelindex], registered as "*%i".
// CS_MODELS+modelindex is that "*N" string only when something registered it
// there. Otherwise the spawn key on the shared gentity is the "*N" that
// matches a bsp2gltf node. World model 0 is rejected above.
static qboolean Bridge_MoverModel( int entityNum, unsigned int *outN )
{
	centity_t *cent = &cg_entities[entityNum];
	gentity_t *gent = &g_entities[entityNum];
	int modelindex;
	const char *configName;

	if ( cent->currentState.solid != SOLID_BMODEL ) {
		return qfalse;
	}
	modelindex = cent->currentState.modelindex;
	if ( modelindex > 0 && modelindex < MAX_MODELS ) {
		configName = CG_ConfigString( CS_MODELS + modelindex );
		if ( Bridge_ParseInlineModel( configName, outN ) ) {
			return qtrue;
		}
	}
	if ( gent->inuse && Bridge_ParseInlineModel( gent->model, outN ) ) {
		return qtrue;
	}
	return qfalse;
}

static uint8_t *Bridge_WriteMover( uint8_t *p, int entityNum, unsigned int inlineModel )
{
	centity_t *cent = &cg_entities[entityNum];
	unsigned int flags = 0;
	uint8_t *start = p;
	int eType = cent->currentState.eType;

	if ( !s_moverWasHere[entityNum] || ( ( cent->currentState.eFlags ^ s_moverLastFlags[entityNum] ) & EF_TELEPORT_BIT ) ) {
		flags |= BRIDGE_MOVER_NEW;
	}
	if ( cent->currentState.pos.trType != TR_STATIONARY || cent->currentState.apos.trType != TR_STATIONARY ) {
		flags |= BRIDGE_MOVER_MOVING;
	}
	if ( cent->currentState.eFlags & EF_NODRAW ) {
		flags |= BRIDGE_MOVER_NODRAW;
	}
	if ( eType < 0 ) {
		eType = 0;
	}

	p = Bridge_WriteU16( p, (unsigned int)entityNum );
	p = Bridge_WriteU16( p, inlineModel );
	p = Bridge_WriteU8( p, flags );
	p = Bridge_WriteU8( p, (unsigned int)eType & 0xffu );
	p = Bridge_WriteU16( p, 0 );
	p = Bridge_WriteVec3( p, cent->lerpOrigin );
	p = Bridge_WriteVec3( p, cent->lerpAngles );
	p = Bridge_WriteVec3( p, cent->currentState.pos.trDelta );
	if ( (int)( p - start ) != BRIDGE_MOVER_ENTRY_SIZE ) {
		return start;
	}
	return p;
}

static void Bridge_SendMovers( uint32_t frame, int serverTime )
{
	int ids[MAX_GENTITIES];
	unsigned int models[MAX_GENTITIES];
	unsigned char here[MAX_GENTITIES];
	int total = 0;
	int i;
	int packetCount;
	int packetIndex;
	uint32_t seq;

	static_assert( BRIDGE_MOVER_ENTRY_SIZE == 44, "mover entry size" );
	static_assert( BRIDGE_MOVER_PACKET_MAX == 1084, "mover packet max" );
	static_assert( BRIDGE_HEADER_SIZE + 8 + BRIDGE_MOVER_ENTRY_SIZE * BRIDGE_MOVER_MAX_PER_PACKET == BRIDGE_MOVER_PACKET_MAX, "mover packet math" );

	if ( !cg.snap ) {
		return;
	}
	memset( here, 0, sizeof( here ) );

	for ( i = 0; i < cg.snap->numEntities; ++i ) {
		int num = cg.snap->entities[i].number;
		unsigned int inlineModel = 0;

		if ( num < 0 || num >= MAX_GENTITIES ) {
			continue;
		}
		if ( !Bridge_MoverModel( num, &inlineModel ) ) {
			continue;
		}
		ids[total] = num;
		models[total] = inlineModel;
		total++;
		here[num] = 1;
	}

	packetCount = total > 0 ? ( total + BRIDGE_MOVER_MAX_PER_PACKET - 1 ) / BRIDGE_MOVER_MAX_PER_PACKET : 1;
	seq = s_sequence++;
	for ( packetIndex = 0; packetIndex < packetCount; ++packetIndex ) {
		uint8_t packet[BRIDGE_MOVER_PACKET_MAX];
		uint8_t *p;
		int begin = packetIndex * BRIDGE_MOVER_MAX_PER_PACKET;
		int count = total - begin;
		int n;

		if ( count > BRIDGE_MOVER_MAX_PER_PACKET ) {
			count = BRIDGE_MOVER_MAX_PER_PACKET;
		}
		if ( count < 0 ) {
			count = 0;
		}
		memset( packet, 0, sizeof( packet ) );
		p = Bridge_WriteHeader( packet, BRIDGE_PACKET_MOVER, seq, frame, serverTime );
		p = Bridge_WriteU16( p, (unsigned int)count );
		p = Bridge_WriteU16( p, (unsigned int)total );
		p = Bridge_WriteU8( p, (unsigned int)packetIndex );
		p = Bridge_WriteU8( p, (unsigned int)packetCount );
		p = Bridge_WriteU16( p, 0 );
		for ( n = 0; n < count; ++n ) {
			uint8_t *next = Bridge_WriteMover( p, ids[begin + n], models[begin + n] );
			if ( next == p ) {
				Bridge_Complain( "mover entry size mismatch" );
				return;
			}
			p = next;
		}
		if ( (int)( p - packet ) != BRIDGE_HEADER_SIZE + 8 + count * BRIDGE_MOVER_ENTRY_SIZE ) {
			Bridge_Complain( "mover packet size mismatch" );
			return;
		}
		Bridge_Send( packet, (int)( p - packet ) );
	}

	for ( i = 0; i < total; ++i ) {
		int num = ids[i];
		s_moverLastFlags[num] = cg_entities[num].currentState.eFlags;
	}
	memcpy( s_moverWasHere, here, sizeof( s_moverWasHere ) );
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
	cgi_Cvar_Register( &bridge_npc_radius, "bridge_npc_radius", "4096", CVAR_ARCHIVE );
	cgi_AddCommand( "bridge_status" );
	s_inited = qtrue;
}

qboolean Bridge_ConsoleCommand( void )
{
	int rate;

	if ( Q_stricmp( CG_Argv( 0 ), "bridge_status" ) ) {
		return qfalse;
	}
	if ( !s_inited ) {
		Bridge_Init();
	}
	cgi_Cvar_Update( &bridge_enable );
	cgi_Cvar_Update( &bridge_host );
	cgi_Cvar_Update( &bridge_port );
	cgi_Cvar_Update( &bridge_rate );
	rate = bridge_rate.integer;
	if ( rate < 0 ) {
		rate = 0;
	} else if ( rate > 480 ) {
		rate = 480;
	}

	CG_Printf( "Bridge: OJB1 v%d\n", BRIDGE_VERSION );
	CG_Printf( "  enabled: %s\n", bridge_enable.integer ? "yes" : "no" );
	CG_Printf( "  streaming: %s\n", s_streaming ? "yes" : "no" );
	CG_Printf( "  target: %s:%d\n", bridge_host.string, bridge_port.integer );
	CG_Printf( "  rate: %d%s\n", rate, rate ? "" : " (every frame)" );
	CG_Printf( "  packets sent: pose %u, map %u, saber %u, render %u, npc %u, hud %u, mover %u (dropped %u, failed %u)\n",
		s_sentPose, s_sentMap, s_sentSaber, s_sentRender, s_sentNpc, s_sentHud, s_sentMover, s_sendDrops, s_sendFails );
	CG_Printf( "  last send error: %s\n", s_lastSendError[0] ? s_lastSendError : "none" );
	if ( !s_streaming && s_lastComplain[0] ) {
		CG_Printf( "  not streaming: %s\n", s_lastComplain );
	}
	return qtrue;
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
	s_haveOrigin = qfalse;
	s_originSnapTime = 0;
	memset( s_npcWasHere, 0, sizeof( s_npcWasHere ) );
	memset( s_npcLastFlags, 0, sizeof( s_npcLastFlags ) );
	memset( s_npcFireCount, 0, sizeof( s_npcFireCount ) );
	memset( s_npcFireBits, 0, sizeof( s_npcFireBits ) );
	memset( s_npcLastFireTime, 0, sizeof( s_npcLastFireTime ) );
	memset( s_moverWasHere, 0, sizeof( s_moverWasHere ) );
	memset( s_moverLastFlags, 0, sizeof( s_moverLastFlags ) );
}

void Bridge_NoteEntityEvent( int entityNum, int event )
{
	if ( entityNum < 0 || entityNum >= MAX_GENTITIES ) {
		return;
	}
	if ( event == EV_FIRE_WEAPON ) {
		if ( s_npcFireCount[entityNum] < 255 ) {
			s_npcFireCount[entityNum]++;
		}
		s_npcFireBits[entityNum] |= BRIDGE_NPC_FIRE_PRIMARY;
		s_npcLastFireTime[entityNum] = cg.time;
	} else if ( event == EV_ALT_FIRE ) {
		if ( s_npcFireCount[entityNum] < 255 ) {
			s_npcFireCount[entityNum]++;
		}
		s_npcFireBits[entityNum] |= BRIDGE_NPC_FIRE_ALT;
		s_npcLastFireTime[entityNum] = cg.time;
	}
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

	rate = bridge_rate.integer;
	if ( rate < 0 ) {
		rate = 0;
	} else if ( rate > 480 ) {
		rate = 480;
	}

	if ( !BridgeNet_Open( bridge_host.string, bridge_port.integer, &error ) ) {
		char badHost[320];

		if ( s_streaming ) {
			BridgeNet_Close();
			s_streaming = qfalse;
		}
		if ( error && !Q_stricmp( error, "bridge_host must be a numeric IPv4 address" ) ) {
			Com_sprintf( badHost, sizeof( badHost ),
				"bridge_host \"%s\" is not a numeric IPv4 address. Use 127.0.0.1. Not sending.",
				bridge_host.string );
			Bridge_Complain( badHost );
		} else {
			Bridge_Complain( error ? error : "could not open UDP socket" );
		}
		return;
	}

	if ( !s_streaming ) {
		s_streaming = qtrue;
		CG_Printf( "Bridge: OJB1 v%d UDP pose stream -> %s:%d (rate %d)\n",
			BRIDGE_VERSION, bridge_host.string, bridge_port.integer, rate );
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
	Bridge_MaybeSendSaber( &cg.predicted_player_state, &cg_entities[0], (uint32_t)cg.clientFrame, cg.time );
	Bridge_SendRender( &cg.predicted_player_state, &cg_entities[0], (uint32_t)cg.clientFrame, cg.time );
	Bridge_SendNpcs( (uint32_t)cg.clientFrame, cg.time );
	Bridge_SendHud( (uint32_t)cg.clientFrame, cg.time );
	Bridge_SendMovers( (uint32_t)cg.clientFrame, cg.time );
}
