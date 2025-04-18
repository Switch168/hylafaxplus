/*	$Id: FaxSend.c++ 1147 2013-02-19 17:55:54Z faxguy $ */
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
#include <errno.h>
#include <stdint.h>

#include "Sys.h"

#include "Dispatcher.h"
#include "tiffio.h"
#include "FaxServer.h"
#include "FaxMachineInfo.h"
#include "FaxRecvInfo.h"
#include "FaxAcctInfo.h"
#include "faxApp.h"			// XXX
#include "UUCPLock.h"
#include "t.30.h"
#include "config.h"

#define BATCH_FIRST 1
#define BATCH_LAST  2

/*
 * FAX Server Transmission Protocol.
 */
void
FaxServer::sendFax(FaxRequest& fax, FaxMachineInfo& clientInfo, FaxAcctInfo& ai, u_int& batched, bool usedf)
{
    useDF = usedf;
    u_int prevPages = fax.npages;
    if (!(batched & BATCH_FIRST) || lockModem()) {
	// we can't set the owner's uid onto batched logs for privacy reasons as they may include other users' jobs, too
        if (batched & BATCH_FIRST) {
	    gid_t uid = 0;
	    if ((batched & BATCH_LAST) || ! batchLogs) {
		struct stat sb;
		if (Sys::stat(fax.qfile, sb) >= 0) uid = sb.st_gid;	// get the owner's uid from the gid of the qfile
	    }
	    beginSession(fax.number, uid);
	    batchid = getCommID();
	} else {
	    if (! batchLogs) {
		gid_t uid = 0;
		struct stat sb;
		if (Sys::stat(fax.qfile, sb) >= 0) uid = sb.st_gid;	// get the owner's uid from the gid of the qfile

		beginSession(fax.number, uid);		// uid perpetuates the owner's uid onto logs
		batchid.append("," | getCommID());
		traceServer("SESSION BATCH %s", (const char*) batchid);
	    }
        }
	fax.commid = getCommID();		// set by beginSession
	traceServer("SEND FAX: JOB %s DEST %s COMMID %s DEVICE '%s' FROM '%s <%s>' USER %s"
	    , (const char*) fax.jobid
	    , (const char*) fax.external
	    , (const char*) fax.commid
	    , (const char*) getModemDevice()
	    , (const char*) fax.sender
	    , (const char*) fax.mailaddr
	    , (const char*) fax.owner
	);

	/* Dispatcher already did setupModem() */

	changeState(SENDING);
	IOHandler* handler =
	    Dispatcher::instance().handler(
		getModemFd(), Dispatcher::ReadMask);
	if (handler)
	    Dispatcher::instance().unlink(getModemFd());
	setServerStatus("Sending job " | fax.jobid);
	/*
	 * Construct the phone number to dial by applying the
	 * dialing rules to the user-specified dialing string.
	 */
	sendFax(fax, clientInfo, prepareDialString(fax.number), batched);
	if ((batched & BATCH_LAST) || (fax.status != send_done)) {
	    /*
	     * Because some modems are impossible to safely hangup in the
	     * event of a problem, we force a close on the device so that
	     * the modem will see DTR go down and (hopefully) clean up any
	     * bad state its in.  We then wait a couple of seconds before
	     * trying to setup the modem again so that it can have some
	     * time to settle.  We want a handle on the modem so that we
	     * can be prepared to answer incoming phone calls.
	     */
	    discardModem(true);
	    changeState(MODEMWAIT, 5);
	    unlockModem();
	    endSession();
	} else if(! batchLogs)
	{
	    traceServer("SESSION BATCH CONTINUES");
	    endSession();
	}
    } else {
	if (state != LOCKWAIT)
	    sendFailed(fax, send_retry,
		"Can not lock modem device", 2*pollLockWait);
	if (state != SENDING && state != ANSWERING && state != RECEIVING)
	    changeState(LOCKWAIT, pollLockWait);
    }
    /*
     * Record transmit accounting information for caller.
     *
     * As the encoded parameters is limited to 32 bits and DCS does
     * not contain V.34-Fax speeds we use both.
     */
    ai.npages = fax.npages - prevPages;		// count of pages transmitted
    ai.params = clientParams.encode();		// encoded negotiated parameters
    clientParams.asciiEncode(ai.faxdcs);	// DCS signal
    fax.sigrate = clientParams.bitRateName();	// (last) signalling rate used
    fax.df = clientParams.dataFormatName();	// negotiated data format
}

void
FaxServer::sendFailed(FaxRequest& fax, FaxSendStatus stat, const char* notice, u_int tts)
{
    fax.status = stat;
    fax.notice = notice;
    /*
     * When requeued for the default interval (called with 3 args),
     * don't adjust the time-to-send field so that the spooler
     * will set it according to the default algorithm that 
     * uses the command-line parameter or requeueOther and a random jitter.
     */
    if (tts != 0)
	fax.tts = Sys::now() + tts;
    traceServer("SEND FAILED: JOB %s DEST %s ERR %s"
	, (const char*) fax.jobid
	, (const char*) fax.external
	, (const char*) notice
    );

}

/*
 * Send the specified TIFF files to the FAX
 * agent at the given phone number.
 */
