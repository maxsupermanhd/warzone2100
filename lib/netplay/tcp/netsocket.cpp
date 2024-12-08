/*
	This file is part of Warzone 2100.
	Copyright (C) 1999-2004  Eidos Interactive
	Copyright (C) 2005-2020  Warzone 2100 Project

	Warzone 2100 is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	Warzone 2100 is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with Warzone 2100; if not, write to the Free Software
	Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
*/
/**
 * @file netsocket.cpp
 *
 * Basic raw socket handling code.
 */

#include "lib/framework/frame.h"
#include "lib/framework/wzapp.h"
#include "netsocket.h"
#include "lib/netplay/error_categories.h"

#include <vector>
#include <algorithm>
#include <map>

#include "lib/netplay/zlib_compression_adapter.h"

#if defined(__clang__)
	#pragma clang diagnostic ignored "-Wshorten-64-to-32" // FIXME!!
#endif

#if defined(WZ_OS_UNIX)
# include <netinet/tcp.h> // For TCP_NODELAY
#elif defined(WZ_OS_WIN)
// Already included Winsock2.h which defines TCP_NODELAY
#endif

namespace tcp
{

enum
{
	SOCK_CONNECTION,
	SOCK_IPV4_LISTEN = SOCK_CONNECTION,
	SOCK_IPV6_LISTEN,
	SOCK_COUNT,
};

struct Socket
{
	/* Multiple socket handles only for listening sockets. This allows us
	 * to listen on multiple protocols and address families (e.g. IPv4 and
	 * IPv6).
	 *
	 * All non-listening sockets will only use the first socket handle.
	 */
	SOCKET fd[SOCK_COUNT];
	bool ready = false;
	optional<std::error_code> writeErrorCode = nullopt;
	bool deleteLater = false;
	char textAddress[40] = {};

	bool isCompressed = false;

	ZlibCompressionAdapter compressionAdapter;
};

struct SocketSet
{
	std::vector<Socket *> fds;
};


static WZ_MUTEX *socketThreadMutex;
static WZ_SEMAPHORE *socketThreadSemaphore;
static WZ_THREAD *socketThread = nullptr;
static bool socketThreadQuit;
typedef std::map<Socket *, std::vector<uint8_t>> SocketThreadWriteMap;
static SocketThreadWriteMap socketThreadWrites;


static void socketCloseNow(Socket *sock);


bool socketReadReady(const Socket& sock)
{
	return sock.ready;
}

// Returns the last error for the calling thread
static int getSockErr()
{
#if   defined(WZ_OS_UNIX)
	return errno;
#elif defined(WZ_OS_WIN)
	return WSAGetLastError();
#endif
}

} // namespace tcp

#if defined(WZ_OS_WIN)
typedef int (WINAPI *GETADDRINFO_DLL_FUNC)(const char *node, const char *service,
        const struct addrinfo *hints,
        struct addrinfo **res);
typedef int (WINAPI *FREEADDRINFO_DLL_FUNC)(struct addrinfo *res);

static HMODULE winsock2_dll = nullptr;

static GETADDRINFO_DLL_FUNC getaddrinfo_dll_func = nullptr;
static FREEADDRINFO_DLL_FUNC freeaddrinfo_dll_func = nullptr;

# define getaddrinfo  getaddrinfo_dll_dispatcher
# define freeaddrinfo freeaddrinfo_dll_dispatcher

# include <ntverp.h>				// Windows SDK - include for access to VER_PRODUCTBUILD
# if VER_PRODUCTBUILD >= 9600
	// 9600 is the Windows SDK 8.1
	# include <VersionHelpers.h>	// For IsWindowsVistaOrGreater()
# else
	// Earlier SDKs may not have VersionHelpers.h - use simple fallback
	inline bool IsWindowsVistaOrGreater()
	{
		DWORD dwMajorVersion = (DWORD)(LOBYTE(LOWORD(GetVersion())));
		return dwMajorVersion >= 6;
	}
# endif

static int getaddrinfo(const char *node, const char *service,
                       const struct addrinfo *hints,
                       struct addrinfo **res)
{
	struct addrinfo hint;
	if (hints)
	{
		memcpy(&hint, hints, sizeof(hint));
	}

//	// Windows 95, 98 and ME
//		debug(LOG_ERROR, "Name resolution isn't supported on this version of Windows");
//		return EAI_FAIL;

	if (!IsWindowsVistaOrGreater())
	{
		// Windows 2000, XP and Server 2003
		if (hints)
		{
			// These flags are only supported from Windows Vista+
			hint.ai_flags &= ~(AI_V4MAPPED | AI_ADDRCONFIG);
		}
	}

	if (!winsock2_dll)
	{
		debug(LOG_ERROR, "Failed to load winsock2 DLL. Required for name resolution.");
		return EAI_FAIL;
	}

	if (!getaddrinfo_dll_func)
	{
		debug(LOG_ERROR, "Failed to retrieve \"getaddrinfo\" function from winsock2 DLL. Required for name resolution.");
		return EAI_FAIL;
	}

	return getaddrinfo_dll_func(node, service, hints ? &hint : NULL, res);
}

static void freeaddrinfo(struct addrinfo *res)
{

//	// Windows 95, 98 and ME
//		debug(LOG_ERROR, "Name resolution isn't supported on this version of Windows");
//		return;

	if (!winsock2_dll)
	{
		debug(LOG_ERROR, "Failed to load winsock2 DLL. Required for name resolution.");
		return;
	}

	if (!freeaddrinfo_dll_func)
	{
		debug(LOG_ERROR, "Failed to retrieve \"freeaddrinfo\" function from winsock2 DLL. Required for name resolution.");
		return;
	}

	freeaddrinfo_dll_func(res);
}
#endif

