/*	$Id: InetFaxServer.c++ 1179 2013-07-31 16:54:48Z faxguy $ */
/*
 * Copyright (c) 1995-1996 Sam Leffler
 * Copyright (c) 1995-1996 Silicon Graphics, Inc.
 * HylaFAX is a trademark of Silicon Graphics
 *
 * Permission to use, copy, modify, distribute, and sell this software and 
 * its documentation for any purpose is hereby granted without fee, provided
 * that (i) the above copyright notices and this permission notice appear in
 * all copies of the software and related documentation, and (ii) the names of
 * Sam Leffler and Silicon Graphics may not be used in any advertising or
 * publicity relating to the software without the specific, prior written
 * permission of Sam Leffler and Silicon Graphics.
 * 
 * THE SOFTWARE IS PROVIDED "AS-IS" AND WITHOUT WARRANTY OF ANY KIND, 
 * EXPRESS, IMPLIED OR OTHERWISE, INCLUDING WITHOUT LIMITATION, ANY 
 * WARRANTY OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.  
 * 
 * IN NO EVENT SHALL SAM LEFFLER OR SILICON GRAPHICS BE LIABLE FOR
 * ANY SPECIAL, INCIDENTAL, INDIRECT OR CONSEQUENTIAL DAMAGES OF ANY KIND,
 * OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 * WHETHER OR NOT ADVISED OF THE POSSIBILITY OF DAMAGE, AND ON ANY THEORY OF 
 * LIABILITY, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE 
 * OF THIS SOFTWARE.
 */
#include "port.h"
#include "Dispatcher.h"
#include "InetFaxServer.h"
#include "Sys.h"
#include "Socket.h"
#include "config.h"

#include <string.h>
#include <netdb.h>
#include <ctype.h>

extern "C" {
#include <arpa/inet.h>

#include <netinet/in_systm.h>
#include <netinet/ip.h>
}

InetSuperServer::InetSuperServer(const char* p, int bl)
    : SuperServer("INET", bl)
    , port(p)
{
    bindaddress = NULL;
    addressfamily = NULL;
    pasv_min_port = 0;
    pasv_max_port = 0;
}

InetSuperServer::~InetSuperServer() {}

void
InetSuperServer::setBindAddress(const char *bindaddr)
{
    bindaddress = bindaddr;
}

void
InetSuperServer::setAddressFamily(const char *addrfamily)
{
    addressfamily = addrfamily;
}

void
InetSuperServer::setPasvPortRange(const char *optarg)
{
    int a = 0, b = 0;

    if (!optarg || !optarg[0]) {
	pasv_min_port = pasv_max_port = 0;
	return;
    }

    if (2 != sscanf(optarg, "%u:%u", &a, &b)) {
	logError("Failed to parse PASV port range from '%s'.", optarg);
	exit(-1);
    }

// PORT 0 has special meaning to us (ie, do NOT restrict range).
// Unix kernels will not allow a user-mode process to bind TCP port 0 anyway.
    if ((a < 1) || (a > 65535) || (b < 1) || (b > 65535)) {
	logError("Invalid PASV port range '%s', ports must be between 1 and 65535, inclusive.", optarg);
	exit(-1);
    }

    if ((a > b) || (b - a < PASV_PORT_RANGE_MIN_COUNT)) {
	logError("Invalid PASV port range size (%d) (not enough ports in range, sanity check).", b - a);
	exit(-1);
    }

    pasv_min_port = a;
    pasv_max_port = b;

    assert (pasv_min_port < pasv_max_port);
    assert (pasv_min_port > 0);
    assert (pasv_min_port < 65535);
    assert (pasv_max_port > 0);
    assert (pasv_max_port < 65535);
    assert (pasv_max_port - pasv_min_port >= PASV_PORT_RANGE_MIN_COUNT);

    logInfo("Set PASV mode TCP port range to %d:%d", pasv_min_port, pasv_max_port);
}