void
FaxServer::sendFax(FaxRequest& fax, FaxMachineInfo& clientInfo, const fxStr& number, u_int& batched)
{
    connTime = 0;				// indicate no connection
    fxStr notice;
    /*
     * Calculate initial page-related session parameters so
     * that braindead Class 2 modems which ignore AT+FIS can constrain
     * the modem with AT+FCC before dialing the telephone, and so that
     * Class 1.0 modems know to not enable V.34 support if ECM is not
     * being used.  Hopefully this doesn't interfere with batched faxes.
     */
    clientParams.decodePage(fax.pagehandling);
    /*
     * In cases where color use is dependent on receiver support, both
     * color and monochrome documents were prepared, but pagehandling
     * will decode with JP_NONE.  So if this is the case, set jp.
     */
    if (fax.usecolor) clientParams.jp = JP_COLOR;

    /*
     * So we want to restrict DF selection to those masked in fax.desireddf
     *
     * clientParams.jp != 0 with clientParams.df == 7 indicates 
     * color-only - don't manipulate df in that case.
     */
    if (!(clientParams.jp && clientParams.df == 7))
	clientParams.df = fxmin(modem->getBestDataFormat(), (u_int)fax.desireddf);
    clientParams.br = fxmin(modem->getBestSignallingRate(), (u_int) fax.desiredbr);
    clientParams.ec = (modem->supportsECM() ? fax.desiredec : EC_DISABLE);
    clientParams.st = fxmax(modem->getBestScanlineTime(), (u_int) fax.desiredst);
    clientParams.bf = BF_DISABLE;
    /*
     * Force the modem into the appropriate class
     * used to send facsimile.  We do this before
     * doing any fax-specific operations such as
     * requesting polling.
     */
    if ((batched & BATCH_FIRST) &&
	!modem->faxService(!clientInfo.getHasV34Trouble() && clientParams.ec != EC_DISABLE && clientParams.br > BR_14400,
	    !clientInfo.getHasV17Trouble() && clientParams.br > BR_9600)) {
	sendFailed(fax, send_failed, "Unable to configure modem for fax use");
	return;
    }
    /*
     * Check if this job includes a poll request, and
     * if it does, inform the modem in case it needs to
     * do something to get back status about whether or
     * not documents are available for retrieval.
     */
    if (fax.findItem(FaxRequest::send_poll) != fx_invalidArrayIndex &&
	!modem->requestToPoll(notice)) {
	sendFailed(fax, send_failed, notice);
	return;
    }
    if (!modem->sendSetup(fax, clientParams, notice)) {
	sendFailed(fax, send_failed, notice);
	return;
    }

    if (tiff2faxCmd.length() && (class2RTFCC || softRTFCC)) {
	/*
	 * Here we enable "intelligent" RTFCC format selection by 
	 * determining the effectiveness of 2-D encoding.  We take all 
	 * fax items and convert them to both MH (1-D) and MMR (2-D).  
	 * Then we can sum up the data sizes and later make a decision 
	 * based on receiver support for which compression format will 
	 * be optimal.
	 */
	formatSize[0] = formatSize[1] = 0;
	const char* argv[7];
	argv[0] = (const char*) tiff2faxCmd;
	argv[1] = "-S";
	argv[3] = "-o";
	argv[6] = NULL;
	char pidstr[6];
	snprintf(pidstr, sizeof(pidstr), "%u", (u_int) getpid());
	fxStr newfile, ext, opt;
	struct stat sb;
	for (u_int i = 0, n = fax.items.length(); i < n; i++) {
	    if (fax.items[i].op == FaxRequest::send_fax) {
		argv[5] = (const char*) fax.items[i].item;
		for (u_short j = 0; j < 2; j++) {
		    switch (j) {
			case 0: opt = "-1"; ext = ".MH."; break;
			case 1: opt = "-3"; ext = ".MMR."; break;
		    }
		    argv[2] = (const char*) opt;
		    newfile = fax.items[i].item;
		    newfile.append(ext);
		    newfile.append(pidstr);
		    argv[4] = (const char*) newfile;
		    pid_t pid = fork();
		    switch (pid) {
			case 0:
			    Sys::execv((const char*) tiff2faxCmd, (char* const*) argv);
			    sleep(1);		// XXX give parent time
			    _exit(127);
			case -1:
			    break;
			default:
			    Sys::waitpid(pid);
			    break;
		    }
		    if (Sys::stat(newfile, sb) < 0) {
			logError("Could not create %s, result: %s", (const char*) newfile, strerror(errno));
			break;
		    }
		    formatSize[j] += sb.st_size;
		    /*
		     * In theory we could leave these files around for other faxsend processes
		     * (if we didn't use the pid in the filename) or to use instead of performing 
		     * RTFCC.  But CPU is usually cheap, and for now it's easier to just delete 
		     * these here rather than having to keep track of cleaning up elsewhere.
		     */
		    Sys::unlink((const char*) newfile);
		}
	    }
	}
	if (formatSize[0] < formatSize[1])
	    traceProtocol("1-D encoding outperforms 2-D by %d%%. Prefer MH format.", 
		(formatSize[1]-formatSize[0])*100/formatSize[1]);
    }

    fax.notice = "";
    notifyCallPlaced(fax);
    CallStatus callstat;

    /* Empty the following that may be inherited from past sessions and get passed on to this session if the fax fails before Phase B completes. */
    clientInfo.setNSF("");
    clientInfo.setCSI("");
    clientInfo.setDIS("");

    if (batched & BATCH_FIRST)
	callstat = modem->dial(number, fax.faxnumber, notice);
    else
	callstat = ClassModem::OK;
    if (callstat == ClassModem::OK)
	connTime = Sys::now();			// connection start time
    (void) abortRequested();			// check for user abort
    if (callstat == ClassModem::OK && !abortCall && fax.probeonly != 1) {
	/*
	 * Call reached a fax machine.  Check remote's
	 * capabilities against those required by the
	 * job and setup for transmission.
	 */
	fax.ndials = 0;				// consec. failed dial attempts
	fax.tottries++;				// total answered calls
	fax.totdials++;				// total attempted calls
	clientInfo.setCalledBefore(true);
	clientInfo.setDialFailures(0);
	modem->sendBegin();
	bool remoteHasDoc = false;
	notifyConnected(fax);
	FaxSendStatus status = modem->getPrologue(
	    clientCapabilities, remoteHasDoc, notice, batched, false);
	if (status != send_ok) {
	    if (!notice.length()) notice = "send failed during Phase B introduction";
	    sendFailed(fax, status, notice, requeueProto);
	} else {
	    // CSI
	    fxStr csi("<UNSPECIFIED>");
	    (void) modem->getSendCSI(csi);
	    clientInfo.setCSI(csi);			// record remote CSI
	    fax.csi = csi;				// store in queue file also for notify

	    // NSF
	    NSF nsf;
	    (void) modem->getSendNSF(nsf);
	    clientInfo.setNSF(nsf.getHexNsf());		// record remote NSF
	    fax.nsf = fxStr::format("Equipment:%s %s:Station:%s", nsf.getVendor(), nsf.getModel(), nsf.getStationId());

	    // DIS
	    fxStr clientdis;
	    clientCapabilities.asciiEncode(clientdis);
	    clientInfo.setDIS(clientdis);

	    // modem used
	    fax.modemused = getModemDeviceID();		// store in queue file also for notify

	    if (fax.probeonly == 2) {			// the job requests that we stop here
		sendFailed(fax, send_retry, "Probe complete.");
	    } else if (!sendClientCapabilitiesOK(fax, clientInfo, notice)) {
		// NB: mark job completed 'cuz there's no way recover
		if (!notice.length()) notice = "send failed during Phase B negotiation";
		sendFailed(fax, send_failed, notice);
	    } else {
		modem->sendSetupPhaseB(fax.passwd, fax.subaddr);
		/*
		 * Group 3 protocol forces any sends to precede any polling.
		 */
		fax.status = send_retry;
		bool dosetup = true;
		while (fax.items.length() > 0) {	// send operations
		    u_int i = fax.findItem(FaxRequest::send_fax);
		    if (i == fx_invalidArrayIndex)
			break;
		    FaxItem& freq = fax.items[i];
		    traceProtocol("SEND file \"%s\"", (const char*) freq.item);
		    fileStart = pageStart = Sys::now();
		    if (!sendFaxPhaseB(fax, freq, clientInfo, batched, dosetup)) {
			/*
			 * Prevent repeated batching errors.
			 */
			if (fax.status == send_batchfail) {
			    fax.status = send_retry;
			    clientInfo.setSupportsBatching(false);
			    batched |= BATCH_LAST;
			}
			/*
			 * Prevent repeated V.34 errors.
			 */ 
			if (fax.status == send_v34fail) {
			    fax.status = send_retry;
			    clientInfo.setHasV34Trouble(true);
			}
			/*
			 * Prevent repeated V.17 errors.
			 */ 
			if (fax.status == send_v17fail) {
			    fax.status = send_retry;
			    clientInfo.setHasV17Trouble(true);
			}
			/*
			 * On protocol errors retry more quickly
			 * (there's no reason to wait is there?).
			 */
			if (fax.status == send_retry ||
			  fax.status == send_reformat)
			    fax.tts = time(0) + requeueProto;
			break;
		    }
		    /*
		     * The file was delivered, notify the server.
		     * Note that a side effect of the notification
		     * is that this file is deleted from the set of
		     * files to send (so that it's not sent again
		     * if the job is requeued).  This is why we call
		     * find again at the top of the loop
		     */
		    notifyDocumentSent(fax, i);
		    dosetup = false;
		}
		fax.nocountcover = modem->getNoCountCoverPages();	// update it
		if ((fax.status == send_done || dosetup) &&
	      fax.findItem(FaxRequest::send_poll) != fx_invalidArrayIndex)
		    sendPoll(fax, remoteHasDoc);
	    }
	}
	if ((batched & BATCH_LAST) || (fax.status != send_done))
	    modem->sendEnd();
	if (fax.status != send_done) {
	    clientInfo.setSendFailures(clientInfo.getSendFailures()+1);
	    clientInfo.setLastSendFailure(fax.notice);
	} else
	    clientInfo.setSendFailures(0);
    } else if (!abortCall) {
	/*
	 * Analyze the call status codes and selectively decide if the
	 * job should be retried.  We try to avoid the situations where
	 * we might be calling the wrong number so that we don't end up
	 * harrassing someone w/ repeated calls.
	 */
	if (callstat != ClassModem::BUSY || !isBusyNoFail()) {	// "BUSY" + nofailbusy = don't increment
	    fax.ndials++;		// number of consecutive failed calls
	    fax.totdials++;		// total attempted calls
	}
	switch (callstat) {
	case ClassModem::NOFCON:	// carrier seen, but handshake failed
	    clientInfo.setCalledBefore(true);
	    /* fall thru... */
	case ClassModem::DATACONN:	// data connection established
	case ClassModem::NOCARRIER:	// no carrier detected on remote side
	case ClassModem::BUSY:		// busy signal
	case ClassModem::NOANSWER:	// no answer or ring back
	    if (!clientInfo.getCalledBefore() && fax.ndials > retryMAX[callstat])
		sendFailed(fax, send_failed, notice);
	    else if (fax.retrytime != 0)
		sendFailed(fax, send_retry, notice, fax.retrytime);
	    else
		sendFailed(fax, send_retry, notice, requeueTTS[callstat]);
	    break;
	case ClassModem::V34FAIL:	// carrier seen, but V.34/V.8 handshake incompatibility
	    clientInfo.setHasV34Trouble(true);
	    /* fall thru... */
	case ClassModem::NODIALTONE:	// no local dialtone, possibly unplugged
	case ClassModem::ERROR:		// modem might just need to be reset
	case ClassModem::FAILURE:	// modem returned something unexpected
	case ClassModem::RING:		// glare - ring detected after dial
	    if (!clientInfo.getCalledBefore() && fax.ndials > retryOther)
		sendFailed(fax, send_failed, notice);
	    else
		sendFailed(fax, send_retry, notice, requeueOther);
	    break;
	case ClassModem::OK:		// user abort? / job request was only a probe
	    if (fax.probeonly == 1) sendFailed(fax, send_retry, "Probe complete.");
	    break;
	}
	if (callstat != ClassModem::OK) {
	    clientInfo.setDialFailures(clientInfo.getDialFailures()+1);
	    clientInfo.setLastDialFailure(fax.notice);
	}
    }
    if (abortCall)
	sendFailed(fax, send_retry, "Call aborted by user");
    else if (fax.status == send_retry) {
	if (!fax.notice.length()) fax.notice = "fax page transmission failure {E053}";
	if (fax.totdials == fax.maxdials) {
	    notice = fax.notice | "; too many attempts to dial";
	    sendFailed(fax, send_failed, notice);
	} else if (fax.tottries == fax.maxtries) {
	    notice = fax.notice | "; too many attempts to send";
	    sendFailed(fax, send_failed, notice);
	}
    }
    if ((batched & BATCH_LAST) || (fax.status != send_done)) {
	/*
	 * Cleanup after the call.  If we have new information on
	 * the client's remote capabilities, the machine info
	 * database will be updated when the instance is destroyed.
	 *
	 * We wait a second to prevent any hangup signal from 
	 * precluding the complete reception of the last-sent
	 * audio.
	 */
	sleep(1);
	modem->hangup();
    }
    /*
     * This may not be exact--the line may already have been
     * dropped--but it should be close enough unless the modem
     * gets wedged and the hangup work times out.  Also be
     * sure to register a non-zero amount of connect time so
     * that folks doing accounting can adjust charge-back costs
     * to reflect any minimum connect time tarrifs imposted by
     * their PTT (e.g. calls < 1 minute are rounded up to 1 min.)
     */
    if (connTime) {
	connTime = Sys::now() - connTime;
	if (connTime == 0)
	    connTime++;
    }
}