namespace tcp
{

static int addressToText(const struct sockaddr *addr, char *buf, size_t size)
{
	auto handleIpv4 = [&](uint32_t addr) {
		uint32_t val = ntohl(addr);
		return snprintf(buf, size, "%u.%u.%u.%u", (val>>24)&0xFF, (val>>16)&0xFF, (val>>8)&0xFF, val&0xFF);
	};

	switch (addr->sa_family)
	{
	case AF_INET:
		{
#if defined(__GNUC__) && !defined(__INTEL_COMPILER) && !defined(__clang__)
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wcast-align"
#endif
			return handleIpv4((reinterpret_cast<const sockaddr_in *>(addr))->sin_addr.s_addr);
#if defined(__GNUC__) && !defined(__INTEL_COMPILER) && !defined(__clang__)
# pragma GCC diagnostic pop
#endif
		}
	case AF_INET6:
		{

			// Check to see if this is really a IPv6 address
#if defined(__GNUC__) && !defined(__INTEL_COMPILER) && !defined(__clang__)
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wcast-align"
#endif
			const struct sockaddr_in6 *mappedIP = reinterpret_cast<const sockaddr_in6 *>(addr);
#if defined(__GNUC__) && !defined(__INTEL_COMPILER) && !defined(__clang__)
# pragma GCC diagnostic pop
#endif
			if (IN6_IS_ADDR_V4MAPPED(&mappedIP->sin6_addr))
			{
				// looks like it is ::ffff:(...) so lets set up a IPv4 socket address structure
				// slightly overkill for our needs, but it shows exactly what needs to be done.
				// At this time, we only care about the address, nothing else.
				struct sockaddr_in addr4;
				memcpy(&addr4.sin_addr.s_addr, mappedIP->sin6_addr.s6_addr + 12, sizeof(addr4.sin_addr.s_addr));
				return handleIpv4(addr4.sin_addr.s_addr);
			}
			else
			{
				static_assert(sizeof(in6_addr::s6_addr) == 16, "Standard expects in6_addr structure that contains member s6_addr[16], a 16-element array of uint8_t");
#if defined(__GNUC__) && !defined(__INTEL_COMPILER) && !defined(__clang__)
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wcast-align"
#endif
				const uint8_t *address_u8 = &((reinterpret_cast<const sockaddr_in6 *>(addr))->sin6_addr.s6_addr[0]);
#if defined(__GNUC__) && !defined(__INTEL_COMPILER) && !defined(__clang__)
# pragma GCC diagnostic pop
#endif
				uint16_t address[8] = {0};
				memcpy(&address, address_u8, sizeof(uint8_t) * 16);
				return snprintf(buf, size,
								"%hx:%hx:%hx:%hx:%hx:%hx:%hx:%hx",
								ntohs(address[0]),
								ntohs(address[1]),
								ntohs(address[2]),
								ntohs(address[3]),
								ntohs(address[4]),
								ntohs(address[5]),
								ntohs(address[6]),
								ntohs(address[7]));
			}
		}
	default:
		ASSERT(!"Unknown address family", "Got non IPv4 or IPv6 address!");
		return -1;
	}
}

static const char *strSockError(int error)
{
#if   defined(WZ_OS_WIN)
	switch (error)
	{
	case 0:                     return "No error";
	case WSAEINTR:              return "Interrupted system call";
	case WSAEBADF:              return "Bad file number";
	case WSAEACCES:             return "Permission denied";
	case WSAEFAULT:             return "Bad address";
	case WSAEINVAL:             return "Invalid argument";
	case WSAEMFILE:             return "Too many open sockets";
	case WSAEWOULDBLOCK:        return "Operation would block";
	case WSAEINPROGRESS:        return "Operation now in progress";
	case WSAEALREADY:           return "Operation already in progress";
	case WSAENOTSOCK:           return "Socket operation on non-socket";
	case WSAEDESTADDRREQ:       return "Destination address required";
	case WSAEMSGSIZE:           return "Message too long";
	case WSAEPROTOTYPE:         return "Protocol wrong type for socket";
	case WSAENOPROTOOPT:        return "Bad protocol option";
	case WSAEPROTONOSUPPORT:    return "Protocol not supported";
	case WSAESOCKTNOSUPPORT:    return "Socket type not supported";
	case WSAEOPNOTSUPP:         return "Operation not supported on socket";
	case WSAEPFNOSUPPORT:       return "Protocol family not supported";
	case WSAEAFNOSUPPORT:       return "Address family not supported";
	case WSAEADDRINUSE:         return "Address already in use";
	case WSAEADDRNOTAVAIL:      return "Can't assign requested address";
	case WSAENETDOWN:           return "Network is down";
	case WSAENETUNREACH:        return "Network is unreachable";
	case WSAENETRESET:          return "Net connection reset";
	case WSAECONNABORTED:       return "Software caused connection abort";
	case WSAECONNRESET:         return "Connection reset by peer";
	case WSAENOBUFS:            return "No buffer space available";
	case WSAEISCONN:            return "Socket is already connected";
	case WSAENOTCONN:           return "Socket is not connected";
	case WSAESHUTDOWN:          return "Can't send after socket shutdown";
	case WSAETOOMANYREFS:       return "Too many references, can't splice";
	case WSAETIMEDOUT:          return "Connection timed out";
	case WSAECONNREFUSED:       return "Connection refused";
	case WSAELOOP:              return "Too many levels of symbolic links";
	case WSAENAMETOOLONG:       return "File name too long";
	case WSAEHOSTDOWN:          return "Host is down";
	case WSAEHOSTUNREACH:       return "No route to host";
	case WSAENOTEMPTY:          return "Directory not empty";
	case WSAEPROCLIM:           return "Too many processes";
	case WSAEUSERS:             return "Too many users";
	case WSAEDQUOT:             return "Disc quota exceeded";
	case WSAESTALE:             return "Stale NFS file handle";
	case WSAEREMOTE:            return "Too many levels of remote in path";
	case WSASYSNOTREADY:        return "Network system is unavailable";
	case WSAVERNOTSUPPORTED:    return "Winsock version out of range";
	case WSANOTINITIALISED:     return "WSAStartup not yet called";
	case WSAEDISCON:            return "Graceful shutdown in progress";
	case WSAHOST_NOT_FOUND:     return "Host not found";
	case WSANO_DATA:            return "No host data of that type was found";
	default:                    return "Unknown error";
	}
#elif defined(WZ_OS_UNIX)
	return strerror(error);
#endif
}

/**
 * Test whether the given socket still has an open connection.
 *
 * @return Empty result when the connection is open, or contains an appropriate `std::error_code`
 *         when it's closed or in an error state.
 */
static net::result<void> connectionStatus(Socket *sock)
{
	const SocketSet set = {std::vector<Socket *>(1, sock)};

	ASSERT_OR_RETURN(tl::make_unexpected(make_network_error_code(EBADF)),
	                 sock && sock->fd[SOCK_CONNECTION] != INVALID_SOCKET, "Invalid socket");

	// Check whether the socket is still connected
	int ret = checkSockets(set, 0);
	if (ret == SOCKET_ERROR)
	{
		return tl::make_unexpected(make_network_error_code(getSockErr()));
	}
	else if (ret == (int)set.fds.size() && sock->ready)
	{
		/* The next recv(2) call won't block, but we're writing. So
		 * check the read queue to see if the connection is closed.
		 * If there's no data in the queue that means the connection
		 * is closed.
		 */
#if defined(WZ_OS_WIN)
		unsigned long readQueue;
		ret = ioctlsocket(sock->fd[SOCK_CONNECTION], FIONREAD, &readQueue);
#else
		int readQueue;
		ret = ioctl(sock->fd[SOCK_CONNECTION], FIONREAD, &readQueue);
#endif
		if (ret == SOCKET_ERROR)
		{
			debug(LOG_NET, "socket error");
			return tl::make_unexpected(make_network_error_code(getSockErr()));
		}
		else if (readQueue == 0)
		{
			// Disconnected
			debug(LOG_NET, "Read queue empty - failing (ECONNRESET)");
			return tl::make_unexpected(make_network_error_code(ECONNRESET));
		}
	}

	return {};
}

static int socketThreadFunction(void *)
{
	wzMutexLock(socketThreadMutex);
	while (!socketThreadQuit)
	{
#if   defined(WZ_OS_UNIX)
		SOCKET maxfd = INT_MIN;
#elif defined(WZ_OS_WIN)
		SOCKET maxfd = 0;
#endif
		fd_set fds;
		FD_ZERO(&fds);
		size_t descriptorsToWaitOn = 0;
		for (SocketThreadWriteMap::iterator i = socketThreadWrites.begin(); i != socketThreadWrites.end();)
		{
			if (!i->second.empty())
			{
				SOCKET fd = i->first->fd[SOCK_CONNECTION];
				maxfd = std::max(maxfd, fd);
				ASSERT(!FD_ISSET(fd, &fds), "Duplicate file descriptor!");  // Shouldn't be possible, but blocking in send, after select says it won't block, shouldn't be possible either.
				FD_SET(fd, &fds);
				++descriptorsToWaitOn;
				++i;
			}
			else
			{
				ASSERT(false, "Empty buffer for pending socket writes"); // This shouldn't happen!
				Socket *sock = i->first;
				i = socketThreadWrites.erase(i);
				if (sock->deleteLater)
				{
					socketCloseNow(sock);
				}
			}
		}
		struct timeval tv = {0, 50 * 1000};

		// Check if we can write to any sockets.
		int ret = -1;
		if (descriptorsToWaitOn > 0)
		{
			wzMutexUnlock(socketThreadMutex);
			ret = select(maxfd + 1, nullptr, &fds, nullptr, &tv);
			wzMutexLock(socketThreadMutex);
		}

		// We can write to some sockets. (Ignore errors from select, we may have deleted the socket after unlocking the mutex, and before calling select.)
		if (ret > 0)
		{
			for (SocketThreadWriteMap::iterator i = socketThreadWrites.begin(); i != socketThreadWrites.end();)
			{
				SocketThreadWriteMap::iterator w = i;
				++i;

				Socket *sock = w->first;
				std::vector<uint8_t> &writeQueue = w->second;
				ASSERT(!writeQueue.empty(), "writeQueue[sock] must not be empty.");

				if (!FD_ISSET(sock->fd[SOCK_CONNECTION], &fds))
				{
					continue;  // This socket is not ready for writing, or we don't have anything to write.
				}

				// Write data.
				// FIXME SOMEHOW AAARGH This send() call can't block, but unless the socket is not set to blocking (setting the socket to nonblocking had better work, or else), does anyway (at least sometimes, when someone quits). Not reproducible except in public releases.
				ssize_t retSent = send(sock->fd[SOCK_CONNECTION], reinterpret_cast<char *>(&writeQueue[0]), writeQueue.size(), MSG_NOSIGNAL);
				if (retSent != SOCKET_ERROR)
				{
					// Erase as much data as written.
					writeQueue.erase(writeQueue.begin(), writeQueue.begin() + retSent);
					if (writeQueue.empty())
					{
						socketThreadWrites.erase(w);  // Nothing left to write, delete from pending list.
						if (sock->deleteLater)
						{
							socketCloseNow(sock);
						}
					}
				}
				else
				{
					switch (getSockErr())
					{
					case EAGAIN:
#if defined(EWOULDBLOCK) && EAGAIN != EWOULDBLOCK
					case EWOULDBLOCK:
#endif
					{
						const auto connStatus = connectionStatus(sock);
						if (!connStatus.has_value())
						{
							const auto errMsg = connStatus.error().message();
							debug(LOG_NET, "Socket error: %s", errMsg.c_str());
							sock->writeErrorCode = connStatus.error();
							socketThreadWrites.erase(w);  // Socket broken, don't try writing to it again.
							if (sock->deleteLater)
							{
								socketCloseNow(sock);
							}
						}
						break;
					}
					case EINTR:
						break;
#if defined(EPIPE)
					case EPIPE:
#endif
					default:
						sock->writeErrorCode = make_network_error_code(getSockErr());
						socketThreadWrites.erase(w);  // Socket broken, don't try writing to it again.
						if (sock->deleteLater)
						{
							socketCloseNow(sock);
						}
						break;
					}
				}
			}
		}

		if (socketThreadWrites.empty())
		{
			// Nothing to do, expect to wait.
			wzMutexUnlock(socketThreadMutex);
			wzSemaphoreWait(socketThreadSemaphore);
			wzMutexLock(socketThreadMutex);
		}
	}
	wzMutexUnlock(socketThreadMutex);

	return 42;  // Return value arbitrary and unused.
}

/**
 * Similar to read(2) with the exception that this function won't be
 * interrupted by signals (EINTR).
 */
net::result<ssize_t> readNoInt(Socket& sock, void *buf, size_t max_size, size_t *rawByteCount)
{
	size_t ignored;
	size_t &rawBytes = rawByteCount != nullptr ? *rawByteCount : ignored;
	rawBytes = 0;

	if (sock.fd[SOCK_CONNECTION] == INVALID_SOCKET)
	{
		debug(LOG_ERROR, "Invalid socket");
		return tl::make_unexpected(make_network_error_code(EBADF));
	}

	if (sock.isCompressed)
	{
		auto& compressAdapter = sock.compressionAdapter;
		if (compressAdapter.decompressionNeedInput())
		{
			// No input data, read some.

			auto& decompressInBuf = compressAdapter.decompressionInBuffer();
			decompressInBuf.resize(max_size + 1000);

			ssize_t received;
			do
			{
				//                                                  v----- This weird cast is because recv() takes a char * on windows instead of a void *...
				received = recv(sock.fd[SOCK_CONNECTION], (char *)&decompressInBuf[0], decompressInBuf.size(), 0);
			}
			while (received == SOCKET_ERROR && getSockErr() == EINTR);
			if (received < 0)
			{
				return tl::make_unexpected(make_network_error_code(getSockErr()));
			}

			compressAdapter.resetDecompressionStreamInputSize(received);
			rawBytes = received;

			if (received == 0)
			{
				// Socket got disconnected. No reason to do anything else here.
				return tl::make_unexpected(make_network_error_code(ECONNRESET));
			}
			else
			{
				compressAdapter.setDecompressionNeedInput(false);
			}
		}

		const auto decompressRes = compressAdapter.decompress(buf, max_size);
		if (!decompressRes.has_value())
		{
			return tl::make_unexpected(decompressRes.error());
		}

		if (compressAdapter.availableSpaceToDecompress() != 0)
		{
			compressAdapter.setDecompressionNeedInput(true);
			ASSERT(compressAdapter.decompressionStreamConsumedAllInput(), "zlib not consuming all input!");
		}

		return max_size - compressAdapter.availableSpaceToDecompress();  // Got some data, return how much.
	}

	ssize_t received;
	do
	{
		received = recv(sock.fd[SOCK_CONNECTION], (char *)buf, max_size, 0);
		if (received == 0)
		{
			// Socket got disconnected. No reason to do anything else here.
			return tl::make_unexpected(make_network_error_code(ECONNRESET));
		}
	}
	while (received == SOCKET_ERROR && getSockErr() == EINTR);

	sock.ready = false;
	rawBytes = received;

	return received;
}

/**
 * Similar to write(2) with the exception that this function will block until
 * <em>all</em> data has been written or an error occurs.
 *
 * @return @c size when successful or @c SOCKET_ERROR if an error occurred.
 */
net::result<ssize_t> writeAll(Socket& sock, const void *buf, size_t size, size_t *rawByteCount)
{
	size_t ignored;
	size_t &rawBytes = rawByteCount != nullptr ? *rawByteCount : ignored;
	rawBytes = 0;

	if (sock.fd[SOCK_CONNECTION] == INVALID_SOCKET)
	{
		debug(LOG_ERROR, "Invalid socket (EBADF)");
		return tl::make_unexpected(make_network_error_code(EBADF));
	}

	if (sock.writeErrorCode.has_value())
	{
		return tl::make_unexpected(sock.writeErrorCode.value());
	}

	if (size > 0)
	{
		if (!sock.isCompressed)
		{
			wzMutexLock(socketThreadMutex);
			if (socketThreadWrites.empty())
			{
				wzSemaphorePost(socketThreadSemaphore);
			}
			std::vector<uint8_t> &writeQueue = socketThreadWrites[&sock];
			writeQueue.insert(writeQueue.end(), static_cast<char const *>(buf), static_cast<char const *>(buf) + size);
			wzMutexUnlock(socketThreadMutex);
			rawBytes = size;
		}
		else
		{
			sock.compressionAdapter.compress(buf, size);
		}
	}

	return size;
}

void socketFlush(Socket& sock, uint8_t player, size_t *rawByteCount)
{
	size_t ignored;
	size_t &rawBytes = rawByteCount != nullptr ? *rawByteCount : ignored;
	rawBytes = 0;

	if (!sock.isCompressed)
	{
		return;  // Not compressed, so don't mess with zlib.
	}

	ASSERT(!sock.writeErrorCode.has_value(), "Socket write error?? (Player: %" PRIu8 "", player);

	sock.compressionAdapter.flushCompressionStream();

	auto& compressionBuf = sock.compressionAdapter.compressionOutBuffer();
	if (compressionBuf.empty())
	{
		return;  // No data to flush out.
	}

	wzMutexLock(socketThreadMutex);
	if (socketThreadWrites.empty())
	{
		wzSemaphorePost(socketThreadSemaphore);
	}
	std::vector<uint8_t> &writeQueue = socketThreadWrites[&sock];
	writeQueue.reserve(writeQueue.size() + compressionBuf.size());
	writeQueue.insert(writeQueue.end(), compressionBuf.begin(), compressionBuf.end());
	wzMutexUnlock(socketThreadMutex);

	// Data sent, don't send again.
	rawBytes = compressionBuf.size();
	compressionBuf.clear();
}

void socketBeginCompression(Socket& sock)
{
	if (sock.isCompressed)
	{
		return;  // Nothing to do.
	}

	wzMutexLock(socketThreadMutex);

	const auto initRes = sock.compressionAdapter.initialize();
	if (!initRes.has_value())
	{
		const auto errMsg = initRes.error().message();
		debug(LOG_NET, "Failed to initialize compression algorithms. Sockets won't work properly! Detailed error message: %s", errMsg.c_str());
		wzMutexUnlock(socketThreadMutex);
		return;
	}
	sock.isCompressed = true;
	wzMutexUnlock(socketThreadMutex);
}

bool socketSetTCPNoDelay(Socket& sock, bool nodelay)
{
#if defined(TCP_NODELAY)
	int value = (nodelay) ? 1 : 0;
	int result = setsockopt(sock.fd[SOCK_CONNECTION], IPPROTO_TCP, TCP_NODELAY, (char *) &value, sizeof(int));
	debug(LOG_NET, "Setting TCP_NODELAY on socket: %s", (result != SOCKET_ERROR) ? "success" : "failure");
	return result != SOCKET_ERROR;
#else
	debug(LOG_NET, "Unable to set TCP_NODELAY on socket - unsupported");
	return false;
#endif
}

SocketSet *allocSocketSet()
{
	return new SocketSet;
}

void deleteSocketSet(SocketSet *set)
{
	delete set;
}

/**
 * Add the given socket to the given socket set.
 *
 * @return true if @c socket is successfully added to @set.
 */
void SocketSet_AddSocket(SocketSet& set, Socket *socket)
{
	/* Check whether this socket is already present in this set (i.e. it
	 * shouldn't be added again).
	 */
	size_t i = std::find(set.fds.begin(), set.fds.end(), socket) - set.fds.begin();
	if (i != set.fds.size())
	{
		debug(LOG_NET, "Already found, socket: (set->fds[%lu]) %p", (unsigned long)i, static_cast<void *>(socket));
		return;
	}

	set.fds.push_back(socket);
	debug(LOG_NET, "Socket added: set->fds[%lu] = %p", (unsigned long)i, static_cast<void *>(socket));
}

/**
 * Remove the given socket from the given socket set.
 */
void SocketSet_DelSocket(SocketSet& set, Socket *socket)
{
	size_t i = std::find(set.fds.begin(), set.fds.end(), socket) - set.fds.begin();
	if (i != set.fds.size())
	{
		debug(LOG_NET, "Socket %p erased (set->fds[%lu])", static_cast<void *>(socket), (unsigned long)i);
		set.fds.erase(set.fds.begin() + i);
	}
}

#if !defined(SOCK_CLOEXEC)
static bool setSocketInheritable(SOCKET fd, bool inheritable)
{
#if   defined(WZ_OS_UNIX)
	int sockopts = fcntl(fd, F_SETFD);
	if (sockopts == SOCKET_ERROR)
	{
		debug(LOG_NET, "Failed to retrieve current socket options: %s", strSockError(getSockErr()));
		return false;
	}

	// Set or clear FD_CLOEXEC flag
	if (inheritable)
	{
		sockopts &= ~FD_CLOEXEC;
	}
	else
	{
		sockopts |= FD_CLOEXEC;
	}

	if (fcntl(fd, F_SETFD, sockopts) == SOCKET_ERROR)
	{
		debug(LOG_NET, "Failed to set socket %sinheritable: %s", (inheritable ? "" : "non-"), strSockError(getSockErr()));
		return false;
	}
#elif defined(WZ_OS_WIN)
	DWORD dwFlags = (inheritable) ? HANDLE_FLAG_INHERIT : 0;
	if (::SetHandleInformation((HANDLE)fd, HANDLE_FLAG_INHERIT, dwFlags) == 0)
	{
		DWORD dwErr = GetLastError();
		debug(LOG_NET, "Failed to set socket %sinheritable: %s", (inheritable ? "" : "non-"), std::to_string(dwErr).c_str());
		return false;
	}
#endif

	debug(LOG_NET, "Socket is set to %sinheritable.", (inheritable ? "" : "non-"));
	return true;
}
#endif // !defined(SOCK_CLOEXEC)

static bool setSocketBlocking(const SOCKET fd, bool blocking)
{
#if   defined(WZ_OS_UNIX)
	int sockopts = fcntl(fd, F_GETFL);
	if (sockopts == SOCKET_ERROR)
	{
		debug(LOG_NET, "Failed to retrieve current socket options: %s", strSockError(getSockErr()));
		return false;
	}

	// Set or clear O_NONBLOCK flag
	if (blocking)
	{
		sockopts &= ~O_NONBLOCK;
	}
	else
	{
		sockopts |= O_NONBLOCK;
	}

	if (fcntl(fd, F_SETFL, sockopts) == SOCKET_ERROR)
#elif defined(WZ_OS_WIN)
	unsigned long nonblocking = !blocking;
	if (ioctlsocket(fd, FIONBIO, &nonblocking) == SOCKET_ERROR)
#endif
	{
		debug(LOG_NET, "Failed to set socket %sblocking: %s", (blocking ? "" : "non-"), strSockError(getSockErr()));
		return false;
	}

	debug(LOG_NET, "Socket is set to %sblocking.", (blocking ? "" : "non-"));
	return true;
}

static void socketBlockSIGPIPE(const SOCKET fd, bool block_sigpipe)
{
#if defined(SO_NOSIGPIPE)
	const int no_sigpipe = block_sigpipe ? 1 : 0;

	if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe)) == SOCKET_ERROR)
	{
		debug(LOG_INFO, "Failed to set SO_NOSIGPIPE on socket, SIGPIPE might be raised when connections gets broken. Error: %s", strSockError(getSockErr()));
	}
	// this is only for unix, windows don't have SIGPIPE
	debug(LOG_NET, "Socket fd %x sets SIGPIPE to %sblocked.", fd, (block_sigpipe ? "" : "non-"));