bool
InetSuperServer::startServer(void)
{
    Socket::Address addr;
    struct addrinfo hints, *ai;
    memset(&hints, 0, sizeof(hints));

    /*
     * Most BSDs seem to have some complications when using IPv6.  Since
     * most users will not be using IPv6 the default is then IPv4-only.
     */
    hints.ai_family = AF_INET;		// default
    if (addressfamily) {
	if (strncmp(addressfamily, "IPv6", 4) == 0) hints.ai_family = AF_INET6;
	else if (strncmp(addressfamily, "IPv4", 4) == 0) hints.ai_family = AF_INET;
	else if (strncmp(addressfamily, "all", 3) == 0) hints.ai_family = AF_UNSPEC;
    }

    hints.ai_flags = AI_PASSIVE;
#ifdef AI_ADDRCONFIG
    hints.ai_flags |= AI_ADDRCONFIG;
#endif
    hints.ai_socktype = SOCK_STREAM;

    struct protoent* pp = getprotobyname(FAX_PROTONAME);
    if (pp)
	hints.ai_protocol = pp->p_proto;

    if (getaddrinfo(bindaddress, port, &hints, &ai) == 0)  {
	memcpy(&addr, ai->ai_addr, ai->ai_addrlen);
	freeaddrinfo(ai);
    } else {
	logDebug("Couldn't get address information for port \"%s\"",
		(const char*) port);
	/*
	 * Somethings broken here, let's pick some conservative defaults
	 */
	memset(&addr, 0, sizeof(addr));
	Socket::family(addr) = AF_INET;
	addr.in.sin_port = htons(FAX_DEFPORT);
    }

    int s = socket(Socket::family(addr), SOCK_STREAM, 0);
    if (s >= 0) {
	int on = 1;
	if (Socket::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) >= 0) {
	    if (Socket::bind(s, &addr, Socket::socklen(addr)) >= 0) {
		(void) listen(s, getBacklog());
		Dispatcher::instance().link(s, Dispatcher::ReadMask, this);
		char hostbuf[128];
		const char* a = inet_ntop(Socket::family(addr), Socket::addr(addr), hostbuf, sizeof(hostbuf));
		switch (Socket::family(addr))
		{
		    case AF_INET:
			logInfo("Listening to %s:%d", a ? a : "", ntohs(Socket::port(addr)));
			break;
		    case AF_INET6:
			logInfo("Listening to [%s]:%d", a ? a : "", ntohs(Socket::port(addr)));
			break;
		}
		return (true);				// success
	    }
	}
	Sys::close(s);
	logError("HylaFAX %s: bind (port %u): %m",
	    getKind(), ntohs(Socket::port(addr)));
    } else
	logError("HylaFAX %s: socket: %m", getKind());
    return (false);
}

HylaFAXServer* InetSuperServer::newChild(void)
{
    InetFaxServer *pServer = new InetFaxServer;
    pServer->setPasvPortRange (pasv_min_port, pasv_max_port);
    return pServer;
}

InetFaxServer* InetFaxServer::_instance = NULL;

InetFaxServer::InetFaxServer()
{
    usedefault = true;
    debug = false;
    swaitmax = 90;			// wait at most 90 seconds
    swaitint = 5;			// interval between retries
    pasv_min_port = 0;
    pasv_max_port = 0;

    memset(&data_dest, 0, sizeof (data_dest));
    Socket::family(data_dest) = AF_INET6;	// We'll allow AF_INET6 by default.

    hostent* hp = Socket::gethostbyname(hostname);

    if (hp != NULL)
    {
	hostname = hp->h_name;
        struct in_addr in;
        memcpy(&in, hp->h_addr, sizeof (in));
        hostaddr = inet_ntoa(in);
    }

    fxAssert(_instance == NULL,
	"Cannot create multiple InetFaxServer instances");
    _instance = this;
}
InetFaxServer::~InetFaxServer() {}

InetFaxServer& InetFaxServer::instance() { return *_instance; }

void
InetFaxServer::initServer(void)
{
    HylaFAXServer::initServer();
    usedefault = true;
}

void
InetFaxServer::open(void)
{
    setupNetwork(STDIN_FILENO);
    HylaFAXServer::open();
    if (TRACE(CONNECT))
        logInfo("HylaFAX INET connection from %s [%s]",
	    (const char*) remotehost, (const char*) remoteaddr);
}

static const char*
topDomain(const fxStr& h)
{
    int dots = 0;
    u_int l = h.length();

    if(l <= 0) return(NULL);  // out early

    for (;;) {
	l = h.nextR(l, '.');
	if (l == 0)
	    return (&h[0]);  // return whole string
	if (++dots == 2)
	    return (&h[l]);
	l -=1;    //back over the dot
    }
}

