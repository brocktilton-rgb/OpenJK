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

#include "cg_bridge_net.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#	ifndef WIN32_LEAN_AND_MEAN
#		define WIN32_LEAN_AND_MEAN
#	endif
#	ifndef _WIN32_WINNT
#		define _WIN32_WINNT 0x0601
#	endif
#	include <winsock2.h>
#	include <ws2tcpip.h>
typedef SOCKET bridgeSocket_t;
static const bridgeSocket_t BRIDGE_BAD_SOCKET = INVALID_SOCKET;
#else
#	include <arpa/inet.h>
#	include <errno.h>
#	include <fcntl.h>
#	include <netinet/in.h>
#	include <sys/socket.h>
#	include <unistd.h>
typedef int bridgeSocket_t;
static const bridgeSocket_t BRIDGE_BAD_SOCKET = -1;
#endif

static bridgeSocket_t	s_socket = BRIDGE_BAD_SOCKET;
static sockaddr_in		s_dest;
static char				s_hostName[64];
static int				s_port = 0;
static char				s_err[160];
#ifdef _WIN32
static bool				s_wsa = false;
#endif

static int BridgeNet_LastError( void )
{
#ifdef _WIN32
	return WSAGetLastError();
#else
	return errno;
#endif
}

static bool BridgeNet_WouldBlock( int err )
{
#ifdef _WIN32
	return err == WSAEWOULDBLOCK || err == WSAEINTR;
#else
	return err == EAGAIN || err == EWOULDBLOCK || err == EINTR;
#endif
}

static void BridgeNet_FormatError( const char *what, int err )
{
#if defined( _MSC_VER ) && _MSC_VER < 1900
	_snprintf( s_err, sizeof( s_err ), "%s (%d)", what, err );
	s_err[sizeof( s_err ) - 1] = '\0';
#else
	snprintf( s_err, sizeof( s_err ), "%s (%d)", what, err );
#endif
}

// Numeric IPv4 only. getaddrinfo can block on DNS, so it is not used.
static bool BridgeNet_ParseIPv4( const char *host, in_addr *out )
{
	unsigned int octets[4];
	int i;

	if ( !host || !host[0] || !out ) {
		return false;
	}

	for ( i = 0; i < 4; ++i ) {
		unsigned int value = 0;
		int digits = 0;

		if ( *host < '0' || *host > '9' ) {
			return false;
		}
		while ( *host >= '0' && *host <= '9' ) {
			value = value * 10u + (unsigned int)( *host - '0' );
			if ( value > 255u ) {
				return false;
			}
			++host;
			++digits;
			if ( digits > 3 ) {
				return false;
			}
		}
		octets[i] = value;
		if ( i < 3 ) {
			if ( *host != '.' ) {
				return false;
			}
			++host;
		}
	}

	if ( *host != '\0' ) {
		return false;
	}

	std::memset( out, 0, sizeof( *out ) );
	reinterpret_cast<unsigned char *>( &out->s_addr )[0] = (unsigned char)octets[0];
	reinterpret_cast<unsigned char *>( &out->s_addr )[1] = (unsigned char)octets[1];
	reinterpret_cast<unsigned char *>( &out->s_addr )[2] = (unsigned char)octets[2];
	reinterpret_cast<unsigned char *>( &out->s_addr )[3] = (unsigned char)octets[3];
	return true;
}

static void BridgeNet_CloseSocket( void )
{
	if ( s_socket != BRIDGE_BAD_SOCKET ) {
#ifdef _WIN32
		closesocket( s_socket );
#else
		close( s_socket );
#endif
		s_socket = BRIDGE_BAD_SOCKET;
	}
	s_hostName[0] = '\0';
	s_port = 0;
}

static bool BridgeNet_EnsureWSA( const char **errorOut )
{
#ifdef _WIN32
	WSADATA data;
	int result;

	if ( s_wsa ) {
		return true;
	}
	result = WSAStartup( MAKEWORD( 2, 2 ), &data );
	if ( result != 0 ) {
		BridgeNet_FormatError( "WSAStartup failed", result );
		*errorOut = s_err;
		return false;
	}
	s_wsa = true;
#else
	(void)errorOut;
#endif
	return true;
}

void BridgeNet_Close( void )
{
	BridgeNet_CloseSocket();
#ifdef _WIN32
	if ( s_wsa ) {
		WSACleanup();
		s_wsa = false;
	}
#endif
}

bool BridgeNet_Open( const char *host, int port, const char **errorOut )
{
	in_addr address;
	bridgeSocket_t sock;

	if ( !errorOut ) {
		return false;
	}
	*errorOut = "";

	if ( s_socket != BRIDGE_BAD_SOCKET && s_port == port && host && std::strcmp( s_hostName, host ) == 0 ) {
		return true;
	}

	// Drop the previous endpoint before validating so a bad cvar change
	// cannot keep sending to the old address.
	BridgeNet_CloseSocket();

	if ( port < 1 || port > 65535 ) {
		*errorOut = "bridge_port must be 1..65535";
		return false;
	}
	if ( !BridgeNet_ParseIPv4( host, &address ) ) {
		*errorOut = "bridge_host must be a numeric IPv4 address";
		return false;
	}

	if ( !BridgeNet_EnsureWSA( errorOut ) ) {
		return false;
	}

	sock = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
	if ( sock == BRIDGE_BAD_SOCKET ) {
		BridgeNet_FormatError( "socket() failed", BridgeNet_LastError() );
		*errorOut = s_err;
		return false;
	}

#ifdef _WIN32
	{
		u_long nonBlocking = 1;
		if ( ioctlsocket( sock, FIONBIO, &nonBlocking ) != 0 ) {
			BridgeNet_FormatError( "ioctlsocket(FIONBIO) failed", BridgeNet_LastError() );
			*errorOut = s_err;
			closesocket( sock );
			return false;
		}
	}
#else
	{
		int flags = fcntl( sock, F_GETFL, 0 );
		if ( flags < 0 || fcntl( sock, F_SETFL, flags | O_NONBLOCK ) != 0 ) {
			BridgeNet_FormatError( "fcntl(O_NONBLOCK) failed", BridgeNet_LastError() );
			*errorOut = s_err;
			close( sock );
			return false;
		}
	}
#endif

	std::memset( &s_dest, 0, sizeof( s_dest ) );
	s_dest.sin_family = AF_INET;
	s_dest.sin_port = htons( (unsigned short)port );
	s_dest.sin_addr = address;

	s_socket = sock;
	s_port = port;
	std::snprintf( s_hostName, sizeof( s_hostName ), "%s", host );
	return true;
}

int BridgeNet_Send( const void *data, int length )
{
	int sent;
	int err;

	if ( s_socket == BRIDGE_BAD_SOCKET || !data || length <= 0 ) {
		return -1;
	}

	sent = (int)sendto( s_socket, (const char *)data, length, 0, (const sockaddr *)&s_dest, (int)sizeof( s_dest ) );
	if ( sent == length ) {
		return 0;
	}
	if ( sent >= 0 ) {
		return -1;
	}

	err = BridgeNet_LastError();
	if ( BridgeNet_WouldBlock( err ) ) {
		return 1;
	}
	return -1;
}
