/*
 * Although highly modified and altered, the code in this file was originally
 * derived from sources taken from (1) GitHub user, "mrwicks", on 9 Oct 2018.
 * That source, itself, was derived from work by "Amlendra" published at
 * Aticleworld on 21 May 2017 (2).  That work, then, references programs (3)
 * Copyright (c) 2000 Sean Walton and Macmillan Publishers (The "Linux Socket
 * Programming" book) and are licensed under the GPL.
 *
 * 1. https://github.com/mrwicks/miscellaneous/tree/master/tls_1.2_example
 * 2. https://aticleworld.com/ssl-server-client-using-openssl-in-c/
 * 3. http://www.cs.utah.edu/~swalton/listings/sockets/programs/
 *
 * It is, therefore, presumed that this work is either under the public
 * domain or is licensed under the GPL.  A copy of the GPL is as follows...
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "config.h"
#if defined(HAVE_SSL)

#include <sys/time.h>
#include "sslfax.h"
#include "Sys.h"
#include "Socket.h"

timeval currentTime() {
    timeval curTime;
    gettimeofday(&curTime, 0);
    return curTime;
}

const long ONE_SECOND = 1000000;

extern timeval operator-(timeval src1, timeval src2);

SSL_CTX* InitServerCTX (void)
{
    const SSL_METHOD *method;
    SSL_CTX *ctx;

    OpenSSL_add_all_algorithms ();     /* load & register all cryptos, etc. */
    SSL_load_error_strings ();         /* load all error messages */
#ifdef HAVE_FLEXSSL
    method = TLS_server_method (); /* create new server-method instance */
#else
    method = TLSv1_2_server_method (); /* create new server-method instance */
#endif
    ctx = SSL_CTX_new (method);        /* create new context from method */
    return ctx;
}

fxStr LoadCertificates (SSL_CTX* ctx, const char* CertFile, const char* KeyFile)
{
    /* set the local certificate from CertFile */
    if (SSL_CTX_use_certificate_file (ctx, CertFile, SSL_FILETYPE_PEM) <= 0) {
	return (fxStr::format("There was a problem with the certificate file \"%s\".", (const char*) CertFile));
    }

    /* set the private key from KeyFile (may be the same as CertFile) */
    if (SSL_CTX_use_PrivateKey_file (ctx, KeyFile, SSL_FILETYPE_PEM) <= 0) {
	return (fxStr::format("There was a problem with the private key in the certificate file \"%s\".", (const char*) KeyFile));
    }

    /* verify private key */
    if (!SSL_CTX_check_private_key (ctx)) {
	return (fxStr("Private key does not match the public certificate."));
    }
    return(fxStr(""));
}

