/*	$Id: Class1Recv.c++ 1133 2012-12-26 06:51:02Z faxguy $ */
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

/*
 * EIA/TIA-578 (Class 1) Modem Driver.
 *
 * Receive protocol.
 */
#include <stdio.h>
#include <sys/time.h>
#include <pthread.h>
#include "Class1.h"
#include "ModemConfig.h"
#include "HDLCFrame.h"
#include "StackBuffer.h"		// XXX

#include "config.h"
#include "t.30.h"
#include "Sys.h"
#include "config.h"
#include "FaxParams.h"

extern timeval operator-(timeval src1, timeval src2);

/*
 * Tell the modem to answer the phone.  We override
 * this method so that we can force the terminal's
 * flow control state to be setup to our liking.
 */
CallType
Class1Modem::answerCall(AnswerType type, fxStr& emsg, const char* number, bool doSecondAnswer)
{
    // Reset modemParams.br to non-V.34 settings.  If V.8 handshaking
    // succeeds, then it will be changed again.
    modemParams.br = nonV34br;

    if (flowControl == FLOW_XONXOFF)
	setXONXOFF(FLOW_NONE, FLOW_NONE, ACT_FLUSH);
    return ClassModem::answerCall(type, emsg, number, (serviceType == SERVICE_CLASS10 && conf.secondAnswerCmd != ""));
}

/*
 * Process an answer response from the modem.
 * Since some Class 1 modems do not give a connect
 * message that distinguishes between DATA and FAX,
 * we override the default handling of "CONNECT"
 * message here to force the high level code to
 * probe further.
 */
const AnswerMsg*
Class1Modem::findAnswer(const char* s)
{
    static const AnswerMsg answer[2] = {
    { "CONNECT ", 8,
      FaxModem::AT_NOTHING, FaxModem::OK, FaxModem::CALLTYPE_DATA },
    { "CONNECT",  7,
      FaxModem::AT_NOTHING, FaxModem::OK, FaxModem::CALLTYPE_UNKNOWN },
    };
    return strneq(s, answer[0].msg, answer[0].len) ? &answer[0] :
	   strneq(s, answer[1].msg, answer[1].len) ? &answer[1] :
	      FaxModem::findAnswer(s);
}

/*
 * Begin the receive protocol.
 */
bool
Class1Modem::recvBegin(FaxSetup* setupinfo, fxStr& emsg)
{
    setInputBuffering(false);
    prevPage = 0;				// no previous page received
    pageGood = false;				// quality of received page
    messageReceived = false;			// expect message carrier
    recvdDCN = false;				// haven't seen DCN
    lastPPM = FCF_DCN;				// anything will do
    PPMseq[0] = 0;
    PPMseq[1] = 0;
    sendCFR = false;				// TCF was not received
    lastMCF = 0;				// no MCF sent yet
    lastRTN = 0;				// no RTN sent yet
    capsUsed = 0;				// no DCS or CTC seen yet
    dataSent = 0;				// start with a clean slate...
    dataMissed = 0;				// unfortunately, this will reset after EOM
    pageDataMissed = 0;
    senderSkipsV29 = false;
    senderConfusesRTN = false;
    senderConfusesPIN = false;
    senderHasV17Trouble = false;
    senderFumblesECM = false;
    sentRTN = false;

    if (setupinfo) {
	senderFumblesECM = setupinfo->senderFumblesECM;
	senderConfusesRTN = setupinfo->senderConfusesRTN;
	senderConfusesPIN = setupinfo->senderConfusesPIN;
	senderSkipsV29 = setupinfo->senderSkipsV29;
	senderHasV17Trouble = setupinfo->senderHasV17Trouble;
    }

    fxStr nsf;
    encodeNSF(nsf, HYLAFAX_VERSION);

    if (!isSSLFax && useV34 && !gotCTRL) waitForDCEChannel(true);	// expect control channel

    FaxParams dis = modemDIS();

    if (setupinfo && setupinfo->senderFumblesECM && !useV34) {
	protoTrace("This sender fumbles ECM.  Concealing ECM support.");
	dis.setBit(FaxParams::BITNUM_ECM, false);
	dis.setBit(FaxParams::BITNUM_2DMMR, false);
	dis.setBit(FaxParams::BITNUM_JBIG_BASIC, false);
	dis.setBit(FaxParams::BITNUM_JBIG_L0, false);
	dis.setBit(FaxParams::BITNUM_JPEG, false);
	dis.setBit(FaxParams::BITNUM_FULLCOLOR, false);
    }

    if (senderSkipsV29 && senderHasV17Trouble) {
	dis.setBit(FaxParams::BITNUM_SIGRATE_14, false);	// disable V.17 support
	protoTrace("This sender skips V.29 and has trouble with V.17.  Concealing V.17 support.");
    }
    if (conf.class1RestrictPoorSenders && setupinfo && setupinfo->senderDataSent && 
	(u_int) (setupinfo->senderDataMissed * 100 / setupinfo->senderDataSent) > conf.class1RestrictPoorSenders) {
	dis.setBit(FaxParams::BITNUM_VR_FINE, false);	// disable fine resolution support
	dis.setBit(FaxParams::BITNUM_VR_R8, false);	// disable superfine resolution support
	dis.setBit(FaxParams::BITNUM_VR_300X300, false);// disable 300x300 dpi support
	dis.setBit(FaxParams::BITNUM_VR_R16, false);	// disable hyperfine resolution support
	dis.setBit(FaxParams::BITNUM_JPEG, false);	// disable JPEG support
	dis.setBit(FaxParams::BITNUM_FULLCOLOR, false);	// disable color JPEG support
	protoTrace("This sender exhibits poor call audio quality.  Concealing resolution and color support.");
    }

    bool ok = FaxModem::recvBegin(setupinfo, emsg) && recvIdentification(
	0, fxStr::null,
	0, fxStr::null,
	conf.class1SendNSF ? FCF_NSF|FCF_RCVR : 0, nsf,
	FCF_CSI|FCF_RCVR, lid,
	FCF_DIS|FCF_RCVR, dis,
	conf.class1RecvIdentTimer, false, emsg);
    if (setupinfo) {
	/*
	 * Update FaxMachine info...
	 */
	setupinfo->senderFumblesECM = senderFumblesECM;
	setupinfo->senderConfusesRTN = senderConfusesRTN;
	setupinfo->senderConfusesPIN = senderConfusesPIN;
	setupinfo->senderSkipsV29 = senderSkipsV29;
	setupinfo->senderHasV17Trouble = senderHasV17Trouble;
	setupinfo->senderDataSent = dataSent;
	setupinfo->senderDataMissed = dataMissed;
    }
    return (ok);
}

/*
 * Begin the receive protocol after an EOM signal.
 */
bool
Class1Modem::recvEOMBegin(FaxSetup* setupinfo, fxStr& emsg)
{
    /*
     * We must raise the transmission carrier to mimic the state following ATA.
     */
    if (!isSSLFax && !useV34) {
	pause(conf.t2Timer);	// T.30 Fig 5.2B requires T2 to elapse
	if (!(atCmd(thCmd, AT_NOTHING) && atResponse(rbuf, 0) == AT_CONNECT)) {
	    emsg = "Failure to raise V.21 transmission carrier. {E101}";
	    protoTrace(emsg);
	    return (false);
	}
    }
    return Class1Modem::recvBegin(setupinfo, emsg);
}

/*
 * Transmit local identification and wait for the
 * remote side to respond with their identification.
 */
bool
Class1Modem::recvIdentification(
    u_int f1, const fxStr& pwd,
    u_int f2, const fxStr& addr,
    u_int f3, const fxStr& nsf,
    u_int f4, const fxStr& id,
    u_int f5, FaxParams& dics,
    u_int timer, bool notransmit, fxStr& emsg)
{
    u_int t1 = howmany(timer, 1000);		// in seconds
    time_t start = Sys::now();
    HDLCFrame frame(conf.class1FrameOverhead);
    bool framesSent = false;
    u_int onhooks = 0;
    bool gotCNG = false;
    bool repeatCED = true;
    bool readPending = false;
    bool fullduplex = false;

    /* Here, "features" defines HylaFAX-specific features. */
    u_char features[1] = { 0 };
    if (conf.class1SSLFaxSupport) features[0] |= frameRev[FaxParams::FEATURE_SSLFAX];	// Enable "SSL Fax"

    emsg = "No sender protocol (T.30 T1 timeout) {E102}";
    if (!notransmit) {
	/*
	 * Transmit (PWD) (SUB) (CSI) DIS frames when the receiving
	 * station or (PWD) (SEP) (CIG) DTC when initiating a poll.
	 */
	if (f1) {
	    startTimeout(7550);
	    framesSent = sendFrame(f1, pwd, false);
	    stopTimeout("sending PWD frame");
	} else if (f2) {
	    startTimeout(7550);
	    framesSent = sendFrame(f2, addr, false);
	    stopTimeout("sending SUB/SEP frame");
	} else if (f3) {
	    startTimeout(7550);
	    framesSent = sendFrame(f3, (const u_char*)HYLAFAX_NSF, (const u_char*) features, nsf, false);
	    stopTimeout("sending NSF frame");
	} else {
	    startTimeout(7550);
	    framesSent = sendFrame(f4, id, false);
	    stopTimeout("sending CSI/CIG frame");
	}
    }
    for (;;) {
	if (framesSent && !notransmit) {
	    if (f1) {
		startTimeout(7550);
		framesSent = sendFrame(f2, addr, false);
		stopTimeout("sending SUB/SEP frame");
	    }
	    if (framesSent && f2) {
		startTimeout(7550);
		framesSent = sendFrame(f3, (const u_char*)HYLAFAX_NSF, (const u_char*) features, nsf, false);
		stopTimeout("sending NSF frame");
	    }
	    if (framesSent && f3) {
		startTimeout(7550);
		framesSent = sendFrame(f4, id, false);
		stopTimeout("sending CSI/CIG frame");
	    }
	    if (framesSent) {
		startTimeout(7550);
		framesSent = sendFrame(f5, dics);
		stopTimeout("sending DIS/DCS frame");
	    }
	}
	if (lastResponse == AT_FRH3) {
	    if (!waitFor(AT_CONNECT, 2*1000)) {
		emsg = "No CONNECT after +FRH:3";
		protoTrace(emsg);
		return (false);
	    }
	    framesSent = true;
	    readPending = true;
	}
	if (framesSent || notransmit) {
	    /*
	     * Wait for a response to be received.
	     *
	     * If somehow we missed the sender's initial response they should
	     * repeat their response after T4 lapses, somewhere between 2550
	     * and 5175 ms later (more probably between 2550 and 3450 ms).
	     * But if they somehow missed our initial signal then they could
	     * be expecting us to repeat them before T2 lapses (6 +/- 1s)
	     * before they give up.
	     *
	     * So, historically we waited T4 here and then changed it to T2
	     * because our T4 can be shorter than theirs and thus we'll miss
	     * their repeated response.  But, waiting T2 can also be a problem
	     * in the event that they did not detect our initial signal enough
	     * to compose a response or we did not detect their initial response
	     * because then we'll both be waiting T2 and may likely end up
	     * signaling over each other until giving up.
	     *
	     * Unfortunately, T4 and T2 overlap between 5000 and 5175 ms.
	     * However, the sender's T4 is likely the automatic values, so our
	     * wait here probably needs to be at least 3450 but less than 5000
	     * ms. Furthermore, the wait time should be somewhat variable so
	     * that we don't always happen to be be waiting the same as them.
	     *
	     * In order to accomplish this and avoid yet another configuration
	     * feature we'll wait midway between our T4 and T2 settings (so 5050
	     * ms, by default) lessened by a random time up to 550 ms.
	     */
	    u_int wait = (conf.t2Timer + conf.t4Timer)/2 - (rand() % 550);
	    if (recvFrame(frame, FCF_RCVR, wait, readPending, true, false, 0, false, true, FCF_TSI)) {
		do {
		    /*
		     * Verify a DCS command response and, if
		     * all is correct, receive phasing/training.
		     */
		    bool gotframe = true;
		    while (gotframe) {
			if (!recvDCSFrames(frame)) {
			    switch (frame.getFCF()) {
				case FCF_DCN:
				    if (sentRTN && curcap->br != BR_2400) {
					protoTrace("This sender appears to not respond properly to RTN.");
					senderConfusesRTN = true;
				    }
				    emsg = "RSPREC error/got DCN (sender abort) {E103}";
				    recvdDCN = true;
				    return (false);
				case FCF_MPS:
				case FCF_EOP:
				case FCF_EOM:
				    if (!switchingPause(emsg)) return (false);
				    transmitFrame(signalSent);
				    traceFCF("RECV send", (u_char) signalSent[2]);
				    if (signalSent[2] == FCF_RTN) gotframe = false;
				    break;
				case FCF_FTT:
				    /* probably just our own echo */
				    break;
				case FCF_CRP:
				    if (!switchingPause(emsg)) return (false);
				    /* do nothing here, just let us repeat NSF, CSI, DIS */
				    break;
				default:	// XXX DTC/DIS not handled
				    emsg = "RSPREC invalid response received {E104}";
				    break;
			    }
			    break;
			}
			gotframe = false;
			if (recvTraining()) {
			    if (fullduplex) {
				// Turn full-duplex off now since we seem to be synced.
				checkReadOnWrite = true;
				atCmd("AT+FAR=1", AT_OK);
			    }
			    emsg = "";
			    return (true);
			} else {
			    if (lastResponse == AT_FRH3 && waitFor(AT_CONNECT, conf.t2Timer)) {
				// It's unclear if we are in "COMMAND REC" or "RESPONSE REC" mode,
				// but since we are already detecting the carrier, wait the longer.
				gotframe = recvFrame(frame, FCF_RCVR, conf.t2Timer, true, false, false);
				if (!gotframe && !frame.getLength() && lastResponse == AT_NOCARRIER) {
				    /*
				     * The modem may have incorrectly detected V.21 HDLC.
				     * The TCF signal is yet to come.  So, try again.
				     */
				    if (recvTraining()) {
					if (fullduplex) {
					    // Turn full-duplex off now since we seem to be synced.
					    checkReadOnWrite = true;
					    atCmd("AT+FAR=1", AT_OK);
					}
					emsg = "";
					return (true);
				    }
				    if (lastResponse == AT_FRH3 && waitFor(AT_CONNECT, conf.t2Timer)) {
					// It's unclear if we are in "COMMAND REC" or "RESPONSE REC" mode,
					// but since we are already detecting the carrier, wait the longer.
					gotframe = recvFrame(frame, FCF_RCVR, conf.t2Timer, true, true, false);
				    }
				}
				lastResponse = AT_NOTHING;
			    }
			}
		    }
		    if (gotframe) break;	// where recvDCSFrames fails without DCN
		    emsg = "Failure to train modems {E105}";
		    /*
		     * Reset the timeout to insure the T1 timer is
		     * used.  This is done because the adaptive answer
		     * strategy may setup a shorter timeout that's
		     * used to wait for the initial identification
		     * frame.  If we get here then we know the remote
		     * side is a fax machine and so we should wait
		     * the full T1 timeout, as specified by the protocol.
		     */
		    t1 = howmany(conf.t1Timer, 1000);
		} while (recvFrame(frame, FCF_RCVR, conf.t2Timer, false, true, false));
	    }
	    if (lastResponse == AT_FRH3) {
		if (!waitFor(AT_CONNECT, 2*1000)) {
		    emsg = "No CONNECT after +FRH:3";
		    protoTrace(emsg);
		    return (false);
		}
		framesSent = true;
		notransmit = true;
		readPending = true;
		continue;
	    }
	    if (conf.class1FullDuplexTrainingSync && !fullduplex) {
		/*
		 * The sender isn't syncing with us easily.  Enable full-duplex
		 * mode to detect V.21 HDLC signals while transmitting to assist us.
		 * We turn checkReadOnWrite off because we expect there to be times
		 * when the +FRH:3 response comes before we're done writing HDLC
		 * data to the modem, and we need to pick up the response afterwards.
		 */
		atCmd("AT+FAR=2", AT_OK);
		fullduplex = true;
		checkReadOnWrite = false;
	    }
	}
	readPending = false;
	notransmit = false;
	if (wasTimeout() && conf.class1PhaseBRecvTimeoutCmd.length()) {
	    /*
	     * For some unintelligible reason, some callers require us
	     * to do something actively (like Google Voice can require
	     * the callee to press "1") in order to bridge the call.
	     */
	    atCmd(conf.class1PhaseBRecvTimeoutCmd, AT_OK);
	}
	if (gotEOT && ++onhooks > conf.class1HookSensitivity) {
	    emsg = "RSPREC error/got EOT {E106}";
	    return (false);
	}
	/*
	 * We failed to send our frames or failed to receive
	 * DCS from the other side.  First verify there is
	 * time to make another attempt...
	 */
	if ((u_int) Sys::now()-start >= t1) {
	    if (fullduplex) {
		// Turn full-duplex off since we're giving up.
		checkReadOnWrite = true;
		atCmd("AT+FAR=1", AT_OK);
	    }
	    return (false);
	}
	if (lastResponse == AT_FRCNG) gotCNG = true;
	if (frame.getFCF() != FCF_CRP) {
	    /*
	     * Historically we waited "Class1TrainingRecovery" (1500 ms)
	     * at this point to try to try to avoid retransmitting while
	     * the sender is also transmitting.  Sometimes it proved to be
	     * a faulty approach.  Really what we're trying to do is to
	     * not be transmitting at the same time as the other end is.
	     * The best way to do that is to make sure that there is
	     * silence on the line, and  we do that with Class1SwitchingCmd.
	     */
	    if (!switchingPause(emsg, fullduplex ? 1 : 3)) {
		return (false);
	    }
	}
	if (!notransmit) {
	    /*
	     * Retransmit ident frames.
	     */
	    if (gotCNG && repeatCED) {
		if (!(atCmd(conf.CEDCmd, AT_NOTHING) && (atResponse(rbuf, 0) == AT_CONNECT || lastResponse == AT_FRH3))) {
		    emsg = "Failure to raise V.21 transmission carrier. {E101}";
		    protoTrace(emsg);
		    return (false);
		}
		if (lastResponse == AT_CONNECT) {
		    if (f1)
			framesSent = sendFrame(f1, pwd, false);
		    else if (f2)
			framesSent = sendFrame(f2, addr, false);
		    else if (f3)
			framesSent = sendFrame(f3, (const u_char*)HYLAFAX_NSF, (const u_char*) features, nsf, false);
		    else
			framesSent = sendFrame(f4, id, false);
		}
	    } else {
		if (f1)
		    framesSent = transmitFrame(f1, pwd, false);
		else if (f2)
		    framesSent = transmitFrame(f2, addr, false);
		else if (f3)
		    framesSent = transmitFrame(f3, (const u_char*)HYLAFAX_NSF, (const u_char*) features, nsf, false);
		else
		    framesSent = transmitFrame(f4, id, false);
	    }
	    if (lastResponse == AT_FRH3) {
		if (!waitFor(AT_CONNECT, 2*1000)) {
		    emsg = "No CONNECT after +FRH:3";
		    protoTrace(emsg);
		    return (false);
		}
		framesSent = true;
		notransmit = true;
		readPending = true;
	    }
	    gotCNG = false;
	    repeatCED = !repeatCED;
	}
    }
    return (false);
}