#else
	// Prevent warnings
	(void)fd;
	(void)block_sigpipe;
#endif
}

int checkSockets(const SocketSet& set, unsigned int timeout)
{
	if (set.fds.empty())
	{
		return 0;
	}

#if   defined(WZ_OS_UNIX)
	SOCKET maxfd = INT_MIN;
#elif defined(WZ_OS_WIN)
	SOCKET maxfd = 0;
#endif

	bool compressedReady = false;
	for (size_t i = 0; i < set.fds.size(); ++i)
	{
		ASSERT(set.fds[i]->fd[SOCK_CONNECTION] != INVALID_SOCKET, "Invalid file descriptor!");

		if (set.fds[i]->isCompressed && !set.fds[i]->compressionAdapter.decompressionNeedInput())
		{
			compressedReady = true;
			break;
		}

		maxfd = std::max(maxfd, set.fds[i]->fd[SOCK_CONNECTION]);
	}

	if (compressedReady)
	{
		// A socket already has some data ready. Don't really poll the sockets.

		int ret = 0;
		for (size_t i = 0; i < set.fds.size(); ++i)
		{
			set.fds[i]->ready = set.fds[i]->isCompressed && !set.fds[i]->compressionAdapter.decompressionNeedInput();
			++ret;
		}
		return ret;
	}

	int ret;
	fd_set fds;
	do
	{
		struct timeval tv = {(int)(timeout / 1000), (int)(timeout % 1000) * 1000};  // Cast to int to avoid narrowing needed for C++11.

		FD_ZERO(&fds);
		for (size_t i = 0; i < set.fds.size(); ++i)
		{
			const SOCKET fd = set.fds[i]->fd[SOCK_CONNECTION];

			FD_SET(fd, &fds);
		}

		ret = select(maxfd + 1, &fds, nullptr, nullptr, &tv);
	}
	while (ret == SOCKET_ERROR && getSockErr() == EINTR);

	if (ret == SOCKET_ERROR)
	{
		debug(LOG_ERROR, "select failed: %s", strSockError(getSockErr()));
		return SOCKET_ERROR;
	}

	for (size_t i = 0; i < set.fds.size(); ++i)
	{
		set.fds[i]->ready = FD_ISSET(set.fds[i]->fd[SOCK_CONNECTION], &fds);
	}

	return ret;
}

