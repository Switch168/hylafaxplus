/*	$Id: faxSendApp.h 1151 2013-02-26 23:46:36Z faxguy $ */
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
#ifndef _faxSendApp_
#define	_faxSendApp_
/*
 * HylaFAX Send Job App.
 */
#include "faxApp.h"
#include "FaxServer.h"

class UUCPLock;

class faxSendApp : public FaxServer, public faxApp {
public:
    struct stringtag {
	const char*	 name;
	fxStr faxSendApp::* p;
	const char*	 def;		// NULL is shorthand for ""
    };
    struct numbertag {
	const char*	 name;
	u_int faxSendApp::* p;
	u_int		 def;
    };
    struct booltag {
	const char*	 name;
	bool faxSendApp::* p;
	bool		 def;
    };

    static int	terminateSignal;	// Termination signal received

private:
// runtime state
    bool	ready;			// modem ready for use
    UUCPLock*	modemLock;		// uucp lockfile handle
    fxStr	pollRcvdCmd;		// command for docs received by polling
    fxStr	shareCallFailures;	// which call failures should be shared to batched jobs
    fxStr	rewriteFaxName;		// desired faxname identifier
    fxStr	rewriteFaxNumber;	// desired faxnumber identifier
    u_int	desiredDF;		// desired data format
    u_int	desiredBR;		// desired signalling rate
    u_int	desiredST;		// desired min-scanline-time
    u_int	desiredEC;		// enable use of ECM if available
    bool	useJobTSI;		// User the Job submitted TSI
    bool	logSend;		// Log sent faxes to xferfaxlog

    static faxSendApp* _instance;

    static stringtag strings[];
    static numbertag numbers[];
    static booltag booleans[];

// configuration support
    void	setupConfig();
    void	resetConfig();
    bool	setConfigItem(const char* tag, const char* value);
// modem handling
    bool	canLockModem();
    bool	lockModem();
    void	unlockModem();
// notification interfaces used by FaxServer
    void	notifyModemReady();
    void	notifyModemWedged();
    void	notifyCallPlaced(const FaxRequest&);
    void	notifyConnected(const FaxRequest&);
    void	notifyPageSent(FaxRequest& req, const char* filename);
    void	notifyDocumentSent(FaxRequest&, u_int fileIndex);
    void	notifyPollRecvd(FaxRequest&, FaxRecvInfo&);
    void	notifyPollDone(FaxRequest&, u_int pollIndex);
public:
    faxSendApp(const fxStr& device, const fxStr& devID);
    ~faxSendApp();

    static faxSendApp& instance();

    void	initialize(int argc, char** argv);
    void	open();
    void	close();

    FaxSendStatus send(const char** filename, int num);

    bool	isReady() const;
};
inline bool faxSendApp::isReady() const 	{ return ready; }
#endif