/*
 * Process a polling request.
 */
void
FaxServer::sendPoll(FaxRequest& fax, bool remoteHasDoc)
{
    u_int ix = fax.findItem(FaxRequest::send_poll);
    if (ix == fx_invalidArrayIndex) {
	fax.notice = "polling operation not done because of internal failure";
	traceServer("internal muckup, lost polling request");
	// NB: job is marked done
    } else if (!remoteHasDoc) {
	fax.notice = "remote has no document to poll";
	traceServer("REJECT: " | fax.notice);
	// override to force status about polling failure
	if (fax.notify == FaxRequest::no_notice)
	    fax.notify = FaxRequest::when_done;
    } else {
	FaxItem& freq = fax.items[ix];
	FaxRecvInfoArray docs;
	fax.status = (pollFaxPhaseB(freq.addr, freq.item, docs, fax.notice) ?
	    send_done : send_retry);
	for (u_int j = 0; j < docs.length(); j++) {
	    FaxRecvInfo& ri = docs[j];
	    if (ri.npages > 0) {
		Sys::chmod(ri.qfile, recvFileMode);
		notifyPollRecvd(fax, ri);
	    } else {
		traceServer("POLL: empty file \"%s\" deleted",
		    (const char*) ri.qfile);
		Sys::unlink(ri.qfile);
	    }
	}
	if (fax.status == send_done)
	    notifyPollDone(fax, ix);
    }
}