/*
 * Check whether host h is in our local domain,
 * defined as sharing the last two components of the domain part,
 * or the entire domain part if the local domain has only one component.
 * If either name is unqualified (contains no '.'),
 * assume that the host is local, as it will be
 * interpreted as such.
 */
bool
InetFaxServer::isLocalDomain(const fxStr& h)
{
    const char* p1 = topDomain(hostname);
    const char* p2 = topDomain(h);
    return (p1 == NULL || p2 == NULL || strcasecmp(p1, p2) == 0);
}

/*
 * Check host identity returned by getnameinfo to
 * weed out clients trying to spoof us (this is mostly
 * a sanity check; if they have full control of DNS
 * they can still spoof)
 * Look up the name and check that the peer's address
 * corresponds to the host name.
 */
bool
InetFaxServer::checkHostIdentity(const char*name)
{

    struct addrinfo hints, *ai;

    memset(&hints, 0, sizeof(hints));
    hints.ai_flags = AI_CANONNAME;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(name, NULL, &hints, &ai) == 0) {
	for (struct addrinfo *aip = ai; aip != NULL; aip = aip->ai_next) {
	    Socket::Address *addr = (Socket::Address*)aip->ai_addr;
	    if (aip->ai_family != Socket::family(peer_addr))
		continue;
	    if ( (aip->ai_family == AF_INET6 &&
		     memcmp(&addr->in6.sin6_addr, &peer_addr.in6.sin6_addr, sizeof(struct in6_addr)) ==0)
		 ||(aip->ai_family == AF_INET &&
		     memcmp(&addr->in.sin_addr, &peer_addr.in.sin_addr, sizeof(struct in_addr)) ==0)
	       ) {
		freeaddrinfo(ai);
		return true;
	    }
	}
	reply(130, "Warning, client address \"%s\" is not listed for host name \"%s\".",
	    (const char*) remoteaddr, name);
	freeaddrinfo(ai);
    } else
	reply(130, "Warning, no inverse address mapping for client host name \"%s\".",
	    (const char*) name);
    return (false);
}

void
InetFaxServer::setupNetwork(int fd)
{
    socklen_t addrlen;

    addrlen = sizeof (peer_addr);
    if (Socket::getpeername(fd, &peer_addr, &addrlen) < 0) {
        logError("getpeername: %m (incorrect hfaxd invocation?)");
        dologout(-1);
    }
    addrlen = sizeof (ctrl_addr);
    if (Socket::getsockname(fd, &ctrl_addr, &addrlen) < 0) {
        logError("getsockname: %m");
        dologout(-1);
    }
#if defined(IPTOS_LOWDELAY)
    { int tos = IPTOS_LOWDELAY;
      if (Socket::setsockopt(fd, IPPROTO_IP, IP_TOS, &tos, sizeof (tos)) < 0)
          logWarning("setsockopt (IP_TOS): %m");
    }
#endif
#if defined(SO_LINGER) && !defined(__linux__)
    { struct linger opt;
      opt.l_onoff = 1;
      opt.l_linger = 60;
      if (Socket::setsockopt(fd, SOL_SOCKET, SO_LINGER, &opt, sizeof (opt)) < 0)
	logWarning("setsockopt (SO_LINGER): %m");
    }
#endif
    /* anchor socket to avoid multi-homing problems */
    data_source = ctrl_addr;
    Socket::port(data_source) = htons(ntohs(Socket::port(ctrl_addr))-1);
#ifdef  F_SETOWN
    if (fcntl(fd, F_SETOWN, getpid()) == -1)
        logError("fcntl (F_SETOWN): %m");
#endif
#if defined(SO_OOBINLINE)
    /* handle urgent data inline */
    { int on = 1;
      if (Socket::setsockopt(fd, SOL_SOCKET, SO_OOBINLINE, &on, sizeof (on)) < 0)
        logError("setsockopt (SO_OOBINLINE): %m");
    }
#endif
#ifdef SIGURG
    /*
     * XXX This is #ifdef'd for ISC but indicates a problem
     * XXX in the underlying system.  Without support for
     * XXX urgent data clients will not be able to abort an
     * XXX ongoing operation such as a file transfer.
     */
    signal(SIGURG, fxSIGHANDLER(sigURG));
#endif
    signal(SIGPIPE, fxSIGHANDLER(sigPIPE));

    char hostbuf[128];

    remoteaddr = inet_ntop(Socket::family(peer_addr), Socket::addr(peer_addr), hostbuf, sizeof(hostbuf));

    getnameinfo((struct sockaddr*)&peer_addr, Socket::socklen(peer_addr),
		    hostbuf, sizeof(hostbuf), NULL, 0, 0);

    if (remoteaddr == "0.0.0.0")
        remotehost = "localhost";
    else if (checkHostIdentity(hostbuf))
	remotehost = hostbuf;
    else
	remotehost =  remoteaddr;
    Dispatcher::instance().link(STDIN_FILENO, Dispatcher::ReadMask, this);
}

