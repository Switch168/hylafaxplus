/*	$Id: FIFO.c++ 2 2005-11-11 21:32:03Z faxguy $ */
/*
 * Copyright (c) 1990-1996 Sam Leffler
 * Copyright (c) 1991-1996 Silicon Graphics, Inc.
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
#include "Sys.h"
#include "config.h"

#include "HylaFAXServer.h"
#include "Dispatcher.h"

#include <sys/ioctl.h>

/*
 * Support for communication with the HylaFAX queuer via FIFO's.
 */

/*
 * Create the client FIFO and open it for use.
 */
bool
HylaFAXServer::initClientFIFO(fxStr& emsg)
{
    clientFIFOName = fxStr::format(FAX_CLIENTDIR "/%u", getpid());
    if (Sys::mkfifo(clientFIFOName, 0622) < 0 && errno != EEXIST) {
	emsg = fxStr::format("Could not create %s: %s",
	    (const char*) clientFIFOName, strerror(errno));
	return (false);
    }
    clientFd = Sys::open(clientFIFOName, O_RDONLY|O_NDELAY);
    if (clientFd == -1) {
	emsg = fxStr::format("Could not open FIFO file %s: %s",
	    (const char*) clientFIFOName, strerror(errno));
	return (false);
    }
    if (!Sys::isFIFOFile(clientFd)) {
	emsg = clientFIFOName | " is not a FIFO special file";
	return (false);
    }
    // open should set O_NDELAY, but just to be sure...
    if (fcntl(clientFd, F_SETFL, fcntl(clientFd, F_GETFL, 0) | O_NDELAY) < 0)
	logError("initClientFIFO %s: fcntl: %m", (const char*) clientFIFOName);

    /*
     * Depending on the system type, a condition where all of the FIFO
     * writers have closed may trigger EOF, and consequently the reader
     * will begin a very fast loop repeatedly reading EOF from the FIFO.
     * In the past this was handled with an option to open the FIFO in
     * read+write mode (CONFIG_OPENFIFO=O_RDWR), but doing so is an
     * undefined usage and thus produces undefined behaviour.  Also was
     * tried closing and reopening the FIFO every time a FIFO message
     * was received was received (CONFIG_FIFOBUG), but that produces a
     * race condition.  So those configuration options have been
     * abandoned, and we merely open a writer that subsequently goes
     * unused.  This prevents the EOF condition.
     */
    ignore_fd = Sys::open(clientFIFOName, O_WRONLY|O_NDELAY);

    Dispatcher::instance().link(clientFd, Dispatcher::ReadMask, this);
    return (true);
}

/*
 * Respond to input on a FIFO file descriptor.
 */
int
HylaFAXServer::FIFOInput(int fd)
{
    char buf[4096];
    int cc;
    while ((cc = Sys::read(fd, buf, sizeof (buf)-1)) > 0) {
	if (cc == sizeof(buf)-1) {
	    int pipesize = -1;
	    int unread = -1;
#ifdef F_GETPIPE_SZ
	    pipesize = fcntl(fd, F_GETPIPE_SZ, 0);
#endif
#ifdef FIONREAD
	    ioctl(fd, FIONREAD, &unread);
#endif
	    logWarning("FIFO Read full: %d, unread: %d, FIFO size: %d", cc, unread, pipesize);
	}
	buf[cc] = '\0';
	char* bp = &buf[0];
	do {
	    if (bp[0] == '!') {
		/*
		 * This is an event message from the scheduler
		 * generated by a previously registered trigger.
		 * Setup the unpacking work and dispatch it.
		 */
		TriggerMsgHeader h;
		u_int left = &buf[cc]-bp;
		bool needy = false;
		if (left < sizeof(TriggerMsgHeader))
		{
		    needy = true;
		} else
		{
		    memcpy(&h, bp, sizeof (h));		// copy to align fields
		    if (left < h.length)
			needy = true;
		}
		if (needy)
		{
		    /*
		     * Handle the case where the buffer was read full.  This means that
		     * we have a "partial" message at the end of buf, and the rest in
		     * the FIFO.
		     *
		     * buf[n] is NEVER read from the file, it's always 1-past.  If we
		     * have reached buf[n], we know we are past the read data.
		     *
		     * We have (left) bytes of the message at the end of the buf.
		     * We move them to the front, and read as much more as we can to
		     * fill buf back up, leaving it in the same state as if this was
		     * the initial read.
		     */
		    memmove(buf, bp, left);
		    cc = Sys::read(fd, buf+left, (sizeof(buf)-1) - left) + left;
		    buf[cc] = '\0';
		    bp = &buf[0];
		} else {
		    triggerEvent(h, bp+sizeof (h));
		    bp += h.length;
		}
	    } else {
		/*
		 * Break up '\0'-separated records and strip
		 * any trailing '\n' so that "echo mumble>FIFO"
		 * works (i.e. echo appends a '\n' character).
		 */
		char* cp = strchr(bp, '\0');

		/*
		 * Handle the case where the buffer was read full.  This means that
		 * we have a "partial" message at the end of buf, and the rest in
		 * the FIFO.
		 *
		 * buf[n] is NEVER read from the file, it's always 1-past.  If we
		 * have reached buf[n], we know we are past the read data.
		 *
		 * We have (cp - bp) bytes of the message at the end of the buf.
		 * We move them to the front, and read as much more as we can to
		 * fill buf back up, leaving it in the same state as if this was
		 * the initial read.
		 */
		if (cp == &buf[sizeof(buf)-1])
		{
		    memmove(buf, bp, cp-bp);
		    cc = Sys::read(fd, buf+(cp-bp), (sizeof(buf)-1) - (cp-bp)) + (cp-bp);
		    buf[cc] = '\0';
		    bp = &buf[0];
		    cp = strchr(bp, '\0');
		}

		if (cp > bp) {
		    if (cp[-1] == '\n') {
			cp[-1] = '\0';
			FIFOMessage(bp, &cp[-1]-bp);
		    } else
			FIFOMessage(bp, cp-bp);
		}
		bp = cp+1;
	    }
	} while (bp < &buf[cc]);
    }
    return (0);
}