/*
 * Phase B of Group 3 protocol.
 */
bool
FaxServer::sendFaxPhaseB(FaxRequest& fax, FaxItem& freq, FaxMachineInfo& clientInfo, u_int batched, bool dosetup)
{
    TIFF* tif = NULL;
    FaxSetup setupinfo;
    setupinfo.senderDataSent = clientInfo.getDataSent() + clientInfo.getDataSent1() + clientInfo.getDataSent2();
    setupinfo.senderDataMissed = clientInfo.getDataMissed() + clientInfo.getDataMissed1() + clientInfo.getDataMissed2();
    setupinfo.senderFumblesECM = false;
    /*
     * If this was not a JPEG-only document, then we must use the ".color" document.
     */
    if (clientParams.jp) {
	tif = TIFFOpen(freq.item|".color", "r");
    }
    if (!tif) tif = TIFFOpen(freq.item, "r");
    if (tif && (freq.dirnum == 0 || TIFFSetDirectory(tif, freq.dirnum))) {
	if (dosetup) {
	    // set up DCS according to file characteristics
	    fax.status = sendSetupParams(tif, clientParams, clientInfo, fax.notice);
	}
	if (fax.status == send_ok) {
	    /*
	     * Count pages sent and advance dirnum so that if we
	     * terminate prematurely we'll only transmit what's left
	     * in the current document/file.  Also, if nothing is
	     * sent, bump the counter on the number of times we've
	     * attempted to send the current page.  We don't try
	     * more than 3 times--to avoid looping.
	     */
	    u_int prevPages = fax.npages;
	    fax.status = modem->sendPhaseB(tif, clientParams, clientInfo,
		fax.pagehandling, fax.notice, batched);
	    modem->getDataStats(&setupinfo);
	    traceProtocol("SEND data sent: %d, data missed: %d", setupinfo.senderDataSent, setupinfo.senderDataMissed);
	    clientInfo.setDataSent2(clientInfo.getDataSent1());
	    clientInfo.setDataSent1(clientInfo.getDataSent());
	    clientInfo.setDataSent(setupinfo.senderDataSent);
	    clientInfo.setDataMissed2(clientInfo.getDataMissed1());
	    clientInfo.setDataMissed1(clientInfo.getDataMissed());
	    clientInfo.setDataMissed(setupinfo.senderDataMissed);
	    if (fax.status == send_v17fail && fax.notice == "") {
		// non-fatal V.17 incompatibility
		clientInfo.setHasV17Trouble(true);
		fax.status = send_ok;
	    }
	    if (fax.npages == prevPages) {
		fax.ntries++;
		if (fax.ntries > 2) {
		    if (fax.notice != "")
			fax.notice.append("; ");
		    fax.notice.append(
			"Giving up after 3 attempts to send same page");
		    traceServer("SEND: %s \"%s\", dirnum %d",
			(const char*) fax.notice, (const char*) freq.item, freq.dirnum);
		    fax.status = send_failed;
		}
	    } else {
		freq.dirnum += fax.npages - prevPages;
		fax.ntries = 0;
	    }
	} else {
	    if (!fax.notice.length()) fax.notice = "fax page transmission failure";
	}
    } else {
	fax.notice = tif ? "Can not set directory in document file" :
			   "Can not open document file";
	traceServer("SEND: %s \"%s\", dirnum %d",
	    (const char*) fax.notice, (const char*) freq.item, freq.dirnum);
    }
    if (tif)
	TIFFClose(tif);
    return (fax.status == send_ok);
}

/*
 * Check client's capabilities (DIS) against those of the
 * modem and select the parameters that are best for us.
 */