void
InetFaxServer::handleUrgentData(void)
{
    /*
     * Only process the urgent data is we are expecting it.
     * Specifically, if a file transfer, synchronous job
     * wait, or other potentially long-running operation is
     * taking place we do a setjmp and set a flag so that
     * if the client requests an abort operation (ABOR) then
     * we longjmp below to break out of the op.
     */
    if ((state & (S_TRANSFER|S_WAITTRIG)) == 0) {
	if (IS(WAITFIFO)) {
	    fifoResponse = "User aborted operation";
	    state &= ~S_WAITFIFO;
	}
        return;
    }
    char line[8];
    if (!getCmdLine(line, sizeof (line))) {
        reply(221, "You could at least say goodbye.");
        dologout(0);
    } else if (strcasecmp(line, "ABOR\n") == 0) {
	/*
	 * Abort the operation in progress.  Two reply
	 * codes are sent on the control channel; one
	 * for the outstanding operation and one for the
	 * abort command.
	 */
        reply(426, "Operation aborted. Data connection closed.");
        reply(226, "Abort successful");
        longjmp(urgcatch, 1);		// XXX potential memory leak
    } else if (strcasecmp(line, "STAT\n") == 0) {
        if (file_size != (off_t) -1)
            reply(213, "Status: %lu of %lu bytes transferred",
                  byte_count, file_size);
        else
            reply(213, "Status: %lu bytes transferred", byte_count);
    } else
	pushCmdData(line, strlen(line));
}

void
InetFaxServer::sigURG(int)
{
    int old_errno = errno;
    InetFaxServer::instance().handleUrgentData();
    errno = old_errno;
}

void
InetFaxServer::lostConnection(void)
{
    if (TRACE(PROTOCOL))
        logDebug("Lost connection to %s [%s]",
	    (const char*) remotehost, (const char*) remoteaddr);
    dologout(-1);
}

void
InetFaxServer::sigPIPE(int)
{
    int old_errno = errno;
    InetFaxServer::instance().lostConnection();
    errno = old_errno;
}

/*
 * InetFaxServer::setupPassiveDataSocket() will:
 * 1) Allocate a socket (of the correct address family),
 * 2) bind the 'this->pasv_addr' socket,
 * 3) refresh its sockname (to learn bound port number)
 * 4) enter the listen mode.
 * On input, 'addr' should have a family, address and port pre-configured.
 * On success, the function returns the socket's file descriptor.
 * On error, returns '-1'.
 */