/**
 * Similar to read(2) with the exception that this function won't be
 * interrupted by signals (EINTR) and will only return when <em>exactly</em>
 * @c size bytes have been received. I.e. this function blocks until all data
 * has been received or a timeout occurred.
 *
 * @param timeout When non-zero this function times out after @c timeout
 *                milliseconds. When zero this function blocks until success or
 *                an error occurs.
 *
 * @c return @c size when successful, less than @c size but at least zero (0)
 * when the other end disconnected or a timeout occurred. Or @c SOCKET_ERROR if
 * an error occurred.
 */
net::result<ssize_t> readAll(Socket& sock, void *buf, size_t size, unsigned int timeout)
{
	ASSERT(!sock.isCompressed, "readAll on compressed sockets not implemented.");

	const SocketSet set = {std::vector<Socket *>(1, &sock)};

	size_t received = 0;

	if (sock.fd[SOCK_CONNECTION] == INVALID_SOCKET)
	{
		debug(LOG_ERROR, "Invalid socket (%p), sock->fd[SOCK_CONNECTION]=%" PRIuPTR"x  (error: EBADF)", static_cast<void *>(&sock), static_cast<uintptr_t>(sock.fd[SOCK_CONNECTION]));
		return tl::make_unexpected(make_network_error_code(EBADF));
	}

	while (received < size)
	{
		ssize_t ret;

		// If a timeout is set, wait for that amount of time for data to arrive (or abort)
		if (timeout)
		{
			ret = checkSockets(set, timeout);
			if (ret < (ssize_t)set.fds.size()
			    || !sock.ready)
			{
				if (ret == 0)
				{
					debug(LOG_NET, "socket (%p) has timed out.", static_cast<void *>(&sock));
					return tl::make_unexpected(make_network_error_code(ETIMEDOUT));
				}
				debug(LOG_NET, "socket (%p) error.", static_cast<void *>(&sock));
				return tl::make_unexpected(make_network_error_code(getSockErr()));
			}
		}

		ret = recv(sock.fd[SOCK_CONNECTION], &((char *)buf)[received], size - received, 0);
		sock.ready = false;
		if (ret == 0)
		{
			debug(LOG_NET, "Socket %" PRIuPTR"x disconnected.", static_cast<uintptr_t>(sock.fd[SOCK_CONNECTION]));
			return tl::make_unexpected(make_network_error_code(ECONNRESET));
		}

		if (ret == SOCKET_ERROR)
		{
			const auto sockErr = getSockErr();
			switch (sockErr)
			{
			case EAGAIN:
#if defined(EWOULDBLOCK) && EAGAIN != EWOULDBLOCK
			case EWOULDBLOCK:
#endif
			case EINTR:
				continue;

			default:
				return tl::make_unexpected(make_network_error_code(sockErr));
			}
		}

		received += ret;
	}

	return received;
}