bool
FaxServer::sendClientCapabilitiesOK(FaxRequest& fax, FaxMachineInfo& clientInfo, fxStr& emsg)
{
    /*
     * Select signalling rate and minimum scanline time
     * for the duration of the session.  These are not
     * changed once they are set here.
     */
    clientInfo.setMaxSignallingRate(clientCapabilities.br);
    int signallingRate =
	modem->selectSignallingRate(
	    fxmin(clientInfo.getMaxSignallingRate(), fax.desiredbr));
    if (signallingRate == -1) {
	emsg = "Modem does not support negotiated signalling rate {E400}";
	return (false);
    }
    clientParams.br = signallingRate;
    if (clientInfo.getHasV17Trouble() && (clientParams.br == BR_14400 || clientParams.br == BR_12000)) clientParams.br = BR_9600;

    clientInfo.setMinScanlineTime(clientCapabilities.st);
    int minScanlineTime =
	modem->selectScanlineTime(
	    fxmax(clientInfo.getMinScanlineTime(), fax.desiredst));
    if (minScanlineTime == -1) {
	emsg = "Modem does not support negotiated min scanline time {E401}";
	return (false);
    }
    clientParams.st = minScanlineTime;

    /*
     * Use optional Error Correction Mode (ECM) if the
     * peer implements and our modem is also capable.
     */
    if ((clientCapabilities.ec != EC_DISABLE) && modem->supportsECM() && fax.desiredec) {
	/*
	 * 1) The receiver expresses a preference one type of T.30-A ECM. In nearly 
	 *    all circumstances 256-bit is the default.
	 * 2) We allow the fax job also to express a preference for one type or the 
	 *    other to be used.  Our default is also 256-bit.
	 * 3) We allow the Class 1 modem to specify a preference between them.  Again,
	 *    the default it 256-bit.
	 * 4) Although T.30-C advises us to honor remote preferences (although 
	 *    discussing a different ECM type than these) the receiver is required to 
	 *    support both if either are supported at all.
	 *
	 * So, to respond to all of these preferences we basically say that if any
	 * of these preferences indicate 64-bit then use 64-bit.  Otherwise, use
	 * 256-bit.
	 */
	if (!modem->supportsECM(EC_ENABLE256) || clientCapabilities.ec != EC_ENABLE256 || fax.desiredec != EC_ENABLE256)
	    clientParams.ec = EC_ENABLE64;
	else
	    clientParams.ec = EC_ENABLE256;
    } else
	clientParams.ec = EC_DISABLE;

    if (clientParams.jp) {
	if (!(clientCapabilities.jp & BIT(clientParams.jp))) {
	    if (clientParams.df == 7) {		// JPEG-only
		emsg = fxStr::format("Remote does not support JPEG fax request. Remote capabilities: %d, Requested parameters: %d {E423}", clientCapabilities.jp, clientParams.jp);
		return (false);
	    }
	    clientParams.jp = JP_NONE;
	}
	if (!modem->supportsJPEG(clientParams.jp)) {
	    if (clientParams.df == 7) {		// JPEG-only
		emsg = "Modem does not support JPEG fax request. {E424}";
		return (false);
	    }
	    clientParams.jp = JP_NONE;
	}
	if (clientParams.ec == EC_DISABLE) {
	    if (clientParams.df == 7) {		// JPEG-only
		emsg = "JPEG fax request requires ECM support. {E425}";
		return (false);
	    }
	    clientParams.jp = JP_NONE;
	}
	/*
	 * Color-fax only supports square resolutions.  So a DIS bit
	 * 98 (100 dpi), 15 (200 dpi), 42 (300 dpi), or 43 (400 dpi) must
	 * be set.  For now we've kept things easy by limiting all color
	 * fax to 200 dpi within the job preparation state.
	 */
	if (!(modem->getVRes() & VR_FINE)) {
	    if (clientParams.df == 7) {		// JPEG-only
		emsg = "Modem does not support JPEG fax resolution. {E426}";
		return (false);
	    }
	    clientParams.jp = JP_NONE;
	}
	if (!(clientCapabilities.vr & VR_FINE)) {
	    if (clientParams.df == 7) {		// JPEG-only
		emsg = "Remote does not support JPEG fax resolution. {E427}";
		return (false);
	    }
	    clientParams.jp = JP_NONE;
	}
	if (clientParams.jp) {
	    /*
	     * The user requested a color fax, and there are no obstacles.
	     * At this point if (jp != 0) then it's color, and df must be 0.
	     */
	    clientParams.df = 0;
	}
    }

    clientParams.bf = BF_DISABLE;
    /*
     * Record the remote machine's capabilities for use below in
     * selecting tranfer parameters for each page sent.  The info
     * constructed here is also recorded in a private database for
     * use in pre-formatting documents sent in future conversations.
     */
    clientInfo.setSupportsVRes(clientCapabilities.vr);
    clientInfo.setSupports2DEncoding(clientCapabilities.df & BIT(DF_2DMR));
    clientInfo.setSupportsMMR(clientCapabilities.df & BIT(DF_2DMMR));
    clientInfo.setMaxPageWidthInPixels(clientCapabilities.pageWidth());
    clientInfo.setMaxPageLengthInMM(clientCapabilities.pageLength());
    traceProtocol("REMOTE best rate %s", clientCapabilities.bitRateName());
    traceProtocol("REMOTE max %s", clientCapabilities.pageWidthName());
    traceProtocol("REMOTE max %s", clientCapabilities.pageLengthName());
    traceProtocol("REMOTE best vres %s", clientCapabilities.bestVerticalResName());
    traceProtocol("REMOTE format support: %s", (const char*) clientCapabilities.dataFormatsName());
    if (clientCapabilities.ec != EC_DISABLE)
	traceProtocol("REMOTE supports %s", clientCapabilities.ecmName());
    traceProtocol("REMOTE best %s", clientCapabilities.scanlineTimeName());
#ifdef notdef
    // NB: don't say anything since it confuses the naive
    traceProtocol("REMOTE %s PostScript transfer",
	clientInfo.getSupportsPostScript() ? "supports" : "does not support");
#endif

    traceProtocol("USE %s", clientParams.bitRateName());
    if (clientParams.ec != EC_DISABLE) traceProtocol("USE error correction mode");
    return (true);
}