bool
Class1Modem::startSSLFax()
{
#if defined(HAVE_SSL)
    if (!isSSLFax && useSSLFax && remoteCSAType == 0x40 && remoteCSAinfo.length()) {
	protoTrace("Connecting to %s for SSL Fax transmission.", (const char*) remoteCSAinfo);
	SSLFax sslfax;
	sslFaxProcess = sslfax.startClient(remoteCSAinfo, sslFaxPasscode, rtcRev, conf.class1SSLFaxServerTimeout);
	if (sslFaxProcess.emsg != "") protoTrace("SSL Fax: \"%s\"", (const char*) sslFaxProcess.emsg);
	if (!sslFaxProcess.server) {
	    protoTrace("SSL Fax connection failed.");
	    if (!conf.class1SSLFaxInfo.length() && !conf.class1SSLFaxProxy.length()) useSSLFax = false;
	} else {
	    protoTrace("SSL Fax connection was successful.");
	    isSSLFax = true;
	    storedBitrate = params.br;
	    params.br = BR_SSLFAX;
	    return (true);
	}
    }
#endif
    return (false);
}

/*
 * Receive DCS preceded by any optional frames.
 * Although technically this is "RESPONSE REC",
 * we wait T2 due to empirical evidence of that need.
 */
bool
Class1Modem::recvDCSFrames(HDLCFrame& frame)
{
    fxStr s;
    do {
	traceFCF("RECV recv", frame.getFCF());
	switch (frame.getFCF()) {
	case FCF_PWD:
	    recvPWD(decodePWD(s, frame));
	    break;
	case FCF_SUB:
	    recvSUB(decodePWD(s, frame));
	    break;
	case FCF_TSI:
	    recvTSI(decodeTSI(s, frame));
	    break;
	case FCF_TSA:
	    {
		fxStr s;
		decodeCSA(s, frame);
		protoTrace("REMOTE TSA \"%s\", type 0x%X", (const char*) s, remoteCSAType);
		if (remoteCSAType == 0x40 && s.length() > 6 && strncmp((const char*) s, "ssl://", 6) == 0) {
		    // Looks like this is an SSL Fax sender.
		    if (conf.class1SSLFaxSupport) useSSLFax = true;
		    u_int atpos = s.next(6, '@');
		    if (atpos < s.length()) {
			sslFaxPasscode = s.extract(6, atpos-6);
			remoteCSAinfo = s.tail(s.length() - atpos - 1);
		    } else {
			sslFaxPasscode = "";
			remoteCSAinfo = s.tail(s.length() - 6);
		    }
		}
	    }
	    break;
	case FCF_DCS:
	    if (frame.getFrameDataLength() < 4) return (false);	// minimum acceptable DCS frame size
	    processDCSFrame(frame);
	    sentRTN = false;	// reset it, the sender appears to have behaved appropriately
	    break;
	case FCF_DCN:
	    gotEOT = true;
	    recvdDCN = true;
	    break;
	}
	/*
	 * Sometimes echo is bad enough that we hear ourselves.  So if we hear DIS, we're probably
	 * hearing ourselves.  Just ignore it and listen again.
	 */
    } while (!recvdDCN && (frame.moreFrames() || frame.getFCF() == FCF_DIS) && recvFrame(frame, FCF_RCVR, conf.t2Timer, false, useSSLFax ? false : true));
    if ((!frame.isOK() || wasTimeout()) && useSSLFax) {
	/*
	 * If we received a corrupt DCS but a valid TSA before that, then we can start the
	 * SSL Fax connection, repeat Phase B, and get DIS through the SSL Fax connection.
	 * This is also why we don't do CRP in recvFrame, above, if we got a valid TSA.
	 * We do have to wait for the sender to start their listener, though, hence the
	 * switchingPause.  Skipping TCF reception like this will mean that the modem isn't
	 * trained, if needed later.
	 */
	 fxStr emsg;
	 if (switchingPause(emsg, 3)) startSSLFax();
    }
    return (frame.isOK() && frame.getFCF() == FCF_DCS);
}

/*
 * Receive training and analyze TCF.
 */
bool
Class1Modem::recvTraining(bool respond)
{
    if (isSSLFax || useV34) {
	sendCFR = true;
	return (true);
    }
    /*
     * It is possible (and with some modems likely) that the sending
     * system has not yet dropped its V.21 carrier because the modem may
     * simply signal OK when the HDLC frame is received completely and not
     * not wait for the carrier drop to occur.  We don't follow the strategy 
     * documented in T.31 Appendix II.1 about issuing another +FRH and 
     * waiting for NO CARRIER because it's possible that the sender does not
     * send enough V.21 HDLC after the last frame to make that work.
     *
     * The remote has to wait 75 +/- 20 ms after DCS before sending us TCF 
     * as dictated by T.30 Chapter 5, Note 3.  If we have a modem that gives
     * us an OK after DCS before the sender actually drops the carrier, then
     * the best approach will be to simply look for silence with AT+FRS=1.
     * Unfortunately, +FRS is not supported on all modems, and so when they
     * need it, they will have to simply use a <delay:n> or possibly use
     * a different command sequence.
     *
     */
    if (!atCmd(conf.class1TCFRecvHackCmd, AT_OK)) {
	return (false);
    }

    protoTrace("RECV training at %s %s",
	modulationNames[curcap->mod],
	Class2Params::bitRateNames[curcap->br]);
    HDLCFrame buf(conf.class1FrameOverhead);
    bool ok = recvTCF(curcap->value, buf, frameRev, conf.class1TCFRecvTimeout);
    if (curcap->mod == V17) senderHasV17Trouble = !ok;	// if TCF failed
    if (ok) {					// check TCF data
	u_int n = buf.getLength();
	u_int nonzero = 0;
	u_int zerorun = 0;
	u_int i = 0;
	/*
	 * Skip any initial non-zero training noise.
	 */
	while (i < n && buf[i] != 0)
	    i++;
	/*
	 * Determine number of non-zero bytes and
	 * the longest zero-fill run in the data.
	 */
	if (i < n) {
	    while (i < n) {
		u_int j;
		for (; i < n && buf[i] != 0; i++)
		    nonzero++;
		for (j = i; j < n && buf[j] == 0; j++)
		    ;
		if (j-i > zerorun)
		    zerorun = j-i;
		i = j;
	    }
	} else {
	    /*
	     * There was no non-zero data.
	     */
	    nonzero = n;
	}
	/*
	 * Our criteria for accepting is that there must be
	 * no more than 10% non-zero (bad) data and the longest
	 * zero-run must be at least at least 2/3'rds of the
	 * expected TCF duration.  This is a hack, but seems
	 * to work well enough.  What would be better is to
	 * anaylze the bit error distribution and decide whether
	 * or not we would receive page data with <N% error,
	 * where N is probably ~5.  If we had access to the
	 * modem hardware, the best thing that we could probably
	 * do is read the Eye Quality register (or similar)
	 * and derive an indicator of the real S/N ratio.
	 */
	u_int fullrun = params.transferSize(TCF_DURATION);
	u_int minrun = params.transferSize(conf.class1TCFMinRun);
	if (params.ec != EC_DISABLE && conf.class1TCFMinRunECMMod > 0) {
	    /*
	     * When using ECM it may not be wise to fail TCF so easily
	     * as retransmissions can compensate for data corruption.
	     * For example, if there is a regular disturbance in the 
	     * audio every second that will cause TCFs to fail, but where
	     * the majority of the TCF data is "clean", then it will
	     * likely be better to pass TCF more easily at the faster
	     * rate rather than letting things slow down where the 
	     * disturbance will only cause slower retransmissions (and
	     * more of them, too).
	     */
	    minrun /= conf.class1TCFMinRunECMMod;
	}
	nonzero = (100*nonzero) / (n == 0 ? 1 : n);
	protoTrace("RECV: TCF %u bytes, %u%% non-zero, %u zero-run",
	    n, nonzero, zerorun);
	if (zerorun < fullrun && nonzero > conf.class1TCFMaxNonZero) {
	    protoTrace("RECV: reject TCF (too many non-zero, max %u%%)",
		conf.class1TCFMaxNonZero);
	    ok = false;
	}
	if (zerorun < minrun) {
	    protoTrace("RECV: reject TCF (zero run too short, min %u)", minrun);
	    ok = false;
	}
	if (!wasTimeout()) {
	    /*
	     * We expect the message carrier to drop.  However, some senders will
	     * transmit garbage after we see <DLE><ETX> but before we see NO CARRIER.
	     */
	    time_t nocarrierstart = Sys::now();
	    bool gotnocarrier = false;
	    do {
		gotnocarrier = waitFor(AT_NOCARRIER, 2*1000);
	    } while (!gotnocarrier && Sys::now() < (nocarrierstart + 5));
	}
    } else {
	// the CONNECT is waited for later...
	if (lastResponse == AT_FCERROR && atCmd(rhCmd, AT_NOTHING)) lastResponse = AT_FRH3;
	if (lastResponse == AT_FRH3) return (false);	// detected V.21 carrier
    }
    /*
     * If we're going to use SSL Fax then it is desireable to ignore the quality of
     * the TCF signal since we're not going to care about the audio quality, anyway.
     * So, we'll connect now, if possible.
     */
    if (startSSLFax()) ok = true;

    if (respond) {
	/*
	 * Send training response; we follow the spec
	 * by delaying 75ms before switching carriers.
	 */
	fxStr emsg;
	if (!switchingPause(emsg)) return (false);
	if (ok) {
	    /*
	     * Send CFR later so that we can cancel
	     * session by DCN if it's needed. 
	     */
	    sendCFR = true;
	    protoTrace("TRAINING succeeded");
	} else {
	    transmitFrame(FCF_FTT|FCF_RCVR);
	    sendCFR = false;
	    protoTrace("TRAINING failed");
	}
    }
    return (ok);
}

void
Class1Modem::processNewCapabilityUsage()
{
    capsUsed |= BIT(curcap->num);		// add this modulation+bitrate to the used list
    if ((capsUsed & 0x94) == 0x94) {
	senderSkipsV29 = ((capsUsed & 0xDC) == 0x94);
    }
}

/*
 * Process a received DCS frame.
 */
void
Class1Modem::processDCSFrame(const HDLCFrame& frame)
{
    FaxParams dcs_caps = frame.getDIS();			// NB: really DCS

    if (dcs_caps.isBitEnabled(FaxParams::BITNUM_FRAMESIZE_DCS)) frameSize = 64;
    else frameSize = 256;
    /*
     * Some equipment out there will signal DCS bits 1 or 3 but cannot subsequently
     * handle a CSA frame (violating T.30) and will disconnect after seeing it.  So,
     * we avoid sending CSA for SSL Fax when only bit 1 (T.37) or bit 3 (T.38) is
     * used.  They must both be used in DCS in order for us to send CSA.
     */
    if (dcs_caps.isBitEnabled(FaxParams::BITNUM_T37) && dcs_caps.isBitEnabled(FaxParams::BITNUM_T38)) {
	protoTrace("REMOTE wants internet fax");
	if (conf.class1SSLFaxSupport) useSSLFax = true;
    } else if (dcs_caps.isBitEnabled(FaxParams::BITNUM_T37)) {
	protoTrace("REMOTE wants internet fax (T.37)");
    } else if (dcs_caps.isBitEnabled(FaxParams::BITNUM_T38)) {
	protoTrace("REMOTE wants internet fax (T.38)");
    }
    params.setFromDCS(dcs_caps);
    if (!isSSLFax && useV34) params.br = primaryV34Rate-1;
    else {
	curcap = findSRCapability((dcs_caps.getByte(1)<<8)&DCS_SIGRATE, recvCaps);
	processNewCapabilityUsage();
	if (isSSLFax) params.br = BR_SSLFAX;
    }
    setDataTimeout(60, params.br);
    recvDCS(params);				// announce session params
}

const u_int Class1Modem::modemPPMCodes[8] = {
    0,			// 0
    PPM_EOM,		// FCF_EOM+FCF_PRI_EOM
    PPM_MPS,		// FCF_MPS+FCF_PRI_MPS
    0,			// 3
    PPM_EOP,		// FCF_EOP+FCF_PRI_EOP
    0,			// 5
    0,			// 6
    0,			// 7
};

/*
 * Data structure for pthread argument for TIFFWriteDirectory wrapper.
 */
struct recvTIFFWriteDirectoryData {
    int	pfd;
    TIFF* tif;

    recvTIFFWriteDirectoryData(int a, TIFF* b) : pfd(a), tif(b) {}
};

/*
 * Static wrapper function for TIFFWriteDirectory
 * with signal used with pthread.
 */
void*
recvTIFFWriteDirectory(void* rd)
{
    recvTIFFWriteDirectoryData *r = (recvTIFFWriteDirectoryData*) rd;
    TIFFWriteDirectory(r->tif);
    char tbuf[1];	// trigger signal
    tbuf[0] = 0xFF;
    Sys::write(r->pfd, tbuf, 1);
    return (NULL);
}