static void socketCloseNow(Socket *sock)
{
	for (unsigned i = 0; i < ARRAY_SIZE(sock->fd); ++i)
	{
		if (sock->fd[i] != INVALID_SOCKET)
		{
#if defined(WZ_OS_WIN)
			int err = closesocket(sock->fd[i]);
#else
			int err = close(sock->fd[i]);
#endif
			if (err)
			{
				debug(LOG_ERROR, "Failed to close socket %p: %s", static_cast<void *>(sock), strSockError(getSockErr()));
			}

			/* Make sure that dangling pointers to this
			 * structure don't think they've got their
			 * hands on a valid socket.
			 */
			sock->fd[i] = INVALID_SOCKET;
		}
	}
	delete sock;
}

void socketClose(Socket *sock)
{
	wzMutexLock(socketThreadMutex);
	//Instead of socketThreadWrites.erase(sock);, try sending the data before actually deleting.
	if (socketThreadWrites.find(sock) != socketThreadWrites.end())
	{
		// Wait until the data is written, then delete the socket.
		sock->deleteLater = true;
	}
	else
	{
		// Delete the socket.
		socketCloseNow(sock);
	}
	wzMutexUnlock(socketThreadMutex);
}

Socket *socketAccept(Socket *sock)
{
	unsigned int i;

	/* Search for a socket that has a pending connection on it and accept
	 * the first one.
	 */
	for (i = 0; i < ARRAY_SIZE(sock->fd); ++i)
	{
		if (sock->fd[i] != INVALID_SOCKET)
		{
			struct sockaddr_storage addr;
			socklen_t addr_len = sizeof(addr);
			Socket *conn;
			unsigned int j;

#if defined(SOCK_CLOEXEC)
			const SOCKET newConn = accept4(sock->fd[i], (struct sockaddr *)&addr, &addr_len, SOCK_CLOEXEC);
#else
			const SOCKET newConn = accept(sock->fd[i], (struct sockaddr *)&addr, &addr_len);
#endif
			if (newConn == INVALID_SOCKET)
			{
				// Ignore the case where no connection is pending
				if (getSockErr() != EAGAIN
				    && getSockErr() != EWOULDBLOCK)
				{
					debug(LOG_ERROR, "accept failed for socket %p: %s", static_cast<void *>(sock), strSockError(getSockErr()));
				}

				continue;
			}

			conn = new Socket;
			if (conn == nullptr)
			{
				debug(LOG_ERROR, "Out of memory!");
				abort();
				return nullptr;
			}

#if !defined(SOCK_CLOEXEC)
			if (!setSocketInheritable(newConn, false))
			{
				debug(LOG_NET, "Couldn't set socket (%p) inheritable status (false). Ignoring...", static_cast<void *>(conn));
				// ignore and continue
			}
#endif

			debug(LOG_NET, "setting socket (%p) blocking status (false).", static_cast<void *>(conn));
			if (!setSocketBlocking(newConn, false))
			{
				debug(LOG_NET, "Couldn't set socket (%p) blocking status (false).  Closing.", static_cast<void *>(conn));
				socketClose(conn);
				return nullptr;
			}

			socketBlockSIGPIPE(newConn, true);

			// Mark all unused socket handles as invalid
			for (j = 0; j < ARRAY_SIZE(conn->fd); ++j)
			{
				conn->fd[j] = INVALID_SOCKET;
			}

			conn->fd[SOCK_CONNECTION] = newConn;

			sock->ready = false;

			addressToText((const struct sockaddr *)&addr, conn->textAddress, sizeof(conn->textAddress));
			debug(LOG_NET, "Incoming connection from [%s]:/*%%d*/ (FIXME: gives strict-aliasing error)", conn->textAddress/*, (unsigned int)ntohs(((const struct sockaddr_in*)&addr)->sin_port)*/);
			debug(LOG_NET, "Using socket %p", static_cast<void *>(conn));
			return conn;
		}
	}

	return nullptr;
}