void
HylaFAXServer::FIFOMessage(const char* cp, u_int)
{
    if (IS(WAITFIFO)) {
	/*
	 * Someone is waiting for a response
	 * from the server.  Stash the response
	 * and notify them by marking the
	 * response as arrived.
	 */
	fifoResponse = cp;
	state &= ~S_WAITFIFO;
	return;
    }
    switch (cp[0]) {
    case 'H':				// HELLO when queuer restarts
	if (faxqFd >= 0)
	    Sys::close(faxqFd), faxqFd = -1;
	if (trigSpec != "") {		// reload trigger
	    fxStr emsg;
	    (void) loadTrigger(emsg);
	}
	break;
    default:
	logError("Bad fifo message \"%s\"", cp);
	break;
    }
}

/*
 * Send a message to the central queuer process.
 */
bool
HylaFAXServer::sendQueuerMsg(fxStr& emsg, const fxStr& msg)
{
    bool retry = false;
again:
    if (faxqFd == -1) {
	faxqFd = Sys::open(faxqFIFOName, O_WRONLY|O_NDELAY);
	if (faxqFd == -1) {
	    emsg = fxStr::format("Unable to open scheduler FIFO: %s",
		strerror(errno));
	    return (false);
	}
	/*
	 * Turn off O_NDELAY so that write will block if FIFO is full.
	 */
	if (fcntl(faxqFd, F_SETFL, fcntl(faxqFd, F_GETFL, 0) &~ O_NDELAY) < 0)
	    logError("fcntl: %m");
    }
    ssize_t len = msg.length()+1;
    if (Sys::write(faxqFd, msg, len) != len) {
	if (errno == EBADF || errno == EPIPE) {
	    /*
	     * The queuer process is gone.  Try again
	     * once in case it has been restarted.
	     */
	    Sys::close(faxqFd), faxqFd = -1;
	    if (!retry) {
		retry = true;
		goto again;
	    }
	}
	emsg = fxStr::format("FIFO write failed: %s", strerror(errno));
	logError(emsg);
	return (false);
    } else
	return (true);
}

/*
 * Send a message to the central queuer process.
 */
bool
HylaFAXServer::sendQueuer(fxStr& emsg, const char* fmt ...)
{
    va_list ap;
    va_start(ap, fmt);
    bool ok = sendQueuerMsg(emsg, fxStr::vformat(fmt, ap));
    va_end(ap);
    return (ok);
}

/*
 * Send a message to the central queuer process
 * and wait for a response on our client FIFO.
 */
bool
HylaFAXServer::sendQueuerACK(fxStr& emsg, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    bool b = vsendQueuerACK(emsg, fmt, ap);
    va_end(ap);
    return (b);
}

bool
HylaFAXServer::vsendQueuerACK(fxStr& emsg, const char* fmt, va_list ap)
{
    if (clientFd == -1) {
	emsg = "Bad server state, client FIFO is not open";
	return (false);
    }
    fxStr msg = fxStr::vformat(fmt, ap);
    if (msg.length() < 2) {			// sanity check
	emsg = "Bad FIFO message, too short to be valid";
	return (false);
    }
    msg.insert(clientFIFOName | ":", 1);	// insert FIFO name for reply 
    state |= S_WAITFIFO;
    bool ok = sendQueuerMsg(emsg, msg);
    if (ok) {
	Dispatcher& disp = Dispatcher::instance();
	for (; IS(WAITFIFO); disp.dispatch())
	    ;
	if (fifoResponse.length() < 2) {	// too short to be valid
	    emsg = "Unexpected response from scheduler: \"" |fifoResponse| "\"";
	    ok = false;
	} else if (fifoResponse[0] == msg[0]) {	// response to our request
	    ok = (fifoResponse[1] == '*');
	    if (!ok)
		emsg = "Unspecified reason (scheduler NAK'd request)";
	} else					// user abort
	    ok = false;
    }
    return (ok);
}

/*
 * Send a message to a modem process via the per-modem FIFO.
 */
bool
HylaFAXServer::sendModem(const char* modem, fxStr& emsg, const char* fmt ...)
{
    fxStr fifoName(modem);
    canonDevID(fifoName);			// convert pathname -> devid
    fifoName.insert("/" FAX_FIFO ".");		// prepend /FIFO. string
    int fd = Sys::open(fifoName, O_WRONLY|O_NDELAY);
    if (fd == -1) {
	emsg = fxStr::format("Unable to open %s: %s",
	    (const char*) fifoName, strerror(errno));
	return (false);
    }
    va_list ap;
    va_start(ap, fmt);
    fxStr msg = fxStr::vformat(fmt, ap);
    va_end(ap);
    ssize_t len = msg.length()+1;
    if (Sys::write(fd, msg, len) != len) {
	emsg = fxStr::format("write to %s failed: %s",
	    (const char*) fifoName, strerror(errno));
	logError(emsg);
	return (false);
    } else
	return (true);
}