int
InetFaxServer::setupPassiveDataSocket(Socket::Address &addr)
{
    socklen_t len = Socket::socklen(addr);
    int fd = -1;

    if (0 > (fd = socket(Socket::family(addr), SOCK_STREAM, 0))) {
	if (debug) logDebug("setupPassiveDataSocket: 'socket()' failed with error %d", errno);
	return -1;
    }

// If "pasv_min_port" is zero, then use the original logic (though hylafaxplus v5.5.3),
// and allow the kernel to allocate an ephemeral port.
    if (pasv_min_port == 0) {
	if (0 > Socket::bind(fd, (struct sockaddr*)&addr, len)) {
	    if (debug) logDebug("setupPassiveDataSocket: 'bind()' failed with error %d", errno);
	    (void)Sys::close(fd);
	    return -1;
	}
    } else {

// New logic, we want to select a local port ($PORT) in range:
//  pasv_min_port <= $PORT <= pasv_max_port.

// Optimization:
// Consider if "hfaxd" is communicating with lots of clients.  It is likely that
// many of them will use a passive data socket concurrently.  If "hfaxd" simply
// starts binding ports at 'pasv_min_port', then we will have an O(n^2) failed binds
// for the 'n' active client.  So, we'll treat our port range as a ring, add a "bias"
// to it, and hope that, statisically, we hit an open port.
//
// NOTE: At first I considered storing the "last used port" in a global, but that won't work
// because "hfaxd" will fork upon client connection.
//
// If we assume that the only software binding ports in hfaxd's range on the local system is "hfaxd",
// Then we can simply use hfaxd's PID as a reasonable value for "bias".
// However, doing this will leak some info about hfaxd's PID to the remote client.
// Is this a security problem?  I have no idea right now.

	int count = (pasv_max_port - pasv_min_port) + 1;
	int bias = getpid();
	for (int offset = 0; offset < count; offset++) {
	    int port = pasv_min_port + (offset + bias) % count;
	    if (debug) logDebug("setupPassiveDataSocket: attempting to bind port %d", port);

	    Socket::port(addr) = htons(port);
	    if (0 <= Socket::bind(fd, (struct sockaddr*)&addr, len)) {
		goto i_haz_a_port_bound;
	    }

	    if (errno != EADDRINUSE) {
		if (debug) logDebug("setupPassiveDataSocket: 'bind()' failed with error %d", errno);
		(void)Sys::close(fd);
		return -1;
	    }
	}

	if (debug) logDebug("setupPassiveDataSocket: 'bind()' all ports between %d and %d are in use",
		pasv_min_port, pasv_max_port);
	(void)Sys::close(fd);
	return -1;
    }

i_haz_a_port_bound:
    if (0 > Socket::getsockname(fd, (struct sockaddr*)&addr, &len)) {
	if (debug) logDebug("setupPassiveDataSocket: 'getsockname' failed with error %d", errno);
	(void)Sys::close(fd);
	return -1;
    }

    if (0 > Socket::listen(fd, 1)) {
	if (debug) logDebug("setupPassiveDataSocket: 'listen' failed with error %d", errno);
	(void)Sys::close(fd);
	return -1;
    }

    return fd;
}

#define UC(b) (((int) b) & 0xff)

/*
 * Note: a response of 425 is not mentioned as a possible response to the
 * PASV command in RFC959. However, it has been blessed as a legitimate
 * response by Jon Postel in a telephone conversation with Rick Adams on 25
 * Jan 89.
 */

/*
 * InetFaxServer::passiveCmd() is called when the fax client sends the 'PASV'
 * or 'EPSV' command to the hfaxd server over the existing control channel.
 * This function implements RFC 2428 (http://tools.ietf.org/html/rfc2428).
 * A properly behaving client will not send 'PASV' or 'EPSV' mutliple times.
 */
void
InetFaxServer::passiveCmd(void)
{
    bool extended_mode = (tokenBody[0] == 'E');		// For brevity.

// Sanity checking
    if (pdata != -1) {
       reply(500, "PASV/EPSV connection already exists");
       return;
       // Maybe we should close the socket instead, and open a new one?
    }

// Sanity checking (MUST use 'EPSV' for IPV6)
    if (!extended_mode && (Socket::family(ctrl_addr) != AF_INET)) {
	reply(500, "Cannot use PASV with IPv6 connections");
	return;
    }

    if (debug) logDebug("Opening socket for '%s' mode for address family %d",
	extended_mode ? "EPSV" : "PASV",
	Socket::family(pasv_addr));

// Create socket (common to both modes).
// NOTE: setupPassiveDataSocket() will choose the proper address family.
    pasv_addr = ctrl_addr;
    Socket::port(pasv_addr) = 0;
    pdata = setupPassiveDataSocket(pasv_addr);
    if (pdata < 0) {
	perror_reply(425, "Cannot setup extended passive connection", errno);
	return;
    }

// Send response via control channel, per RFC 2428.
// NOTE: The affirmative response codes are DIFFERENT between EPSV and PASV.
    if (extended_mode) {
	    reply(229, "Entering Extended Passive Mode (|||%u|)",
		    ntohs(Socket::port(pasv_addr)));
    } else {
	const u_char* a = (const u_char*) &pasv_addr.in.sin_addr;
	const u_char* p = (const u_char*) &pasv_addr.in.sin_port;
	reply(227, "Entering passive mode (%d,%d,%d,%d,%d,%d)", UC(a[0]),
	      UC(a[1]), UC(a[2]), UC(a[3]), UC(p[0]), UC(p[1]));
    }
}