net::result<Socket*> socketOpen(const SocketAddress *addr, unsigned timeout)
{
	unsigned int i;
	int ret;

	Socket *const conn = new Socket;

	ASSERT(addr != nullptr, "NULL Socket provided");

	addressToText(addr->ai_addr, conn->textAddress, sizeof(conn->textAddress));
#if defined(__GNUC__) && !defined(__INTEL_COMPILER) && !defined(__clang__)
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wcast-align"
#endif
	debug(LOG_NET, "Connecting to [%s]:%d", conn->textAddress, (int)ntohs((reinterpret_cast<const sockaddr_in *>(addr->ai_addr))->sin_port));
#if defined(__GNUC__) && !defined(__INTEL_COMPILER) && !defined(__clang__)
# pragma GCC diagnostic pop
#endif

	// Mark all unused socket handles as invalid
	for (i = 0; i < ARRAY_SIZE(conn->fd); ++i)
	{
		conn->fd[i] = INVALID_SOCKET;
	}

	int socket_type = addr->ai_socktype;
#if defined(SOCK_CLOEXEC)
	socket_type |= SOCK_CLOEXEC;
#endif
	conn->fd[SOCK_CONNECTION] = socket(addr->ai_family, socket_type, addr->ai_protocol);

	if (conn->fd[SOCK_CONNECTION] == INVALID_SOCKET)
	{
		const auto sockErr = getSockErr();
		debug(LOG_ERROR, "Failed to create a socket (%p): %s", static_cast<void *>(conn), strSockError(sockErr));
		socketClose(conn);
		return tl::make_unexpected(make_network_error_code(sockErr));
	}

#if !defined(SOCK_CLOEXEC)
	if (!setSocketInheritable(conn->fd[SOCK_CONNECTION], false))
	{
		debug(LOG_NET, "Couldn't set socket (%p) inheritable status (false). Ignoring...", static_cast<void *>(conn));
		// ignore and continue
	}
#endif

	debug(LOG_NET, "setting socket (%p) blocking status (false).", static_cast<void *>(conn));
	if (!setSocketBlocking(conn->fd[SOCK_CONNECTION], false))
	{
		const auto sockErr = getSockErr();
		debug(LOG_NET, "Couldn't set socket (%p) blocking status (false).  Closing.", static_cast<void *>(conn));
		socketClose(conn);
		return tl::make_unexpected(make_network_error_code(sockErr));
	}

	socketBlockSIGPIPE(conn->fd[SOCK_CONNECTION], true);

	ret = connect(conn->fd[SOCK_CONNECTION], addr->ai_addr, addr->ai_addrlen);
	if (ret == SOCKET_ERROR)
	{
		fd_set conReady;
#if   defined(WZ_OS_WIN)
		fd_set conFailed;
#endif

		if ((getSockErr() != EINPROGRESS
		     && getSockErr() != EAGAIN
		     && getSockErr() != EWOULDBLOCK)
#if   defined(WZ_OS_UNIX)
		    || conn->fd[SOCK_CONNECTION] >= FD_SETSIZE
#endif
		    || timeout == 0)
		{
			const auto sockErr = getSockErr();
			debug(LOG_NET, "Failed to start connecting: %s, using socket %p", strSockError(sockErr), static_cast<void *>(conn));
			socketClose(conn);
			return tl::make_unexpected(make_network_error_code(sockErr));
		}

		do
		{
			struct timeval tv = {(int)(timeout / 1000), (int)(timeout % 1000) * 1000};  // Cast to int to avoid narrowing needed for C++11.

			FD_ZERO(&conReady);
			FD_SET(conn->fd[SOCK_CONNECTION], &conReady);
#if   defined(WZ_OS_WIN)
			FD_ZERO(&conFailed);
			FD_SET(conn->fd[SOCK_CONNECTION], &conFailed);
#endif

#if   defined(WZ_OS_WIN)
			ret = select(conn->fd[SOCK_CONNECTION] + 1, NULL, &conReady, &conFailed, &tv);
#else
			ret = select(conn->fd[SOCK_CONNECTION] + 1, nullptr, &conReady, nullptr, &tv);
#endif
		}
		while (ret == SOCKET_ERROR && getSockErr() == EINTR);

		if (ret == SOCKET_ERROR)
		{
			const auto sockErr = getSockErr();
			debug(LOG_NET, "Failed to wait for connection: %s, socket %p.  Closing.", strSockError(sockErr), static_cast<void *>(conn));
			socketClose(conn);
			return tl::make_unexpected(make_network_error_code(sockErr));
		}

		if (ret == 0)
		{
			const auto sockErr = ETIMEDOUT;
			debug(LOG_NET, "Timed out while waiting for connection to be established: %s, using socket %p.  Closing.", strSockError(sockErr), static_cast<void *>(conn));
			socketClose(conn);
			return tl::make_unexpected(make_network_error_code(sockErr));
		}

#if   defined(WZ_OS_WIN)
		ASSERT(FD_ISSET(conn->fd[SOCK_CONNECTION], &conReady) || FD_ISSET(conn->fd[SOCK_CONNECTION], &conFailed), "\"sock\" is the only file descriptor in set, it should be the one that is set.");
#else
		ASSERT(FD_ISSET(conn->fd[SOCK_CONNECTION], &conReady), "\"sock\" is the only file descriptor in set, it should be the one that is set.");
#endif

#if   defined(WZ_OS_WIN)
		if (FD_ISSET(conn->fd[SOCK_CONNECTION], &conFailed))
#elif defined(WZ_OS_UNIX)
		if (connect(conn->fd[SOCK_CONNECTION], addr->ai_addr, addr->ai_addrlen) == SOCKET_ERROR
		    && getSockErr() != EISCONN)
#endif
		{
			const auto sockErr = getSockErr();
			debug(LOG_NET, "Failed to connect: %s, with socket %p.  Closing.", strSockError(sockErr), static_cast<void *>(conn));
			socketClose(conn);
			return tl::make_unexpected(make_network_error_code(sockErr));
		}
	}

	return conn;
}