/*
 * Select session parameters according to the info
 * in the TIFF file.  We setup the encoding scheme,
 * page width & length, and vertical-resolution
 * parameters.  If the remote machine is incapable
 * of handling the image, we bail out.
 *
 * Note that we shouldn't be rejecting too many files
 * because we cache the capabilities of the remote machine
 * and use this to image the facsimile.  This work is
 * mainly done to optimize transmission and to reject
 * anything that might sneak by.
 */
FaxSendStatus
FaxServer::sendSetupParams1(TIFF* tif,
    Class2Params& params, const FaxMachineInfo& clientInfo, fxStr& emsg)
{
    uint16_t compression;
    (void) TIFFGetField(tif, TIFFTAG_COMPRESSION, &compression);
    if (params.jp == JP_NONE) {
	if (compression != COMPRESSION_CCITTFAX3 && compression != COMPRESSION_CCITTFAX4) {
	    emsg = fxStr::format("Document is not in a Group 3 or Group 4 compatible"
		" format (compression %u) {E402}", compression);
	    return (send_failed);
	}
	// XXX perhaps should verify samples and bits/sample???
	uint32_t g3opts;
	if (!TIFFGetField(tif, TIFFTAG_GROUP3OPTIONS, &g3opts))
	    g3opts = 0;
	/*
	 * RTFCC lets us ignore our file data format, but our data
	 * format may be based upon a requested data format, and without
	 * re-reading the q file, we won't know if the data format was
	 * requested.  So, RTFCC defeats requested data formatting. :-(
	 */
	if (class2RTFCC || softRTFCC) {
	    /*
	     * Determine the "maximum" compression.
	     *
	     * params.df prior to here represents how faxq formatted the image.
	     * clientCapabilities.df represents what formats the remote supports.
	     *
	     * Ignore what faxq did and send with the "highest" (monochrome) 
	     * compression that both the modem and the remote supports.
	     */
	    if (useDF) {
		/*
		 * The job has been restricted to a specific data format per
		 * JobControls or something similar - that value is now 
		 * found in clientParams.df.
		 */
		params.df = fxmin(clientParams.df, params.df);
	    } else {
		params.df = 0;
		u_int bits = clientCapabilities.df;
		bits &= BIT(DF_JBIG+1)-1;		// cap at JBIG, only deal with monochrome
		while (bits) {			// puts params.df to the "best" support by the remote
		    bits >>= 1;
		    if (bits) params.df++;
		}
	    }
	    // Class 2 RTFCC doesn't support JBIG
	    if (params.df == DF_JBIG && (!modem->supportsJBIG() || (params.ec == EC_DISABLE) || class2RTFCC))
		params.df = DF_2DMMR;
	    // even if RTFCC supported uncompressed mode (and it doesn't)
	    // it's likely that the remote was incorrect in telling us it does
	    if (params.df == DF_2DMRUNCOMP) params.df = DF_2DMR;
	    // don't let RTFCC cause problems with restricted modems...
	    if (params.df == DF_2DMMR && (!modem->supportsMMR() || (params.ec == EC_DISABLE) || !(clientCapabilities.df & BIT(DF_2DMMR))))
		params.df = DF_2DMR;
	    if (params.df == DF_2DMR && (!modem->supports2D() || !(clientCapabilities.df & BIT(DF_2DMR))))
		params.df = DF_1DMH;

	    if (tiff2faxCmd.length() && formatSize[0] != 0 && formatSize[0] != 0 && formatSize[0] != 0) {
		/*
		 * T.4 and T.6 compress poorly when there are frequent alternations between
		 * black and white (i.e. a dithered photograph).  Thus, in some cases the
		 * "highest" compression may actually be MH.  Since MH support is requisite 
		 * we can switch "down" to it freely.
		 *
		 * If MH is tighter than MMR then it would be extremely unlikely that MH would
		 * not also be tighter than MR.  So we don't consider dropping from MMR to MR.
		 *
		 * JBIG will virtually always outcompress any other format, so we don't consider
		 * another format when we can use JBIG.
		 */
		if ((params.df == DF_2DMMR || params.df == DF_2DMR) && formatSize[0] < formatSize[1]) 
		    params.df = DF_1DMH;
	    }
	} else {
	    if (compression == COMPRESSION_CCITTFAX4) {
		if (!clientInfo.getSupportsMMR()) {
		    emsg = "Document was encoded with 2DMMR, but client does not support this data format {E403}";
		    return (send_reformat);
		}
		if (!modem->supportsMMR()) {
		    emsg = "Document was encoded with 2DMMR, but modem does not support this data format {E404}";
		    return (send_reformat);
		}
		if (params.ec == EC_DISABLE) {
		    emsg = "Document was encoded with 2DMMR, but ECM is not being used. {E405}";
		    return (send_reformat);
		}
		params.df = DF_2DMMR;
	    } else if (g3opts & GROUP3OPT_2DENCODING) {
		if (!clientInfo.getSupports2DEncoding()) {
		    emsg = "Document was encoded with 2DMR, but client does not support this data format {E406}";
		    return (send_reformat);
		}
		if (!modem->supports2D()) {
		    emsg = "Document was encoded with 2DMR, but modem does not support this data format {E407}";
		    return (send_reformat);
		}
		params.df = DF_2DMR;
	    } else
		params.df = DF_1DMH;
	}
    } else {
	// send JPEG
	if (params.jp == JP_COLOR) {
	    if (compression != COMPRESSION_NONE) {
		emsg = "Invalid compression for color JPEG fax request. {E421}";
		return (send_failed);
	    }
	    uint16_t w;
	    if (!TIFFGetField(tif, TIFFTAG_PHOTOMETRIC, &w) || (w != PHOTOMETRIC_RGB)) {
		emsg = "Invalid photometric for color JPEG fax request. {E422}";
		return (send_failed);
	    }
	} else {
	    emsg = "Requested JPEG parameters are unsupported. {E420}";
	    return (send_failed);
	}
    }

    /*
     * Try to deduce the resolution of the image
     * image.  This can be problematical for arbitrary TIFF
     * images 'cuz vendors sometimes don't give the units.
     * We, however, can depend on the info in images that
     * we generate 'cuz we're careful to include valid info.
     */
    float yres;
    if (TIFFGetField(tif, TIFFTAG_YRESOLUTION, &yres)) {
	short resunit = RESUNIT_INCH;		// TIFF spec default
	(void) TIFFGetField(tif, TIFFTAG_RESOLUTIONUNIT, &resunit);
	if (resunit == RESUNIT_INCH)
	    yres /= 25.4;
	if (resunit == RESUNIT_NONE)
	    yres /= 720.0;			// postscript units ?
    } else {
	/*
	 * No vertical resolution is specified, try
	 * to deduce one from the image length.
	 */
	u_long l;
	TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &l);
	yres = (l < 1450 ? 3.85 : 7.7);		// B4 at 98 lpi is ~1400 lines
    }
    float xres;
    if (TIFFGetField(tif, TIFFTAG_XRESOLUTION, &xres)) {
	short resunit = RESUNIT_INCH;		// TIFF spec default
	(void) TIFFGetField(tif, TIFFTAG_RESOLUTIONUNIT, &resunit);
	if (resunit == RESUNIT_INCH)
	    xres /= 25.4;
	if (resunit == RESUNIT_NONE)
	    xres /= 720.0;			// postscript units ?
    } else {
	/*
	 * No horizontal resolution is specified, try
	 * to deduce one from the image width.
	 */
	u_long l;
	TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &l);
	xres = (l < 1729 ? 8 : 16);		// R8 = 8 l/mm, R16 = 16 l/mm
    }
    /*
     * JPEG fax requires square resolutions.  The job preparation state should have
     * got this right, but we double-check it here.
     */
    if (params.jp == JP_COLOR) {
	if (xres != yres) {
	    emsg = "JPEG fax requires square resolutions. {E428}";
	    return (send_reformat);
	}
	if (xres < 7.6 || xres > 8.3) {
	    emsg = fxStr::format("Unsupported JPEG fax resolution %g l/mm. {E429}", xres);
	    return (send_reformat);
	}
	params.vr = VR_FINE;
    } else {
	if (yres >= 15.) {
	    if (xres > 10) {
		if (!(clientInfo.getSupportsVRes() & VR_R16)) {
		    emsg = fxStr::format("Hyperfine resolution document is not supported"
			" by client, image resolution %g x %g lines/mm {E408}", xres, yres);
		    return (send_reformat);
		}
		if (!modem->supportsVRes(20)) {	// "20" is coded for R16
		    emsg = fxStr::format("Hyperfine resolution document is not supported"
			" by modem, image resolution %g x %g lines/mm {E409}", xres, yres);
		    return (send_reformat);
		}
		params.vr = VR_R16;
	    } else {
		if (!((clientInfo.getSupportsVRes() & VR_R8) || (clientInfo.getSupportsVRes() & VR_200X400))) {
		    emsg = fxStr::format("Superfine resolution document is not supported"
			" by client, image resolution %g lines/mm {E410}", yres);
		    return (send_reformat);
		}
		if (!modem->supportsVRes(yres)) {
		    emsg = fxStr::format("Superfine resolution document is not supported"
			" by modem, image resolution %g lines/mm {E411}", yres);
		    return (send_reformat);
		}
		if (clientInfo.getSupportsVRes() & VR_R8) params.vr = VR_R8;
		else params.vr = VR_200X400;
	    }
	} else if (yres >= 10.) {
	    if (!(clientInfo.getSupportsVRes() & VR_300X300)) {
		emsg = fxStr::format("300x300 resolution document is not supported"
		    " by client, image resolution %g lines/mm {E412}", yres);
		return (send_reformat);
	    }
	    if (!modem->supportsVRes(yres)) {
		emsg = fxStr::format("300x300 resolution document is not supported"
		    " by modem, image resolution %g lines/mm {E413}", yres);
		return (send_reformat);
	    }
	    params.vr = VR_300X300;
	} else if (yres >= 7.) {
	    if (!((clientInfo.getSupportsVRes() & VR_FINE) || (clientInfo.getSupportsVRes() & VR_200X200))) {
		emsg = fxStr::format("High resolution document is not supported"
		    " by client, image resolution %g lines/mm {E414}", yres);
		return (send_reformat);
	    }
	    if (!modem->supportsVRes(yres)) {
		emsg = fxStr::format("High resolution document is not supported"
		    " by modem, image resolution %g lines/mm {E415}", yres);
		return (send_reformat);
	    }
	    if (clientInfo.getSupportsVRes() & VR_FINE) params.vr = VR_FINE;
	    else params.vr = VR_200X200;
	} else
	    params.vr = VR_NORMAL;		// support required
    }

    /*
     * Max page width depends on the resolution, so this must come after VR.
     * maxPageWidth is stored in normal values, so we must compare against
     * the corresponding normal resolution page width.
     */
    uint32_t w;
    (void) TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w);
    double rf = (params.vr == VR_R16 ? 2 : params.vr == VR_300X300 ? 1.5 : 1);
    if (w > (clientInfo.getMaxPageWidthInPixels()*rf)) {
	emsg = fxStr::format("Client does not support document page width"
		", max remote page width %g pixels, image width %lu pixels {E416}",
		(uint32_t) (clientInfo.getMaxPageWidthInPixels()*rf), w);
	return (send_reformat);
    }
    if (!modem->supportsPageWidth((u_int) w, params.vr)) {
	static const double widths[8] = {
	    1728,	// 1728 in 215 mm line
	    2048,	// 2048 in 255 mm line
	    2432,	// 2432 in 303 mm line
	    1216,	// 1216 in 151 mm line
	    864,	// 864 in 107 mm line
	    0,
	    0,
	    0,
	};
	emsg = fxStr::format("Modem does not support document page width"
		", max page width %g pixels, image width %lu pixels {E417}",
		widths[modem->getBestPageWidth()&7]*rf, w);
	return (send_reformat);
    }
    // NB: only common values
    params.wd = ((uint32_t) (w/rf) <= 1728 ? WD_A4 : (uint32_t) (w/rf) <= 2048 ? WD_B4 : WD_A3);

    /*
     * Select page length according to the image size and
     * vertical resolution.  Note that if the resolution
     * info is bogus, we may select the wrong page size.
     * Note also that we're a bit lenient in places here
     * to take into account sloppy coding practice (e.g.
     * using 200 dpi for high-res facsimile.
     */
    if (clientInfo.getMaxPageLengthInMM() != (u_short) -1 || !modem->supportsPageLength((u_int) -1)) {
	u_long h = 0;
	(void) TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h);
	float len = h / yres;			// page length in mm
	if ((int) len > clientInfo.getMaxPageLengthInMM()) {
	    emsg = fxStr::format("Client does not support document page length"
			  ", max remote page length %d mm"
			  ", image length %lu rows (%.2f mm) {E418}",
		clientInfo.getMaxPageLengthInMM(), h, len);
	    return (send_reformat);
	}
	if (!modem->supportsPageLength((u_int) len)) {
	    static const char* lengths[4] = {
		"297",		// A4 paper
		"364",		// B4 paper
		"<unlimited>",	// unlimited
		"<undefined>",	// US letter (used internally)
	    };
	    emsg = fxStr::format("Modem does not support document page length"
			  ", max page length %s mm"
			  ", image length %lu rows (%.2f mm) {E419}",
		lengths[modem->getBestPageLength()&3], h, len);
	    return (send_reformat);
	}
	// 330 is chosen 'cuz it's half way between A4 & B4 lengths
	params.ln = (len < 330 ? LN_A4 : LN_B4);
    } else
	params.ln = LN_INF;
    /*
     * Scanline time varies depending on the remote capabilities,
     * upon ECM usage, and upon the resolution.  The MS2 parameters
     * are not available in DCS.
     */
    if (params.st == ST_40MS2) {
        if (params.vr) params.st = ST_20MS;
        else params.st = ST_40MS;
    }
    if (params.st == ST_20MS2) {
        if (params.vr) params.st = ST_10MS;
        else params.st = ST_20MS;
    }
    if (params.st == ST_10MS2) {
        if (params.vr) params.st = ST_5MS;
        else params.st = ST_10MS;
    }
    if (params.ec != EC_DISABLE) params.st = ST_0MS;	// T.30 Table 2 Note 8 - ECM imposes 0ms/scanline
    return (send_ok);
}