/*
 * Receive a page of data.
 *
 * This routine is called after receiving training or after
 * sending a post-page response in a multi-page document.
 */
bool
Class1Modem::recvPage(TIFF* tif, u_int& ppm, fxStr& emsg, const fxStr& id, u_int maxPages)
{
    if (lastPPM == FCF_MPS && prevPage) {
	/*
	 * Resume sending HDLC frame (send data)
	 * The carrier is already raised.  Thus we
	 * use sendFrame() instead of transmitFrame().
	 */
	if (pageGood) {
	    startTimeout(7550);
	    (void) sendFrame((sendERR ? FCF_ERR : FCF_MCF)|FCF_RCVR);
	    stopTimeout("sending HDLC frame");
	    lastMCF = Sys::now();
	} else if (conf.badPageHandling == FaxModem::BADPAGE_RTNSAVE) {
	    startTimeout(7550);
	    (void) sendFrame(FCF_RTN|FCF_RCVR);
	    stopTimeout("sending HDLC frame");
	    sentRTN = true;
	    lastRTN = Sys::now();
	    FaxParams dis = modemDIS();
	    if (!recvIdentification(0, fxStr::null, 0, fxStr::null, 
		0, fxStr::null, 0, fxStr::null, 0, dis,
		conf.class1RecvIdentTimer, true, emsg)) {
		return (false);
	    }
	}
    }

    time_t t2end = 0;
    signalRcvd = 0;
    sendERR = false;
    gotCONNECT = true;

    do {
	ATResponse rmResponse = AT_NOTHING;
	long timer = conf.t2Timer;
	if (!messageReceived) {
	    if (sendCFR ) {
#if defined(HAVE_SSL)
		startSSLFax();
		if (!isSSLFax && useSSLFax && (conf.class1SSLFaxInfo.length() || conf.class1SSLFaxProxy.length())) {
		    SSLFax sslfax;
		    fxStr info = conf.class1SSLFaxInfo;
		    bool haspasscode = false;
		    if (conf.class1SSLFaxInfo.length()) {
			sslFaxProcess = sslfax.startServer(conf.class1SSLFaxInfo, conf.class1SSLFaxCert);
		    } else {
			u_int atpos = conf.class1SSLFaxProxy.next(0, '@');
			fxStr passcode;
			if (atpos < conf.class1SSLFaxProxy.length()) {
			    passcode = conf.class1SSLFaxProxy.extract(0, atpos);
			} else {
			    passcode = "";
			    atpos = -1;
			}
			fxStr hostport = conf.class1SSLFaxProxy.extract(atpos+1, conf.class1SSLFaxProxy.length()-atpos-1);
			protoTrace("Connecting to SSL Fax Proxy %s", (const char*) conf.class1SSLFaxProxy);

			sslFaxProcess = sslfax.startClient(hostport, passcode, rtcRev, conf.class1SSLFaxServerTimeout);
			if (sslFaxProcess.server) {
			    info = "";
			    haspasscode = true;
			    char buf[2];
			    int r;
			    do {
				r = sslfax.read(sslFaxProcess, buf, 1, 0, 1000);
				if (r > 0 && buf[0] != 0) info.append( buf[0]&0xFF );
			    } while (r > 0 && (buf[0]&0xFF) != 0);
			    protoTrace("Proxy SSL Fax info: \"%s\"", (const char*) info);
			    usingSSLFaxProxy = true;
			}
		    }
		    if (sslFaxProcess.emsg.length()) protoTrace("SSL Fax: \"%s\"", (const char*) sslFaxProcess.emsg);
		    if (sslFaxProcess.server == 0 ) {
			setSSLFaxFd(0);
			useSSLFax = false;
		    }
		    if (useSSLFax) {
			fxStr csa;
			encodeCSA(csa, info, haspasscode);
			protoTrace("Send CSA frame to accept HylaFAX SSL fax feature.");
			startTimeout(7550);
			transmitFrame(FCF_CSA|FCF_RCVR, csa, false);
			stopTimeout("sending CSA frame");
			startTimeout(7550);
			sendFrame(FCF_CFR|FCF_RCVR);
			stopTimeout("sending CFR frame");
		    } else {
			startTimeout(7550);
			transmitFrame(FCF_CFR|FCF_RCVR);
			stopTimeout("sending CFR frame");
		    }
		} else {
#endif
		    startTimeout(7550);
		    transmitFrame(FCF_CFR|FCF_RCVR);
		    stopTimeout("sending CFR frame");
#if defined(HAVE_SSL)
		}
#endif
		sendCFR = false;
#if defined(HAVE_SSL)
		if (!isSSLFax) {
		    if (sslFaxProcess.server && useSSLFax) {
			// We setSSLFaxFd here because we needed to get the modem's "OK" before interfering with modem reads.
			setSSLFaxFd(sslFaxProcess.server);	// tells ModemServer to also watch for an incoming SSL Fax connection
		    } else {
			// Seems that both we and the sender support internet fax, but there is no service advertised.
			useSSLFax = false;
		    }
		}
#endif
	    }
	    pageGood = pageStarted = false;
	    resetLineCounts();		// in case we don't make it to initializeDecoder
	    recvSetupTIFF(tif, group3opts, FILLORDER_LSB2MSB, id);
	    rmResponse = AT_NOTHING;
	    if (params.ec != EC_DISABLE || useV34) {
		pageGood = recvPageData(tif, emsg);
		if (!sendCFR) messageReceived = true;
		prevPage++;
	    } else {
		bool retryrmcmd;
		int rmattempted = 0;
		do {
		    retryrmcmd = false;
		    /*
		     * Look for message carrier and receive Phase C data.
		     */
		    /*
		     * Same reasoning here as before receiving TCF.
		     */
		    if (!isSSLFax && !atCmd(conf.class1MsgRecvHackCmd, AT_OK)) {
			emsg = "Failure to receive silence (synchronization failure). {E100}";
			return (false);
		    }
		    /*
		     * Set high speed carrier & start receive.  If the
		     * negotiated modulation technique includes short
		     * training, then we use it here (it's used for all
		     * high speed carrier traffic other than the TCF).
		     *
		     * Timing here is very critical.  It is more "tricky" than timing
		     * for AT+FRM for TCF because unlike with TCF, where the direction
		     * of communication doesn't change, here it does change because 
		     * we just sent CFR but now have to do AT+FRM.  In practice, if we 
		     * issue AT+FRM after the sender does AT+FTM then we'll get +FCERROR.
		     * Using Class1MsgRecvHackCmd often only complicates the problem.
		     * If the modem doesn't drop its transmission carrier (OK response
		     * following CFR) quickly enough, then we'll see more +FCERROR.
		     */
		    fxStr rmCmd(curcap[HasShortTraining(curcap)].value, rmCmdFmt);
		    u_short attempts = 0;
		    if (!isSSLFax) {
			if (useSSLFax) sslWatchModem = true;
			do {
			    (void) atCmd(rmCmd, AT_NOTHING);
			    rmResponse = atResponse(rbuf, conf.class1RMPersistence ? conf.t2Timer + 2900 : conf.t2Timer - 2900);
			} while ((rmResponse == AT_NOTHING || rmResponse == AT_FCERROR || rmResponse == AT_OK) && ++attempts < conf.class1RMPersistence && !getSSLFaxConnection());
#if defined(HAVE_SSL)
			if (useSSLFax) {
			    SSLFax sslfax;
			    if (!getSSLFaxConnection()) {
				protoTrace("Traditional fax detected.  Shutting down SSL Fax listener.");
				sslfax.cleanup(sslFaxProcess);
				useSSLFax = false;
				sslWatchModem = false;
				setSSLFaxFd(0);
			    } else {
				if (!usingSSLFaxProxy) {
				    protoTrace("SSL Fax connection detected.");
				    sslfax.acceptClient(sslFaxProcess, sslFaxPasscode, getModemFd(), conf.class1SSLFaxClientTimeout);
				    if (sslFaxProcess.emsg != "") protoTrace("SSL Fax accept client: %s", (const char*) sslFaxProcess.emsg);
				}
				sslWatchModem = false;
				setSSLFaxFd(0);
				if (!sslFaxProcess.server) {
				    protoTrace("SSL Fax client accept failure.  Expecting a traditional fax now.");
				    sslfax.cleanup(sslFaxProcess);
				    useSSLFax = false;
				    do {
					rmResponse = atResponse(rbuf, conf.class1RMPersistence ? conf.t2Timer + 2900 : conf.t2Timer - 2900);
				    } while ((rmResponse == AT_NOTHING || rmResponse == AT_FCERROR) && ++attempts < conf.class1RMPersistence && atCmd(rmCmd, AT_NOTHING));
			        } else {
				    abortReceive();
				    setSSLFaxFd(sslFaxProcess.server);
				    isSSLFax = true;
				    storedBitrate = params.br;
				    params.br = BR_SSLFAX;
				}
			    }
			}
#endif
		    }
		    if (isSSLFax || rmResponse == AT_CONNECT) {
			/*
			 * We don't want the AT+FRM=n command to get buffered,
			 * so buffering and flow control must be done after CONNECT.
			 * Flushing now would be a mistake as data may already be
			 * in the buffer.
			 */
			setInputBuffering(true);
			if (flowControl == FLOW_XONXOFF)
			    (void) setXONXOFF(FLOW_NONE, FLOW_XONXOFF, ACT_NOW);
			/*
			 * The message carrier was recognized;
			 * receive the Phase C data.
			 */
			timeval pagestart;
			gettimeofday(&pagestart, 0);
			protoTrace("RECV: begin page");
			pageGood = recvPageData(tif, emsg);
			protoTrace("RECV: end page");
			timeval pageend;
			gettimeofday(&pageend, 0);

			if (wasSSLFax) {
			    wasSSLFax = false;
			} else if (!wasTimeout()) {
			    /*
			     * The data was received correctly, wait patiently
			     * for the modem to signal carrier drop.  Some senders
			     * may send a lot of garbage after RTC, so be patient.
			     */
			    time_t nocarrierstart = Sys::now();
			    do {
				messageReceived = (isSSLFax || waitFor(AT_NOCARRIER, 60*1000));
			    } while (!messageReceived && Sys::now() < (nocarrierstart + 60));
			    if (messageReceived) {
				prevPage++;
				if (!isSSLFax) {
				    timeval delta = pageend - pagestart;
				    time_t duration = delta.tv_sec * 1000 + delta.tv_usec / 1000;
				    if (duration < 500) {
					/*
					 * The sender's Phase C carrier started and then stopped almost immediately.
					 * There are cases like this where the sender then just restarts the carrier
					 * so, this will accommodate those.
					 */
					retryrmcmd = true;
				    }
				}
			    }
			    timer = conf.t2Timer;
			    if ((BIT(curcap->br) & BR_ALL) && !getSeenRTC()) {
				timeval delta = pageend - pagestart;
				time_t duration = delta.tv_sec * 1000 + delta.tv_usec / 1000;
				/*
				 * Without getting RTC and without ECM we don't have a precise expectation for
				 * how long we need to wait for Phase D signals.  So, we calculate an additional
				 * wait time based on an expected page size plus the T2 timer.  We can deduct
				 * the time already spent in Phase C reception.
				 */
				timer += conf.class1ExpectedPageSize * 10 / (3 * (curcap->br+1)) - duration;
				if (timer < conf.t2Timer) timer = conf.t2Timer;
			    }
			}
		    } else if (rmResponse == AT_FRCNG) {
			if (!respondToCNG(emsg)) return (false);
			messageReceived = false;
			sendCFR = true;
		    } else {
			if (wasTimeout()) {
			    abortReceive();		// return to command mode
			    setTimeout(false);
			}
			bool getframe = false;
			long wait = conf.t2Timer;
			// We didn't get RTC, so we have to account for additional wait, as above.
			if (BIT(curcap->br) & BR_ALL) wait += conf.class1ExpectedPageSize * 10 / (3 * (curcap->br+1));
			if (rmResponse == AT_FRH3) getframe = waitFor(AT_CONNECT, wait);
			else if (!isSSLFax && rmResponse != AT_NOCARRIER && rmResponse != AT_ERROR) getframe = atCmd(rhCmd, AT_CONNECT, wait);	// wait longer
			if (getframe) {
			    HDLCFrame frame(conf.class1FrameOverhead);
			    if (recvFrame(frame, FCF_RCVR, conf.t2Timer, true, false)) {
				traceFCF("RECV recv", frame.getFCF());
				signalRcvd = frame.getFCF();
				messageReceived = true;
			    } else {
				if (lastResponse != AT_OK) {
				    /*
				     * V.21 HDLC was detected and then the carrier was lost without
				     * receiving any data or the data received was corrupt.  It's
				     * possible that the modem erred in its detection of the high-speed
				     * carrier.  But, it's also possible that echo of our CFR was
				     * detected or that there is another receiver on the line (another
				     * fax machine sharing the line somewhere), and we heard them.
				     * Often we can still acquire the high-speed carrier if we just
				     * re-issue AT+FRM=n.
				     */
				    retryrmcmd = true;
				} else {
				    /*
				     * The modem responded with an odd OK.  Look for V.21 HDLC again.
				     */
				    signalRcvd = 0;
				}
			    }
			}
		    }
		} while (retryrmcmd && ++rmattempted < 2);
	    }
	    if (signalRcvd != 0) {
		if (flowControl == FLOW_XONXOFF)
		    (void) setXONXOFF(FLOW_NONE, FLOW_NONE, ACT_DRAIN);
		setInputBuffering(false);
	    }
	    if (!messageReceived && rmResponse != AT_FCERROR && rmResponse != AT_FRH3 && rmResponse != AT_FRCNG) {
		if (rmResponse != AT_ERROR) {
		    /*
		     * One of many things may have happened:
		     * o if we lost carrier, then some modems will return
		     *   AT_NOCARRIER or AT_EMPTYLINE in response to the
		     *   AT+FRM request.
		     * o otherwise, there may have been a timeout receiving
		     *   the message data, or there was a timeout waiting
		     *   for the carrier to drop.
		     */
		    if (wasTimeout()) {
			/*
			 * The timeout expired - thus we missed the carrier either
			 * raising or dropping.
			 */
			abortReceive();		// return to command state
			break;
		    }
		    /*
		     * We found the wrong carrier, which means that there
		     * is an HDLC frame waiting for us--in which case it
		     * should get picked up below.
		     */
		} else {
		    /*
		     * T.31 8.1 does state, "All of the action commands (see 8.3.1 to
		     * 8.3.6) return an ERROR result code if issued when the DCE is on-hook."
		     * However, if the modem developers overlooked that statement and only
		     * referred to T.31 8.3.4, there would be no understood specification
		     * for an ERROR result to +FRM=<mod>.
		     *
		     * Consequently, some modems (apparently old Zyxels) use an ERROR result
		     * to indicate that the wrong carrier was detected, like +FCERROR, but
		     * unlike +FCERROR they do not return to command state and need the
		     * following abortReceive().
		     */
		    if (conf.class1HasRMHookIndication) {
			emsg = "Failed to properly detect high-speed data carrier. {E112}";
			gotEOT = true;
			return (false);
		    } else {
			abortReceive();		// return to command state
		    }
		}
	    }
	}
	/*
	 * T.30 says to process operator intervention requests
	 * here rather than before the page data is received.
	 * This has the benefit of not recording the page as
	 * received when the post-page response might need to
	 * be retransmited.
	 */
	if (abortRequested()) {
	    // XXX no way to purge TIFF directory
	    emsg = "Receive aborted due to operator intervention {E301}";
	    return (false);
	}

	if (!sendCFR) {
	    /*
	     * Acknowledge PPM from ECM protocol.
	     */
	    HDLCFrame frame(conf.class1FrameOverhead);
	    bool ppmrcvd;
	    if (signalRcvd != 0) {
		ppmrcvd = true;
		lastPPM = signalRcvd;
		if (lastPPM == FCF_MPS || lastPPM == FCF_EOP || lastPPM == FCF_EOM || lastPPM == FCF_PRI_MPS || lastPPM == FCF_PRI_EOP || lastPPM == FCF_PRI_EOM) {
		    PPMseq[1] = PPMseq[0];
		    PPMseq[0] = lastPPM;
		    if (PPMseq[0] != PPMseq[1] && lastRTN > lastMCF) {
			/*
			 * We responded RTN to the PPM after the previous page and now next a different PPM from them.
			 * So, they didn't resend the same page.  There's nothing we can do at this point
			 * now to counteract their receipt-confirmation interpretation of our previous
			 * RTN signal.  Even if we send DCN now there's no assurance that they would ever
			 * resend the previous page, because they think that we confirmed it received.
			 * At this point the best we can do is mark them as confusing RTN for reference
			 * on future calls.  Hopefully we got enough of the previous page to make sense
			 * of it and saved it if the admin had badpagehandling so configured.
			 */
			protoTrace("This sender appears to not respond properly to RTN.");
			senderConfusesRTN = true;
		    }
		}
		for (u_int i = 0; i < frameRcvd.length(); i++) frame.put(frameRcvd[i]);
		frame.setOK(true);
	    } else {
		u_short recvFrameCount = 0;
		time_t ppmstart = Sys::now();
		do {
		    /*
		     * Some modems will report CONNECT erroniously on high-speed Phase C data.
		     * Then they will time-out on HDLC data presentation instead of dumping
		     * garbage or quickly resulting ERROR.  So we give instances where CONNECT
		     * occurs a bit more tolerance here...
		     */
		    ppmrcvd = recvFrame(frame, FCF_RCVR, timer, false, false);	// no CRP, as senders can mistake it for MCF
		} while (!ppmrcvd && gotCONNECT && !wasTimeout() && !gotEOT && (++recvFrameCount < 3 || Sys::now() < (ppmstart + 5)));
		if (ppmrcvd) {
		    lastPPM = frame.getFCF();
		    if (lastPPM == FCF_MPS || lastPPM == FCF_EOP || lastPPM == FCF_EOM || lastPPM == FCF_PRI_MPS || lastPPM == FCF_PRI_EOP || lastPPM == FCF_PRI_EOM) {
			PPMseq[1] = PPMseq[0];
			PPMseq[0] = lastPPM;
			if (PPMseq[0] != PPMseq[1] && lastRTN > lastMCF) {
			    /* We responded RTN to the PPM and now see a different PPM from them.  See the note above. */
			    protoTrace("This sender appears to not respond properly to RTN.");
			    senderConfusesRTN = true;
			}
		    }
		} else if (lastResponse == AT_FRCNG) {
		    if (!respondToCNG(emsg)) return (false);
		    messageReceived = false;
		    sendCFR = true;
		    gotCONNECT = true;
		    continue;
		}
		/*
		 * To combat premature carrier loss leading to MCF instead of RTN on short/partial pages,
		 * We started a timer above and measured the time it took to receive PPM.  If longer
		 * longer than 5 seconds, and if we did not see RTC, then we assume that premature
		 * carrier loss occurred and set pageGood to false.
		 */
		if (Sys::now() - ppmstart > 5 && !getSeenRTC()) {
		    protoTrace("RECV detected premature Phase C carrier loss.");
		    pageGood = false;
		}
	    }
	    /*
	     * Do command received logic.
	     */
	    if (ppmrcvd) {
		switch (lastPPM) {
		case FCF_DIS:			// XXX no support
		    if (!pageGood) recvResetPage(tif);
		    protoTrace("RECV DIS/DTC");
		    emsg = "Can not continue after DIS/DTC {E107}";
		    return (false);
		case FCF_PWD:
		case FCF_SUB:
		case FCF_NSS:
		case FCF_TSI:
		case FCF_DCS:
		    {
			sentRTN = false;	// reset it, the sender appears to have behaved appropriately
			signalRcvd = 0;
			if (!pageGood) recvResetPage(tif);
			// look for high speed carrier only if training successful
			messageReceived = !(FaxModem::recvBegin(NULL, emsg));
			bool trainok = true;
			short traincount = 0;
			do {
			    if (!messageReceived) messageReceived = !(recvDCSFrames(frame));
			    if (recvdDCN) {
				if (!emsg.length()) emsg = "RSPREC error/got DCN (sender abort) {E103}";
				messageReceived = true;
				signalRcvd = FCF_DCN;
				lastResponse = AT_NOTHING;
				return (false);
			    }
			    if (!messageReceived) {
				trainok = recvTraining();
				messageReceived = (!trainok && lastResponse == AT_FRH3);
			    }
			} while (!trainok && traincount++ < 3 && lastResponse != AT_FRH3 && recvFrame(frame, FCF_RCVR, timer));
			if (messageReceived && lastResponse == AT_FRH3 && waitFor(AT_CONNECT, conf.t2Timer)) {
			    messageReceived = false;
			    if (recvFrame(frame, FCF_RCVR, conf.t2Timer, true, false)) {
				messageReceived = true;
				signalRcvd = frame.getFCF();
			    }
			    if (!frame.getLength() && lastResponse == AT_NOCARRIER) {
				// The modem may have indicated V.21 HDLC incorrectly.  TCF may be coming.  Get ready.
				trainok = recvTraining();
				messageReceived = (!trainok && lastResponse == AT_FRH3);
				if (messageReceived && lastResponse == AT_FRH3 && waitFor(AT_CONNECT, conf.t2Timer)) {
				    messageReceived = false;
				    if (recvFrame(frame, FCF_RCVR, conf.t2Timer, true)) {
					messageReceived = true;
					signalRcvd = frame.getFCF();
				    }
				}
			    }
			    lastResponse = AT_NOTHING;
			} else messageReceived = false;
			break;
		    }
		case FCF_MPS:			// MPS
		case FCF_EOM:			// EOM
		case FCF_EOP:			// EOP
		case FCF_PRI_MPS:			// PRI-MPS
		case FCF_PRI_EOM:			// PRI-EOM
		case FCF_PRI_EOP:			// PRI-EOP
		    if (!getRecvEOLCount()) {
			// We have a null page, don't save it because it makes readers fail.
			pageGood = false;
			if (params.ec != EC_DISABLE) {
			    if (emsg == "") {
				/*
				 * We negotiated ECM, got no valid ECM image data, and the
				 * ECM page reception routines did not set an error message.
				 * The empty emsg is due to the ECM routines detecting a
				 * non-ECM-specific partial-page signal and wants it to
				 * be handled here.  The sum total of all of this, and the
				 * fact that we got MPS/EOP/EOM tells us that the sender
				 * is not using ECM.  In an effort to salvage the session we'll
				 * disable ECM now and try to continue.
				 */
				params.ec = EC_DISABLE;
			    } else
				return (false);
			}
		    }
		    if (!pageGood && conf.badPageHandling == FaxModem::BADPAGE_RTN)
			recvResetPage(tif);
		    if (signalRcvd == 0) traceFCF("RECV recv", lastPPM);

		    /*
		     * [Re]transmit post page response.
		     */
		    if (pageGood || (conf.badPageHandling == FaxModem::BADPAGE_RTNSAVE && getRecvEOLCount())) {
			if (!pageGood) lastPPM = FCF_MPS;	// FaxModem::BADPAGE_RTNSAVE
			/*
			 * If post page message confirms the page
			 * that we just received, write it to disk.
			 */
			if (messageReceived) {
			    if (!useV34 && emsg == "") (void) switchingPause(emsg);
			    /*
			     * On servers where disk access may be bottlenecked or stressed,
			     * the TIFFWriteDirectory call can lag.  The strategy, then, is
			     * to employ RNR/RR flow-control for ECM sessions and to use CRP
			     * in non-ECM sessions in order to grant TIFFWriteDirectory
			     * sufficient time to complete.
			     *
			     * We used to do this with a forked process, but because in an
			     * SSL Fax session immovable data is being manipulated both with
			     * the SSL operations as well as with the TIFF operation, it is
			     * much easier to use threads rather than ensure one of those
			     * operations is conducted entirely in memory shared between the
			     * forked processes.
			     */
			    int fcfd[2];		// flow control file descriptors for the pipe
			    if (pipe(fcfd) >= 0) {
				// Start the thread calling TIFFWriteDirectory.
				pthread_t t;
				recvTIFFWriteDirectoryData r(fcfd[1], tif);
				if (pthread_create(&t, NULL, &recvTIFFWriteDirectory, (void *) &r) == 0) {
				    char tbuf[1];	// trigger signal
				    tbuf[0] = 0xFF;
				    time_t rrstart = Sys::now();
				    do {
					fd_set rfds;
					FD_ZERO(&rfds);
					FD_SET(fcfd[0], &rfds);
					struct timeval tv;
					tv.tv_sec = 2;		// we've got a 3-second window, use it
					tv.tv_usec = 0;
#if CONFIG_BADSELECTPROTO
					if (!select(fcfd[0]+1, (int*) &rfds, NULL, NULL, &tv)) {
#else
					if (!select(fcfd[0]+1, &rfds, NULL, NULL, &tv)) {
#endif
					    bool gotresponse = true;
					    u_short rnrcnt = 0;
					    do {
						if (emsg != "") break;
						(void) transmitFrame(params.ec != EC_DISABLE ? FCF_RNR : FCF_CRP|FCF_RCVR);
						traceFCF("RECV send", params.ec != EC_DISABLE ? FCF_RNR : FCF_CRP);
						HDLCFrame rrframe(conf.class1FrameOverhead);
						gotresponse = recvFrame(rrframe, FCF_RCVR, conf.t2Timer);
						if (gotresponse) {
						    traceFCF("RECV recv", rrframe.getFCF());
						    if (rrframe.getFCF() == FCF_DCN) {
							protoTrace("RECV recv DCN");
							emsg = "COMREC received DCN (sender abort) {E108}";
							gotEOT = true;
							recvdDCN = true;
							if (sentRTN && curcap->br != BR_2400) {
							    protoTrace("This sender appears to not respond properly to RTN.");
							    senderConfusesRTN = true;
							}
						    } else if (params.ec != EC_DISABLE && rrframe.getFCF() != FCF_RR) {
							protoTrace("Ignoring invalid response to RNR.");
						    }
						    (void) switchingPause(emsg);
						}
					    } while (!gotEOT && !recvdDCN && !gotresponse && ++rnrcnt < 2 && Sys::now()-rrstart < 60);
					    if (!gotresponse) emsg = "No response to RNR repeated 3 times. {E109}";
					} else {		// thread finished TIFFWriteDirectory
					    tbuf[0] = 0;
					}
				    } while (!gotEOT && !recvdDCN && tbuf[0] != 0 && Sys::now()-rrstart < 60);
				    Sys::read(fcfd[0], NULL, 1);
				    pthread_join(t, NULL);
				} else {
				    protoTrace("Protocol flow control unavailable due to threading error.");
				    TIFFWriteDirectory(tif);
				}
				Sys::close(fcfd[0]);
				Sys::close(fcfd[1]);
			    } else {
				protoTrace("Protocol flow control unavailable due to pipe error.");
				TIFFWriteDirectory(tif);
			    }
			    if (emsg == "" && prevPage > maxPages) {
				protoTrace("Number of pages received (%d) exceeds maxRecvPages (%d).", prevPage, maxPages);
				emsg = "Maximum receive page count exceeded";
			    }
			    if (emsg == "") {	// confirm only if there was no error
				if (pageGood) {
				    traceFCF("RECV send", sendERR ? FCF_ERR : FCF_MCF);
				    lastMCF = Sys::now();
				} else
				    traceFCF("RECV send", FCF_RTN);

				if (lastPPM == FCF_MPS) {
				    /*
				     * Raise the HDLC transmission carrier but do not
				     * transmit MCF now.  This gives us at least a 3-second
				     * window to buffer any delays in the post-page
				     * processes.
				     */
				    if (!useV34 && !isSSLFax && !atCmd(thCmd, AT_CONNECT)) {
					emsg = "Failure to raise V.21 transmission carrier. {E101}";
					return (false);
				    }
				} else {
				    (void) transmitFrame((sendERR ? FCF_ERR : FCF_MCF)|FCF_RCVR);
				    lastMCF = Sys::now();
				    if (lastPPM == FCF_EOP) {
					/*
					 * Because there are a couple of notifications that occur after this
					 * things can hang and we can miss hearing DCN.  So we do it now.
					 */
					recvdDCN = recvEnd(NULL, emsg);
				    }
				}
			    }
			    /*
			     * Reset state so that the next call looks
			     * first for page carrier or frame according
			     * to what's expected.  (Grr, where's the
			     * state machine...)
			     */
			    messageReceived = (lastPPM == FCF_EOM);
			    ppm = modemPPMCodes[lastPPM&7];
			    return (true);
			}
		    } else {
			/*
			 * Page not received, or unacceptable; tell
			 * other side to retransmit after retrain.
			 */
			/*
			 * As recommended in T.31 Appendix II.1, we try to
			 * prevent the rapid switching of the direction of
			 * transmission by using +FRS.  Theoretically, "OK"
			 * is the only response, but if the sender has not
			 * gone silent, then we cannot continue anyway,
			 * and aborting here will give better information.
			 *
			 * Using +FRS is better than a software pause, which
			 * could not ensure loss of carrier.  +FRS is easier
			 * to implement than using +FRH and more reliable than
			 * using +FTS
			 */
			if (!switchingPause(emsg)) {
			    return (false);
			}
			signalRcvd = 0;
			if (params.ec == EC_DISABLE && rmResponse != AT_CONNECT && !getRecvEOLCount()) {
			    if (Sys::now() - lastMCF < 9 && PPMseq[0] == PPMseq[1]) {
				/*
				 * We last transmitted MCF a very, very short time ago, received no image data
				 * since then, and now we're seeing the same PPM again.  In non-ECM mode the chances
				 * of this meaning that we simply missed a very short page are extremely remote.  It
				 * probably means that the sender did not properly hear our MCF and that we just
				 * need to retransmit it.
				 */
				(void) transmitFrame(FCF_MCF|FCF_RCVR);
				traceFCF("RECV send", FCF_MCF);
				lastMCF = Sys::now();
				messageReceived = (lastPPM != FCF_MPS);	// expect Phase C if MPS
			    } else {
				/*
				 * We missed training on the Phase C carrier, and so we missed the Phase C image data.
				 * T.30 (Figure 5-2c in 09/2005 T.30) makes it clear with "CAPABLE RE-XMIT" that RTN
				 * should not be interpreted as receipt confirmation.  Senders who are not capable of
				 * retransmitting should respond to RTN with DCN.  So, it is unfortunate that there are
				 * some senders out there which interpret RTN as a receipt confirmation.  Our options,
				 * therefore, are limited.  We can send RTN, and senders who properly follow T.30 should
				 * retransmit the page, but this will be perceived as a message confirmation by those
				 * bothersome RTN-confused senders.  If we send DCN then we certainly get the message
				 * accross about no receipt confirmation, but we risk the sender not calling back, and
				 * we miss the real likelihood that the sender could have retransmitted in the same call.
				 * Another option we have is to send an out-of-spec CFR signal which senders will not
				 * perceive as receipt confirmation, but unless the sender handles this Phase B signal
				 * while in Phase D (and HylaFAX may be the only one that does) this will lead to the
				 * sender disconnecting.  Having this decision as configurable allows the administrator
				 * to adjust with DynamicConfig or something, if needed.
				 */
				protoTrace("Phase C data was missed.  Attempt to get the sender to retransmit.");
				if (senderConfusesRTN) protoTrace("This sender is known to confuse the RTN signal.");
				if (senderConfusesPIN) protoTrace("This sender is known to confuse the PIN signal.");
				if (conf.missedPageHandling == FaxModem::MISSEDPAGE_CFR) {
				    (void) transmitFrame(FCF_CFR|FCF_RCVR);
				    traceFCF("RECV send", FCF_CFR);
				    messageReceived = false;	// stay in Phase C
				} else if ((senderConfusesRTN && senderConfusesPIN) || conf.missedPageHandling == FaxModem::MISSEDPAGE_DCN) {
				    emsg = "PPM received with no image data.  To continue risks receipt confirmation. {E155}";
				    (void) transmitFrame(FCF_DCN|FCF_RCVR);
				    traceFCF("RECV send", FCF_DCN);
				    recvdDCN = true;
				    return (false);
				} else if (senderConfusesRTN || conf.missedPageHandling == FaxModem::MISSEDPAGE_PIN) {
				    if (!performProceduralInterrupt(false, false, emsg)) return (false);
				    messageReceived = false;
				    sendCFR = true;
				} else {	// MISSEDPAGE_RTN
				    (void) transmitFrame(FCF_RTN|FCF_RCVR);
				    traceFCF("RECV send", FCF_RTN);
				    messageReceived = true;		// return to Phase B
				    sentRTN = true;
				    lastRTN = Sys::now();
				}
			    }
			} else {
			    u_int rtnfcf = FCF_RTN;
			    sentRTN = true;
			    lastRTN = Sys::now();
			    if (!getRecvEOLCount() || conf.badPageHandling == FaxModem::BADPAGE_DCN) {
				/*
				 * Regardless of the BadPageHandling setting, if we get no page image data at
				 * all, then sending RTN at all risks confirming the non-page to RTN-confused 
				 * senders, which risk is far worse than just simply hanging up.
				 */
				emsg = "PPM received with no image data.  To continue risks receipt confirmation. {E155}";
				rtnfcf = FCF_DCN;
				sentRTN = false;
			    }
			    (void) transmitFrame(rtnfcf|FCF_RCVR);
			    traceFCF("RECV send", rtnfcf);
			    if (rtnfcf == FCF_DCN) {
				recvdDCN = true;
				return (false);
			    }
			    /*
			     * Reset the TIFF-related state so that subsequent
			     * writes will overwrite the previous data.
			     */
			    messageReceived = true;	// expect DCS next
			}
		    }
		    break;
		case FCF_RR:
		    // The sender did not hear our MCF signal.  Treat it like CRP.
		case FCF_CRP:
		    // command repeat... just repeat whatever we last sent
		    if (!switchingPause(emsg)) return (false);
		    transmitFrame(signalSent);
		    traceFCF("RECV send", (u_char) signalSent[2]);
		    /* fall through - to clear messageReceived and signalRcvd */
		case FCF_MCF:
		case FCF_CFR:
		case FCF_RTN:
		case FCF_ERR:
		    /* It's probably just our own echo. */
		    messageReceived = false;
		    signalRcvd = 0;
		    break;
		case FCF_DCN:			// DCN
		    protoTrace("RECV recv DCN");
		    emsg = "COMREC received DCN (sender abort) {E108}";
		    recvdDCN = true;
		    if (sentRTN && curcap->br != BR_2400) {
			protoTrace("This sender appears to not respond properly to RTN.");
			senderConfusesRTN = true;
		    }
		    if (prevPage && conf.saveUnconfirmedPages && getRecvEOLCount()) {	// only if there was data
			TIFFWriteDirectory(tif);
			protoTrace("RECV keeping unconfirmed page");
			return (true);
		    }
		    return (false);
		default:
		    if (!pageGood) recvResetPage(tif);
		    emsg = "COMREC invalid response received {E110}";
		    return (false);
		}
		t2end = 0;
	    } else {
		/*
		 * We didn't get a message.  Try to be resiliant by
		 * looking for the signal again, but prevent infinite
		 * looping with a timer.  However, if the modem is on
		 * hook, then modem responds ERROR or NO CARRIER, and
		 * for those cases there is no point in resiliancy.
		 */
		if (lastResponse == AT_NOCARRIER || lastResponse == AT_ERROR) break;
		if (t2end) {
		    if (Sys::now() > t2end)
			break;
		} else {
		    t2end = Sys::now() + howmany(conf.t2Timer, 1000);
		}
	    }
	}
    } while (gotCONNECT && !wasTimeout() && lastResponse != AT_EMPTYLINE);
    if (sentRTN) {
	protoTrace("This sender appears to not respond properly to RTN.");
	senderConfusesRTN = true;
    }
    emsg = "V.21 signal reception timeout; expected page possibly not received in full {E111}";
    if (prevPage && conf.saveUnconfirmedPages && getRecvEOLCount()) {
	TIFFWriteDirectory(tif);
	protoTrace("RECV keeping unconfirmed page");
	return (true);
    }
    return (false);
}

void
Class1Modem::abortPageRecv()
{
    if (wasSSLFax || isSSLFax || useV34) return;	// nothing to do in V.34
    if (conf.class1RecvAbortOK) {
	char c = CAN;				// anything other than DC1/DC3
	putModem(&c, 1, 1);
    } else {
        (void) atCmd("AT", AT_NOTHING);
    }


}

bool
Class1Modem::raiseRecvCarrier(bool& dolongtrain, fxStr& emsg)
{
    if (!atCmd(conf.class1MsgRecvHackCmd, AT_OK)) {
	emsg = "Failure to receive silence (synchronization failure). {E100}";
	return (false);
    }
    /*
     * T.30 Section 5, Note 5 states that we must use long training
     * on the first high-speed data message following CTR.
     */
    fxStr rmCmd;
    if (dolongtrain) rmCmd = fxStr(curcap->value, rmCmdFmt);
    else rmCmd = fxStr(curcap[HasShortTraining(curcap)].value, rmCmdFmt);
    u_short attempts = 0;
    lastResponse = AT_NOTHING;
    if (useSSLFax) sslWatchModem = true;
    do {
	(void) atCmd(rmCmd, AT_NOTHING);
	lastResponse = atResponse(rbuf, conf.class1RMPersistence ? conf.t2Timer + 2900 : conf.t2Timer - 2900);
    } while ((lastResponse == AT_NOTHING || lastResponse == AT_FCERROR || lastResponse == AT_OK) && ++attempts < conf.class1RMPersistence && !getSSLFaxConnection());
#if defined(HAVE_SSL)
    if (useSSLFax) {
	SSLFax sslfax;
	if (!getSSLFaxConnection()) {
	    protoTrace("Traditional fax detected.  Shutting down SSL Fax listener.");
	    sslfax.cleanup(sslFaxProcess);
	    useSSLFax = false;
	    sslWatchModem = false;
	    setSSLFaxFd(0);
	} else {
	    if (!usingSSLFaxProxy) {
		protoTrace("SSL Fax connection detected.");
		sslfax.acceptClient(sslFaxProcess, sslFaxPasscode, getModemFd(), conf.class1SSLFaxClientTimeout);
		if (sslFaxProcess.emsg != "") protoTrace("SSL Fax accept client: %s", (const char*) sslFaxProcess.emsg);
	    }
	    sslWatchModem = false;
	    setSSLFaxFd(0);
	    if (!sslFaxProcess.server) {
		protoTrace("SSL Fax client accept failure.  Expecting a traditional fax now.");
		sslfax.cleanup(sslFaxProcess);
		useSSLFax = false;
		do {
		    lastResponse = atResponse(rbuf, conf.class1RMPersistence ? conf.t2Timer + 2900 : conf.t2Timer - 2900);
		} while ((lastResponse == AT_NOTHING || lastResponse == AT_FCERROR) && ++attempts < conf.class1RMPersistence && atCmd(rmCmd, AT_NOTHING));
	    } else {
		abortReceive();
		setSSLFaxFd(sslFaxProcess.server);
		isSSLFax = true;
		storedBitrate = params.br;
		params.br = BR_SSLFAX;
	    }
	}
    }
#endif
    if (lastResponse == AT_ERROR) gotEOT = true;	// on hook
    if (lastResponse == AT_FRH3 && waitFor(AT_CONNECT, conf.t2Timer)) {
	gotRTNC = true;
	gotEOT = false;
    }
    if (lastResponse != AT_CONNECT && !gotRTNC && !isSSLFax) {
	emsg = "Failed to properly detect high-speed data carrier. {E112}";
	return (false);
    }
    dolongtrain = false;
    return (true);
}

void
Class1Modem::abortPageECMRecv(TIFF* tif, const Class2Params& params, u_char* block, u_int fcount, u_short seq, bool pagedataseen, fxStr& emsg)
{
    if (pagedataseen) {
	writeECMData(tif, block, (fcount * frameSize), params, (seq |= 2), emsg);
	if (conf.saveUnconfirmedPages) {
	    protoTrace("RECV keeping unconfirmed page");
	    prevPage++;
	}
    }
    free(block);
}

/*
 * Data structure for pthread argument for writeECMData wrapper.
 */
struct recvWriteECMDataData {
    Class1Modem* obj;
    int pfd;
    TIFF* tif;
    u_char* block;
    u_int cc;
    const Class2Params& params;
    u_short seq;
    fxStr& emsg;

    recvWriteECMDataData(Class1Modem* o, int a, TIFF* b, u_char* c, u_int d, const Class2Params& e, u_short f, fxStr& g) :
	obj(o), pfd(a), tif(b), block(c), cc(d), params(e), seq(f), emsg(g) {}
};

/*
 * Static wrapper function for writeECMData
 * with signal used with pthread.
 */
void*
recvWriteECMData(void* rd)
{
    recvWriteECMDataData *r = (recvWriteECMDataData*) rd;
    (r->obj)->writeECMData(r->tif, r->block, r->cc, r->params, r->seq, r->emsg);
    char tbuf[1];	// trigger signal
    tbuf[0] = 0xFF;
    Sys::write(r->pfd, tbuf, 1);
    return (NULL);
}

/*
 * Receive Phase C data in T.30-A ECM mode.
 */
bool
Class1Modem::recvPageECMData(TIFF* tif, Class2Params& params, fxStr& emsg)
{
    HDLCFrame frame(5);					// A+C+FCF+FCS=5 bytes
    u_char* block = (u_char*) malloc(frameSize*256);	// 256 frames per block - totalling 16/64KB
    fxAssert(block != NULL, "ECM procedure error (receive block).");
    memset(block, 0, (size_t) frameSize*256);
    bool lastblock = false;
    bool pagedataseen = false;
    u_short seq = 1;					// sequence code for the first block
    prevBlock = 0;
    u_int lastSignalRcvd = 0;

    do {
	u_int fnum = 0;
	char ppr[32];					// 256 bits
	for (u_int i = 0; i < 32; i++) ppr[i] = 0xff;	// ppr defaults to all 1's, T.4 A.4.4
	u_short rcpcnt = 0;
	u_short pprcnt = 0;
	u_int fcount = 0;
	u_short syncattempts = 0;
	bool blockgood = false, dolongtrain = false;
	bool gotoPhaseD = false;
	bool carrierup = false;
	do {
	    sendERR = false;
	    resetBlock();
	    lastSignalRcvd = signalRcvd;
	    signalRcvd = 0;
	    rcpcnt = 0;
	    bool dataseen = false;
	    bool retryrmcmd;
	    int rmattempted = 0;
	    if (!carrierup) do {
		retryrmcmd = false;
		if (!isSSLFax && !useV34 && !gotoPhaseD) {
		    gotRTNC = false;
		    if (!raiseRecvCarrier(dolongtrain, emsg) && !gotRTNC && !isSSLFax) {
			if (wasTimeout()) {
			    abortReceive();		// return to command mode
			    setTimeout(false);
			}
			if (lastResponse == AT_FRCNG) {
			    if (!respondToCNG(emsg)) return (false);
			    messageReceived = false;
			    sendCFR = true;
			    abortPageECMRecv(tif, params, block, fcount, seq, pagedataseen, emsg);
			    prevPage--;		// counteract the forthcoming increment
			    return (true);
			}
			long wait = BIT(curcap->br) & BR_ALL ? 273066 / (curcap->br+1) : conf.t2Timer;
			if (lastResponse != AT_NOCARRIER && atCmd(rhCmd, AT_CONNECT, wait)) {	// wait longer
			    // sender is transmitting V.21 instead, we may have
			    // missed the first signal attempt, but should catch
			    // the next attempt.  This "simulates" adaptive receive.
			    emsg = "";	// reset
			    gotRTNC = true;
			} else {
			    if (wasTimeout()) abortReceive();
			    abortPageECMRecv(tif, params, block, fcount, seq, pagedataseen, emsg);
			    return (false);
			}
		    }
		}
		if ((!isSSLFax && useV34) || gotRTNC) {		// V.34 mode or if +FRH:3 in adaptive reception
		    if (!gotEOT) {
			bool gotprimary = false;
			if (!isSSLFax && useV34) {
			    if (useSSLFax) sslWatchModem = true;
			    gotprimary = waitForDCEChannel(false);
#if defined(HAVE_SSL)
			    if (useSSLFax) {
				SSLFax sslfax;
				if (!getSSLFaxConnection()) {
				    protoTrace("Traditional fax detected.  Shutting down SSL Fax listener.");
				    sslfax.cleanup(sslFaxProcess);
				    useSSLFax = false;
				    sslWatchModem = false;
				    setSSLFaxFd(0);
				} else {
				    if (!usingSSLFaxProxy) {
					protoTrace("SSL Fax connection detected.");
					sslfax.acceptClient(sslFaxProcess, sslFaxPasscode, getModemFd(), conf.class1SSLFaxClientTimeout);
					if (sslFaxProcess.emsg != "") protoTrace("SSL Fax accept client: %s", (const char*) sslFaxProcess.emsg);
				    }
				    sslWatchModem = false;
				    setSSLFaxFd(0);
				    if (!sslFaxProcess.server) {
					protoTrace("SSL Fax client accept failure.  Expecting a traditional fax now.");
					sslfax.cleanup(sslFaxProcess);
					useSSLFax = false;
					gotprimary = waitForDCEChannel(false);	// resume V.34-Fax session
				    } else {
					setSSLFaxFd(sslFaxProcess.server);
					isSSLFax = true;
					storedBitrate = params.br;
					params.br = BR_SSLFAX;
				    }
				}
			    }
#endif
			}
			while (!sendERR && !gotEOT && (gotRTNC || (ctrlFrameRcvd != fxStr::null))) {
			    /*
			     * Remote requested control channel retrain, the remote didn't
			     * properly hear our last signal, and/or we got an EOR signal 
			     * after PPR.  So now we have to use a signal from the remote
			     * and then respond appropriately to get us back or stay in sync.
			     * DCS::CFR - PPS::PPR/MCF - EOR::ERR
			     */
			    HDLCFrame rtncframe(conf.class1FrameOverhead);
			    bool gotrtncframe = false;
			    if (!isSSLFax && useV34) {
				if (ctrlFrameRcvd != fxStr::null) {
				    gotrtncframe = true;
				    for (u_int i = 0; i < ctrlFrameRcvd.length(); i++)
					rtncframe.put(frameRev[ctrlFrameRcvd[i] & 0xFF]);
				    traceHDLCFrame("-->", rtncframe);
				} else
				    gotrtncframe = recvFrame(rtncframe, FCF_RCVR, conf.t2Timer, false, false);	// no CRP, as it could be mistaken for MCF
			    } else {
				gotrtncframe = recvFrame(rtncframe, FCF_RCVR, conf.t2Timer, true, false);
			    }
			    if (gotrtncframe) {
				traceFCF("RECV recv", rtncframe.getFCF());
				switch (rtncframe.getFCF()) {
				    case FCF_PPS:
					if (rtncframe.getLength() > 5) {
					    u_int fc = frameRev[rtncframe[6]] + 1;
					    if ((fc == 256 || fc == 1) && !dataseen) fc = 0;	// distinguish 0 from 1 and 256
					    traceFCF("RECV recv", rtncframe.getFCF2());
					    u_int pgcount = u_int(prevPage/256)*256+frameRev[rtncframe[4]];	// cope with greater than 256 pages
					    protoTrace("RECV received %u frames of block %u of page %u", \
						fc, frameRev[rtncframe[5]]+1, pgcount+1);
					    switch (rtncframe.getFCF2()) {
						case 0: 	// PPS-NULL
						case FCF_EOM:
						case FCF_MPS:
						case FCF_EOP:
						case FCF_PRI_EOM:
						case FCF_PRI_MPS:
						case FCF_PRI_EOP:
						    if (!switchingPause(emsg)) {
							abortPageECMRecv(tif, params, block, fcount, seq, pagedataseen, emsg);
							return (false);
						    }
						    if (pgcount > prevPage || (pgcount == prevPage && frameRev[rtncframe[5]] >= prevBlock)) {
							(void) transmitFrame(FCF_PPR, fxStr(ppr, 32));
							traceFCF("RECV send", FCF_PPR);
						    } else {
							(void) transmitFrame(FCF_MCF|FCF_RCVR);
							traceFCF("RECV send", FCF_MCF);
						    }
						    break;
					    }
					}
					break;
				    case FCF_EOR:
					if (rtncframe.getLength() > 5) {
					    traceFCF("RECV recv", rtncframe.getFCF2());
					    switch (rtncframe.getFCF2()) {
						case 0: 	// PPS-NULL
						case FCF_EOM:
						case FCF_MPS:
						case FCF_EOP:
						case FCF_PRI_EOM:
						case FCF_PRI_MPS:
						case FCF_PRI_EOP:
						    if (fcount) {
							/*
							 * The block hasn't been written to disk.
							 * This is expected when the sender sends
							 * EOR after our PPR (e.g. after the 4th).
							 */
							blockgood = true;
							signalRcvd = rtncframe.getFCF2();
							if (signalRcvd) lastblock = true;
							sendERR = true;
						    } else {
							if (!switchingPause(emsg)) {
							    abortPageECMRecv(tif, params, block, fcount, seq, pagedataseen, emsg);
							    return (false);
							}
							(void) transmitFrame(FCF_ERR|FCF_RCVR);
							traceFCF("RECV send", FCF_ERR);
						    }
						    break;
					    }
					}
					break;
				    case FCF_CTC:
					{
					    u_int dcs;			// possible bits 1-16 of DCS in FIF
					    if (isSSLFax || useV34) {
						// T.30 F.3.4.5 Note 1 does not permit CTC in V.34-fax
						emsg = "Received invalid CTC signal in V.34-Fax. {E113}";
						abortPageECMRecv(tif, params, block, fcount, seq, pagedataseen, emsg);
						return (false);
					    }
					    /*
					     * See the other comments about T.30 A.1.3.  Some senders
					     * are habitually wrong in sending CTC at incorrect moments.
					     */
					    // use 16-bit FIF to alter speed, curcap
					    dcs = rtncframe[3] | (rtncframe[4]<<8);
					    curcap = findSRCapability(dcs&DCS_SIGRATE, recvCaps);
					    processNewCapabilityUsage();
					    // requisite pause before sending response (CTR)
					    if (!switchingPause(emsg)) {
						abortPageECMRecv(tif, params, block, fcount, seq, pagedataseen, emsg);
						return (false);
					    }
					    (void) transmitFrame(FCF_CTR|FCF_RCVR);
					    traceFCF("RECV send", FCF_CTR);
					    dolongtrain = true;
					    pprcnt = 0;
					    break;
					}
				    case FCF_RR:
					// The sender did not hear our MCF signal.  Treat it like CRP.
				    case FCF_CRP:
					// command repeat... just repeat whatever we last sent
					if (!switchingPause(emsg)) {
					    abortPageECMRecv(tif, params, block, fcount, seq, pagedataseen, emsg);
					    return (false);
					}
					transmitFrame(signalSent);
					traceFCF("RECV send", (u_char) signalSent[2]);
					break;
				    case FCF_DCN:
					emsg = "COMREC received DCN (sender abort) {E108}";
					gotEOT = true;
					recvdDCN = true;
					continue;
				    case FCF_MCF:
				    case FCF_CFR:
				    case FCF_CTR:
				    case FCF_PPR:
				    case FCF_ERR:
					if ((rtncframe[2] & 0x80) == FCF_RCVR) {
					    /*
					     * Echo on the channel may be so lagged that we're hearing 
					     * ourselves.  Ignore it.  Try again.
					     */
					    break;
					}
					/* intentional pass-through */
				    default:
					// The message is not ECM-specific: fall out of ECM receive, and let
					// the earlier message-handling routines try to cope with the signal.
					signalRcvd = rtncframe.getFCF();
					messageReceived = true;
					abortPageECMRecv(tif, params, block, fcount, seq, pagedataseen, emsg);
					frameRcvd = "";
					for (u_int i = 0; i < rtncframe.getLength(); i++) frameRcvd.append(rtncframe[i]);
					if (getRecvEOLCount() == 0) {
					    prevPage--;		// counteract the forthcoming increment
					    return (true);
					} else {
					    emsg = "COMREC invalid response received {E110}";	// plain ol' error
					    return (false);
					}
				}
				if (!sendERR) {	// as long as we're not trying to send the ERR signal (set above)
				    if (!isSSLFax && useV34) gotprimary = waitForDCEChannel(false);
				    else {
					gotRTNC = false;
					if (!isSSLFax && !raiseRecvCarrier(dolongtrain, emsg) && !gotRTNC) {
					    if (wasTimeout()) {
						abortReceive();	// return to command mode
						setTimeout(false);
					    }
					    long wait = BIT(curcap->br) & BR_ALL ? 273066 / (curcap->br+1) : conf.t2Timer;
					    if (lastResponse != AT_NOCARRIER && atCmd(rhCmd, AT_CONNECT, wait)) {	// wait longer
						// simulate adaptive receive
						emsg = "";		// clear the failure
						gotRTNC = true;
					    } else {
						if (wasTimeout()) abortReceive();
						abortPageECMRecv(tif, params, block, fcount, seq, pagedataseen, emsg);
						return (false);
					    }
					} else gotprimary = true;
				    }
				}
			    } else {
				gotprimary = false;
				if (!isSSLFax && !useV34) {
				    if (wasTimeout()) {
					if (lastResponse != AT_OK) abortReceive();
					break;
				    }
				    if (lastResponse != AT_OK) {
					/*
					 * The modem reported V.21 HDLC but then reported that carrier was lost
					 * (AT_NOCARRIER) or that the received frame was corrupt (AT_ERROR).
					 * The modem may have erred in its detection of the high-speed carrier.
					 * But it may also have detected echo from our end or there may be
					 * another receiver on the line (the sender could be sharing the line
					 * with two machines).  Try AT+FRM=n again.
					 */
					retryrmcmd = true;
					if (rmattempted+1 < 2) {
					    gotRTNC = false;
					} else {
					    gotRTNC = true;
					    if (!atCmd(rhCmd, AT_CONNECT, conf.t1Timer)) {
						if (wasTimeout()) abortReceive();
						break;
					    }
					}
				    } else {
					if (!atCmd(rhCmd, AT_CONNECT, conf.t1Timer)) {
					    if (wasTimeout()) abortReceive();
					    break;
					}
				    }
				}
			    }
			}
			if (!isSSLFax && !gotprimary && !sendERR && !(retryrmcmd && rmattempted+1 < 2)) {
			    if (emsg == "") {
				if (!isSSLFax && useV34) emsg = "Failed to properly open V.34 primary channel. {E114}";
				else emsg = "Failed to properly detect high-speed data carrier. {E112}";
			    }
			    protoTrace(emsg);
			    abortPageECMRecv(tif, params, block, fcount, seq, pagedataseen, emsg);
			    return (false);
			}
		    }
		    if (gotEOT) {		// intentionally not an else of the previous if
			if (useV34 && emsg == "") emsg = "Received premature V.34 termination. {E115}";
			protoTrace(emsg);
			abortPageECMRecv(tif, params, block, fcount, seq, pagedataseen, emsg);
			return (false);
		    }
		}
	    } while (retryrmcmd && ++rmattempted < 2);
	    /*
	     * Buffering and flow control must be done after AT+FRM=n.
	     * We do not flush in order avoid losing data already buffered.
	     */
	    setInputBuffering(true);
	    if (flowControl == FLOW_XONXOFF)
		(void) setXONXOFF(FLOW_NONE, FLOW_XONXOFF, ACT_NOW);
	    gotoPhaseD = false;
	    carrierup = false;

	    timeval pagestart;
	    gettimeofday(&pagestart, 0);

	    if (!sendERR && ((!isSSLFax && useV34) || syncECMFrame())) {	// no synchronization needed w/V.34-fax
		time_t start = Sys::now();
		do {
		    frame.reset();
		    if (recvECMFrame(frame)) {
			if (frame[2] == FCF_FCD) {	// FCF is FCD
			    dataseen = true;
			    pagedataseen = true;
			    rcpcnt = 0;			// reset RCP counter
			    fnum = frameRev[frame[3]];	// T.4 A.3.6.1 says LSB2MSB
			    protoTrace("RECV received frame number %u", fnum);
			    // some modems may erroneously recreate valid CRC on short frames, so possibly check length, too
			    if (conf.class1ECMCheckFrameLength ? frame.checkCRC() && frame.getLength() == frameSize+6 : frame.checkCRC()) {
				// store received frame in block at position fnum (A+C+FCF+Frame No.=4 bytes)
				for (u_int i = 0; i < frameSize; i++) {
				    if (frame.getLength() - 6 > i)	// (A+C+FCF+Frame No.+FCS=6 bytes)
					block[fnum*frameSize+i] = frameRev[frame[i+4]];	// LSB2MSB
				}
				if (fcount < (fnum + 1)) fcount = fnum + 1;
				// valid frame, set the corresponding bit in ppr to 0
				u_int pprpos, pprval;
				for (pprpos = 0, pprval = fnum; pprval >= 8; pprval -= 8) pprpos++;
				if (ppr[pprpos] & frameRev[1 << pprval]) ppr[pprpos] ^= frameRev[1 << pprval];
			    } else {
				protoTrace("RECV frame FCS check failed");
			    }
			} else if (frame[2] == FCF_RCP && frame.checkCRC()) {	// FCF is RCP
			    rcpcnt++;
			} else {
			    dataseen = true;
			    if (frame.getLength() > 4 && frame.checkCRC()) {
				traceFCF("Invalid and confusing placement of", frame.getFCF());
				triggerInterrupt = true;
			    } else {
				protoTrace("HDLC frame with bad FCF %#x", frame[2]);
			    }
			}
		    } else {
			dataseen = true;	// assume that garbage was meant to be data
			if (wasTimeout() || sslSawBlockEnd) break;
			if (isSSLFax || !useV34) syncECMFrame();
			if ((!isSSLFax && useV34) && (gotEOT || gotCTRL)) rcpcnt = 3;
		    }
		    // some senders don't send the requisite three RCP signals
		} while (rcpcnt == 0 && (unsigned) Sys::now()-start < 5*60);	// can't expect 50 ms of flags, some violate T.4 A.3.8
		if (flowControl == FLOW_XONXOFF)
		    (void) setXONXOFF(FLOW_NONE, FLOW_NONE, ACT_DRAIN);
		setInputBuffering(false);
		if (!isSSLFax && useV34) {
		    if (!gotEOT && !gotCTRL && !waitForDCEChannel(true)) {
			emsg = "Failed to properly open V.34 control channel. {E116}";
			protoTrace(emsg);
			abortPageECMRecv(tif, params, block, fcount, seq, pagedataseen, emsg);
			return (false);
		    }
		    if (gotEOT) {
			emsg = "Received premature V.34 termination. {E115}";
			protoTrace(emsg);
			abortPageECMRecv(tif, params, block, fcount, seq, pagedataseen, emsg);
			return (false);
		    }
		} else {
		    if (!endECMBlock()) {				// wait for <DLE><ETX>
			if (wasTimeout()) {
			    abortReceive();
			    emsg = "Timeout waiting for Phase C carrier drop. {E154}";
			    protoTrace(emsg);
			    abortPageECMRecv(tif, params, block, fcount, seq, pagedataseen, emsg);
			    return (false);
			}
		    }
		}
		if (!isSSLFax && !useV34 && !sslSawBlockEnd) {
		    // wait for message carrier to drop
		    time_t nocarrierstart = Sys::now();
		    bool gotnocarrier = false;
		    do {
			gotnocarrier = waitFor(AT_NOCARRIER, 2*1000);
			if (lastResponse == AT_OK) gotnocarrier = true;	// some modems may return to command-mode in some instances
		    } while (!gotnocarrier && lastResponse != AT_EMPTYLINE && Sys::now() < (nocarrierstart + 5));
		}

		if (!isSSLFax) {
		    timeval pageend;
		    gettimeofday(&pageend, 0);
		    timeval delta = pageend - pagestart;
		    time_t duration = delta.tv_sec * 1000 + delta.tv_usec / 1000;
		    if (duration < 500 && fakedRCP) {
			/*
			 * The sender's Phase C carrier started and then stopped almost immediately.
			 * There are cases like this where the sender then just restarts the carrier
			 * so, this will accommodate those.
			 */
			signalRcvd = lastSignalRcvd;
			continue;
		    }
		}

		bool gotpps = false;
		sslSawBlockEnd = false;
		wasSSLFax = false;
		HDLCFrame ppsframe(conf.class1FrameOverhead);
		u_short recvFrameCount = 0;
		start = Sys::now();
		do {
		    /*
		     * It is possible that the high-speed carrier drop was
		     * detected in error or that some line noise caused it but
		     * that the sender has not actually ended the carrier.  It is
		     * possible then that T2 will not be long enough to receive the
		     * partial-page signal because the sender is still transmitting
		     * high-speed data.  So we calculate the wait for 80KB (the
		     * ECM block plus some wriggle-room) at the current bitrate.
		     *
		     * Sadly, some senders will interpret CRP as MCF, so we cannot
		     * use CRP here, else we risk leading the sender to believe that
		     * we've confirmed the block.  Our options, then, are to blindly
		     * respond with PPR or to wait for the sender to retransmit PPS.
		     * Both of those options run risks - PPR can lead to our PPR count
		     * being different from the sender's, so risking CTC unexpectedly,
		     * and waiting for a PPS retransmission could lead the sender to
		     * believe that we've hung up.  This latter option seems the
		     * least likely to be problematic, so that's what we're doing.
		     */
		    if (isSSLFax && fcount == 0 && rcpcnt == 1 && sslFaxPastData[0] == 0xFF && sslFaxPastData[1] == 0x13 && sslFaxPastData[2] == 0xBF) {
			// For some reason the sender has skipped over Phase C data and sent PPS.  Cope as best we can.
			protoTrace("The sender seems to have skipped Phase C.");
			for (u_int i = 0; i < 9; i++) ppsframe.put(frameRev[sslFaxPastData[i] & 0xFF]);
			traceHDLCFrame("-->", ppsframe);
			gotpps = true;
		    } else {
			u_int br = useV34 ? primaryV34Rate : curcap->br + 1;
			long wait = br >= 1 && br <= 15 ? 273066 / br : conf.t2Timer;
			gotpps = recvFrame(ppsframe, FCF_RCVR, wait, false, false);	// wait longer, no CRP
			if (ppsframe.getFCF() == FCF_RCP) gotpps = false;
		    }
		} while (!gotpps && gotCONNECT && !wasTimeout() && !gotEOT && (++recvFrameCount < 5 || Sys::now() < (start + 5)));
		if (gotpps) {
		    traceFCF("RECV recv", ppsframe.getFCF());
		    if (ppsframe.getFCF() == FCF_PPS) {
			// sender may violate T.30-A.4.3 and send another signal (i.e. DCN)
			traceFCF("RECV recv", ppsframe.getFCF2());
		    }
		    if ((ppsframe.getFCF() == FCF_MPS || ppsframe.getFCF() == FCF_EOP || ppsframe.getFCF() == FCF_EOR || ppsframe.getFCF() == 0x00) && ppsframe.getLength() > 5) {
			// sender appears to have sent a PPS signal but omitted the PPS FCF, let's fix it for them
			protoTrace("We'll fix the sender's careless PPS signal.");
			int fb = ppsframe.getLength() - 1;
			ppsframe.put(ppsframe[fb]);
			for (; fb > 2; fb--) ppsframe[fb] = ppsframe[fb-1];
			ppsframe[2] = FCF_PPS;
		    }
		    // cope with greater than 256 pages
		    u_int pgcount = u_int(prevPage/256)*256;
		    if (ppsframe.getLength() > 4) pgcount += frameRev[ppsframe[4]];
		    switch (ppsframe.getFCF()) {
			/*
			 * PPS is the only valid signal, Figure A.8/T.30; however, some
			 * senders don't handle T.30 A.1.3 ("When PPR is received four
			 * times for the same block...") properly (possibly because T.30
			 * A.4.1 isn't clear about the "per-block" requirement), and so
			 * it is possible for us to get CTC or EOR here (if the modem
			 * quickly reported NO CARRIER when we went looking for the
			 * non-existent high-speed carrier and the sender is persistent).
			 *
			 * CRP is a bizarre signal to get instead of PPS, but some
			 * senders repeatedly transmit this instead of PPS.  So to
			 * handle it as best as possible we interpret the signal as
			 * meaning PPS-NULL (full block) unless there was no data seen
			 * (in which case PPS-MPS is assumed) in order to prevent 
			 * any data loss, and we let the sender cope with it from there.
			 *
			 * Because there is no way to express "zero" in the frame count
			 * byte there exists some confusion in some senders which attempt
			 * to do just that.  Consequently, the frame count values of 0x00
			 * and 0xFF need consideration as to whether they represent 0, 1, 
			 * or 256.  To allow for the bizarre situation where a sender may
			 * signal an initial PPS-NULL with a frame count less than 256 we
			 * trust the PPS-NULL frame count except in cases where it is
			 * determined to be "1" because most-likely that determination only
			 * comes from some garbage detected during the high-speed carrier.
			 */
			case FCF_PPS:
			    if (triggerInterrupt && ppsframe.getFCF2() != 0x00 && !prevPage) {
				protoTrace("This sender seems to have trouble with ECM. Attempting procedural interrupt to disable ECM.");
				senderFumblesECM = true;
				if (!performProceduralInterrupt(senderFumblesECM, false, emsg)) return (false);
				messageReceived = false;
				sendCFR = true;
				abortPageECMRecv(tif, params, block, fcount, seq, pagedataseen, emsg);
				prevPage--;		// counteract the forthcoming increment
				return (true);
			    }
			case FCF_CRP:
			    senderFumblesECM = false;	// clear this flag if somehow we get here with it set
			    {
				u_int fc = ppsframe.getFCF() == FCF_CRP ? 256 : frameRev[ppsframe[6]] + 1;
				if ((fc == 256 || fc == 1) && !dataseen) fc = 0;	// distinguish 0 from 1 and 256
				// See comment above.  It's extremely unlikely to get PPS-NULL with a frame-count meaning "1"...
				if (ppsframe.getFCF() == FCF_PPS && ppsframe.getFCF2() == 0x00 && fc == 1 && !pprcnt) fc = 0;
				if (fcount < fc) fcount = fc;
				if (ppsframe.getFCF() == FCF_CRP) {
				    if (fc) ppsframe[3] = 0x00;		// FCF2 = NULL
				    else ppsframe[3] = FCF_MPS;
				    protoTrace("RECV unexpected CRP - assume %u frames of block %u of page %u", \
					fc, prevBlock + 1, prevPage + 1);
				} else {
				    protoTrace("RECV received %u frames of block %u of page %u", \
					fc, frameRev[ppsframe[5]]+1, pgcount+1);
				}
				blockgood = true;
				/*
				 * The sender may send no frames.  This will happen for at least three
				 * reasons.
				 *
				 * 1) If we previously received data from this block and responded
				 * with PPR but the sender is done retransmitting frames as the sender
				 * thinks that our PPR signal did not indicate any frame that the 
				 * sender transmitted.  This should only happen with the last frame
				 * of a block due to counting errors.  So, in the case where we received
				 * no frames from the sender we ignore the last frame in the block when
				 * checking.
				 *
				 * 2) The sender feeds paper into a scanner during the initial
				 * synchronization and it expected another page but didn't get it 
				 * (e.g. paper feed problem).  We respond with a full PPR in hopes that
				 * the sender knows what they're doing by sending PPS instead of DCN.
				 * The sender can recover by sending data with the block retransmission.
				 *
				 * 3) The sender sent only one frame but for some reason we did not see
				 * any data, and so the frame-count in the PPS signal ended up getting
				 * interpreted as a zero.  Only in the case that the frame happens to be
				 * the last frame in a block and we're dealing with MH, MR, or MMR data 
				 * we will send MCF (to accomodate #1), and so this frame will then be 
				 * lost.  This should be rare and have little impact on actual image data
				 * loss when it does occur.  This approach cannot be followed with JPEG
				 * and JBIG data formats or when the signal is PPS-NULL.  This approach
				 * cannot be followed when we previously saw exactly one frame of data.
				 */
				if (fcount) {
				    u_int fbad = 0;
				    for (u_int i = 0; i <= (fcount - ((fcount < 2 || fc || params.df > DF_2DMMR || ppsframe.getFCF() == 0) ? 1 : 2)); i++) {
					u_int pprpos, pprval;
					for (pprpos = 0, pprval = i; pprval >= 8; pprval -= 8) pprpos++;
					if (ppr[pprpos] & frameRev[1 << pprval]) {
					    blockgood = false;
					    fbad++;
					}
				    }
				    dataSent += fcount;
				    dataMissed += fbad;
				    pageDataMissed += fbad;
				    if (fcount && ! blockgood) protoTrace("Block incomplete: %d frames (%d%%) corrupt or missing", fbad, ((fbad*100)/fcount));
				    if (pgcount < prevPage || (pgcount == prevPage && frameRev[ppsframe[5]] < prevBlock))
					blockgood = false;	// we already confirmed this block receipt... (see below)
				} else {
				    blockgood = false;	// MCF only if we have data
				}

				// requisite pause before sending response (PPR/MCF)
				if (!blockgood && !switchingPause(emsg)) {
				    abortPageECMRecv(tif, params, block, fcount, seq, pagedataseen, emsg);
				    return (false);
				}
			    }
			    /* ... pass through ... */
			case FCF_CTC:
			case FCF_EOR:
			    if (! blockgood) {
				if ((ppsframe.getFCF() == FCF_CTC || ppsframe.getFCF() == FCF_EOR) &&
				     (!useV34 || !conf.class1PersistentECM)) {	// only if we can make use of the signal
				    signalRcvd = ppsframe.getFCF();
				    pprcnt = 4;
				}
				if (signalRcvd == 0) {
				    if (pgcount > prevPage || (pgcount == prevPage && frameRev[ppsframe[5]] >= prevBlock)) {
					// inform the remote that one or more frames were invalid
					transmitFrame(FCF_PPR, fxStr(ppr, 32));
					traceFCF("RECV send", FCF_PPR);
					pprcnt++;
					if (pprcnt > 4) pprcnt = 4;		// could've been 4 before increment
				    } else {
					/*
					 * The silly sender already sent us this block and we already confirmed it.
					 * Just confirm it again, but let's behave as if we sent a full PPR without
					 * incrementing pprcnt.
					 */
					(void) transmitFrame(FCF_MCF|FCF_RCVR);
					traceFCF("RECV send", FCF_MCF);
					for (u_int i = 0; i < 32; i++) ppr[i] = 0xff;	// ppr defaults to all 1's, T.4 A.4.4
					fcount = 0;					// reset our expectations
				    }
				}
				if (pprcnt == 4 && ((!useV34 && !isSSLFax) || !conf.class1PersistentECM)) {
				    HDLCFrame rtnframe(conf.class1FrameOverhead);
				    if (signalRcvd == 0) {
					// expect sender to send CTC/EOR after every fourth PPR, not just the fourth
					protoTrace("RECV sent fourth PPR");
					if (curcap->br == BR_2400 && dataSent == dataMissed) {
					    protoTrace("This sender seems to have trouble with ECM.");
					    senderFumblesECM = true;
					}
				    } else {
					// we already got the signal
					rtnframe.put(ppsframe, ppsframe.getLength());
				    }
				    pprcnt = 0;
				    if (signalRcvd != 0 || recvFrame(rtnframe, FCF_RCVR, conf.t2Timer, false, true, true, FCF_PPR)) {
					bool gotrtnframe = true;
					if (signalRcvd == 0) traceFCF("RECV recv", rtnframe.getFCF());
					else signalRcvd = 0;		// reset it, we're in-sync now
					recvFrameCount = 0;
					lastResponse = AT_NOTHING;
					while (rtnframe.getFCF() == FCF_PPS && !gotEOT && recvFrameCount < 5 && gotrtnframe) {
					    // we sent PPR, but got PPS again...
					    if (!switchingPause(emsg)) {
						abortPageECMRecv(tif, params, block, fcount, seq, pagedataseen, emsg);
						return (false);
					    }
					    transmitFrame(FCF_PPR, fxStr(ppr, 32));
					    traceFCF("RECV send", FCF_PPR);
					    gotrtnframe = recvFrame(rtnframe, FCF_RCVR, conf.t2Timer, false, true, true, FCF_PPR);
					    if (gotrtnframe)
						traceFCF("RECV recv", rtnframe.getFCF());
					    recvFrameCount++;
					}
					u_int dcs;			// possible bits 1-16 of DCS in FIF
					switch (rtnframe.getFCF()) {
					    case FCF_CTC:
						if (useV34) {
						    // T.30 F.3.4.5 Note 1 does not permit CTC in V.34-fax
						    emsg = "Received invalid CTC signal in V.34-Fax. {E113}";
						    abortPageECMRecv(tif, params, block, fcount, seq, pagedataseen, emsg);
						    return (false);
						}
						// use 16-bit FIF to alter speed, curcap
						dcs = rtnframe[3] | (rtnframe[4]<<8);
						curcap = findSRCapability(dcs&DCS_SIGRATE, recvCaps);
						processNewCapabilityUsage();
						// requisite pause before sending response (CTR)
						if (!switchingPause(emsg)) {
						    abortPageECMRecv(tif, params, block, fcount, seq, pagedataseen, emsg);
						    return (false);
						}
						(void) transmitFrame(FCF_CTR|FCF_RCVR);
						traceFCF("RECV send", FCF_CTR);
						dolongtrain = true;
						break;
					    case FCF_EOR:
						traceFCF("RECV recv", rtnframe.getFCF2());
						/*
						 * It may be wise to disconnect here if MMR is being
						 * used because there will surely be image data loss.
						 * However, since the sender knows what the extent of
						 * the data loss will be, we'll naively assume that
						 * the sender knows what it's doing, and we'll
						 * proceed as instructed by it.
						 */
						blockgood = true;
						switch (rtnframe.getFCF2()) {
						    case 0:
							// EOR-NULL partial page boundary
							break;
						    case FCF_EOM:
						    case FCF_MPS:
						    case FCF_EOP:
						    case FCF_PRI_EOM:
						    case FCF_PRI_MPS:
						    case FCF_PRI_EOP:
							lastblock = true;
							signalRcvd = rtnframe.getFCF2();
							break;
						    default:
							emsg = "COMREC invalid response to repeated PPR received {E117}";
							abortPageECMRecv(tif, params, block, fcount, seq, pagedataseen, emsg);
							return (false);
						}
						sendERR = true;		// do it later
						break;
					    case FCF_DCN:
						emsg = "COMREC received DCN (sender abort) {E108}";
						gotEOT = true;
						recvdDCN = true;  
					    default:
						if (emsg == "") emsg = "COMREC invalid response to repeated PPR received {E117}";
						abortPageECMRecv(tif, params, block, fcount, seq, pagedataseen, emsg);
						return (false);
					}
				    } else {
					emsg = "T.30 T2 timeout, expected signal not received {E118}";
					abortPageECMRecv(tif, params, block, fcount, seq, pagedataseen, emsg);
					return (false);
				    }
				}
			    }
			    if (signalRcvd == 0) {		// don't overwrite EOR settings
				switch (ppsframe.getFCF2()) {
				    case 0:
					// PPS-NULL partial page boundary
					break;
				    case FCF_EOM:
				    case FCF_MPS:
				    case FCF_EOP:
				    case FCF_PRI_EOM:
				    case FCF_PRI_MPS:
				    case FCF_PRI_EOP:
					lastblock = true;
					signalRcvd = ppsframe.getFCF2();
					break;
				    default:
					if (blockgood) {
					    emsg = "COMREC invalid partial-page signal received {E119}";
					    abortPageECMRecv(tif, params, block, fcount, seq, pagedataseen, emsg);
					    return (false);
					}
					/*
					 * If the sender signalled PPS-<??> (where the FCF2 is meaningless) 
					 * and if the block isn't good then we already signalled back PPR... 
					 * which is appropriate despite whatever the strange FCF2 was supposed
					 * to mean, and hopefully it will not re-use it on the next go-around.
					 */
					break;
				}
			    }
			    break;
			case FCF_DCN:
			    emsg = "COMREC received DCN (sender abort) {E108}";
			    gotEOT = true;
			    recvdDCN = true;
			    abortPageECMRecv(tif, params, block, fcount, seq, pagedataseen, emsg);
			    return (false);
			case FCF_FCD:
			    if (useV34) {
				/*
				 * The modem had indicated the control channel to us, so we thought that
				 * we were in Phase D now.  But we got another ECM frame, so we're really back
				 * in Phase C.  This is weird, but we can cope with it.  Should we maybe
				 * try to store this frame in the block?
				 */
				protoTrace("Odd.  Received ECM frame in Phase D.  Return to Phase C.");
				carrierup = true;
				rcpcnt = 0;		// reset RCP counter
				break;
			    }
			default:
			    // The message is not ECM-specific: fall out of ECM receive, and let
			    // the earlier message-handling routines try to cope with the signal.
			    signalRcvd = ppsframe.getFCF();
			    messageReceived = true;
			    abortPageECMRecv(tif, params, block, fcount, seq, pagedataseen, emsg);
			    if (getRecvEOLCount() == 0) {
				 prevPage--;		// counteract the forthcoming increment
				return (true);
			    } else {
				emsg = "COMREC invalid response received {E110}";	// plain ol' error
				return (false);
			    }
		    }
		} else {
		    emsg = "T.30 T2 timeout, expected signal not received {E118}";
		    abortPageECMRecv(tif, params, block, fcount, seq, pagedataseen, emsg);
		    return (false);
		}
	    } else {
		if (wasTimeout()) {
		    abortReceive();
		    if (!useV34 && !isSSLFax) {
			// must now await V.21 signalling
			long wait = BIT(curcap->br) & BR_ALL ? 273066 / (curcap->br+1) : conf.t2Timer;
			gotRTNC = (lastResponse == AT_FRH3 && waitFor(AT_CONNECT, conf.t2Timer)) || atCmd(rhCmd, AT_CONNECT, wait);
			gotoPhaseD = gotRTNC;
			if (!gotRTNC) syncattempts = 21;
			else setTimeout(false);
		    }
		}
		if (syncattempts++ > 20) {
		    emsg = "Cannot synchronize ECM frame reception. {E120}";
		    abortPageECMRecv(tif, params, block, fcount, seq, true, emsg);
		    return(false);
		}
	    }
	} while (! blockgood);

	u_int cc = fcount * frameSize;
	if (lastblock) {
	    // trim zero padding
	    while (cc > 0 && block[cc - 1] == 0) cc--;
	}
	// write the block to file
	if (lastblock) seq |= 2;			// seq code for the last block

	/*
	 * On servers where disk or CPU may be bottlenecked or stressed,
	 * the writeECMData call can lag.  The strategy, then, is
	 * to employ RNR/RR flow-control in order to grant writeECMData
	 * sufficient time to complete.
	 *
	 * We used to do this with a forked process, but because in an
	 * SSL Fax session immovable data is being manipulated both with
	 * the SSL operations as well as with the TIFF operation, it is
	 * much easier to use threads rather than ensure one of those
	 * operations is conducted entirely in memory shared between the
	 * forked processes.
	 */
	int fcfd[2];		// flow control file descriptors for the pipe
	if (pipe(fcfd) >= 0) {

	    pthread_t t;
	    recvWriteECMDataData r(this, fcfd[1], tif, block, cc, params, seq, emsg);
	    if (pthread_create(&t, NULL, &recvWriteECMData, (void *) &r) == 0) {

		char tbuf[1];	// trigger signal
		tbuf[0] = 0xFF;
		time_t rrstart = Sys::now();
		do {
		    fd_set rfds;
		    FD_ZERO(&rfds);
		    FD_SET(fcfd[0], &rfds);
		    struct timeval tv;
		    tv.tv_sec = 1;		// 1000 ms should be safe
		    tv.tv_usec = 0;
#if CONFIG_BADSELECTPROTO
		    if (!select(fcfd[0]+1, (int*) &rfds, NULL, NULL, &tv)) {
#else
		    if (!select(fcfd[0]+1, &rfds, NULL, NULL, &tv)) {
#endif
			bool gotresponse = true;
			u_short rnrcnt = 0;
			do {
			    (void) switchingPause(emsg);
			    if (emsg != "") break;
			    (void) transmitFrame(FCF_RNR|FCF_RCVR);
			    traceFCF("RECV send", FCF_RNR);
			    HDLCFrame rrframe(conf.class1FrameOverhead);
			    gotresponse = recvFrame(rrframe, FCF_RCVR, conf.t2Timer);
			    if (gotresponse) {
				traceFCF("RECV recv", rrframe.getFCF());
				if (rrframe.getFCF() == FCF_DCN) {
				    protoTrace("RECV recv DCN");
				    emsg = "COMREC received DCN (sender abort) {E108}";
				    gotEOT = true;
				    recvdDCN = true;
				} else if (params.ec != EC_DISABLE && rrframe.getFCF() != FCF_RR) {
				    protoTrace("Ignoring invalid response to RNR.");
				}
			    }
			} while (!recvdDCN && !gotEOT && !gotresponse && ++rnrcnt < 2 && Sys::now()-rrstart < 60);
			if (!gotresponse) emsg = "No response to RNR repeated 3 times. {E109}";
		    } else tbuf[0] = 0;	// thread finished writeECMData
		} while (!gotEOT && !recvdDCN && tbuf[0] != 0 && Sys::now()-rrstart < 60);
		Sys::read(fcfd[0], NULL, 1);
		pthread_join(t, NULL);
	    } else {
		protoTrace("Protocol flow control unavailable due to fork error.");
		writeECMData(tif, block, cc, params, seq, emsg);
	    }
	    Sys::close(fcfd[0]);
	    Sys::close(fcfd[1]);
	} else {
	    protoTrace("Protocol flow control unavailable due to pipe error.");
	    writeECMData(tif, block, cc, params, seq, emsg);
	}
	seq = 0;					// seq code for in-between blocks

	if (!lastblock) {
	    // confirm block received as good
	    (void) switchingPause(emsg);
	    (void) transmitFrame((sendERR ? FCF_ERR : FCF_MCF)|FCF_RCVR);
	    traceFCF("RECV send", sendERR ? FCF_ERR : FCF_MCF);
	}
	prevBlock++;
    } while (! lastblock);

    free(block);
    recvEndPage(tif, params);

    if (getRecvEOLCount() == 0) {
	// Just because image data blocks are received properly doesn't guarantee that
	// those blocks actually contain image data.  If the decoder finds no image
	// data at all we send DCN instead of MCF in hopes of a retransmission.
	emsg = "ECM page received containing no image data. {E121}";
	return (false);
    }
    if (!signalRcvd) {
	// It appears that the sender did something bizarre such as first signaling
	// PPS-MPS and then after a PPR exchange later signaled PPS-NULL.  We've come
	// to here because lastblock was set true as was blockgood.  So let's restore
	// signalRcvd to the non-null value so that we can do something useful after.
	signalRcvd = lastSignalRcvd;
    }
    return (true);   		// signalRcvd is set, full page is received...
}

/*
 * Receive Phase C data w/ or w/o copy quality checking.
 */
bool
Class1Modem::recvPageData(TIFF* tif, fxStr& emsg)
{
    bool ret = false;
    /*
     * T.30-A ECM mode requires a substantially different protocol than non-ECM faxes.
     */
    if (params.ec != EC_DISABLE) {
	if (!recvPageECMData(tif, params, emsg)) {
	    /*
	     * The previous page experienced some kind of error.  Falsify
	     * some event settings in order to cope with the error gracefully.
	     */
	    signalRcvd = FCF_EOP;
	    messageReceived = true;
	    if (prevPage)
		recvEndPage(tif, params);
	}
        /* data regeneration always occurs in ECM */
	TIFFSetField(tif, TIFFTAG_CLEANFAXDATA, pageDataMissed ?
	    CLEANFAXDATA_REGENERATED : CLEANFAXDATA_CLEAN);
	if (pageDataMissed) {
	    TIFFSetField(tif, TIFFTAG_BADFAXLINES, pageDataMissed);
	}
	ret = true;		// no RTN with ECM
    } else {
	(void) recvPageDLEData(tif, checkQuality(), params, emsg);
	dataSent += getRecvEOLCount();
	dataMissed += getRecvBadLineCount();
        /* data regeneration only occurs in copy quality checking */
	TIFFSetField(tif, TIFFTAG_CLEANFAXDATA, getRecvBadLineCount() ?
	    (checkQuality() ? CLEANFAXDATA_REGENERATED : CLEANFAXDATA_UNCLEAN) : CLEANFAXDATA_CLEAN);
	if (getRecvBadLineCount()) {
	    TIFFSetField(tif, TIFFTAG_BADFAXLINES, getRecvBadLineCount());
	    TIFFSetField(tif, TIFFTAG_CONSECUTIVEBADFAXLINES,
		getRecvConsecutiveBadLineCount());
	}
	ret = isQualityOK(params);
    }
    pageDataMissed = 0;
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH, getRecvEOLCount());
    return (ret);
}

/*
 * CNG detection triggers us to return to the beginning of Phase B and with CED.
 */
bool
Class1Modem::respondToCNG(fxStr& emsg)
{
    (void) switchingPause(emsg);
    if (!(atCmd(conf.CEDCmd, AT_NOTHING) && atResponse(rbuf, 0) == AT_CONNECT)) {
	emsg = "Failure to raise V.21 transmission carrier. {E101}";
	protoTrace(emsg);
	return (false);
    }
    if (gotEOT) return (false);

    FaxSetup setupinfo;
    setupinfo.senderConfusesRTN = senderConfusesRTN;
    setupinfo.senderConfusesPIN = senderConfusesPIN;
    setupinfo.senderSkipsV29 = senderSkipsV29;
    setupinfo.senderHasV17Trouble = senderHasV17Trouble;
    setupinfo.senderDataSent = dataSent;
    setupinfo.senderDataMissed = dataMissed;
    setupinfo.senderFumblesECM = senderFumblesECM;

    return Class1Modem::recvBegin(&setupinfo, emsg);
}

/*
 * Perform a procedure interrupt (PIN or PIP).
 *
 * A procedure interrupt is designed to allow human operators to suspend fax operations
 * and communicate vocally over the phone line before resuming or terminating the fax
 * operations.  If fax operations are resumed they do so at the beginning of Phase B.
 * This is a rarely-used feature that may or may not be supported by remote equipment
 * and great tolerances in the handling of an interrupt should be employed.  For our
 * purposes, a procedure interrupt can be used in order to change our capabilities in
 * an effort to get the sender to change modes.  For example, this could be used to
 * disable ECM and force a sender with bad ECM protocol to resume the fax without ECM.
 *
 * Procedure interrupts are outlined in ITU T.30 flow charts and example figures.
 * In a nutshell, when the procedure interrupt is initiated on the receiver it does so
 * by responding with PIP or PIN to MPS/EOP/EOM/PPS.  The sender should respond
 * positively to this interrupt request with a PRI-Q or PPS-PRI-Q (MPS/EOP/EOM/PPS FCF
 * with the 5th bit set).  The receiver should, then, confirm receipt of PRI-Q/PPS-PRI-Q
 * with another PIP or PIN signal.  At that point, the operators are expected to
 * communicate vocally and resume operations at the beginning of Phase B.  So, the
 * sender should be signaling CNG and expecting CED or V.21 HDLC frames.  The
 * receiver should be expecting CNG or V.21 HDLC frames.
 *
 * There is an expectation that both sides in a procedure interrupt should be tolerant
 * to a T3 timeout (10 +/- 5 sec) after the PIN/PRI-Q exchange.  (Presumably this is the
 * amount of time that the remote equipment gives the human operator to respond to the
 * interrupt alert.)  However, the lapse of that timeout is not mandatory and is
 * unlikely to happen at all as most fax systems are unattended.
 *
 * In practice, some senders appear to treat PIN as they would RTN.  That is, they
 * start transmitting TSI/DCS/TCF after seeing PIN.  They, therefore, are getting ahead
 * of things.  Hopefully those senders will detect our NSF/CSI/DIS and adjust their DCS
 * and TCF appropriately.
 */
bool
Class1Modem::performProceduralInterrupt(bool disableECM, bool positive, fxStr& emsg)
{
    HDLCFrame frame(conf.class1FrameOverhead);
    u_int interruptFCF = positive ? FCF_PIP : FCF_PIN;

    (void) switchingPause(emsg);
    (void) transmitFrame(interruptFCF|FCF_RCVR);
    traceFCF("RECV send", interruptFCF);

    bool gotpriq = false;
    u_short counter = 0;
    bool again = false;
    bool doCED = true;
    do {
	if (recvFrame(frame, FCF_RCVR, TIMER_T3, false, true, true, interruptFCF)) {
	    traceFCF("RECV recv", frame.getFCF());
	    again = frame.moreFrames();
	    switch (frame.getFCF()) {
		case FCF_MPS:
		case FCF_EOP:
		case FCF_EOM:
		    // The sender repeated the post-page message.  Maybe they didn't hear our interrupt signal.  Try again.
		    if (counter > 1) {
			// The sender is failing to respond properly to the interrupt signal. There's not much we can do that does not risk page receipt confirmation.
			protoTrace("This sender appears to not respond properly to procedural interrupts.");
			senderConfusesPIN = true;
			emsg = "RSPREC invalid response to procedural interrupt";
			return (false);
		    }
		    (void) switchingPause(emsg);
		    (void) transmitFrame(interruptFCF|FCF_RCVR);
		    traceFCF("RECV send", interruptFCF);
		    again = true;
		    break;
		case FCF_PRI_MPS:
		case FCF_PRI_EOP:
		case FCF_PRI_EOM:
		    gotpriq = true;
		    break;
		case FCF_PPS:
		    traceFCF("RECV recv", frame.getFCF2());
		    switch (frame.getFCF2()) {
			case FCF_MPS:
			case FCF_EOP:
			case FCF_EOM:
			    // The sender repeated the partial-page message.  Maybe they didn't hear our interrupt signal.  Try again.
			    if (counter > 1) {
				// The sender is failing to respond properly to the interrupt signal. There's not much we can do other than respond with PPR, which would defeat our reason for performing the interrupt.
				protoTrace("This sender appears to not respond properly to procedural interrupts.");
				senderConfusesPIN = true;
				emsg = "RSPREC invalid response to procedural interrupt";
				return (false);
			    }
			    (void) switchingPause(emsg);
			    (void) transmitFrame(interruptFCF|FCF_RCVR);
			    traceFCF("RECV send", interruptFCF);
			    again = true;
			    break;
			case FCF_PRI_MPS:
			case FCF_PRI_EOP:
			case FCF_PRI_EOM:
			    gotpriq = true;
			    break;
		    }
		    break;
		case FCF_DCS:
		    // The sender treats PIN as RTN.  Expect TCF to follow, but we'll disregard both signals
		    recvTraining(false);
		    again = false;
		    doCED = false;	// CED introduces too much delay - and this sender isn't expecting it, anyway.
		    break;
		case FCF_DCN:
		    protoTrace("This sender appears to not respond properly to procedural interrupts.");
		    senderConfusesPIN = true;
		    emsg = "RSPREC error/got DCN (sender abort) {E103}";
		    recvdDCN = true;
		    gotEOT = true;
		    return (false);
		    break;
	    }
	} else {
	    if (wasTimeout() || gotEOT) {
		/*
		 * T3 elapsed without any signal from the sender.
		 * The sender may be waiting for us to signal NSF/CSI/DIS, instead.
		 * Or, the call may have been disconnected.
		 */
		again = false;
	    } else {
		// we received a corrupt signal, wait for another
		again = true;
	    }
	}
    } while (again && ++counter < 10);

    if (gotpriq && !gotEOT) {
	// re-send PIP/PIN, per T.30 figures
	(void) switchingPause(emsg);
	if (!gotEOT) transmitFrame(interruptFCF|FCF_RCVR);
	traceFCF("RECV send", interruptFCF);
    }
    if (!isSSLFax && !useV34 && !gotEOT) {
	(void) switchingPause(emsg);
	if (!(atCmd(doCED ? conf.CEDCmd : thCmd, AT_NOTHING) && atResponse(rbuf, 0) == AT_CONNECT)) {
	    emsg = "Failure to raise V.21 transmission carrier. {E101}";
	    protoTrace(emsg);
	    return (false);
	}
    }
    if (gotEOT) return (false);

    FaxSetup setupinfo;
    setupinfo.senderConfusesRTN = senderConfusesRTN;
    setupinfo.senderConfusesPIN = senderConfusesPIN;
    setupinfo.senderSkipsV29 = senderSkipsV29;
    setupinfo.senderHasV17Trouble = senderHasV17Trouble;
    setupinfo.senderDataSent = dataSent;
    setupinfo.senderDataMissed = dataMissed;
    setupinfo.senderFumblesECM = disableECM;

    return Class1Modem::recvBegin(&setupinfo, emsg);
}

/*
 * Complete a receive session.
 */
bool
Class1Modem::recvEnd(FaxSetup* setupinfo, fxStr& emsg)
{
    if (setupinfo) {
	/*
	 * Update FaxMachine info...
	 */
	setupinfo->senderFumblesECM = senderFumblesECM;
	setupinfo->senderConfusesRTN = senderConfusesRTN;
	setupinfo->senderConfusesPIN = senderConfusesPIN;
	setupinfo->senderSkipsV29 = senderSkipsV29;
	setupinfo->senderHasV17Trouble = senderHasV17Trouble;
	setupinfo->senderDataSent = dataSent;
	setupinfo->senderDataMissed = dataMissed;
    }
    if (!recvdDCN && !gotEOT) {
	u_int t1 = howmany(conf.t1Timer, 1000);	// T1 timer in seconds
	time_t start = Sys::now();
	/*
	 * Wait for DCN and retransmit ack of EOP if needed.
	 */
	HDLCFrame frame(conf.class1FrameOverhead);
	do {
	    gotRTNC = false;
	    if (recvFrame(frame, FCF_RCVR, conf.t2Timer)) {	// here we permit CRP because we don't care if the sender interprets it as MCF
		traceFCF("RECV recv", frame.getFCF());
		switch (frame.getFCF()) {
		case FCF_PPS:
		case FCF_EOP:
		case FCF_CRP:
		case FCF_RR:
		    (void) switchingPause(emsg);
		    if (!gotEOT) (void) transmitFrame(FCF_MCF|FCF_RCVR);
		    traceFCF("RECV send", FCF_MCF);
		    break;
		case FCF_DCN:
		    recvdDCN = true;
		    break;
		default:
		    (void) switchingPause(emsg);
		    if (!gotEOT) transmitFrame(FCF_DCN|FCF_RCVR);
		    recvdDCN = true;
		    break;
		}
	    } else if (gotRTNC) {
		(void) transmitFrame(FCF_MCF|FCF_RCVR);
		traceFCF("RECV send", FCF_MCF);
	    } else if (!wasTimeout() && lastResponse != AT_FCERROR && lastResponse != AT_FRH3) {
		/*
		 * Beware of unexpected responses from the modem.  If
		 * we lose carrier, then we can loop here if we accept
		 * null responses, or the like.
		 */
		break;
	    }
	} while ((unsigned) Sys::now()-start < t1 && !gotEOT && (!frame.isOK() || !recvdDCN));
    }
    setInputBuffering(true);
    return (true);
}

/*
 * Abort an active receive session.
 */
void
Class1Modem::recvAbort()
{
    if (!recvdDCN && !gotEOT) {
	checkReadOnWrite = false;		// we're aborting and expect that there may be data to read but need to ignore it
	fxStr emsg;
	switchingPause(emsg);
	if (!gotEOT) transmitFrame(FCF_DCN|FCF_RCVR);
    }
    recvdDCN = true;				// don't hang around in recvEnd
}