net::result<Socket*> socketListen(unsigned int port)
{
	/* Enable the V4 to V6 mapping, but only when available, because it
	 * isn't available on all platforms.
	 */
#if defined(IPV6_V6ONLY)
	static const int ipv6_v6only = 0;
#endif
	static const int so_reuseaddr = 1;

	struct sockaddr_in addr4;
	struct sockaddr_in6 addr6;
	unsigned int i;

	Socket *const conn = new Socket;

	// Mark all unused socket handles as invalid
	for (i = 0; i < ARRAY_SIZE(conn->fd); ++i)
	{
		conn->fd[i] = INVALID_SOCKET;
	}

	strncpy(conn->textAddress, "LISTENING SOCKET", sizeof(conn->textAddress));

	// Listen on all local IPv4 and IPv6 addresses for the given port
	addr4.sin_family      = AF_INET;
	addr4.sin_port        = htons(port);
	addr4.sin_addr.s_addr = INADDR_ANY;

	addr6.sin6_family   = AF_INET6;
	addr6.sin6_port     = htons(port);
	addr6.sin6_addr     = in6addr_any;
	addr6.sin6_flowinfo = 0;
	addr6.sin6_scope_id = 0;

	int socket_type = SOCK_STREAM;
#if defined(SOCK_CLOEXEC)
	socket_type |= SOCK_CLOEXEC;
#endif
	conn->fd[SOCK_IPV4_LISTEN] = socket(addr4.sin_family, socket_type, 0);
	conn->fd[SOCK_IPV6_LISTEN] = socket(addr6.sin6_family, socket_type, 0);

	if (conn->fd[SOCK_IPV4_LISTEN] == INVALID_SOCKET
	    && conn->fd[SOCK_IPV6_LISTEN] == INVALID_SOCKET)
	{
		const auto errorCode = getSockErr();
		debug(LOG_ERROR, "Failed to create an IPv4 and IPv6 (only supported address families) socket (%p): %s.  Closing.", static_cast<void *>(conn), strSockError(errorCode));
		socketClose(conn);
		return tl::make_unexpected(make_network_error_code(errorCode));
	}

	if (conn->fd[SOCK_IPV4_LISTEN] != INVALID_SOCKET)
	{
		debug(LOG_NET, "Successfully created an IPv4 socket (%p)", static_cast<void *>(conn));
	}

	if (conn->fd[SOCK_IPV6_LISTEN] != INVALID_SOCKET)
	{
		debug(LOG_NET, "Successfully created an IPv6 socket (%p)", static_cast<void *>(conn));
	}

#if defined(IPV6_V6ONLY)
	if (conn->fd[SOCK_IPV6_LISTEN] != INVALID_SOCKET)
	{
		if (setsockopt(conn->fd[SOCK_IPV6_LISTEN], IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&ipv6_v6only, sizeof(ipv6_v6only)) == SOCKET_ERROR)
		{
			debug(LOG_INFO, "Failed to set IPv6 socket to perform IPv4 to IPv6 mapping. Falling back to using two sockets. Error: %s", strSockError(getSockErr()));
		}
		else
		{
			debug(LOG_NET, "Successfully enabled IPv4 to IPv6 mapping. Cleaning up IPv4 socket.");
#if   defined(WZ_OS_WIN)
			closesocket(conn->fd[SOCK_IPV4_LISTEN]);
#else
			close(conn->fd[SOCK_IPV4_LISTEN]);
#endif
			conn->fd[SOCK_IPV4_LISTEN] = INVALID_SOCKET;
		}
	}
#endif

	if (conn->fd[SOCK_IPV4_LISTEN] != INVALID_SOCKET)
	{
#if !defined(SOCK_CLOEXEC)
		if (!setSocketInheritable(conn->fd[SOCK_IPV4_LISTEN], false))
		{
			debug(LOG_NET, "Couldn't set socket (%p) inheritable status (false). Ignoring...", static_cast<void *>(conn));
			// ignore and continue
		}
#endif

		if (setsockopt(conn->fd[SOCK_IPV4_LISTEN], SOL_SOCKET, SO_REUSEADDR, (const char *)&so_reuseaddr, sizeof(so_reuseaddr)) == SOCKET_ERROR)
		{
			debug(LOG_WARNING, "Failed to set SO_REUSEADDR on IPv4 socket. Error: %s", strSockError(getSockErr()));
		}

		debug(LOG_NET, "setting socket (%p) blocking status (false, IPv4).", static_cast<void *>(conn));
		if (bind(conn->fd[SOCK_IPV4_LISTEN], (const struct sockaddr *)&addr4, sizeof(addr4)) == SOCKET_ERROR
		    || listen(conn->fd[SOCK_IPV4_LISTEN], 5) == SOCKET_ERROR
		    || !setSocketBlocking(conn->fd[SOCK_IPV4_LISTEN], false))
		{
			debug(LOG_ERROR, "Failed to set up IPv4 socket for listening on port %u: %s", port, strSockError(getSockErr()));
#if   defined(WZ_OS_WIN)
			closesocket(conn->fd[SOCK_IPV4_LISTEN]);
#else
			close(conn->fd[SOCK_IPV4_LISTEN]);
#endif
			conn->fd[SOCK_IPV4_LISTEN] = INVALID_SOCKET;
		}
	}

	if (conn->fd[SOCK_IPV6_LISTEN] != INVALID_SOCKET)
	{
#if !defined(SOCK_CLOEXEC)
		if (!setSocketInheritable(conn->fd[SOCK_IPV6_LISTEN], false))
		{
			debug(LOG_NET, "Couldn't set socket (%p) inheritable status (false). Ignoring...", static_cast<void *>(conn));
			// ignore and continue
		}
#endif

		if (setsockopt(conn->fd[SOCK_IPV6_LISTEN], SOL_SOCKET, SO_REUSEADDR, (const char *)&so_reuseaddr, sizeof(so_reuseaddr)) == SOCKET_ERROR)
		{
			debug(LOG_INFO, "Failed to set SO_REUSEADDR on IPv6 socket. Error: %s", strSockError(getSockErr()));
		}

		debug(LOG_NET, "setting socket (%p) blocking status (false, IPv6).", static_cast<void *>(conn));
		if (bind(conn->fd[SOCK_IPV6_LISTEN], (const struct sockaddr *)&addr6, sizeof(addr6)) == SOCKET_ERROR
		    || listen(conn->fd[SOCK_IPV6_LISTEN], 5) == SOCKET_ERROR
		    || !setSocketBlocking(conn->fd[SOCK_IPV6_LISTEN], false))
		{
			debug(LOG_ERROR, "Failed to set up IPv6 socket for listening on port %u: %s", port, strSockError(getSockErr()));
#if   defined(WZ_OS_WIN)
			closesocket(conn->fd[SOCK_IPV6_LISTEN]);
#else
			close(conn->fd[SOCK_IPV6_LISTEN]);
#endif
			conn->fd[SOCK_IPV6_LISTEN] = INVALID_SOCKET;
		}
	}

	// Check whether we still have at least a single (operating) socket.
	if (conn->fd[SOCK_IPV4_LISTEN] == INVALID_SOCKET
	    && conn->fd[SOCK_IPV6_LISTEN] == INVALID_SOCKET)
	{
		const auto errorCode = getSockErr();
		debug(LOG_NET, "No IPv4 or IPv6 sockets created.");
		socketClose(conn);
		return tl::make_unexpected(make_network_error_code(errorCode));
	}

	return conn;
}