FaxSendStatus
FaxServer::sendSetupParams(TIFF* tif, Class2Params& params, const FaxMachineInfo& clientInfo, fxStr& emsg)
{
    FaxSendStatus status = sendSetupParams1(tif, params, clientInfo, emsg);
    if (status == send_ok) {
	traceProtocol("USE %s", params.pageWidthName());
	traceProtocol("USE %s", params.pageLengthName());
	traceProtocol("USE %s", params.verticalResName());
	traceProtocol("USE %s", params.dataFormatName());
	traceProtocol("USE %s", params.scanlineTimeName());
    } else if (status == send_reformat) {
	traceServer(emsg);
    } else if (status == send_failed) {
	traceServer("REJECT: " | emsg);
    }
    return (status);
}

/*
 * Send Notification Support.
 */

void
FaxServer::notifyPageSent(FaxRequest& req, const char*)
{
    req.npages++;			// count transmitted page
    if (req.nocountcover) {
	req.nocountcover--;
	req.skippedpages--;
    }
    /*
     * If the system is busy then req.writeQFile may not return quickly.
     * Thus we run it in a child process and move on.
     */
    pid_t pid = req.writeQFilePid;
    req.writeQFilePid = fork();
    switch (req.writeQFilePid) {
	case 0:
	    if (pid > 0) (void) Sys::waitpid(pid);	// the calls to writeQFile must be sequenced serially
	    req.writeQFile();		// update q file for clients
	    traceProtocol("SEND FAX (%s): FROM %s TO %s (page %u of %u sent in %s)"
		, (const char*) req.commid
		, (const char*) req.mailaddr
		, (const char*) req.external
		, req.npages
		, req.totpages
		, fmtTime(getPageTransferTime())
	    );
	    sleep(1);               // XXX give parent time
	    exit(0);
	case -1:
	    logError("Can not fork for non-priority processing.");
	    req.writeQFile();		// update q file for clients
	    traceProtocol("SEND FAX (%s): FROM %s TO %s (page %u of %u sent in %s)"
		, (const char*) req.commid
		, (const char*) req.mailaddr
		, (const char*) req.external
		, req.npages
		, req.totpages
		, fmtTime(getPageTransferTime())
	    );
	    break;
	default:
	    Dispatcher::instance().startChild(req.writeQFilePid, this);
	    break;
    }
}