void
InetFaxServer::printaddr(FILE* fd, const char* leader, const struct sockaddr_in& sin)
{
    const u_char* a = (const u_char *) &sin.sin_addr;
    const u_char* p = (const u_char *) &sin.sin_port;
    fprintf(fd, "%s (%d,%d,%d,%d,%d,%d)\r\n", leader,
       UC(a[0]), UC(a[1]), UC(a[2]), UC(a[3]), UC(p[0]), UC(p[1]));
}

void
InetFaxServer::netStatus(FILE* fd)
{
    if (data != -1)
        fprintf(fd, "    Client data connection open\r\n");
    else if (pdata != -1)
        printaddr(fd, "    In passive mode", pasv_addr);
    else if (!usedefault)
        printaddr(fd, "    PORT", data_dest);
    else
        fprintf(fd, "    No client data connection\r\n");
}

/*
 * Creat a socket for a data transfer.
 */
FILE*
InetFaxServer::getDataSocket(const char* mode)
{
    if (data >= 0)
        return (fdopen(data, mode));
    int s = socket(Socket::family(data_dest), SOCK_STREAM, 0);
    if (s < 0)
        goto bad;
    { int on = 1;
      if (Socket::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0)
        goto bad;
    }
    { int ntry;						// XXX for __GNUC__
      for (ntry = 1;; ntry++) {
        if (Socket::bind(s, &data_source, Socket::socklen(data_source)) >= 0)
            break;
        if (errno != EADDRINUSE || ntry > 10)
            goto bad;
        sleep(ntry);
      }
    }
#ifdef IPTOS_THROUGHPUT
    { int on = IPTOS_THROUGHPUT;
      if (Socket::setsockopt(s, IPPROTO_IP, IP_TOS, &on, sizeof (on)) < 0)
          logWarning("setsockopt (IP_TOS): %m");
    }
#endif
    return (fdopen(s, mode));
bad:
    (void) Sys::close(s);
    return (NULL);
}

bool
InetFaxServer::dataConnect(void)
{
    return Socket::connect(data, &data_dest, Socket::socklen(data_dest)) >= 0;
}

/*
 * Establish a data connection for a file transfer operation.
 */
FILE*
InetFaxServer::openDataConn(const char* mode, int& code)
{
    byte_count = 0;
    if (pdata >= 0) {			// passive mode, wait for connection
        struct sockaddr_in from;
        socklen_t fromlen = sizeof (from);
	// XXX don't block, select....????
        int s = Socket::accept(pdata, (struct sockaddr*) &from, &fromlen);
        if (s < 0) {
            reply(425, "Cannot open data connection.");
            (void) Sys::close(pdata);
            pdata = -1;
            return (NULL);
        }
	// XXX verify peer address???
        (void) Sys::close(pdata);
        pdata = s;
#ifdef IPTOS_LOWDELAY
        int tos = IPTOS_LOWDELAY;
        (void) Socket::setsockopt(s, IPPROTO_IP, IP_TOS, &tos, sizeof (tos));

#endif
	code = 150;
        return (fdopen(pdata, mode));
    }
    if (data >= 0) {
	code = 125;
        usedefault = 1;
        return (fdopen(data, mode));
    }
    if (usedefault)
        data_dest = peer_addr;
    usedefault = 1;
    FILE* file = getDataSocket(mode);
    if (file == NULL) {
        reply(425, "Cannot create data socket (%s,%d): %s.",
              inet_ntoa(data_source.in.sin_addr),
              ntohs(data_source.in.sin_port), strerror(errno));
	return (NULL);
    }
    data = fileno(file);
    for (int swait = 0; !dataConnect(); swait += swaitint) {
	if (!(errno == EADDRINUSE || errno == EINTR) || swait >= swaitmax) {
	    perror_reply(425, "Cannot build data connection", errno);
	    fclose(file);
	    data = -1;
	    return (NULL);
	}
	sleep(swaitint);
    }
    code = 150;
    return (file);
}