net::result<Socket*> socketOpenAny(const SocketAddress *addr, unsigned timeout)
{
	net::result<Socket*> res;
	while (addr != nullptr)
	{
		res = socketOpen(addr, timeout);
		if (res)
		{
			return res;
		}
		addr = addr->ai_next;
	}

	return res;
}

bool socketHasIPv4(const Socket& sock)
{
	if (sock.fd[SOCK_IPV4_LISTEN] != INVALID_SOCKET)
	{
		return true;
	}
	else
	{
#if defined(IPV6_V6ONLY)
		if (sock.fd[SOCK_IPV6_LISTEN] != INVALID_SOCKET)
		{
			int ipv6_v6only = 1;
			socklen_t len = sizeof(ipv6_v6only);
			if (getsockopt(sock.fd[SOCK_IPV6_LISTEN], IPPROTO_IPV6, IPV6_V6ONLY, (char *)&ipv6_v6only, &len) == 0)
			{
				return ipv6_v6only == 0;
			}
		}
#endif
		return false;
	}
}

bool socketHasIPv6(const Socket& sock)
{
	return sock.fd[SOCK_IPV6_LISTEN] != INVALID_SOCKET;
}

char const *getSocketTextAddress(const Socket& sock)
{
	return sock.textAddress;
}

std::vector<unsigned char> ipv4_AddressString_To_NetBinary(const std::string& ipv4Address)
{
	std::vector<unsigned char> binaryForm(sizeof(struct in_addr), 0);
	if (inet_pton(AF_INET, ipv4Address.c_str(), binaryForm.data()) <= 0)
	{
		// inet_pton failed
		binaryForm.clear();
	}
	return binaryForm;
}

#ifndef INET_ADDRSTRLEN
# define INET_ADDRSTRLEN 16
#endif

std::string ipv4_NetBinary_To_AddressString(const std::vector<unsigned char>& ip4NetBinaryForm)
{
	if (ip4NetBinaryForm.size() != sizeof(struct in_addr))
	{
		return "";
	}

	char buf[INET_ADDRSTRLEN] = {0};
	if (inet_ntop(AF_INET, ip4NetBinaryForm.data(), buf, sizeof(buf)) == nullptr)
	{
		return "";
	}
	std::string ipv4Address = buf;
	return ipv4Address;
}

std::vector<unsigned char> ipv6_AddressString_To_NetBinary(const std::string& ipv6Address)
{
	std::vector<unsigned char> binaryForm(sizeof(struct in6_addr), 0);
	if (inet_pton(AF_INET6, ipv6Address.c_str(), binaryForm.data()) <= 0)
	{
		// inet_pton failed
		binaryForm.clear();
	}
	return binaryForm;
}

#ifndef INET6_ADDRSTRLEN
# define INET6_ADDRSTRLEN 46
#endif

std::string ipv6_NetBinary_To_AddressString(const std::vector<unsigned char>& ip6NetBinaryForm)
{
	if (ip6NetBinaryForm.size() != sizeof(struct in6_addr))
	{
		return "";
	}

	char buf[INET6_ADDRSTRLEN] = {0};
	if (inet_ntop(AF_INET6, ip6NetBinaryForm.data(), buf, sizeof(buf)) == nullptr)
	{
		return "";
	}
	std::string ipv6Address = buf;
	return ipv6Address;
}

net::result<SocketAddress*> resolveHost(const char *host, unsigned int port)
{
	struct addrinfo *results;
	std::string service;
	struct addrinfo hint;
	int flags = 0;

	hint.ai_family    = AF_UNSPEC;
	hint.ai_socktype  = SOCK_STREAM;
	hint.ai_protocol  = 0;
#ifdef AI_V4MAPPED
	flags             |= AI_V4MAPPED;
#endif
#ifdef AI_ADDRCONFIG
	flags             |= AI_ADDRCONFIG;
#endif
	hint.ai_flags     = flags;
	hint.ai_addrlen   = 0;
	hint.ai_addr      = nullptr;
	hint.ai_canonname = nullptr;
	hint.ai_next      = nullptr;

	service = astringf("%u", port);

	auto error = getaddrinfo(host, service.c_str(), &hint, &results);
	if (error != 0)
	{
		const auto ec = make_getaddrinfo_error_code(error);
		const auto errMsg = ec.message();
		debug(LOG_NET, "getaddrinfo failed for %s:%s: %s", host, service.c_str(), errMsg.c_str());
		return tl::make_unexpected(ec);
	}

	return results;
}

void deleteSocketAddress(SocketAddress *addr)
{
	freeaddrinfo(addr);
}

// ////////////////////////////////////////////////////////////////////////
// setup stuff
void SOCKETinit()
{
#if defined(WZ_OS_WIN)
	static bool firstCall = true;
	if (firstCall)
	{
		firstCall = false;

		static WSADATA stuff;
		WORD ver_required = (2 << 8) + 2;
		if (WSAStartup(ver_required, &stuff) != 0)
		{
			debug(LOG_ERROR, "Failed to initialize Winsock: %s", strSockError(getSockErr()));
			return;
		}

		winsock2_dll = LoadLibraryA("ws2_32.dll");
		if (winsock2_dll)
		{
			getaddrinfo_dll_func = reinterpret_cast<GETADDRINFO_DLL_FUNC>(reinterpret_cast<void*>(GetProcAddress(winsock2_dll, "getaddrinfo")));
			freeaddrinfo_dll_func = reinterpret_cast<FREEADDRINFO_DLL_FUNC>(reinterpret_cast<void*>(GetProcAddress(winsock2_dll, "freeaddrinfo")));
		}
	}
#endif

	if (socketThread == nullptr)
	{
		socketThreadQuit = false;
		socketThreadMutex = wzMutexCreate();
		socketThreadSemaphore = wzSemaphoreCreate(0);
		socketThread = wzThreadCreate(socketThreadFunction, nullptr);
		wzThreadStart(socketThread);
	}
}

void SOCKETshutdown()
{
	if (socketThread != nullptr)
	{
		wzMutexLock(socketThreadMutex);
		socketThreadQuit = true;
		socketThreadWrites.clear();
		wzMutexUnlock(socketThreadMutex);
		wzSemaphorePost(socketThreadSemaphore);  // Wake up the thread, so it can quit.
		wzThreadJoin(socketThread);
		wzMutexDestroy(socketThreadMutex);
		wzSemaphoreDestroy(socketThreadSemaphore);
		socketThread = nullptr;
	}

#if defined(WZ_OS_WIN)
	WSACleanup();

	if (winsock2_dll)
	{
		FreeLibrary(winsock2_dll);
		winsock2_dll = NULL;
		getaddrinfo_dll_func = NULL;
		freeaddrinfo_dll_func = NULL;
	}
#endif
}

} // namespace tcp