/*
 * Handle notification that a document has been successfully
 * transmitted.  We remove the file from the request array so
 * that it's not resent if the job is requeued.
 *
 * NB: Proper operation of the reference counting scheme used
 *     to handle delayed-removal of the imaged documents requires
 *     that the central scheduler be notified when a document
 *     is transmitted (so that it can update its global table
 *     of document uses); this is normally done in the derived
 *     class by overriding this method.
 */
void
FaxServer::notifyDocumentSent(FaxRequest& req, u_int fi)
{
    const FaxItem& freq = req.items[fi];
    if (freq.op != FaxRequest::send_fax) {
	logError("notifyDocumentSent called for non-TIFF file");
	return;
    }
    traceProtocol("SEND FAX (%s): FROM %s TO %s (%s sent in %s)"
	, (const char*) req.commid
	, (const char*) req.mailaddr
	, (const char*) req.external
	, (const char*) freq.item
	, fmtTime(getFileTransferTime())
    );
    logInfo("SEND FAX: JOB %s SENT in %s"
	, (const char*) req.jobid
	, fmtTime(getFileTransferTime())
    );

    // we must ensure that the previous writeQFiles have completed
    if (req.writeQFilePid > 0) (void) Sys::waitpid(req.writeQFilePid);

    req.items.remove(fi);
    req.writeQFile();
}