bool
InetFaxServer::hostPort()
{
    if (debug) logDebug("Parsing hostPort(): \"%s\"", (const char*)tokenBody);

    if (tokenBody[0] == 'E')
    {
	fxStr s;
	if (! STRING(s))
	{
	    logDebug("Couldn't get string: \"%s\"", (const char*)tokenBody);
	    syntaxError("EPRT |family|address|port|");
	    return false;
	}
	if (debug) logDebug("Parsing \"%s\"", (const char*)s);
	/*
	 * Minimual length for EPRT is: 9
	 *       |X|X::|X|
	 */
	char c = s[0];
	if (debug) {
	    logDebug(" `-> s.length() = %d", s.length());
	    logDebug(" `-> s[0] = '%c'", s[0]);
	    logDebug(" `-> s[2] = '%c'", s[2]);
	    logDebug(" `-> s[%d] = '%c'", s.length()-1, s[s.length()-1]);
	}
	if (s.length() > 9
		&& c == s[0] && (s[1] == '1' || s[1] == '2') && c == s[2]
		&& c == s[s.length()-1]) {
	    if (debug) logDebug("Looks like extended syntax: \"%s\" [%X: %c]", (const char*)s, c&0xFF, c);

	    u_int pos = 3;
	    fxStr a = s.token(pos, c);
	    if (debug) logDebug("`-> Got a: %s[%u]", (const char*)a, pos);
	    fxStr p = s.token(pos, c);
	    if (debug) logDebug("`-> Got a: %s[%u]", (const char*)p, pos);
	    if (pos != s.length() )
	    {
		logDebug("Parsing EPRT style failed");
		syntaxError("EPRT |family|address|port|");
		return false;
	    }

	    if (debug) logDebug("Parsed: Family %c Address %s Port %s", s[1], (const char*)a, (const char*)p);
	    struct addrinfo hints, *ai;

	    memset(&hints, 0, sizeof(hints));
#ifdef AI_NUMERICHOST
	    hints.ai_flags |= AI_NUMERICHOST;
#endif
#ifdef AI_NUMERICSERV
	    hints.ai_flags |= AI_NUMERICSERV;
#endif
	    hints.ai_socktype = SOCK_STREAM;
	    switch (s[1])
	    {
		case '1':
		    hints.ai_family = AF_INET;
		    break;
		case '2':
		    hints.ai_family = AF_INET6;
		    break;
		default:
		    reply(500, "EPRT: Invalid family \"%c\"", s[1]);
	    }
	    if (getaddrinfo(a, p, &hints, &ai) != 0) {
		    reply(500, "EPRT couldn't parse family \"%c\" address \"%s\" port \"%s\"",
				s[1], (const char*)a, (const char*)p);
		    return false;
	    }

	    memcpy(&data_dest, ai->ai_addr, ai->ai_addrlen);
	    freeaddrinfo(ai);
	    return true;
	}
	logDebug("Couldn't parse \"%s\"", (const char*)s);
	syntaxError("Couldn't parse extended port");
	return false;
    }

    long a0, a1, a2, a3;
    long p0, p1;
    bool syntaxOK = 
		      NUMBER(a0)
	&& COMMA() && NUMBER(a1)
	&& COMMA() && NUMBER(a2)
	&& COMMA() && NUMBER(a3)
	&& COMMA() && NUMBER(p0)
	&& COMMA() && NUMBER(p1)
	;
    if (syntaxOK) {
	data_dest.in.sin_family = AF_INET;
	u_char* a = (u_char*) &data_dest.in.sin_addr;
	u_char* p = (u_char*) &data_dest.in.sin_port;
	a[0] = UC(a0); a[1] = UC(a1); a[2] = UC(a2); a[3] = UC(a3);
	p[0] = UC(p0); p[1] = UC(p1);
	return (true);
    }

    return false;
}

void
InetFaxServer::portCmd(Token t)
{
    if (t == T_EPRT)
	logcmd(T_EPRT, "|%d|%s|%u|", 1, inet_ntoa(data_dest.in.sin_addr), ntohs(data_dest.in.sin_port));
    else
	logcmd(T_PORT, "%s;%u", inet_ntoa(data_dest.in.sin_addr), ntohs(data_dest.in.sin_port));
    usedefault = false;
    if (pdata >= 0)
       (void) Sys::close(pdata), pdata = -1;

    if (t == T_EPRT)
	ack(200, cmdToken(T_EPRT));
    else
	ack(200, cmdToken(T_PORT));
}