// Create the SSL socket and intialize the socket address structure
int OpenListener (int port, fxStr& emsg)
{
    int sd;
    struct sockaddr_in addr;

    sd = socket (PF_INET, SOCK_STREAM, 0);
    if (sd <= 1) {
	emsg = fxStr::format("Can't open socket: %s", strerror(errno));
	return 0;
    }

    bzero (&addr, sizeof (addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons (port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind (sd, (struct sockaddr*)&addr, sizeof (addr)) != 0) {
	emsg = fxStr::format("Can't bind port %d: %s", port, strerror(errno));
	return 0;
    }
    /* Use of INT_MAX here allows for the limit to be set by /proc/sys/net/core/somaxconn.
       Proxy servers are going to want this high.  For individual SSL Fax connections this
       may be unnecessarily high, but it's probably harmless, still. */
    if (listen (sd, INT_MAX) != 0) {
	emsg = fxStr::format("Can't configure listening port %d.", port);
	return 0;
    }
    return sd;
}

fxStr ShowCerts (SSL* ssl)
{
    fxStr msg;
    X509 *cert;
    cert = SSL_get_peer_certificate (ssl); /* get the server's certificate */
    if (cert != NULL) {
	msg = fxStr::format("Server certificates: Subject: \"%s\", Issuer: \"%s\"",
		X509_NAME_oneline (X509_get_subject_name (cert), 0, 0),
		X509_NAME_oneline (X509_get_issuer_name (cert), 0, 0));
	X509_free (cert);  /* free the malloc'ed certificate copy */
    } else {
	msg = "Info: No client certificates configured.";
    }
    return msg;
}

int OpenConnection(const char *hostname, uint16_t port, timeval start, long ms, fxStr& emsg)
{
    int sd;
    struct hostent *host;
    struct sockaddr_in addr;

    if ((host = gethostbyname(hostname)) == NULL) {
	emsg = fxStr::format("Problem with resolving host \"%s\".", hostname);
	return 0;
    }
    sd = socket(PF_INET, SOCK_STREAM, 0);
    if (sd <= 1) {
	emsg = fxStr::format("Can't open socket: %s", strerror(errno));
	return 0;
    }

    if (fcntl(sd, F_SETFL, fcntl(sd, F_GETFL, 0) | O_NONBLOCK) == -1) {
	emsg = "Unable to set SSL Fax socket to non-blocking.";
	return 0;
    }
    bzero (&addr, sizeof (addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons (port);
    addr.sin_addr.s_addr = * (long*) (host->h_addr);

    if (connect(sd, (struct sockaddr*)&addr, sizeof (addr)) != 0) {
	if (errno == EINPROGRESS) {
	    /* Now we wait 3 seconds client to finish the connect. */
	    fd_set sfd;
	    FD_ZERO(&sfd);
	    FD_SET(sd, &sfd);
	    struct timeval tv;
	    tv.tv_sec = (int) ms / 1000;
	    tv.tv_usec = (ms % 1000)*1000;
	    tv = tv - (currentTime() - start);
#if CONFIG_BADSELECTPROTO
	    if (!select(sd+1, NULL, (int*) &sfd, NULL, &tv)) {
#else
	    if (!select(sd+1, NULL, &sfd, NULL, &tv)) {
#endif
		close (sd);
		emsg = "Timeout waiting for SSL Fax connect completion.";
		return 0;
	    } else {
		int code;
		socklen_t codelen = sizeof(code);
		if (!getsockopt(sd, SOL_SOCKET, SO_ERROR, &code, &codelen)) {
		    if (!code) {
			// connect completed
			return sd;
		    } else {
			emsg = fxStr::format("SSL Fax connection failed.  Error: %s", strerror(code));
			close(sd);
			return 0;
		    }
		} else {
		    close(sd);
		    emsg = "Unable to query the SSL Fax connection status.";
		    return 0;
		}
	    }
	}
	emsg = fxStr::format("Unable to connect to SSL Fax receiver \"%s\" at port %d (%s)", hostname, port, strerror(errno));
	close (sd);
	return 0;
    }
    return sd;
}

SSL_CTX* InitCTX (void)
{
    const SSL_METHOD *method;
    SSL_CTX *ctx;

    OpenSSL_add_all_algorithms ();     /* Load cryptos, et.al. */
    SSL_load_error_strings ();         /* Bring in and register error messages */
#ifdef HAVE_FLEXSSL
    method = TLS_client_method (); /* Create new client-method instance */
#else
    method = TLSv1_2_client_method (); /* Create new client-method instance */
#endif
    ctx = SSL_CTX_new (method);        /* Create new context */
    return ctx;
}

char *ssl_err_string (void)
{
    BIO *bio = BIO_new (BIO_s_mem ());
    ERR_print_errors (bio);
    char *buf = NULL;
    size_t len = BIO_get_mem_data (bio, &buf);
    char *ret = (char *) calloc (1, 1 + len);
    if (ret) memcpy (ret, buf, len);
    BIO_free (bio);
    return ret;
}

/*
 * About serverAddress and clientAddress...
 *
 * A port number for the client is meaningless.  However, for the
 * server the port number is essential.  IP address notation with
 * port numbers is substantially different between IPv4 and IPv6.
 * An IPv4 address with port will be given as "192.168.0.1:8081".
 * However, that same address with IPv6 will be (per RFC3986)
 * "[0:0:0:0:0:ffff:c0a8:1]:8081".
 *
 * We'll therefore necessarily distinguish between IPv6 and IPv4
 * in a serverAddress by the presence of brackets.  However, the
 * client address may not have brackets at all and must, therefore,
 * be understood to be IPv4 or IPv6 through use of inet_pton.
 */
int SSLFax::getAddressFamily(fxStr& address)
{
    char buf[16];
    u_int pos1 = address.next(0, '[');
    if (pos1 < address.length()) {
	// This looks like a bracketed IPv6 address (likely followed by the port number).
	u_int pos2 = address.next(pos1, ']');
	if (pos2 < address.length()) {
	    address.remove(pos2, address.length()-pos2);
	    if (address.length() > pos1) address.remove(0, pos1+1);
	}
    } else {
	pos1 = address.next(0, ':');
	if (address.length() > pos1) {
	    u_int pos2 = address.next(pos1+1, ':');
	    if (pos2 == address.length()) {
		// This looks like an IPv4 address with the port number specified.
		address.remove(pos1, address.length()-pos1);
	    }
	}
    }
    if (inet_pton(AF_INET, (const char*) address, buf)) {
	return AF_INET;
    } else if (inet_pton(AF_INET6, (const char*) address, buf)) {
	return AF_INET6;
    }
    return -1;
}

int SSLFax::pending(SSLFaxProcess& sfp)
{
    return (SSL_pending(sfp.ssl));
}

int SSLFax::read(SSLFaxProcess& sfp, void *buf, size_t count, int modemFd, long ms, bool sustain, bool carryon)
{
    /*
     * We cannot just use select() on the socket to see if there is data waiting
     * to be read because the SSL encryption and decryption operates somewhat
     * independently of the socket activity. Likewise SSL_pending() will not
     * help us here as it only tells us about any data already in the buffer.
     * There really is no way around just calling SSL_read() and letting it
     * work its magic.  That is why we have it set to non-blocking I/O and are
     * prepared to then use select() if it returns an error indicating
     * SSL_ERROR_WANT_READ or SSL_ERROR_WANT_WRITE.
     *
     * With non-blocking sockets, SSL_ERROR_WANT_READ means "wait for the socket
     * to be readable, then call this function again."; conversely,
     * SSL_ERROR_WANT_WRITE means "wait for the socket to be writeable, then
     * call this function again.".
     *
     * We do this same thing with SSL_connect() and SSL_accept(), also.
     *
     * In the event that we do turn to a select() then here we also monitor the
     * modem for activity, since that would indicate failure of the SSL Fax
     * communication.
     *
     * The special modemFd value of "0" tells us to not monitor the modem.
     * This is necessary because we can't select() a modem file descriptor if
     * it's at an EOF (it will always be readable).  The modem file descriptor
     * will be at an EOF if it is in command mode after an "OK" after a command
     * completed.  We can only select() it when we're waiting for a response.
     */


    int sslfd = sfp.client ? sfp.client : sfp.server;

    struct timeval start = currentTime();
    int cerror;
    int ret;
    do {
	cerror = 0;
	ret = SSL_read(sfp.ssl, buf, count);
	if (ret <= 0) {
	    cerror = SSL_get_error(sfp.ssl, ret);
	    if (cerror == SSL_ERROR_WANT_READ || cerror == SSL_ERROR_WANT_WRITE) {
		int selret;
		fd_set rfds;
		FD_ZERO(&rfds);
		if (modemFd) FD_SET(modemFd, &rfds);
		struct timeval tv;
		tv.tv_sec = (int) ms / 1000;
		tv.tv_usec = (ms % 1000)*1000;
		tv = tv - (currentTime() - start);
		if (cerror == SSL_ERROR_WANT_READ) {	// wait for the socket to be readable
		    FD_SET(sslfd, &rfds);
#if CONFIG_BADSELECTPROTO
		    selret = select((modemFd > sslfd) ? modemFd+1 : sslfd+1, (int*) &rfds, NULL, NULL, &tv);
#else
		    selret = select((modemFd > sslfd) ? modemFd+1 : sslfd+1, &rfds, NULL, NULL, &tv);
#endif
		} else {	// SSL_ERROR_WANT_WRITE, wait for the socket to be writable
		    fd_set wfds;
		    FD_ZERO(&wfds);
		    FD_SET(sslfd, &wfds);
#if CONFIG_BADSELECTPROTO
		    selret = select((modemFd > sslfd) ? modemFd+1 : sslfd+1, (int*) &rfds, (int*) &wfds, NULL, &tv);
#else
		    selret = select((modemFd > sslfd) ? modemFd+1 : sslfd+1, &rfds, &wfds, NULL, &tv);
#endif
		}
		if (!selret) {
		    sfp.emsg = fxStr::format("Timeout waiting for SSL Fax read (wanting to %s).", (cerror == SSL_ERROR_WANT_READ ? "read" : "write"));
		    cleanup(sfp, sustain);
		    return (0);
		} else if (selret < 0) {
		    sfp.emsg = fxStr::format("Error waiting for SSL Fax read (wanting to %s): %s", (cerror == SSL_ERROR_WANT_READ ? "read" : "write"), strerror(errno));
		    cleanup(sfp, sustain);
		    return (0);
		}
		if (modemFd && FD_ISSET(modemFd, &rfds)) {
		    // The modem got a signal.  This probably means that SSL Fax is not happening.
		    if (!carryon) {
			sfp.emsg = "Modem has data when waiting for SSL Fax read.  Terminating SSL Fax.";
			cleanup(sfp, sustain);
		    }
		    return (-1);
		}
	    }
	}
    } while (cerror == SSL_ERROR_WANT_READ || cerror == SSL_ERROR_WANT_WRITE);
    if (ret <= 0) {
	if (cerror == SSL_ERROR_SYSCALL) {
	    sfp.emsg = fxStr::format("Unable to read from SSL Fax connection (syscall).  Error %d: %s, Remote: %s", ret, strerror(ret), (const char*) sfp.addr);
	} else {
	    sfp.emsg = fxStr::format("Unable to read from SSL Fax connection.  Error %d: %s, Remote: %s", cerror, ssl_err_string(), (const char*) sfp.addr);
	}
	cleanup(sfp, sustain);
	return (-2);
    }
    return (ret);
}

int SSLFax::write(SSLFaxProcess& sfp, const u_char *buf, u_int count, const u_char* bitrev, int modemFd, long ms, bool filter, bool sustain)
{
    /*
     * Similar approach here as with read() above; however...
     *
     * Because SSL Fax doesn't use carrier loss as a signal it uses
     * <DLE><ETX> as an end-of-data signal.  Therefore, we're required
     * here to "filter" DLEs (by doubling them) except for the end-of-
     * data signal; the receiver will be required to "un-filter" them
     * (by removing doubles and watching for the end-of-data signal).
     * So, we process buf one byte at a time.
     */
    u_int pos;
    bool isDLE = false;
    struct timeval start = currentTime();
    int cerror;
    int ret = 0;
    int sslfd = sfp.client ? sfp.client : sfp.server;
    for (pos = 0; pos < count; pos++) {
	do {
	    cerror = 0;
	    ret = SSL_write(sfp.ssl, &bitrev[buf[pos]], 1);
	    if (ret <= 0) {
		cerror = SSL_get_error(sfp.ssl, ret);
		if (cerror == SSL_ERROR_WANT_READ || cerror == SSL_ERROR_WANT_WRITE) {
		    int selret;
		    fd_set rfds;
		    FD_ZERO(&rfds);
		    if (modemFd) FD_SET(modemFd, &rfds);
		    struct timeval tv;
		    tv.tv_sec = (int) ms / 1000;
		    tv.tv_usec = (ms % 1000)*1000;
		    tv = tv - (currentTime() - start);
		    if (cerror == SSL_ERROR_WANT_READ) {	// wait for the socket to be readable
			FD_SET(sslfd, &rfds);
#if CONFIG_BADSELECTPROTO
			selret = select((modemFd > sslfd) ? modemFd+1 : sslfd+1, (int*) &rfds, NULL, NULL, &tv);
#else
			selret = select((modemFd > sslfd) ? modemFd+1 : sslfd+1, &rfds, NULL, NULL, &tv);
#endif
		    } else {	// SSL_ERROR_WANT_WRITE, wait for the socket to be writable
			fd_set wfds;
			FD_ZERO(&wfds);
			FD_SET(sslfd, &wfds);
#if CONFIG_BADSELECTPROTO
			selret = select((modemFd > sslfd) ? modemFd+1 : sslfd+1, (int*) &rfds, (int*) &wfds, NULL, &tv);
#else
			selret = select((modemFd > sslfd) ? modemFd+1 : sslfd+1, &rfds, &wfds, NULL, &tv);
#endif
		    }
		    if (!selret) {
			sfp.emsg = fxStr::format("Timeout waiting for SSL Fax write (wanting to %s).", (cerror == SSL_ERROR_WANT_READ ? "read" : "write"));
			cleanup(sfp, sustain);
			return (0);
		    } else if (selret < 0) {
			sfp.emsg = fxStr::format("Error waiting for SSL Fax write (wanting to %s): %s", (cerror == SSL_ERROR_WANT_READ ? "read" : "write"), strerror(errno));
			cleanup(sfp, sustain);
			return (0);
		    }
		    if (modemFd && FD_ISSET(modemFd, &rfds)) {
			// The modem got a signal.  This probably means that SSL Fax is not happening.
			sfp.emsg = "Modem has data when waiting for SSL Fax write.  Terminating SSL Fax.";
			cleanup(sfp, sustain);
			return (-1);
		    }
		}
	    }
	} while (cerror == SSL_ERROR_WANT_READ || cerror == SSL_ERROR_WANT_WRITE);
	if (ret <= 0) {
	    if (cerror == SSL_ERROR_SYSCALL) {
		sfp.emsg = fxStr::format("Unable to write to SSL Fax connection (syscall).  Error %d: %s", ret, strerror(ret));
	    } else {
		sfp.emsg = fxStr::format("Unable to write to SSL Fax connection.  Error %d: %s", cerror, ssl_err_string());
	    }
	    cleanup(sfp, sustain);
	    return (-2);
	}
	if (filter && buf[pos] == bitrev[16] && !isDLE) {
	    // We need to duplicate this DLE.  We do that by forcing the loop to repeat this byte once.
	    pos--;
	    isDLE = true;
	} else {
	    isDLE = false;
	}
    }
    return (count);
}

SSLFaxProcess SSLFax::null()
{
    SSLFaxProcess sfp;
    sfp.ctx = NULL;
    sfp.ssl = NULL;
    sfp.emsg = "";
    sfp.addr = "";
    sfp.server = 0;
    sfp.client = 0;
    return(sfp);
}

SSLFaxProcess SSLFax::startClient(fxStr info, fxStr passcode, const u_char* bitrev, long ms)
{
    SSLFaxProcess sfp;
    sfp.ctx = NULL;
    sfp.ssl = NULL;
    sfp.emsg = "";
    sfp.addr = "";
    sfp.server = 0;
    sfp.client = 0;

    // Initialize the SSL library
    SSL_library_init();

    u_int ppos = info.nextR(info.length(), ':');
    fxStr port = info.tail(info.length()-ppos);
    int portnum = atoi((const char*) port);

    fxStr host = fxStr(info);	// getAddressFamily will modify
    getAddressFamily(host);	// host now omits the port, delimiter, and possible brackets

    sfp.ctx = InitCTX();
    if (sfp.ctx == NULL) {
	sfp.emsg = fxStr::format("Unable to initialize OpenSSL: %s", ssl_err_string());
	cleanup(sfp);
	return (sfp);
    }
    timeval start = currentTime();
    sfp.server = OpenConnection(host, portnum, start, ms, sfp.emsg);
    if (sfp.emsg != "") {
	cleanup(sfp);
	return(sfp);
    }
    sfp.ssl = SSL_new(sfp.ctx);		/* get new SSL state with context */
    SSL_set_fd(sfp.ssl, sfp.server);	/* attach the socket descriptor */

    int cerror;
    int ret;
    do {
	cerror = 0;
	ret = SSL_connect(sfp.ssl);	/* perform the connection */
	if (ret <= 0) {
	    cerror = SSL_get_error(sfp.ssl, ret);
	    /*
	     * SSL_connect() can fail with SSL_ERROR_WANT_READ or SSL_ERROR_WANT_WRITE
	     * because we're using a non-blocking socket.  These conditions
	     * probably mean that the server has an open socket but that it
	     * hasn't yet started its SSL_accept() - in other words, we may
	     * just be a bit ahead of the receiver.  So, according to the
	     * SSL_connect() man page we then will need to use a select()
	     * on the socket for read or write and then re-run SSL_connect().
	     * We are under a time constraint, however.  So, we have to
	     * also watch for that.
	     */
	    if (cerror == SSL_ERROR_WANT_READ || cerror == SSL_ERROR_WANT_WRITE) {
		fd_set sfd;
		FD_ZERO(&sfd);
		FD_SET(sfp.server, &sfd);
		struct timeval tv;
		tv.tv_sec = (int) ms / 1000;
		tv.tv_usec = (ms % 1000)*1000;
		tv = tv - (currentTime() - start);
		if (cerror == SSL_ERROR_WANT_READ) {
#if CONFIG_BADSELECTPROTO
		    if (!select(sfp.server+1, (int*) &sfd, NULL, NULL, &tv)) {
#else
		    if (!select(sfp.server+1, &sfd, NULL, NULL, &tv)) {
#endif
			sfp.emsg = "Timeout waiting for SSL Fax connection (wanting to read).";
			cleanup(sfp);
			return (sfp);
		    }
		} else {	// SSL_ERROR_WANT_WRITE
#if CONFIG_BADSELECTPROTO
		    if (!select(sfp.server+1, NULL, (int*) &sfd, NULL, &tv)) {
#else
		    if (!select(sfp.server+1, NULL, &sfd, NULL, &tv)) {
#endif
			sfp.emsg = "Timeout waiting for SSL Fax connection (wanting to write).";
			cleanup(sfp);
			return (sfp);
		    }
		}
	    }
	}
    } while (cerror == SSL_ERROR_WANT_READ || cerror == SSL_ERROR_WANT_WRITE);
    if (ret <= 0) {
	sfp.emsg = fxStr::format("Unable to connect to \"%s\".  Error %d: %s", (const char*) info, cerror, ssl_err_string());
	cleanup(sfp);
	return (sfp);
    }
    // Now send the passcode.
    const char* p = passcode;
    if (write(sfp, (const u_char*) p, passcode.length(), bitrev, 0, 1000, false) <= 0) {
	sfp.emsg.append(" (passcode)");
	cleanup(sfp);
	return (sfp);
    }
    sfp.emsg.append(fxStr::format("SSL Fax connection with %s encryption.  ", SSL_get_cipher(sfp.ssl)));
    sfp.emsg.append(ShowCerts(sfp.ssl));	/* get any certificates */
    return (sfp);
}

SSLFaxProcess SSLFax::startServer(fxStr info, fxStr pemFile)
{
    SSLFaxProcess sfp;
    sfp.ctx = NULL;
    sfp.ssl = NULL;
    sfp.emsg = "";
    sfp.addr = "";
    sfp.server = 0;
    sfp.client = 0;

    u_int ppos = info.nextR(info.length(), ':');
    fxStr port = info.tail(info.length()-ppos);
    int portnum = atoi((const char*) port);

    if (portnum < 1) {
	sfp.emsg = fxStr::format("Could not determine port number from \"%s\", got \"%s\".", (const char*) info, (const char*) port);
	return (sfp);
    }
    // portnum = 1 is a special case for dynamic port allocation by the server
    if (portnum == 1) portnum = 0;

    SSL_library_init();		/* Initialize the SSL library */
    sfp.ctx = InitServerCTX();	/* initialize SSL */
    if (sfp.ctx == NULL) {
	sfp.emsg = fxStr::format("Unable to initialize OpenSSL: %s", ssl_err_string());
	cleanup(sfp);
	return (sfp);
    }
    sfp.emsg = LoadCertificates(sfp.ctx, (const char*) pemFile, (const char*) pemFile); /* load certs */
    if (sfp.emsg != "") {
	sfp.emsg.append(ssl_err_string());
	cleanup(sfp);
	return (sfp);
    }
    sfp.server = OpenListener(portnum, sfp.emsg); /* create server socket */
    if (sfp.emsg != "") {
	sfp.emsg.append(ssl_err_string());
	cleanup(sfp);
	return (sfp);
    }
    if (fcntl(sfp.server, F_SETFL, fcntl(sfp.server, F_GETFL, 0) | O_NONBLOCK) == -1) {
	sfp.emsg.append("Unable to set server SSL Fax socket to non-blocking.");
	cleanup(sfp);
	return (sfp);
    }
    // All good so far.
    return (sfp);
}

bool SSLFax::acceptClient1(SSLFaxProcess& sfp, long ms, bool sustain)
{
    /* Now we wait for the client to connect. */
    /* We can use select() here without SSL telling us to because SSL hasn't started yet. */
    fd_set sfd;
    FD_ZERO(&sfd);
    FD_SET(sfp.server, &sfd);
    struct timeval tv;
    tv.tv_sec = (int) ms / 1000;
    tv.tv_usec = (ms % 1000)*1000;
#if CONFIG_BADSELECTPROTO
    if (!select(sfp.server+1, (int*) &sfd, NULL, NULL, ms == 0 ? NULL : &tv)) {
#else
    if (!select(sfp.server+1, &sfd, NULL, NULL, ms == 0 ? NULL : &tv)) {
#endif
	sfp.emsg = "Timeout waiting for SSL Fax client connection.";
	cleanup(sfp, sustain);
	return (false);
    }
    /* A client is waiting... */
    struct sockaddr_in addr;
    socklen_t len = sizeof (addr);
    sfp.client = accept(sfp.server, (struct sockaddr*) &addr, &len);  /* accept connection as usual */
    if (sfp.client <= 0) {
	sfp.emsg.append(fxStr::format("Restart the listener.  Accept returned %d: %s", sfp.client, strerror(errno)));
	cleanup(sfp);	/* Restart the listening service.  It seems to be malfunctioning. */
	return (false);
    }
    if (fcntl(sfp.client, F_SETFL, fcntl(sfp.client, F_GETFL, 0) | O_NONBLOCK) == -1) {
	sfp.emsg.append("Unable to set client SSL Fax socket (%d) to non-blocking.", sfp.client);
	cleanup(sfp, sustain);
	return (false);
    }
    char address[50];
    if (inet_ntop(addr.sin_family, &addr.sin_addr, address, 50)) {
	if (addr.sin_family == AF_INET6) {
	    sfp.addr = fxStr::format("[%s]:%d", address, ntohs(addr.sin_port));
	} else {
	    sfp.addr = fxStr::format("%s:%d", address, ntohs(addr.sin_port));
	}
    } else {
	    sfp.addr = fxStr::format("<unknown address>:%d", ntohs(addr.sin_port));
    }
    sfp.emsg = fxStr::format("SSL Fax connection: %s ", (const char*) sfp.addr);
    return (true);
}

void SSLFax::acceptClient2(SSLFaxProcess& sfp, fxStr passcode, int modemFd, long ms, bool sustain)
{
    sfp.ssl = SSL_new(sfp.ctx);			/* get new SSL state with context */
    if (!sfp.ssl) {
	sfp.emsg = fxStr::format("SSL_new returned null");
	cleanup(sfp, sustain);
	return;
    }
    if (!SSL_set_fd(sfp.ssl, sfp.client)) {	/* set connection socket to SSL state */
	sfp.emsg = fxStr::format("SSL_set_fd failed");
	cleanup(sfp, sustain);
	return;
    }

    struct timeval start = currentTime();
    int cerror;
    int ret;
    do {
	cerror = 0;
	ret = SSL_accept(sfp.ssl);	/* do SSL-protocol accept */
	if (ret <= 0) {
	    cerror = SSL_get_error(sfp.ssl, ret);
	    if (cerror == SSL_ERROR_WANT_READ || cerror == SSL_ERROR_WANT_WRITE) {
		int selret;
		fd_set rfds;
		FD_ZERO(&rfds);
		if (modemFd) FD_SET(modemFd, &rfds);
		struct timeval tv;
		tv.tv_sec = (int) ms / 1000;
		tv.tv_usec = (ms % 1000)*1000;
		tv = tv - (currentTime() - start);
		if (cerror == SSL_ERROR_WANT_READ) {	// wait for the socket to be readable
		    FD_SET(sfp.client, &rfds);
#if CONFIG_BADSELECTPROTO
		    selret = select((modemFd > sfp.client) ? modemFd+1 : sfp.client+1, (int*) &rfds, NULL, NULL, &tv);
#else
		    selret = select((modemFd > sfp.client) ? modemFd+1 : sfp.client+1, &rfds, NULL, NULL, &tv);
#endif
		} else {	// SSL_ERROR_WANT_WRITE, wait for the socket to be writable
		    fd_set wfds;
		    FD_ZERO(&wfds);
		    FD_SET(sfp.client, &wfds);
#if CONFIG_BADSELECTPROTO
		    selret = select((modemFd > sfp.client) ? modemFd+1 : sfp.client+1, (int*) &rfds, (int*) &wfds, NULL, &tv);
#else
		    selret = select((modemFd > sfp.client) ? modemFd+1 : sfp.client+1, &rfds, &wfds, NULL, &tv);
#endif
		}
		if (!selret) {
		    sfp.emsg = fxStr::format("Timeout in waiting for SSL Fax accept (wanting to %s).", (cerror == SSL_ERROR_WANT_READ ? "read" : "write"));
		    cleanup(sfp, sustain);
		    return;
		} else if (selret < 0) {
		    sfp.emsg = fxStr::format("Error in waiting for SSL Fax accept (wanting to %s): %s", (cerror == SSL_ERROR_WANT_READ ? "read" : "write"), strerror(errno));
		    cleanup(sfp, sustain);
		    return;
		}
		if (modemFd && FD_ISSET(modemFd, &rfds)) {
		    // The modem got a signal.  This probably means that SSL Fax is not happening.
		    sfp.emsg = "Modem has data when waiting for SSL Fax accept.  Terminating SSL Fax.";
		    cleanup(sfp, sustain);
		    return;
		}
	    }
	}
    } while (cerror == SSL_ERROR_WANT_READ || cerror == SSL_ERROR_WANT_WRITE);
    if (ret <= 0) {
	if (cerror == SSL_ERROR_SYSCALL) {
	    sfp.emsg = fxStr::format("Unable to accept SSL Fax connection (syscall).  Error %d: %s", ret, strerror(ret));
	} else {
	    sfp.emsg = fxStr::format("Unable to accept SSL Fax connection.  Error %d: %s", cerror, ssl_err_string());
	}
	cleanup(sfp, sustain);
	return;
    }

    // Now read the passcode.
    u_char p[1];
    for (u_int i = 0; i < passcode.length(); i++) {
	if (read(sfp, p, 1, modemFd, 1000, sustain) <= 0) {
	    sfp.emsg.append(" (passcode)");
	    cleanup(sfp, sustain);
	    return;
	}
	if (p[0] != passcode[i]) {
	    sfp.emsg.append("Invalid Passcode");
	    cleanup(sfp, sustain);
	    return;
	}
    }
    sfp.emsg.append(ShowCerts(sfp.ssl));	/* get any certificates */
    return;
}

void SSLFax::acceptClient(SSLFaxProcess& sfp, fxStr passcode, int modemFd, long ms)
{
    if (!acceptClient1(sfp, ms)) return;
    acceptClient2(sfp, passcode, modemFd, (ms == 0) ? 1000 : ms); // Set a reasonable timeout after our indefinite wait for a client connection.
    return;
}

void SSLFax::cleanup(SSLFaxProcess& sfp, bool sustain)
{
    sfp.ssl = NULL;
    if (!sustain) {
	if (sfp.ctx) {
	    ERR_free_strings();		/* free memory from SSL_load_error_strings */
	    EVP_cleanup();		/* free memory from OpenSSL_add_all_algorithms */
	    SSL_CTX_free(sfp.ctx);	/* release context */
	}
	sfp.ctx = NULL;
	if (sfp.server) {
	    /*
	     * This is the client.  We want the client-side to shut down
	     * first so that the server-side is not left with TIME_WAIT.
	     * We'll get the TIME_WAIT on the client-side, and that's okay.
	     */
	    shutdown(sfp.server, SHUT_RDWR);
	    close(sfp.server);
	}
	sfp.server = 0;
    }
    if (sfp.client) {
	/*
	 * This is the server.  We want to avoid TIME_WAIT, and so we
	 * wait up to 5 seconds for the client to shut down, and if
	 * they don't, then we'll RST the connection using SO_LINGER.
	 */
	fcntl(sfp.client, F_SETFL, fcntl(sfp.server, F_GETFL, 0) &~ O_NONBLOCK);	// we want the read() below to block.

	char* buf[1];
	bool done = false;
	fd_set sfd;
	FD_ZERO(&sfd);
	FD_SET(sfp.client, &sfd);
	struct timeval tv;
	do {
	    tv.tv_sec = 5;
	    tv.tv_usec = 0;
#if CONFIG_BADSELECTPROTO
	    if (!select(sfp.client+1, (int*) &sfd, NULL, NULL, &tv)) {
#else
	    if (!select(sfp.client+1, &sfd, NULL, NULL, &tv)) {
#endif
		// The client did not shut down first.  RST the connection.
		struct linger ling;
		ling.l_onoff = 1;
		ling.l_linger = 0;
		setsockopt(sfp.client, SOL_SOCKET, SO_LINGER, (char*) &ling, sizeof(ling));
		done = true;
	    } else {
		done = (::read(sfp.client, buf, 1) <= 0);
	    }
	} while (!done);
	close(sfp.client);
    }
    sfp.client = 0;
    return;
}

#endif
