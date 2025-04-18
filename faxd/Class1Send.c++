/*	$Id: Class1Send.c++ 1178 2013-07-29 22:14:23Z faxguy $ */
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
 * Send protocol.
 */
#include "config.h"
#include "Sys.h"
#include "Class1.h"
#include "ModemConfig.h"
#include "HDLCFrame.h"
#include "t.30.h"
#include "itufaxicc.h"
#include "FaxRequest.h"

/*
 * Force the tty into a known flow control state.
 */
bool
Class1Modem::sendSetup(FaxRequest& req, const Class2Params& dis, fxStr& emsg)
{
    if (!req.usesslfax) {
	// The client has requested that we disable SSL Fax.
	suppressSSLFax = true;
	useSSLFax = false;
    }
    if (flowControl == FLOW_XONXOFF)
	setXONXOFF(FLOW_NONE, FLOW_NONE, ACT_FLUSH);
    if (isSSLFax) {
	return (FaxModem::sendSetup(req, dis, emsg));
    } else {
	return (FaxModem::sendSetup(req, dis, emsg) && FaxModem::setOrigin(req));
    }
}

/*
 * Wait-for and process a dial command response.
 */
CallStatus
Class1Modem::dialResponse(fxStr& emsg)
{
    // This is as good a time as any, perhaps, to reset modemParams.br.
    // If the call does V.8 handshaking, then it will be altered.
    modemParams.br = nonV34br;

    int ntrys = 0;
    ATResponse r;
    do {
	r = atResponse(rbuf, conf.dialResponseTimeout);
	
	/*
	 * Blacklisting is handled internally just like a NOCARRIER.
	 * emsg is customized to let the user know the problem lies in
	 * the modem and not in line conditions, cables ...
	 * The known blacklisting modem responses are:
	 * 1. "BLACKLISTED"
	 * 2. "DELAYED HH:MM:SS" (ie: "DELAYED 00:59:02")
	 * 3. "DIALING DISABLED" (USR)
	 * User can switch on/off the modem or use appropriate reset commands
	 * to clear/disable blacklisted numbers, ie:
	 * ModemResetCmds: AT%TCB  (Some rockwell chipsets)
	 * ModemResetCmds: AT%D0   (Some topic chipsets)
	 * ModemResetCmds: ATS40=7 (Some usr chipsets)
	 */
	if (strncmp(rbuf, "BLACKLISTED", 11) == 0
		|| strncmp(rbuf, "DELAYED", 7) == 0
		|| strncmp(rbuf, "DIALING DISABLED", 16) == 0) {
	    emsg = "Blacklisted by modem {E010}";
	    return (NOCARRIER);
	}

	switch (r) {
	case AT_ERROR:	    return (ERROR);	// error in dial command
	case AT_BUSY:	    return (BUSY);	// busy signal
	case AT_NOCARRIER:  return (NOCARRIER);	// no carrier detected
	case AT_NODIALTONE: return (NODIALTONE);// local phone connection hosed
	case AT_NOANSWER:   return (NOANSWER);	// no answer or ring back
	case AT_TIMEOUT:    return (FAILURE);	// timed out w/o response
	case AT_CONNECT:    return (OK);	// fax connection
	case AT_DLEEOT:
	case AT_OK:
	    /*
	     * Apparently some modems (like the AT&T DataPort) can respond OK
	     * to indicate NO CARRIER.
             *
	     * Other modems, like the Lucent/Agere Venus and Agere/LSI OCM/OCF and
	     * CFAX34 can respond OK or DLE+EOT to indicate a V.34/V.8 handshake
	     * incompatibility.  We need to trigger hasV34Trouble for this case.
	     *
	     * And yet other modems, like the Silicon Laboratories Si2435, appear
	     * to attempt a spotty ITU V.251 section 7 implementation and result OK
	     * whenever ANSam is not detected.  (This appears to defy the last
	     * direction in V.251 7.3 as it does this even if <v8o>=6 and V.21 HDLC
	     * is detected.  Apparently this is a stated effort to allow the DTE to re-attempt
	     * V.8 handshaking.)  Therefore, in this case an automatic fallback to G3 mode does
	     * not occur and to fall back we are required issue +FRH=3 and interpret
	     * a timeout as a NO CARRIER condition (because it reports +A8A:4 instead
	     * of +A8A:0 when any signal - such as a busy signal - occurs after an
	     * answer condition is detected).
	     */
	    if (conf.class1EnableV34Cmd != "" && serviceType == SERVICE_CLASS10) {
		if (conf.class10AutoFallback) return (V34FAIL);
		/*
		 * As the modem says that it already detected a carrier of some sort, presumably
		 * V.21 HDLC, we don't need to wait T1 or even T2 for it.  So, we set a short
		 * timeout period for the CONNECT message here in case the modem glitch mentioned
		 * below is an issue on this call.
		 */
		if (atCmd(rhCmd, AT_NOTHING) && atResponse(rbuf, 2000) == AT_CONNECT) return (OK);
		if (wasTimeout()) {
		    /*
		     * The Si2435 has a glitch (at least in "D" firmware) where it can miss the +FRH=3
		     * command above at this point in the call.  The workaround is to just re-issue the
		     * command.  We deliberately do not abortRecive() here, as that trips things up further.
		     */
		    if (atCmd(rhCmd, AT_NOTHING) && atResponse(rbuf, conf.t1Timer) == AT_CONNECT) return (OK);
		    if (wasTimeout()) {
			// Apparently T1 elapsed with no further carrier signal.
			abortReceive();
			return (NOCARRIER);
		    }
		    if (lastResponse == AT_OK) {
			/*
			 * The second +FRH=3 actually canceled the first.  So, in this instance there was
			 * no glitch, and we probably should have had a longer timeout setting on the first
			 * +FRH=3.  But, since the glitch condition is more likely we'll just re-issue +FRH=3
			 * one last time.
			 */
			if (atCmd(rhCmd, AT_NOTHING) && atResponse(rbuf, conf.t1Timer) == AT_CONNECT) return (OK);
			if (wasTimeout()) {
			    // Apparently T1 elapsed with no further carrier signal.
			    abortReceive();
			    return (NOCARRIER);
			}
			if (lastResponse == AT_OK) {
			    // The Si2435 appears to result OK on its own after 30 seconds' wait at this point.
			    return (NOCARRIER);
			}
			// We got some other result (besides CONNECT or OK) to the third +FRH=3 command.
			return (FAILURE);
		    }
		    // We got some other result (besides CONNECT or OK) to the second +FRH=3 command.
		    return (FAILURE);
		}
		// We got some other result (besides CONNECT) to the second +FRH=3 command.
		return (FAILURE);
	    }
	    if (r == AT_OK)
		return (NOCARRIER);
	    else
		return (FAILURE);
	case AT_FCERROR:
	    /*
	     * Some modems that support adaptive-answer assert a data
	     * carrier first and then fall back to answering as a fax
	     * modem.  For these modems we'll get an initial +FCERROR
	     * (wrong carrier) result that we ignore.  We actually
	     * accept three of these in case the modem switches carriers
	     * several times (haven't yet encountered anyone that does).
	     */
	case AT_DLEETX:				// silly modem
	    if (++ntrys == 3) {
		emsg = "Ringback detected, no answer without CED {E011}"; // XXX
		protoTrace(emsg);
		return (NOFCON);
	    }
	    r = AT_OTHER;
	    break;
	case AT_DTMF:
	    r = AT_OTHER;
	    break;
	}
    } while (r == AT_OTHER && isNoise(rbuf));
    return (FAILURE);
}

void
Class1Modem::sendBegin()
{
    FaxModem::sendBegin();
    setInputBuffering(false);
    // polling accounting?
    params.br = (u_int) -1;			// force initial training
    dataSent = 0;
    dataMissed = 0;
}

#define BATCH_FIRST 1
#define BATCH_LAST  2

void
Class1Modem::checkReceiverDIS(Class2Params& params)
{
    if (useV34) {
	if (params.ec == EC_DISABLE) {
	    protoTrace("V.34-Fax session, but DIS signal contains no ECM bit; ECM forced.");
	    params.ec = EC_ENABLE256;
	}
	if (params.br != BR_33600) {
	    protoTrace("V.34-Fax session, but DIS signal contains no V.8 bit; compensating.");
	    params.br = BR_33600;
	}
    }
}

/*
 * Get the initial DIS command.
 */
FaxSendStatus
Class1Modem::getPrologue(Class2Params& params, bool& hasDoc, fxStr& emsg, u_int& batched, bool docng)
{
    u_int t1 = howmany(conf.t1Timer, 1000);		// T1 timer in seconds
    time_t start = Sys::now();
    HDLCFrame frame(conf.class1FrameOverhead);

    if (!isSSLFax && useV34 && (batched & BATCH_FIRST))
	waitForDCEChannel(true);		// expect control channel

    bool framerecvd = false;
    if (batched & BATCH_FIRST) {		// receive carrier raised
	// T.38 invite/transfer will often cause a "stutter" in the audio carrying the initial V.21 HDLC
	// To cope with that we deliberately tell recvFrame to handle stutter by re-issuing +FRH=3 if
	// an immediate NO CARRIER result is had after the CONNECT.
	framerecvd = recvFrame(frame, FCF_SNDR, conf.t1Timer, true, true, true, 0, false, true, FCF_CSI);	// T1 is used here, handle stutter
    } else if (docng) {
	framerecvd = recvFrame(frame, FCF_SNDR, conf.t1Timer, false, true, true, 0, docng);
    } else {					// receive carrier not raised
	// We're not really switching directions of communication, but we don't want
	// to start listening for prologue frames until we're sure that the receiver 
	// has dropped the carrier used to signal MCF.
	(void) switchingPause(emsg);
	// The receiver will allow T2 to elapse intentionally here.
	// To keep recvFrame from timing out we double our wait.
	framerecvd = recvFrame(frame, FCF_SNDR, conf.t2Timer * 2);
    }

    for (;;) {
	if (gotEOT) {
	    if (!isSSLFax && useV34) hadV34Trouble = true;	// it appears that the connection is too poor for the V.34 carrier
	    break;
	}
	if (framerecvd) {
	    /*
	     * An HDLC frame was received; process any
	     * optional frames that precede the DIS.
	     */
	    do {
		switch (frame.getRawFCF()) {
		case FCF_NSF:
		    {
			const u_char* features = recvNSF(NSF(frame.getFrameData(), frame.getFrameDataLength()-1, frameRev));
			if (features) dis_caps.features = features[0];
		    }
		    break;
		case FCF_CSI:
		    { fxStr csi; recvCSI(decodeTSI(csi, frame)); }
		    break;
		case FCF_DIS:
		    u_char features = dis_caps.features;
		    dis_caps = frame.getDIS();
		    dis_caps.features = features;

		    if (dis_caps.isBitEnabled(FaxParams::BITNUM_T37) && dis_caps.isBitEnabled(FaxParams::BITNUM_T38)) {
			/*
			 * DIS bit 1 implies T.37 support, and bit 3 implies T.38 support, but
			 * these bits really just mean that CIA, TSA, and CSA are supported.
			 * Unfortunately, many devices that enable them don't really support
			 * these frames.  So, we have to make distinctions.
			 */
			protoTrace("REMOTE supports internet fax");
		    } else if (dis_caps.isBitEnabled(FaxParams::BITNUM_T37)) {
			protoTrace("REMOTE supports internet fax (T.37)");
		    } else if (dis_caps.isBitEnabled(FaxParams::BITNUM_T38)) {
			protoTrace("REMOTE supports internet fax (T.38)");
		    }
		    if (dis_caps.isBitEnabled(FaxParams::BITNUM_BFT) && dis_caps.isBitEnabled(FaxParams::BITNUM_ECM)) {
			protoTrace("REMOTE supports T.434 binary file transfer");
			if (dis_caps.isBitEnabled(FaxParams::BITNUM_BFT_SIMPLE)) {
			    protoTrace("REMOTE supports Annex B simple Phase C binary file transfer negotiations");
			}
			if (dis_caps.isBitEnabled(FaxParams::BITNUM_BFT_EXTENDED)) {
			    protoTrace("REMOTE supports extended binary file transfer negotiations");
			}
		    }

		    params.setFromDIS(dis_caps);
		    checkReceiverDIS(params);
		    curcap = NULL;			// force initial setup
		    break;
		}
	    } while (frame.moreFrames() && recvFrame(frame, FCF_SNDR, conf.class1FRHNeedsReset ? conf.t4Timer : ((t1-(Sys::now()-start))*1000 + conf.t2Timer)));	// wait to T1 timeout, but at least T2 unless reset needed
	    if (frame.isOK()) {
		switch (frame.getRawFCF()) {
		case FCF_DIS:
		    hasDoc = dis_caps.isBitEnabled(FaxParams::BITNUM_T4XMTR);// documents to poll?
		    if (!dis_caps.isBitEnabled(FaxParams::BITNUM_T4RCVR)) {
			emsg = "Remote has no T.4 receiver capability {E122}";
			protoTrace(emsg);
		    	if (! hasDoc)	// don't die if we can poll
			    return (send_failed);
		    }
		    emsg = "";
		    return (send_ok);
		case FCF_DTC:				// NB: don't handle DTC
		    emsg = "DTC received when expecting DIS (not supported) {E123}";
		    protoTrace(emsg);
		    return (send_retry);
		case FCF_DCN:
		    emsg = "COMREC error in transmit Phase B/got DCN {E124}";
		    protoTrace(emsg);
		    return (send_retry);
		default:
		    if (frame.getRawFCF() & FCF_SNDR) {
			protoTrace("The receiver thinks that they are the sender.");
		    }
		    traceFCF("Unexpectedly received", frame.getFCF());
		    emsg = "COMREC invalid command received/no DIS or DTC {E125}";
		    protoTrace(emsg);
		    break;
		}
	    }
	}
	/*
	 * Wait up to T1 for a valid DIS.
	 */
	time_t now = Sys::now();
	if ((unsigned) now-start >= t1)
	    break;
	framerecvd = recvFrame(frame, FCF_SNDR, ((t1-(now-start)))*1000 + conf.t2Timer);	// wait to T1 timeout, but wait at least T2
    }
    if (emsg == "") {
	emsg = "No receiver protocol (T.30 T1 timeout) {E126}";
	protoTrace(emsg);
    }
    return (send_retry);
}

/*
 * Setup file-independent parameters prior
 * to entering Phase B of the send protocol.
 */
void
Class1Modem::sendSetupPhaseB(const fxStr& p, const fxStr& s)
{
    if (p != fxStr::null && dis_caps.isBitEnabled(FaxParams::BITNUM_PWD))
	encodePWD(pwd, p);
    else
	pwd = fxStr::null;
    if (s != fxStr::null && dis_caps.isBitEnabled(FaxParams::BITNUM_SUB))
	encodePWD(sub, s);
    else
	sub = fxStr::null;
}

const u_int Class1Modem::modemPFMCodes[8] = {
    FCF_MPS,		// PPM_MPS
    FCF_EOM,		// PPM_EOM
    FCF_EOP,		// PPM_EOP
    FCF_EOP,		// 3 ??? XXX
    FCF_PRI_MPS,	// PPM_PRI_MPS
    FCF_PRI_EOM,	// PPM_PRI_EOM
    FCF_PRI_EOP,	// PPM_PRI_EOP
    FCF_EOP,		// 7 ??? XXX
};

bool
Class1Modem::decodePPM(const fxStr& pph, u_int& ppm, fxStr& emsg)
{
    if (FaxModem::decodePPM(pph, ppm, emsg)) {
	ppm = modemPFMCodes[ppm];
	return (true);
    } else
	return (false);
}

void
Class1Modem::getDataStats(FaxSetup* setupinfo)
{
    setupinfo->senderDataSent = dataSent;
    setupinfo->senderDataMissed = dataMissed;
    return;
}

/*
 * Send the specified document using the supplied
 * parameters.  The pph is the post-page-handling
 * indicators calculated prior to initiating the call.
 */
FaxSendStatus
Class1Modem::sendPhaseB(TIFF* tif, Class2Params& next, FaxMachineInfo& info,
    fxStr& pph, fxStr& emsg, u_int& batched)
{
    int ntrys = 0;			// # retraining/command repeats
    bool morePages = true;		// more pages still to send
    bool sawProcedureInterrupt = false;	// whether or not PIP/PIN was seen
    HDLCFrame frame(conf.class1FrameOverhead);

    do {
        hadV34Trouble = false;		// to monitor failure type
        hadV17Trouble = false;		// to monitor failure type
	batchingError = false;
	signalRcvd = 0;
	if (abortRequested())
	    return (send_failed);
	if (repeatPhaseB) {
	    /*
	     * We need to repeat protocol from the beginning of Phase B
	     * due to a non-batched EOM signal (formatting change) and/or
	     * a procedural interrupt.  However, because document preparation
	     * was handled during the job preparation phase (before the fax
	     * call was placed), and because we have no way to change it
	     * during the session, we cannot continue if the new remote
	     * capabilties are now incompatible with those parameters in
	     * the page we're sending.
	     */
	    batched = batched & ~BATCH_FIRST;	// must raise V.21 receive carrier
	    bool hasDoc;
	    FaxSendStatus status = getPrologue(params, hasDoc, emsg, batched, sawProcedureInterrupt);
	    if (status != send_ok) return (send_retry);

	    // Here we scale down our parameters to match the new remote capabilities or fail if we can't do that.
	    if (!(params.vr & BIT(next.vr))) {
		protoTrace("Cannot continue due to incompatible change in receiver resolution.");
		return (send_retry);
	    }
	    if (params.br < next.br) next.br = params.br;
	    if (params.wd < next.wd) {
		protoTrace("Cannot continue due to incompatible change in receiver page width.");
		return (send_retry);
	    }
	    if (params.ln < next.ln) {
		protoTrace("Cannot continue due to incompatible change in receiver page length.");
		return (send_retry);
	    }
	    if (!(params.df & BIT(next.df)) && !conf.softRTFCC) {
		protoTrace("Cannot continue due to incompatible change in receiver format support.");
		return (send_retry);
	    } else {
		if (params.df & BIT(DF_JBIG)) next.df = DF_JBIG;
		else if (params.df & BIT(DF_2DMMR)) next.df = DF_2DMMR;
		else if (params.df & BIT(DF_2DMR)) next.df = DF_2DMR;
		else params.df = DF_1DMH;
	    }
	    if (params.ec < next.ec) next.ec = params.ec;
	    if (params.bf < next.bf) {
		protoTrace("Cannot continue due to incompatible change in receiver binary format support.");
		return (send_retry);
	    }
	    if (params.jp < next.jp) {
		protoTrace("Cannot continue due to incompatible change in receiver color format support.");
		return (send_retry);
	    }
	    params.br = (u_int) -1;		// force retraining
	    repeatPhaseB = false;
	    sawProcedureInterrupt = false;
	}
	/*
	 * Check the next page to see if the transfer
	 * characteristics change.  If so, update the
	 * T.30 session parameters and do training.
	 * Note that since the initial parameters are
	 * setup to be "undefined", training will be
	 * sent for the first page after receiving the
	 * DIS frame.
	 */
	if (params != next) {
	    if (!sendTraining(next, 3, emsg)) {
		if (hadV34Trouble) {
		    protoTrace("The destination appears to have trouble with V.34-Fax.");
		    return (send_v34fail);
		}
		if (hadV17Trouble) {
		    protoTrace("The destination appears to have trouble with V.17.");
		    return (send_v17fail);
		}
		if (!(batched & BATCH_FIRST)) {
		    protoTrace("The destination appears to not support batching.");
		    return (send_batchfail);
		}
		return (send_retry);
	    }
	    params = next;
	}

	if (params.ec == EC_DISABLE || useSSLFax) {		// ECM does it later
	    /*
	     * According to T.30 5.3.2.4 we must pause at least 75 ms "after 
	     * receipt of a signal using the T.30 binary coded modulation" and 
	     * "before sending any signals using V.27 ter/V.29/V.33/V.17 
	     * modulation system"
	     *
	     * We also do this pause when doing SSL Fax to help the receiver
	     * in being ready for our connection.
	     */
	    if (!switchingPause(emsg)) {
		return (send_failed);
	    }
	}
#if defined(HAVE_SSL)
	if (!suppressSSLFax && !isSSLFax && useSSLFax && remoteCSAType == 0x40 && remoteCSAinfo.length()) {
	    protoTrace("Connecting to %s for SSL Fax transmission.", (const char*) remoteCSAinfo);
	    SSLFax sslfax;
	    sslFaxProcess = sslfax.startClient(remoteCSAinfo, sslFaxPasscode, rtcRev, conf.class1SSLFaxServerTimeout);
	    if (sslFaxProcess.emsg != "") protoTrace("SSL Fax: \"%s\"", (const char*) sslFaxProcess.emsg);
	    if (!sslFaxProcess.server) {
		protoTrace("SSL Fax connection failed.");
		useSSLFax = false;
	    } else {
		protoTrace("SSL Fax connection was successful.");
		isSSLFax = true;
		storedBitrate = params.br;
		params.br = BR_SSLFAX;
	    }
	}
#endif
	/*
	 * The ECM protocol needs to know PPM, so this must be done beforehand...
	 */
	morePages = !TIFFLastDirectory(tif);
	u_int cmd;
	if (!decodePPM(pph, cmd, emsg))
	    return (send_failed);
	/*
	 * If pph tells us that the PPM is going to be EOM it means that the
	 * next page is formatted differently from the current page.  To handle this
	 * situation properly, EOM is used.  Following EOM we must repeat the
	 * entire protocol of Phase B.  This triggers that.  Batching is handled
	 * also with EOM, but not with the repeatPhaseB flag.
	 */
	repeatPhaseB = (cmd == FCF_EOM);
	if (cmd == FCF_EOP && !(batched & BATCH_LAST))
	    cmd = FCF_EOM;

	/*
	 * Transmit the facsimile message/Phase C.
	 */
	if (!sendPage(tif, params, decodePageChop(pph, params), cmd, emsg)) {
	    if (hadV34Trouble) {
		protoTrace("The destination appears to have trouble with V.34-Fax.");
		return (send_v34fail);
	    }
	    if (batchingError && (batched & BATCH_FIRST)) {
		protoTrace("The destination appears to not support batching.");
		return (send_batchfail);
	    }
	    return (send_retry);	// a problem, disconnect
	}

	int ncrp = 0;

	if (params.ec == EC_DISABLE) {
	    /*
	     * Delay before switching to the low speed carrier to
	     * send the post-page-message frame according to 
	     * T.30 chapter 5 note 4.  We provide for a different
	     * setting following EOP because, empirically, some 
	     * machines may need more time. Beware that, reportedly, 
	     * lengthening this delay too much can permit echo 
	     * suppressors to kick in with bad results.
	     *
	     * Historically this delay was done using a software pause
	     * rather than +FTS because the time between +FTS and the 
	     * OK response is longer than expected, and this was blamed
	     * for timing problems.  However, this "longer than expected"
	     * delay is a result of the time required by the modem's
	     * firmware to actually release the carrier.  T.30 requires
	     * a delay (period of silence), and this cannot be guaranteed
	     * by a simple pause.  +FTS must be used.
	     */
	    if (!isSSLFax && !atCmd(cmd == FCF_MPS ? conf.class1PPMWaitCmd : conf.class1EOPWaitCmd, AT_OK)) {
		emsg = "Stop and wait failure (modem on hook) {E127}";
		protoTrace(emsg);
		if (lastResponse == AT_ERROR) gotEOT = true;
		return (send_retry);
	    }
	}

	u_int ppr;
	do {
	    if (signalRcvd == 0) {
		/*
		 * Send post-page message and get response.
		 */
		dataSent++;	// non-ECM pages are whole units instead of per-scanline units giving preference to ECM sessions
		if (!sendPPM(cmd, frame, emsg)) {
		    if (cmd == FCF_EOM && (batched & BATCH_FIRST)) {
			protoTrace("The destination appears to not support batching.");
			return (send_batchfail);
		    }
		    return (send_retry);
		}
		ppr = frame.getFCF();
		traceFCF("SEND recv", ppr);
	    } else {
		// ECM protocol already got post-page response
		ppr = signalRcvd;
		frame.reset();
		for (u_int i = 0; i < frameRcvd.length(); i++) frame.put(frameRcvd[i]);
		frame.setOK(true);
	    }
	    switch (ppr) {
	    case FCF_RTP:		// ack, continue after retraining
		params.br = (u_int) -1;	// force retraining above
		/* fall thru... */
	    case FCF_MCF:		// ack confirmation
		countPage();		// bump page count
		notifyPageSent(tif);	// update server
		if (pph[4] == 'Z')
		    pph.remove(0,4+5+1);// discard page-chop+handling info
		else
		    pph.remove(0,5);	// discard page-handling info
		if (params.ec == EC_DISABLE) (void) switchingPause(emsg);
		ntrys = 0;
		if (morePages) {	// meaning, more pages in this file, but there may be other files
		    if (!TIFFReadDirectory(tif)) {
			emsg = "Problem reading document directory {E302}";
			protoTrace(emsg);
			return (send_failed);
		    }
		}
		if (cmd != FCF_EOP) {
		    if (ppr == FCF_MCF && !repeatPhaseB) {
			/*
			 * The session parameters cannot change except following
			 * the reception of an RTN or RTP signal or the transmission
			 * of an EOM signal.
			 *
			 * Since we did not receive RTN or RTP, if EOM was not used
			 * (repeating from the start of Phase B) then we require that
			 * the next page have the same characteristics as this page.
			 */
			next = params;
		    }
		}
		break;
	    case FCF_DCN:		// disconnect, abort
		emsg = "Remote fax disconnected prematurely {E128}";
		protoTrace(emsg);
		return (send_retry);
	    case FCF_RTN:		// nak, retry after retraining
		/*
		 * The receiver rejected the quality of the previous Phase C communication.
		 * This may mean that the image quality was unacceptable or it may mean that
		 * the receiver picked up on some other demodulation artifact that it didn't
		 * like.  However, the RTN signal does not indicate with any degree of
		 * reliability whether or not the receiver printed the page at all or what it
		 * expects us to do with the previous page (retransmit or not).  So it's very
		 * vague.  The default we follow is to retransmit two more times and then to
		 * move on to the next page if the RTN signals persist.
		 *
		 * With respect to the handling of dataMissed here, we really don't know how
		 * much data was missed or received corrupt by the sender because the RTN
		 * signal doesn't give us any indication.  So we make a nominal effort here
		 * only in that regard.
		 */
		dataMissed++;	// non-ECM pages are whole units instead of per-scanline units giving preference to ECM sessions
                switch( conf.rtnHandling ){
		case RTN_RETRANSMITIGNORE:
		    if (ntrys < 2) break;
                case RTN_IGNORE:
			// ignore error and try to send next page
			// after retraining
		    params.br = (u_int) -1;	// force retraining above
		    countPage();		// bump page count
		    notifyPageSent(tif);	// update server
		    if (pph[4] == 'Z')
			pph.remove(0,4+5+1);// discard page-chop+handling info
		    else
			pph.remove(0,5);	// discard page-handling info
		    ntrys = 0;
		    if (morePages) {
			if (!TIFFReadDirectory(tif)) {
			    emsg = "Problem reading document directory {E302}";
			    protoTrace(emsg);
			    return (send_failed);
			}
			FaxSendStatus status =
			    sendSetupParams(tif, next, info, emsg);
			if (status != send_ok)
			    return (status);
		    }
		    continue;
                case RTN_GIVEUP:
                    emsg = "Unable to transmit page (giving up after RTN) {E130}";
		    protoTrace(emsg);
                    return (send_failed); // "over and out"
                }
                // case RTN_RETRANSMIT
                if (++ntrys >= 3) {
                    emsg = "Unable to transmit page (giving up after 3 attempts) {E131}";
		    protoTrace(emsg);
                    return (send_retry);
                }
		params.br = (u_int) -1;	// force training
		if (!dropToNextBR(next)) {
                    emsg = "Unable to transmit page (NAK at all possible signalling rates) {E132}";
		    protoTrace(emsg);
                    return (send_retry);
		}
                morePages = true;	// retransmit page
		break;
	    case FCF_PIP:		// ack, w/ operator intervention
		countPage();		// bump page count
		notifyPageSent(tif);	// update server
		if (pph[4] == 'Z')
		    pph.remove(0,4+5+1);// discard page-chop+handling info
		else
		    pph.remove(0,5);	// discard page-handling info
		if (params.ec == EC_DISABLE) (void) switchingPause(emsg);
		ntrys = 0;
		if (morePages) {	// meaning, more pages in this file, but there may be other files
		    if (!TIFFReadDirectory(tif)) {
			emsg = "Problem reading document directory {E302}";
			protoTrace(emsg);
			return (send_failed);
		    }
		}
		/* fall thru... */
	    case FCF_PIN:		// nak, retry w/ operator intervention
		{
		    if (params.ec == EC_DISABLE) {
			// Transmit PRI-Q before looking for the FCF_PIN reply.  ECM already transmitted PPS-PRI-Q.
			if (!switchingPause(emsg)) return (send_retry);
			switch (cmd) {
			    case FCF_MPS:
			    case FCF_EOP:
			    case FCF_EOM:
				traceFCF("SEND send", cmd|FCF_PRIQ);
				if (!transmitFrame(cmd|FCF_PRIQ)) return (send_retry);
				break;
			    // If the receiver signaled PIP/PIN out-of-spec, don't send a PRI-Q response, just wait for the next signals.
			}
		    }
		    u_short counter = 0;
		    bool again = false;
		    do {
			if (recvFrame(frame, FCF_SNDR, TIMER_T3, false, true, true, cmd)) {
			    traceFCF("RECV recv", frame.getFCF());
			    again = frame.moreFrames();
			} else {
			    if (wasTimeout()) {
				// T3 elapsed without any signal
				emsg = "Procedure interrupt timer T3 elapsed without any signal.";
				protoTrace(emsg);
				return (send_retry);
			    } else {
				// we received a corrupt signal, wait for another
				again = true;
			    }
			}
		    } while (again && ++counter < 10);
		}
		// Now we need to transmit CNG and listen for V.21 HDLC from the receiver for the start of Phase B.
                if (ppr == FCF_PIN) morePages = true;	// retransmit page
		repeatPhaseB = true;
		sawProcedureInterrupt = true;
		if (++ntrys > 2) {
                    emsg = "Unable to transmit page (giving up after 3 attempts) {E131}";
		    protoTrace(emsg);
                    return (send_retry);
		}
		break;
	    case FCF_CRP:
		if (!switchingPause(emsg)) {
		    return (send_retry);
		}
		break;
	    case FCF_CFR:
		/*
		 * CFR is out of spec for Phase D.  However, its use would seem to indicate that the receiver
		 * wants us to repeat this iteration of Phase C.  Presumably this will be done by a receiver
		 * that did not train on the Phase C data signal but knew that the call was not hung up, so waited
		 * for the previous iteration of Phase C to end, and repeated CFR in an effort for us to repeat.
		 */
		protoTrace("Apparent out-of-spec attempt to get us to retransmit the page.");
		morePages = true;	// retransmit page
		if (++ntrys > 2) {
                    emsg = "Unable to transmit page (giving up after 3 attempts) {E131}";
		    protoTrace(emsg);
                    return (send_retry);
		}
		break;
	    case FCF_FTT:
	    case FCF_NSF:
	    case FCF_CSI:
	    case FCF_DIS:
		{
		    /*
		     * Prologue frames NSF, CSI, and DIS are out-of-spec in Phase D.  However, some receivers do
		     * use them in Phase D.  Presumably this means that we must return to Phase B and, essentially,
		     * treat these signals similar to what we do with PIN or RTN.
		     */
		    protoTrace("Received out-of-spec Phase B signal in Phase D.  Return to Phase B.");
		    u_short counter = 0;
		    bool again = frame.moreFrames();
		    while (again && counter++ < 10) {
			if (recvFrame(frame, FCF_SNDR, TIMER_T3, false, true, true)) {
			    traceFCF("RECV recv", frame.getFCF());
			    again = frame.moreFrames();
			} else {
			    if (wasTimeout()) {
				// T3 elapsed without any signal
				emsg = "Procedure interrupt timer T3 elapsed without any signal.";
				protoTrace(emsg);
				return (send_retry);
			    } else {
				// we received a corrupt signal, wait for another
				again = true;
			    }
			}
		    }
		}
		params.br = (u_int) -1;	// force retraining
		morePages = true;	// retransmit page
		if (++ntrys > 2) {
                    emsg = "Unable to transmit page (giving up after 3 attempts) {E131}";
		    protoTrace(emsg);
                    return (send_retry);
		}
		break;
	    default:			// unexpected abort
		emsg = "Fax protocol error (unknown frame received) {E134}";
		protoTrace(emsg);
		return (send_retry);
	    }
	} while (ppr == FCF_CRP && ++ncrp < 3);
	if (ncrp == 3) {
	    emsg = "Fax protocol error (command repeated 3 times) {E135}";
	    protoTrace(emsg);
	    return (send_retry);
	}
    } while (morePages);
    if (hadV17Trouble) return (send_v17fail);
    return (send_ok);
}

/*
 * Send ms's worth of zero's at the current signalling rate.
 * Note that we send real zero data here as recommended
 * by T.31 8.3.3 rather than using the Class 1 modem facility
 * to do zero fill.
 *
 * We prefix the zero data with a byte of ones (0xFF) in case
 * a modem does not transmit constant 1 bits as required by
 * T.31 8.3.3 (or we are so fast that the modem is not done
 * training before we start sending data).  This way there is
 * a 1 -> 0 transition which may be expected by the remote.
 */
bool
Class1Modem::sendTCF(const Class2Params& params, u_int ms)
{
    u_int tcfLen = params.transferSize(ms) + 1;
    u_char* tcf = new u_char[tcfLen];
    memset(tcf, 0xFF, 1);
    memset(tcf+1, 0, tcfLen-1);
    bool ok = transmitData(curcap->value, tcf, tcfLen, frameRev, true);
    delete[] tcf;
    return ok;
}

/*
 * Send the training prologue frames:
 *     PWD	password (optional)
 *     SUB	subaddress (optional)
 *     TSI	transmitter subscriber id
 *     DCS	digital command signal
 */
bool
Class1Modem::sendPrologue(FaxParams& dcs_caps, const fxStr& tsi)
{
    /*
     * T.31 8.3.5 requires the DCE to respond CONNECT or result OK within
     * five seconds or it must result ERROR.  T.30 requires the data of
     * the HDLC frame to be tranmsitted in 3 s +/- 15%.  Thus, our
     * timeouts here must be at least 7500 ms and no more than 8500 ms.
     */
    bool frameSent;
    if (isSSLFax || useV34) frameSent = true;
    else {
	fxStr emsg;
	if (!switchingPause(emsg)) {
	    return (false);
	}
	frameSent = (atCmd(thCmd, AT_NOTHING) && atResponse(rbuf, 7550) == AT_CONNECT);
    }
    if (!frameSent)
	return (false);
    if (pwd != fxStr::null) {
	startTimeout(7550);
	bool frameSent = sendFrame(FCF_PWD|FCF_SNDR, pwd, false);
	stopTimeout("sending PWD frame");
	if (!frameSent)
	    return (false);
    }
    if (sub != fxStr::null) {
	startTimeout(7550);
	bool frameSent = sendFrame(FCF_SUB|FCF_SNDR, sub, false);
	stopTimeout("sending SUB frame");
	if (!frameSent)
	    return (false);
    }
    startTimeout(7550);
    frameSent = sendFrame(FCF_TSI|FCF_SNDR, tsi, false);
    stopTimeout("sending TSI frame");
    if (!frameSent)
	return (false);
#if defined(HAVE_SSL)
    if (!suppressSSLFax && !isSSLFax && conf.class1SSLFaxSupport && \
	(conf.class1SSLFaxInfo.length() || conf.class1SSLFaxProxy.length()) && \
	(dis_caps.features & FaxParams::FEATURE_SSLFAX || (dis_caps.isBitEnabled(FaxParams::BITNUM_T37) && dis_caps.isBitEnabled(FaxParams::BITNUM_T38)))) {
	// We can't trust T.38 support indications to indicate that the remote device actually supports TSA frames.
	useSSLFax = true;
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
	    fxStr tsa;
	    encodeCSA(tsa, info, haspasscode);
	    protoTrace("Sending TSA frame to initiate HylaFAX SSL fax feature.");
	    startTimeout(7550);
	    frameSent = sendFrame(FCF_TSA|FCF_SNDR, tsa, false);
	    stopTimeout("sending TSA frame");
	    if (!frameSent) return (false);
	}
    }
#endif
    startTimeout(7550);
    frameSent = sendFrame(FCF_DCS|FCF_SNDR, dcs_caps);
    stopTimeout("sending DCS frame");
    return (frameSent);
}

/*
 * Return whether or not the previously received DIS indicates
 * the remote side is capable of the T.30 DCS signalling rate.
 */
bool
Class1Modem::isCapable(u_int sr, FaxParams& dis)
{
    switch (sr) {
    case DCSSIGRATE_2400V27:
        if (!dis.isBitEnabled(FaxParams::BITNUM_SIGRATE_11) &&
            !dis.isBitEnabled(FaxParams::BITNUM_SIGRATE_12) &&
            !dis.isBitEnabled(FaxParams::BITNUM_SIGRATE_13) &&
            !dis.isBitEnabled(FaxParams::BITNUM_SIGRATE_14))
	    return (true);
	/* fall thru... */
    case DCSSIGRATE_4800V27:
	return dis.isBitEnabled(FaxParams::BITNUM_SIGRATE_12);
    case DCSSIGRATE_9600V29:
    case DCSSIGRATE_7200V29:
	return dis.isBitEnabled(FaxParams::BITNUM_SIGRATE_11);
    case DCSSIGRATE_14400V33:
    case DCSSIGRATE_12000V33:
	// post-1994 revisions of T.30 indicate that V.33 should
	// only be used when it is specifically permitted by DIS
	return(dis.isBitEnabled(FaxParams::BITNUM_SIGRATE_11) &&
	    dis.isBitEnabled(FaxParams::BITNUM_SIGRATE_12) &&
	    dis.isBitEnabled(FaxParams::BITNUM_SIGRATE_13) &&
	    !dis.isBitEnabled(FaxParams::BITNUM_SIGRATE_14));
    case DCSSIGRATE_14400V17:
    case DCSSIGRATE_12000V17:
    case DCSSIGRATE_9600V17:
    case DCSSIGRATE_7200V17:
	return(dis.isBitEnabled(FaxParams::BITNUM_SIGRATE_11) &&
	    dis.isBitEnabled(FaxParams::BITNUM_SIGRATE_12) &&
	    !dis.isBitEnabled(FaxParams::BITNUM_SIGRATE_13) &&
	    dis.isBitEnabled(FaxParams::BITNUM_SIGRATE_14));
    }
    return (false);
}

/*
 * Send capabilities and do training.
 */
bool
Class1Modem::sendTraining(Class2Params& params, int tries, fxStr& emsg)
{
    bool again = false;
    u_short attempt = 0;
    if (tries == 0) {
	emsg = "DIS/DTC received 3 times; DCS not recognized {E136}";
	protoTrace(emsg);
	if (!isSSLFax && useV34) hadV34Trouble = true;	// sadly, some receivers will do this with V.34
	return (false);
    }
    params.update(false);
    // we should respect the frame-size preference indication by the remote (DIS_FRAMESIZE)
    if (params.ec != EC_DISABLE && 
	(params.ec == EC_ENABLE64 || conf.class1ECMFrameSize == 64 || dis_caps.isBitEnabled(FaxParams::BITNUM_FRAMESIZE_DIS))) {
	params.setBit(FaxParams::BITNUM_FRAMESIZE_DCS, true); // we don't want to add this bit if not using ECM
	frameSize = 64;
    } else
	frameSize = 256;

    // Tell the receiver that we support CIA, TSA, and CSA frames.
    if (!suppressSSLFax && conf.class1SSLFaxSupport) {
	if (dis_caps.isBitEnabled(FaxParams::BITNUM_T37)) params.setBit(FaxParams::BITNUM_T37, true);
	if (dis_caps.isBitEnabled(FaxParams::BITNUM_T38)) params.setBit(FaxParams::BITNUM_T38, true);
    }

    if (!isSSLFax && !useV34) {
	/*
	 * Select Class 1 capability: use params.br to hunt
	 * for the best signalling scheme acceptable to both
	 * local and remote (based on received DIS and modem
	 * capabilities gleaned at modem setup time).
	 */
	if (!curcap)
	    curcap = findBRCapability(params.br, xmitCaps);
	curcap++;
	/*
	 * We assume that if the sender requested 9600 or 7200 baud
	 * and if the modem supports both V.17 and V.29 that the
	 * desired modulation is V.29.
	 */
	do {
	    if (!dropToNextBR(params))
		goto failed;
	} while ((params.br == BR_9600 || params.br == BR_7200) && curcap->mod != V29);
    }
    do {
	attempt++;
	if (!isSSLFax && !useV34) {
	    /*
	     * Override the Class 2 parameter bit rate
	     * capability and use the signalling rate
	     * calculated from the modem's capabilities
	     * and the received DIS.  This is because
	     * the Class 2 state does not include the
	     * modulation technique (v.27, v.29, v.17, v.33).
	     *
	     * Technically, according to post-1994 revisions
	     * of T.30, V.33 should not be used except
	     * in the case where the remote announces
	     * specific V.33 support with the 1,1,1,0 rate
	     * bits set.  However, for the sake of versatility
	     * we'll not enforce this upon modems that support
	     * V.33 but not V.17 and will require the user
	     * to disable V.33 if it becomes problematic.  For
	     * modems that support both V.17 and V.33 the
	     * latter is never used.
	     */
	    params.br = curcap->br;
	    params.setBit(FaxParams::BITNUM_SIGRATE_11, curcap->sr&DCSSIGRATE_9600V29);
	    params.setBit(FaxParams::BITNUM_SIGRATE_12, curcap->sr&DCSSIGRATE_4800V27);
	    params.setBit(FaxParams::BITNUM_SIGRATE_13, curcap->sr&DCSSIGRATE_14400V33);
	    params.setBit(FaxParams::BITNUM_SIGRATE_14, curcap->sr&DCSSIGRATE_14400V17);
	} else {
	    /*
	     * T.30 Table 2 Note 33 says that when V.34-fax is used
	     * DCS bits 11-14 should be set to zero.
	     */
	    params.setBit(FaxParams::BITNUM_SIGRATE_11, false);
	    params.setBit(FaxParams::BITNUM_SIGRATE_12, false);
	    params.setBit(FaxParams::BITNUM_SIGRATE_13, false);
	    params.setBit(FaxParams::BITNUM_SIGRATE_14, false);
	}
	/*
	 * Set the number of train attempts on the same modulation.
	 * HylaFAX v4.1 changed this value from 2 to 1 in order to
	 * accelerate the pace at which we'd scan through bitrates
	 * and modulations in order to find one that is acceptable
	 * to the receiver.  T.30 does not specify a pattern to follow
	 * here (as it also does not specify bitrate "fallback"
	 * patterns following RTN/RTP or when using CTC).  The change
	 * made in v4.1 was attributed to "common sense" because if
	 * the receiver rejected training with FTT there seems little
	 * value in retrying the same bitrate and modulation again.
	 * However, it may not be wise to be so eager to slow down
	 * our bitrate in the event of a non-response from the
	 * receiver (or a CRP signal).  So, we return to setting this
	 * value to 2, but in the event of an FTT we drop the bitrate.
	 */
	int t = 2;
	do {
	    bool readPending = false;
	    if (!useV34 && !isSSLFax) protoTrace("SEND training at %s %s",
		modulationNames[curcap->mod],
		Class2Params::bitRateNames[curcap->br]);
	    if (!sendPrologue(params, lid)) {
		if (abortRequested() || gotEOT)
		    goto done;
		if (lastResponse == AT_FRH3 && waitFor(AT_CONNECT, 2*1000)) {
		    readPending = true;
		} else {
		    protoTrace("Error sending T.30 prologue frames");
		    continue;
		}
	    }

	    /*
	     * V.8 handshaking provides training for V.34-fax connections
	     */
	    if (!isSSLFax && !useV34 && !readPending) {
		/*
		 * Delay before switching to high speed carrier
		 * to send the TCF data as required by T.30 chapter
		 * 5 note 3.
		 *
		 * Historically this delay was enforced by a pause,
		 * however, +FTS must be used.  See the notes preceding
		 * Class1PPMWaitCmd above.
		 */
		if (!atCmd(conf.class1TCFWaitCmd, AT_OK)) {
		    if (lastResponse == AT_FRH3 && waitFor(AT_CONNECT, 2*1000)) {
			readPending = true;
		    } else {
			emsg = "Stop and wait failure (modem on hook) {E127}";
			protoTrace(emsg);
			if (lastResponse == AT_ERROR) gotEOT = true;
			return (send_retry);
		    }
		}

		setDataTimeout(5, params.br);		// TCF data is only 1.5 s
		if (!readPending && !sendTCF(params, TCF_DURATION)) {
		    if (lastResponse == AT_FRH3 && waitFor(AT_CONNECT, 2*1000)) {
			readPending = true;
		    } else {
			if (abortRequested())
			    goto done;
			protoTrace("Problem sending TCF data");
		    }
		}

		/*
		 * Some modems may respond OK following TCF transmission
		 * so quickly that the carrier signal has not actually 
		 * dropped.  T.30 requires the receiver to wait 75 +/- 20 
		 * ms before sending a response.  Here we explicitly look for 
		 * that silence before looking for the low-speed carrier.  
		 * Doing this resolves "DIS/DTC received 3 times" errors 
		 * between USR modems and certain HP OfficeJets, in 
		 * particular.
        	 */
		if (conf.class1ResponseWaitCmd != "" && !readPending) {
		    atCmd(conf.class1ResponseWaitCmd, AT_OK);
		}
	    }
#if defined(HAVE_SSL)
	    // This will get recvFrame below to fail if there is an SSL Fax connection.
	    if (useSSLFax) {
		setSSLFaxFd(sslFaxProcess.server);
	    }
#endif
	    /*
	     * Receive response to training.  Acceptable
	     * responses are: DIS or DTC (DTC not handled),
	     * FTT, or CFR; and also a premature DCN.
	     */
	    HDLCFrame frame(conf.class1FrameOverhead);
	    bool gf = recvFrame(frame, FCF_SNDR, conf.t4Timer, readPending, false);
#if defined(HAVE_SSL)
	    if (!isSSLFax && useSSLFax) {
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
			protoTrace("SSL Fax client accept failure.  Proceeding with a traditional fax now.");
			sslfax.cleanup(sslFaxProcess);
			useSSLFax = false;
		    } else {
			abortReceive();
			setSSLFaxFd(sslFaxProcess.server);
			isSSLFax = true;
			storedBitrate = params.br;
			params.br = BR_SSLFAX;
		    }
		    // The SSL Fax connection tripped up recvFrame, so we need to repeat it.
		    if (!isSSLFax) {
			gf = (waitFor(AT_CONNECT, conf.t4Timer) && recvFrame(frame, FCF_SNDR, conf.t4Timer, true, false));
		    } else {
			gf = recvFrame(frame, FCF_SNDR, conf.t4Timer, false, false);
		    }
		}
	    }
#endif
	    if (gf) {
		traceFCF("SEND recv", frame.getFCF());
		do {
		    switch (frame.getFCF()) {
		    case FCF_CSA:
			{
			    fxStr s;
			    decodeCSA(s, frame);
			    protoTrace("REMOTE CSA \"%s\", type 0x%X", (const char*) s, remoteCSAType);
			    if (remoteCSAType == 0x40 && s.length() > 6 && strncmp((const char*) s, "ssl://", 6) == 0) {
				// Looks like this is an SSL Fax receiver.
				if (!suppressSSLFax && conf.class1SSLFaxSupport) useSSLFax = true;
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
		    case FCF_NSF:
			recvNSF(NSF(frame.getFrameData(), frame.getFrameDataLength()-1, frameRev));
			break;
		    case FCF_CSI:
			{ fxStr csi; recvCSI(decodeTSI(csi, frame)); }
			break;
		    }
		} while (frame.moreFrames() && recvFrame(frame, FCF_SNDR, conf.t2Timer));
	    } else if (!fullDuplex && conf.class1FullDuplexTrainingSync) {
		/*
		 * The receiver isn't syncing with us easily.  Try full duplex.
		 */
		atCmd("AT+FAR=2", AT_OK);
		fullDuplex = true;
		checkReadOnWrite = false;
	    }
	    if (gotEOT) goto done;
	    if (frame.isOK()) {
		switch (frame.getFCF()) {
		case FCF_CFR:		// training confirmed
		    if (!isSSLFax && !useV34) protoTrace("TRAINING succeeded");
		    setDataTimeout(60, params.br);
		    if (fullDuplex) {
			// Turn full-duplex off now since we are done.
			fullDuplex = false;
			checkReadOnWrite = true;
			atCmd("AT+FAR=1", AT_OK);
		    }
		    return (true);
		case FCF_RTN:		// confusing signal to receive in Phase B, but this is how to handle it
		case FCF_FTT:		// failure to train, retry
		    t--;		// immediately drop bitrate rather than trying the same bitrate again
		case FCF_CRP:		// command repeat
		    break;
		case FCF_DIS:		// new capabilities, maybe
		    {
			FaxParams newDIS = frame.getDIS();
			if (newDIS != dis_caps) {
			    /*
			       dis_caps = newDIS;
			       params.setFromDIS(dis_caps);
			     *
			     * The above code was commented because to
			     * use the newDIS we need to do more work like
			     *     sendClientCapabilitiesOK()
			     *     sendSetupParams()
			     * So we ignore newDIS.
			     * It will work if old dis 'less' then newDIS.
			     */
			    checkReceiverDIS(params);
			    curcap = NULL;
			}
			if (!fullDuplex && conf.class1FullDuplexTrainingSync) {
			    /*
			     * The receiver isn't syncing with us easily.  Enable full-duplex
			     * mode to detect V.21 HDLC signals while transmitting to assist us.
			     * We turn checkReadOnWrite off because we expect there to be times
			     * when the +FRH:3 response comes before we're done writing data
			     * to the modem, and we need to pick up the response afterwards.
			     */
			    atCmd("AT+FAR=2", AT_OK);
			    fullDuplex = true;
			    checkReadOnWrite = false;
			}
			// If it's our last attempt, disable SSL Fax in case that's being blocked.
			if (tries == 2) suppressSSLFax = true;
			bool tr = sendTraining(params, --tries, emsg);
			if (tr && fullDuplex) {
			    // Turn full-duplex off now since we are done.
			    fullDuplex = false;
			    checkReadOnWrite = true;
			    atCmd("AT+FAR=1", AT_OK);
			}
			return (tr);
		    }
		default:
		    if (frame.getFCF() == FCF_DCN) {
			/*
			 * The receiver is disconnecting.  This can happen
			 * if the receiver is exhaused from retraining, but
			 * it also happens in situations where, for example,
			 * the receiver determines that there is a modulator
			 * incompatibility (i.e. with V.17).  In that case,
			 * we must prevent ourselves from redialing and
			 * reattempting V.17 training, or we'll never get
			 * through.
			 */
			if (!isSSLFax && !useV34 && curcap->mod == V17 && attempt == 1 && tries == 3) hadV17Trouble = true;
			emsg = "RSPREC error/got DCN (receiver abort) {E103}";
		    } else if (wasTimeout() && useSSLFax) {
			/*
			 * The receiver sent us CSA but not CFR.
			 * Probably the modem had buffered the CSA while earlier
			 * waiting for an SSL Fax connection from the receiver,
			 * and the CFR got lost. The receiver may now be waiting
			 * for us to connect to them, instead.  Since CSA should
			 * only ever preced CFR, let's assume we got CFR.
			 */
			if (fullDuplex) {
			    // Turn full-duplex off now since we are done.
			    fullDuplex = false;
			    checkReadOnWrite = true;
			    atCmd("AT+FAR=1", AT_OK);
			}
			return (true);
		    } else
			emsg = "RSPREC invalid response received {E104}";
		    goto done;
		}
	    } else {
		/*
		 * Historically we waited "Class1TrainingRecovery" (1500 ms)
		 * at this point to try to try to avoid retransmitting while
		 * the receiver is also transmitting.  Sometimes it proved to be
		 * a faulty approach.  Really what we're trying to do is to
		 * not be transmitting at the same time as the other end is.
		 * The best way to do that is to make sure that there is
		 * silence on the line, and  we do that with Class1SwitchingCmd.
		 */
		if (isSSLFax || useV34 || !switchingPause(emsg)) {
		    emsg = "RSPREC error/got DCN (receiver abort) {E103}";
		    return (false);
		}
	    }
	} while (--t > 0);
	/*
	 * (t) attempts at the current speed failed, drop
	 * the signalling rate to the next lower rate supported
	 * by the local & remote sides and try again.
	 */
	if (!isSSLFax && !useV34) {
	    do {
		/*
		 * We don't fallback to V.17 9600 or V.17 7200 because
		 * after V.17 14400 and V.17 12000 fail they're not likely
		 * to succeed, and some receivers will give up after three
		 * failed TCFs.
		 */
		again = dropToNextBR(params);
	    } while (again && (params.br == BR_9600 || params.br == BR_7200) && curcap->mod != V29);
	}
    } while (!isSSLFax && !useV34 && (again || attempt < 3));
failed:
    emsg = "Failure to train remote modem at 2400 bps or minimum speed {E137}";
done:
    if (!emsg.length()) emsg = "Unspecified failure to train with receiver {E156}";
    if (!isSSLFax && !useV34) protoTrace("TRAINING failed");
    return (false);
}

/*
 * Select the next lower signalling rate that's
 * acceptable to both local and remote machines.
 */
bool
Class1Modem::dropToNextBR(Class2Params& params)
{
    if (curcap->br == BR_2400)
	return (false);
    const Class1Cap* oldcap = curcap;
    curcap--;
    for (;;) {
	if (curcap) {
	    /*
	     * Hunt for compatibility with remote at this baud rate.
	     * We don't drop from V.29 to V.17 because if the 
	     * receiver supports V.17 then we probably tried
	     * it already without success.
	     */
	    while (curcap->br == params.br) {
		if (isCapable(curcap->sr, dis_caps) && !(oldcap->mod == V29 && curcap->mod == V17))
		    return (true);
		curcap--;
	    }
	}
	if (params.br <= minsp)
	    return (false);
	params.br--;
	// get ``best capability'' of modem at this baud rate
	curcap = findBRCapability(params.br, xmitCaps);
    }
    /*NOTREACHED*/
}

/*
 * Select the next higher signalling rate that's
 * acceptable to both local and remote machines.
 */
bool
Class1Modem::raiseToNextBR(Class2Params& params)
{
    for (;;) {
	if (params.br == BR_14400)	// highest speed
	    return (false);
	// get ``best capability'' of modem at this baud rate
	curcap = findBRCapability(++params.br, xmitCaps);
	if (curcap) {
	    // hunt for compatibility with remote at this baud rate
	    do {
		if (isCapable(curcap->sr, dis_caps))
		    return (true);
		curcap--;
	    } while (curcap->br == params.br);
	}
    }
    /*NOTREACHED*/
}

void
Class1Modem::blockData(u_int byte, bool flag)
{
    if (!isSSLFax && useV34) {
	// With V.34-fax the DTE makes the stuffing
	byte =  (((byte>>0)&1)<<7)|(((byte>>1)&1)<<6)|
		(((byte>>2)&1)<<5)|(((byte>>3)&1)<<4)|
		(((byte>>4)&1)<<3)|(((byte>>5)&1)<<2)|
		(((byte>>6)&1)<<1)|(((byte>>7)&1)<<0);
	ecmStuffedBlock[ecmStuffedBlockPos++] = byte;
	return;
    }
    for (u_int j = 8; j > 0; j--) {
	u_short bit = (byte & (1 << (j - 1))) != 0 ? 1 : 0;
	ecmByte |= (bit << ecmBitPos);
	ecmBitPos++;
	if (ecmBitPos == 8) {
	    ecmStuffedBlock[ecmStuffedBlockPos++] = ecmByte;
	    ecmBitPos = 0;
	    ecmByte = 0;
	}
	// add transparent zero bits if needed
	if (bit == 1 && !flag) ecmOnes++;
	else ecmOnes = 0;
	if (ecmOnes == 5) {
	    ecmBitPos++;
	    if (ecmBitPos == 8) {
		ecmStuffedBlock[ecmStuffedBlockPos++] = ecmByte;
		ecmBitPos = 0;
		ecmByte = 0;
	    }
	    ecmOnes = 0;
	}
    }
}

/*
 * Buffer ECM HDLC image frames until a full block is received.
 * Perform T.30-A image block transmission protocol.
 */
bool
Class1Modem::blockFrame(const u_char* bitrev, bool lastframe, u_int ppmcmd, fxStr& emsg)
{
    // we have a full image frame

    for (u_int i = 0; i < ecmFramePos; i++)
	ecmBlock[ecmBlockPos++] = ecmFrame[i];
    ecmFramePos = 0;
    if (frameNumber == 256 || lastframe) {
	fxAssert(frameNumber <= 256, "Invalid frameNumber value.");
	ecmBlockPos = 0;
	bool lastblock = lastframe;

	// We now have a full image block without stuffed zero bits.
	// As the remote can request any frame of the block until
	// MCF we must keep the full image block available.

	bool blockgood = false, renegotiate = false, constrain = false, duplicate = false;
	u_short pprcnt = 0;
	char ppr[32];				// 256 bits
	for (u_int i = 0; i < 32; i++) ppr[i] = 0xff;
	u_short badframes = frameNumber, badframesbefore = frameNumber, retrained = 0;
	bool dolongtrain = false;

	do {
	    u_short fcount = 0;
	    u_short fcountuniq = 0;
	    ecmStuffedBlockPos = 0;

	    if (isSSLFax || !useV34) {
		if (!isSSLFax && !useV34) {
		    // If, perhaps, our modem does not consistently transmit only 1s after raising the transmit carrier,
		    // and if the receiver unnecessarily requires the expected ...1111110111111001111110... pattern where
		    // flags start with a single zero bit, then we may need to deliberately start our data transmission
		    // with a series of ones to get the expectation met.
		    blockData(0xff, true);
		}
		// synchronize with 200 ms of 0x7e flags
		for (u_int i = 0; i < params.transferSize(200); i++)
		    blockData(0x7e, true);
	    }

	    u_char* firstframe = (u_char*) malloc(frameSize + 6);
	    fxAssert(firstframe != NULL, "ECM procedure error (frame duplication).");
	    firstframe[0] = 0x1;			// marked as unused
	    for (u_short fnum = 0; fnum < frameNumber; fnum++) {
		u_int pprpos, pprval;
        	for (pprpos = 0, pprval = fnum; pprval >= 8; pprval -= 8) pprpos++;
        	if (ppr[pprpos] & frameRev[1 << pprval]) {
		    HDLCFrame ecmframe(5);
		    // frame bit marked for transmission
		    fcount++;
		    for (u_int i = fnum * (frameSize + 4);
			i < (fnum + 1) * (frameSize + 4); i++) {
			ecmframe.put(ecmBlock[i]);
			blockData(ecmBlock[i], false);
		    }
		    int fcs1 = (ecmframe.getCRC() >> 8) & 0xff;	// 1st byte FCS
		    int fcs2 = ecmframe.getCRC() & 0xff;	// 2nd byte FCS
		    ecmframe.put(fcs1); ecmframe.put(fcs2);
		    blockData(fcs1, false);
		    blockData(fcs2, false);
		    traceHDLCFrame("<--", ecmframe, true);
		    protoTrace("SEND send frame number %u", fnum);

		    if (isSSLFax || !useV34) {
			// separate frames with a 0x7e flag
			blockData(0x7e, true);
		    }

		    if (firstframe[0] == 0x1) {
			for (u_int i = 0; i < (frameSize + 6); i++) {
			    firstframe[i] = ecmframe[i];
			}
		    }
		}
	    }
	    fcountuniq = fcount;
	    if (fcount == 1 && pprcnt > 0) {
		/*
		 * Some receivers may have a hard time hearing the first frame,
		 * However, some receivers do not like frame duplication within a
		 * block.  So the first time we get here we merely send a single
		 * frame, but we flag duplicate so that if we come back through
		 * here we'll repeat it and hopefully catch those that have a
		 * hard time hearing the first frame.
		 */
		if (duplicate) {
		    HDLCFrame ecmframe(5);
		    fcount++;
		    for (u_int i = 0; i < (frameSize + 6); i++) {
			blockData(firstframe[i], false);
		    }
		    ecmframe.put(firstframe, (frameSize + 6));
		    traceHDLCFrame("<--", ecmframe, true);
		    protoTrace("SEND send frame number %u", frameRev[firstframe[3]]);
		    if (isSSLFax || !useV34) blockData(0x7e, true);
		} else {
		    duplicate = true;
		}
	    }
	    free(firstframe);
	    HDLCFrame rcpframe(5);
	    rcpframe.put(0xff); rcpframe.put(0xc0); rcpframe.put(FCF_RCP); rcpframe.put(0x96); rcpframe.put(0xd3);
	    for (u_short k = 0; k < 3; k++) {		// three RCP frames
		for (u_short j = 0; j < 5; j++)
		    blockData(rcpframe[j], false);
		traceHDLCFrame("<--", rcpframe, true);

		// separate frames with a 0x7e flag
		if (isSSLFax || !useV34) blockData(0x7e, true);
	    }
	    // add one more flag to ensure one full flag gets transmitted before DLE+ETX
	    if (isSSLFax || !useV34) blockData(0x7e, true);

	    // start up the high-speed carrier...
	    if (flowControl == FLOW_XONXOFF)   
		setXONXOFF(FLOW_XONXOFF, FLOW_NONE, ACT_FLUSH);
	    if (!isSSLFax && !useV34) {
		// T.30 5.3.2.4 (03/93) gives this to be a 75ms minimum
		if (!switchingPause(emsg)) {
		    return (false);
		}
		/*
		 * T.30 Section 5, Note 5 states that we must use long training
		 * on the first high-speed data message following CTC.
		 */
		fxStr tmCmd;
		if (dolongtrain) tmCmd = fxStr(curcap->value, tmCmdFmt);
		else tmCmd = fxStr(curcap[HasShortTraining(curcap)].value, tmCmdFmt);
		if (!atCmd(tmCmd, AT_CONNECT))
		    return (false);
		pause(conf.class1TMConnectDelay);
	    }
	    dolongtrain = false;

	    // The block is assembled.  Transmit it, adding transparent DLEs.  End with DLE+ETX.
	    u_char buf[2];
	    if (!isSSLFax && useV34) {
		// switch to primary channel
		buf[0] = DLE; buf[1] = 0x6B;	// <DLE><pri>
		HDLCFrame rtncframe(conf.class1FrameOverhead);
		u_short limit = 5;
		bool gotprate, gotrtncframe = false;
		// wait for the ready indicator, <DLE><pri><DLE><prate>
		do {
		    if (!putModemData(buf, 2)) return (false);
		    gotprate = waitForDCEChannel(false);
		    if (!gotprate && ctrlFrameRcvd != fxStr::null) {
			/*
			 * Maybe we took so long to assemble the block that 
			 * the receiver has retransmitted CFR or MCF.
			 */
			for (u_int i = 0; i < ctrlFrameRcvd.length(); i++)
			    rtncframe.put(frameRev[ctrlFrameRcvd[i] & 0xFF]);
			traceHDLCFrame("-->", rtncframe);
			gotrtncframe = true;
		    }
		} while (!gotprate && gotrtncframe && --limit);
		if (renegotiate) {
		    // Although spec says this can be done any time,
		    // in practice it must be done at this moment.
		    renegotiatePrimary(constrain);
		    renegotiate = false;
		    constrain = false;
		}
	    }
	    if (!isSSLFax && useV34) {
		// we intentionally do not send the FCS bytes as the DCE regenerates them
		// send fcount frames separated by <DLE><ETX>
		u_short v34frame;
		for (v34frame = 0; v34frame < fcount; v34frame++) {
		    if (!sendClass1Data(ecmStuffedBlock, frameSize + 4, bitrev, true, getDataTimeout()))
			return (false);
		    ecmStuffedBlock += (frameSize + 6);
		}
		// send 3 RCP frames separated by <DLE><ETX>
		for (v34frame = 0; v34frame < 3; v34frame++) {
		    if (!sendClass1Data(ecmStuffedBlock, 3, bitrev, true, getDataTimeout()))
			return (false);
		    ecmStuffedBlock += 5;
		}
		ecmStuffedBlock -= ecmStuffedBlockPos;
	    } else {
		if (!sendClass1Data(ecmStuffedBlock, ecmStuffedBlockPos, bitrev, true, getDataTimeout()) && !wasSSLFax)
		    return (false);
	    }
	    if (!isSSLFax && !wasSSLFax && useV34) {
		// switch to control channel
		buf[0] = DLE; buf[1] = 0x6D;	// <DLE><ctrl>
		if (!putModemData(buf, 2)) return (false);
		// wait for the ready indicator, <DLE><ctrl><DLE><crate>
		if (!waitForDCEChannel(true)) {
		    emsg = "Failed to properly open control V.34 channel. {E116}";
		    protoTrace(emsg);
		    return (false);
		}
	    } else if (!wasSSLFax && !isSSLFax) {
		// Wait for transmit buffer to empty.
		ATResponse r;
		while ((r = atResponse(rbuf, getDataTimeout())) == AT_OTHER);
        	if (!(r == AT_OK)) {
		    if (r == AT_NOCARRIER) {
			/*
			 * The NO CARRIER result here is not per-spec.  However,
			 * some modems capable of detecting hangup conditions will
			 * use this to indicate a disconnection.  Because we did
			 * not check for modem responses during the entire data
			 * transfer we flush the modem input so as to avoid reading
			 * any modem responses related to misinterpreted Phase C
			 * data that occurred after the hangup.
			 */
			flushModemInput();
		    }
		    return (false);
		}
	    }
	    if (wasSSLFax) wasSSLFax = false;

	    if (flowControl == FLOW_XONXOFF)
		setXONXOFF(FLOW_NONE, FLOW_NONE, ACT_DRAIN);

	    if (!isSSLFax && !useV34 && !atCmd(ppmcmd == FCF_MPS ? conf.class1PPMWaitCmd : conf.class1EOPWaitCmd, AT_OK)) {
		emsg = "Stop and wait failure (modem on hook) {E127}";
		protoTrace(emsg);
		if (lastResponse == AT_ERROR) gotEOT = true;
		return (false);
	    }
	    /*
	     * Build PPS frame and send it.  We don't use I3 = 0xFF when sending
	     * zero-count frames because some receivers will read it as 256.
	     * Instead we send I3 = 0x00, which technically indicates one frame,
	     * but it should be harmless since any interpretation of I3 will not
	     * exceed previous indications, and the receiver has already acknowledged
	     * all frames properly received.  I3 indicates the *unique* frame count.
	     */
	    char pps[4];
	    if (!lastblock)
		pps[0] = 0x00;
	    else
		pps[0] = ppmcmd | FCF_SNDR;
	    pps[1] = frameRev[(FaxModem::getPageNumberOfCall() - 1) & 0xFF];	// rolls-over after 256
	    pps[2] = frameRev[blockNumber];
	    pps[3] = frameRev[(fcountuniq == 0 ? 0 : (fcountuniq - 1))];
	    dataSent += fcountuniq;
	    u_short ppscnt = 0, crpcnt = 0;
	    bool gotppr = false;
	    /* get MCF/PPR/RNR */
	    HDLCFrame pprframe(conf.class1FrameOverhead);
	    do {
		if (!isSSLFax && !useV34 && !atCmd(thCmd, AT_CONNECT))
		    break;
		startTimeout(3000);
		sendFrame(FCF_PPS|FCF_SNDR, fxStr(pps, 4));
		stopTimeout("sending PPS frame");
		traceFCF("SEND send", FCF_PPS);
		traceFCF("SEND send", pps[0]);

		// Some receivers will almost always miss our first PPS message, and
		// in those cases waiting T2 for a response will cause the remote to
		// hang up.  So, using T4 here is imperative so that our second PPS
		// message happens before the remote decides to hang up. As we're in
		// a "RESPONSE REC" operation, anyway, this is correct behavior.
		//
		// We don't use CRP here, because it isn't well-received.
		gotppr = recvFrame(pprframe, FCF_SNDR, conf.t4Timer, false, false, true, FCF_PPS);
		if (gotppr) {
		    traceFCF("SEND recv", pprframe.getFCF());
		    if (pprframe.getFCF() == FCF_CRP) {
			gotppr = false;
			crpcnt++;
			ppscnt = 0;
		    }
		}
	    } while (!gotppr && (++ppscnt < 3) && (crpcnt < 3) && !gotEOT && switchingPause(emsg));
	    if (gotppr) {
		if (pprframe.getFCF() == FCF_NSF || pprframe.getFCF() == FCF_CSI || pprframe.getFCF() == FCF_DIS  || pprframe.getFCF() == FCF_FTT || pprframe.getFCF() == FCF_RTN) {
		    // Takes us back to Phase B.  Break out of the loop, and handle it upstream.
		    blockgood = true;
		    signalRcvd = pprframe.getFCF();
		    break;
		}
		if (!switchingPause(emsg)) {
		    return (false);
		}
		if (pprframe.getFCF() == FCF_RNR) {
		    u_int t1 = howmany(conf.t1Timer, 1000);
		    time_t start = Sys::now();
		    gotppr = false;
		    do {
			if ((unsigned) Sys::now()-start >= t1) {
			    // we use T1 rather than T5 to "minimize transmission inefficiency" (T.30 A.5.4.1)
			    emsg = "Receiver flow control exceeded timer. {E138}";
			    if (ppmcmd == FCF_EOM) batchingError = true;
			    protoTrace(emsg);
			    return (false);
			}
			u_short rrcnt = 0, crpcnt = 0;
			bool gotmsg = false;
			do {
			    if (!isSSLFax && !useV34 && !atCmd(thCmd, AT_CONNECT))
				break;
			    startTimeout(3000);
			    sendFrame(FCF_RR|FCF_SNDR);
			    stopTimeout("sending RR frame");
			    traceFCF("SEND send", FCF_RR);
			    // T.30 states that we must wait no more than T4 between unanswered RR signals.
			    gotmsg = recvFrame(pprframe, FCF_SNDR, conf.t4Timer, false, false, true, FCF_RR);
			    if (gotmsg) {	// no CRP, stick to RR only
				traceFCF("SEND recv", pprframe.getFCF());
				if (pprframe.getFCF() == FCF_CRP) {
				    gotmsg = false;
				    crpcnt++;
				    rrcnt = 0;
				    if (!switchingPause(emsg)) {
					return (false);
				    }
				}
			    }
			} while (!gotmsg && (++rrcnt < 3) && (crpcnt < 3) && !gotEOT && switchingPause(emsg));
			if (!gotmsg) {
			    emsg = "No response to RR repeated 3 times. {E139}";
			    protoTrace(emsg);
			    return (false);
			}
			switch (pprframe.getFCF()) {
			    case FCF_PPR:
			    case FCF_MCF:
			    case FCF_NSF:
			    case FCF_CSI:
			    case FCF_DIS:
			    case FCF_RTN:
				gotppr = true;
				break;
			    case FCF_RNR:
				if (!switchingPause(emsg)) {
				    return (false);
				}
				break;
			    default:
				if (pprframe.getFCF() == FCF_DCN) {
				    emsg = "COMREC error/got DCN (receiver abort) {E103}";
				} else {
				    emsg = "COMREC invalid response received to RR. {E140}";
				}
				protoTrace(emsg);
				return (false);
			}
		    } while (!gotppr);		
		    if (pprframe.getFCF() == FCF_NSF || pprframe.getFCF() == FCF_CSI || pprframe.getFCF() == FCF_DIS || pprframe.getFCF() == FCF_FTT || pprframe.getFCF() == FCF_RTN) {
			// Takes us back to Phase B.  Break out of the loop, and handle it upstream.
			blockgood = true;
			signalRcvd = pprframe.getFCF();
			break;
		    }
		}
		bool doctceor;
		switch (pprframe.getFCF()) {
		    case FCF_MCF:
			hadV34Trouble = false;
			blockgood = true;
			signalRcvd = pprframe.getFCF();
			break;
		    case FCF_CFR:
			/*
			 * The most likely situation where this could arise would be if the receiver
			 * didn't catch our Phase C signal on the first iteration of the first block
			 * on the first page.  So, the receiver oddly chose to respond to PPS with a
			 * repeated CFR instead of a PPR signal.  So, we treat it as a PPR signal, but
			 * we do need to reset pprcnt, as we do with an out-of-sync CTR response.
			 * However, in the event that we're not following the first iteration of the
			 * first block on the first page we're just going to presume that the receiver
			 * wants us to reset our status back to the first iteration of this block. So,
			 * we need to reset the ppr map.
			 */
			for (u_int i = 0; i < 32; i++) ppr[i] = 0xff;
		    case FCF_CTR:
			/*
			 * We didn't send CTC yet received CTR.  We may be out-of-sync with the receiver
			 * on the PPR count, and the receiver has wrongly assumed a PPS signal was
			 * actually CTC signal (presumably for the same modulation and bitrate). To try
			 * to get back in sync with the receiver we need to reset pprcnt and operate
			 * on the saved PPR.
			 */
			pprcnt = -1;
		    case FCF_PPR:
			{
			    pprcnt++;
			    // update ppr
			    for (u_int i = 3; i < (pprframe.getLength() - (conf.class1FrameOverhead - 2)) && i < 36; i++) {
				ppr[i-3] = pprframe[i];
			    }
			    badframesbefore = badframes;
			    badframes = 0;
			    for (u_int j = 0; j < frameNumber; j++) {
				u_int pprpos, pprval;
				for (pprpos = 0, pprval = j; pprval >= 8; pprval -= 8) pprpos++;
				if (ppr[pprpos] & frameRev[1 << pprval]) {
				    badframes++;
				}
			    }
			    dataMissed += badframes;
			}
			doctceor = (pprcnt == 4);
			/*
			 * T.30 Annex F prohibits CTC in V.34-Fax.  Per the spec our options
			 * are to do EOR or DCN.  However, many receivers will allow us to
			 * simply continue sending image blocks followed by PPS.  Coupled with
			 * primary rate negotiations, this becomes a better-than-CTC solution.
			 * Do this up to 12 attempts and only when something has gotten through.
			 */
			if (isSSLFax || useV34) {
			    if (!isSSLFax && useV34 && pprcnt >= 4 && badframes) hadV34Trouble = true;	// problematic V.34?
			    if (pprcnt == 8) doctceor = true;
			    if (conf.class1PersistentECM && badframes && badframes != frameNumber) doctceor = false;
			    if (pprcnt == 12) doctceor = true;
			    if (!doctceor && badframes && badframes >= badframesbefore) {
				/*
				 * Request to renegotiate the primary rate.  (T.31-A1 B.8.5)
				 * In practice, if we do not constrain the rate then
				 * we may likely speed up the rate; so we constrain it.
				 */
				renegotiate = true;
				constrain = true;
			    }
			}
			if (doctceor) {
			    pprcnt = 0;
			    // Some receivers will ignorantly transmit PPR showing all frames good,
			    // so if that's the case then do EOR instead of CTC.
			    if (badframes == 0) {
				hadV34Trouble = false;
				blockgood = true;
				signalRcvd = FCF_MCF;
			    }
			    if (!isSSLFax && !useV34 && curcap->mod == V17 && badframes == frameNumber && retrained > 0 && FaxModem::getPageNumberOfCall() == 1) {
				// looks like a V.17 modulator incompatibility that managed to pass TCF
				// we set hasV17Trouble to help future calls to this destination
				protoTrace("The destination appears to have trouble with V.17.");
				hadV17Trouble = true;
			    }
			    protoTrace("Bad frame count: %d, Previously: %d, Retrained: %d", badframes, badframesbefore, retrained);
			    // There is no CTC with V.34-fax (T.30 Annex F.3.4.5 Note 1).
			    if (conf.class1PersistentECM && !isSSLFax && !useV34 && (blockgood == false) && 
				!((curcap->br == 0) && (badframes >= badframesbefore) && retrained > 0)) {
				/*
				 * We send ctc even at 2400 baud if we're getting somewhere, and
				 * often training down to a slower speed only makes matters worse.
				 * So, if we seem to be making adequate progress we don't train down.
				 */
				if (retrained++ > 0 && curcap->br != 0 && (badframes >= badframesbefore)) {
				    retrained = 0;
				    do {
					if (!dropToNextBR(params)) {
					    // We have a minimum speed that's not BR_2400,
					    // and we're there now.  Undo curcap change...
					    curcap++;
					    break;
					}
				    } while ((params.br == BR_9600 || params.br == BR_7200) && curcap->mod != V29);
				}
				char ctc[2];
				ctc[0] = 0;
				ctc[1] = curcap->sr >> 8;
				bool gotctr = false;
				u_short ctccnt = 0, crpcnt = 0;
				HDLCFrame ctrframe(conf.class1FrameOverhead);
				do {
				    if (!atCmd(thCmd, AT_CONNECT))
					break;
				    startTimeout(3000);
				    sendFrame(FCF_CTC|FCF_SNDR, fxStr(ctc, 2));
				    stopTimeout("sending CTC frame");
				    traceFCF("SEND send", FCF_CTC);
				    gotctr = recvFrame(ctrframe, FCF_SNDR, conf.t4Timer, false, false, true, FCF_CTC);
				    if (gotctr) {
					traceFCF("SEND recv", ctrframe.getFCF());
					if (ctrframe.getFCF() == FCF_PPR) {
					    gotctr = false;
					    if (!switchingPause(emsg)) {
						return (false);
					    }
					}
					if (ctrframe.getFCF() == FCF_CRP) {
					    gotctr = false;
					    crpcnt++;
					    ctccnt = 0;
					    if (!switchingPause(emsg)) {
						return (false);
					    }
					}
				    }
				} while (!gotctr && (++ctccnt < 3) && (crpcnt < 3) && !gotEOT);
				if (!gotctr) {
				    emsg = "No response to CTC repeated 3 times. {E141}";
				    protoTrace(emsg);
				    return (false);
				}
				if (ctrframe.getFCF() == FCF_NSF || ctrframe.getFCF() == FCF_CSI || ctrframe.getFCF() == FCF_DIS || ctrframe.getFCF() == FCF_FTT || ctrframe.getFCF() == FCF_RTN) {
				    // Takes us back to Phase B.  Break out of the loop, and handle it upstream.
				    blockgood = true;
				    signalRcvd = ctrframe.getFCF();
				    break;
				}
				if (!(ctrframe.getFCF() == FCF_CTR)) {
				    if (ctrframe.getFCF() == FCF_DCN) {
					emsg = "COMREC error/got DCN (receiver abort) {E103}";
				    } else {
					emsg = "COMREC invalid response received to CTC. {E142}";
				    }
				    protoTrace(emsg);
				    return (false);
				}
				setDataTimeout(60, params.br);	// adjust dataTimeout to new bitrate
				dolongtrain = true;	// T.30 states that we must use long-training next
			    } else {
				/*
				 * At this point data corruption is inevitable if all data
				 * frames have not been received correctly.  With MH and MR 
				 * data formats this may be tolerable if the bad frames are 
				 * few and not in an unfortunate sequence.  With MMR data the
				 * receiving decoder should truncate the image at the point
				 * of the corruption.  The effect of corruption in JBIG or JPEG
				 * data is quite unpredictable.  So if all frames have not been
				 * received correctly and we're looking at an unacceptable
				 * imaging situation on the receiver's end, then we disconnect,
				 * and hopefully we try again successfully.
				 */
				if (blockgood == false && (params.df >= DF_2DMMR ||
				    (params.df <= DF_2DMR && badframes > frameNumber/2))) {
				    emsg = "Failure to transmit clean ECM image data. {E143}";
				    protoTrace(emsg);
				    return (false);
				}
				bool goterr = false;
				u_short eorcnt = 0, crpcnt = 0;
				HDLCFrame errframe(conf.class1FrameOverhead);
				do {
				    if (!isSSLFax && !useV34 && !atCmd(thCmd, AT_CONNECT))
					break;
				    startTimeout(3000);
				    sendFrame(FCF_EOR|FCF_SNDR, fxStr(pps, 1));
				    stopTimeout("sending EOR frame");
				    traceFCF("SEND send", FCF_EOR);
				    traceFCF("SEND send", pps[0]);
				    goterr = recvFrame(errframe, FCF_SNDR, conf.t4Timer, false, false, true, FCF_EOR);
				    if (goterr) {
					traceFCF("SEND recv", errframe.getFCF());
					if (errframe.getFCF() == FCF_CRP) {
					    goterr = false;
					    crpcnt++;
					    eorcnt = 0;
					    if (!switchingPause(emsg)) {
						return (false);
					    }
					}
				    }
				} while (!goterr && (++eorcnt < 3) && (crpcnt < 3) && !gotEOT);
				if (!goterr) {
				    emsg = "No response to EOR repeated 3 times. {E144}";
				    if (ppmcmd == FCF_EOM) batchingError = true;
				    protoTrace(emsg);
				    return (false);
				}
				if (errframe.getFCF() == FCF_RNR) {
				    u_int t1 = howmany(conf.t1Timer, 1000);
				    time_t start = Sys::now();
				    goterr = false;
				    do {
					if ((unsigned) Sys::now()-start >= t1) {
					    // we use T1 rather than T5 to "minimize transmission inefficiency" (T.30 A.5.4.1)
					    emsg = "Receiver flow control exceeded timer. {E138}";
					    if (ppmcmd == FCF_EOM) batchingError = true;
					    protoTrace(emsg);
					    return (false);
					}
					u_short rrcnt = 0, crpcnt = 0;
					bool gotmsg = false;
					do {
					    if (!isSSLFax && !useV34 && !atCmd(thCmd, AT_CONNECT))
						break;
					    startTimeout(3000);
					    sendFrame(FCF_RR|FCF_SNDR);
					    stopTimeout("sending RR frame");
					    traceFCF("SEND send", FCF_RR);
					    // T.30 states that we must wait no more than T4 between unanswered RR signals.
					    gotmsg = recvFrame(errframe, FCF_SNDR, conf.t4Timer, false, false, true, FCF_RR);
					    if (gotmsg) {	// no CRP, stick to RR only
						traceFCF("SEND recv", errframe.getFCF());
						if (errframe.getFCF() == FCF_CRP) {
						    gotmsg = false;
						    crpcnt++;
						    rrcnt = 0;
						    if (!switchingPause(emsg)) {
							return (false);
						    }
						}
					    }
					} while (!gotmsg && (++rrcnt < 3) && (crpcnt < 3) && !gotEOT && switchingPause(emsg));
					if (!gotmsg) {
					    emsg = "No response to RR repeated 3 times. {E139}";
					    protoTrace(emsg);
					    return (false);
					}
					switch (errframe.getFCF()) {
					    case FCF_ERR:
						goterr = true;
						break;
					    case FCF_RNR:
						if (!switchingPause(emsg)) {
						    return (false);
						}
						break;
					    default:
						if (errframe.getFCF() == FCF_DCN) {
						    emsg = "COMREC error/got DCN (receiver abort) {E103}";
						} else {
						    emsg = "COMREC invalid response received to RR. {E140}";
						}
						protoTrace(emsg);
						return (false);
					}
				    } while (!goterr);		
				}
				if (errframe.getFCF() == FCF_NSF || errframe.getFCF() == FCF_CSI || errframe.getFCF() == FCF_DIS || errframe.getFCF() == FCF_FTT || errframe.getFCF() == FCF_RTN) {
				    // Takes us back to Phase B.  Break out of the loop, and handle it upstream.
				    blockgood = true;
				    signalRcvd = errframe.getFCF();
				    break;
				}
				if (!(errframe.getFCF() == FCF_ERR)) {
				    if (errframe.getFCF() == FCF_DCN) {
					emsg = "COMREC error/got DCN (receiver abort) {E103}";
				    } else {
					emsg = "COMREC invalid response received to EOR. {E145}";
				    }
				    protoTrace(emsg);
				    return (false);
				}
				blockgood = true;
				signalRcvd = FCF_MCF;
			    }
			}
			break;
		    case FCF_PIP:
		    case FCF_PIN:
			if (!isSSLFax && !useV34 && !atCmd(thCmd, AT_CONNECT))
			    break;
			pps[0] |= FCF_PRIQ;
			startTimeout(3000);
			sendFrame(FCF_PPS|FCF_SNDR, fxStr(pps, 4));
			stopTimeout("sending PPS frame");
			traceFCF("SEND send", FCF_PPS);
			traceFCF("SEND send", pps[0]);
			blockgood = true;	// we'll analyze PIN vs PIP later
			signalRcvd = pprframe.getFCF();
			break;
		    case FCF_DCN:
			emsg = "RSPREC error/got DCN (receiver abort) {E103}";
			protoTrace(emsg);
			return(false);
		    default:
			emsg = "COMREC invalid response received to PPS. {E146}";
			protoTrace(emsg);
			return(false);
		}
	    } else {
		emsg = "No response to PPS repeated 3 times. {E147}";
		if (ppmcmd == FCF_EOM) batchingError = true;
		protoTrace(emsg);
		return (false);
	    }
	} while (!blockgood);
	frameNumber = 0;
	if (lastblock) blockNumber = 0;
	else blockNumber++;
    }
    return (true);
}

/*
 * Send T.30-A framed image data.
 */
bool
Class1Modem::sendClass1ECMData(const u_char* data, u_int cc, const u_char* bitrev, bool eod, u_int ppmcmd, fxStr& emsg)
{
    /*
     * Buffer data into the block.  We buffer the entire block
     * before sending it to prevent any modem buffer underruns.
     * Later we send it to sendClass1Data() which adds the 
     * transparent DLE characters and transmits it.
     */
    for (u_int i = 0; i < cc; i++) {
	if (ecmFramePos == 0) {
	    ecmFrame[ecmFramePos++] = 0xff; 	// address field
	    ecmFrame[ecmFramePos++] = 0xc0;	// control field
	    ecmFrame[ecmFramePos++] = FCF_FCD;	// FCD FCF
	    ecmFrame[ecmFramePos++] = frameRev[frameNumber++];	// block frame number
	}
	ecmFrame[ecmFramePos++] = frameRev[data[i]];
	if (ecmFramePos == (frameSize + 4)) {
	    bool lastframe = ((i == (cc - 1)) && eod);
	    if (!blockFrame(bitrev, lastframe, ppmcmd, emsg))
		return (false);
	    if (lastframe)
		return (true);
	    switch (signalRcvd) {
		case FCF_NSF:
		case FCF_CSI:
		case FCF_DIS:
		case FCF_PIP:
		case FCF_PIN:
		case FCF_FTT:
		    /*
		     * We got one of these signals in response to PPS-NULL.
		     * We need to go return upstream to start this page over again.
		     * We'll have to return to the first block even if a previous
		     * block was confirmed as received.
		     */
		    repeatPhaseB = true;
		    return (true);
		    break;
	    }
	}
    }
    if (eod) {
	if (ecmFramePos != 0)	{
	    // frame must be filled to end with zero-data
	    while (ecmFramePos < (frameSize + 4)) ecmFrame[ecmFramePos++] = 0x00;
	}
	if (!blockFrame(bitrev, true, ppmcmd, emsg))
	    return (false);
    }
    return (true);
}

/*
 * Send data for the current page.
 */
bool
Class1Modem::sendPageData(u_char* data, u_int cc, const u_char* bitrev, bool ecm, fxStr& emsg)
{
    if (imagefd > 0) Sys::write(imagefd, (const char*) data, cc);
    beginTimedTransfer();
    bool rc;
    if (ecm)
	rc = sendClass1ECMData(data, cc, bitrev, false, 0, emsg);
    else {
	rc = sendClass1Data(data, cc, bitrev, false, getDataTimeout());
	protoTrace("SENT %u bytes of data", cc);
    }
    endTimedTransfer();
    if (wasSSLFax) {
	/*
	 * A value of false for rc here means that there was an error in writing the data.
	 * If there was an error writing to the modem, then this would be fatal.  But, because
	 * it was an error writing SSL Fax data, then we need to fall back to the modem
	 * rather than turning the SSL Fax write error into a fatal condition.
	 */
	rc = true;
    }
    return rc;
}

/*
 * Send RTC to terminate a page.  Note that we pad the
 * RTC with zero fill to "push it out on the wire".  It
 * seems that some Class 1 modems do not immediately
 * send all the data they are presented.
 */
bool
Class1Modem::sendRTC(Class2Params params, u_int ppmcmd, uint32_t rows, fxStr& emsg)
{
    if (params.df > DF_2DMR) {
	/*
	 * If we ever needed to send NEWLEN or other JBIG-terminating
	 * markers this is where we would do it.
	 */
	//u_char newlen[8] = { 0xFF, 0x05, 
	//    (rows>>24)&0xFF, (rows>>16)&0xFF, (rows>>8)&0xFF, rows&0xFF,
	//    0xFF, 0x02 };	// SDNORM is added per spec
	//return sendClass1ECMData(newlen, 8, rtcRev, true, ppmcmd, emsg);
	return sendClass1ECMData(NULL, 0, rtcRev, true, ppmcmd, emsg);
    }
    /*
     * These are intentionally reverse-encoded in order to keep
     * rtcRev and bitrev in sendPage() in agreement.  They are also
     * end-padded with zeros to help "push" them out of the modem.
     * We explicitly set the zeros in the padding so as to prevent
     * the "unset" garbage values from confusing the receiver.
     * We don't send that padding with ECM, as it's unnecessary.
     */
    static const u_char RTC1D[9+20] =
	{ 0x00,0x08,0x80,0x00,0x08,0x80,0x00,0x08,0x80,
	  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 };
    static const u_char RTC2D[10+20] =
	{ 0x00,0x18,0x00,0x03,0x60,0x00,0x0C,0x80,0x01,0x30,
	  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 };
    if (params.is2D()) {
	protoTrace("SEND 2D RTC");
	if (params.ec != EC_DISABLE)
	    return sendClass1ECMData(RTC2D, 10, rtcRev, true, ppmcmd, emsg);
	else
	    return sendClass1Data(RTC2D, sizeof (RTC2D), rtcRev, true, getDataTimeout());
    } else {
	protoTrace("SEND 1D RTC");
	if (params.ec != EC_DISABLE)
	    return sendClass1ECMData(RTC1D, 9, rtcRev, true, ppmcmd, emsg);
	else
	    return sendClass1Data(RTC1D, sizeof (RTC1D), rtcRev, true, getDataTimeout());
    }
}

#define	EOLcheck(w,mask,code) \
    if ((w & mask) == code) { w |= mask; return (true); }

/*
 * Check the last 24 bits of received T.4-encoded
 * data (presumed to be in LSB2MSB bit order) for
 * an EOL code and, if one is found, foul the data
 * to insure future calls do not re-recognize an
 * EOL code.
 */
static bool
EOLcode(u_long& w)
{
    if ((w & 0x00f00f) == 0) {
	EOLcheck(w, 0x00f0ff, 0x000080);
	EOLcheck(w, 0x00f87f, 0x000040);
	EOLcheck(w, 0x00fc3f, 0x000020);
	EOLcheck(w, 0x00fe1f, 0x000010);
    }
    if ((w & 0x00ff00) == 0) {
	EOLcheck(w, 0x00ff0f, 0x000008);
	EOLcheck(w, 0x80ff07, 0x000004);
	EOLcheck(w, 0xc0ff03, 0x000002);
	EOLcheck(w, 0xe0ff01, 0x000001);
    }
    if ((w & 0xf00f00) == 0) {
	EOLcheck(w, 0xf0ff00, 0x008000);
	EOLcheck(w, 0xf87f00, 0x004000);
	EOLcheck(w, 0xfc3f00, 0x002000);
	EOLcheck(w, 0xfe1f00, 0x001000);
    }
    return (false);
}
#undef EOLcheck

/*
 * Send a page of data.
 */
bool
Class1Modem::sendPage(TIFF* tif, Class2Params& params, u_int pageChop, u_int ppmcmd, fxStr& emsg)
{
    if (!useSSLFax && params.ec == EC_DISABLE) {	// ECM does it later
	/*
	 * Set high speed carrier & start transfer.  If the
	 * negotiated modulation technique includes short
	 * training, then we use it here (it's used for all
	 * high speed carrier traffic other than the TCF).
	 */
	fxStr tmCmd(curcap[HasShortTraining(curcap)].value, tmCmdFmt);
	if (!atCmd(tmCmd, AT_CONNECT)) {
	    emsg = "Unable to establish message carrier {E148}";
	    protoTrace(emsg);
	    return (false);
	}
	// As with TCF, T.31 8.3.3 requires the DCE to report CONNECT at the beginning
	// of transmission of the training pattern rather than at the end.  We pause here
	// to allow the remote's +FRM to result in CONNECT.
	pause(conf.class1TMConnectDelay);
	if (flowControl == FLOW_XONXOFF)
	    setXONXOFF(FLOW_XONXOFF, FLOW_NONE, ACT_FLUSH);
    }

    bool rc = true;
    blockNumber = frameNumber = ecmBlockPos = ecmFramePos = ecmBitPos = ecmOnes = ecmByte = 0;
    protoTrace("SEND begin page");

    tstrip_t nstrips = TIFFNumberOfStrips(tif);
    uint32_t rowsperstrip = 0;
    if (nstrips > 0) {

	/*
	 * RTFCC may mislead us here, so we temporarily
	 * adjust params.
	 */
	Class2Params newparams = params;
	uint16_t compression;
	TIFFGetField(tif, TIFFTAG_COMPRESSION, &compression);
	if (!params.jp) {
	    if (compression != COMPRESSION_CCITTFAX4) {  
		uint32_t g3opts = 0;
		TIFFGetField(tif, TIFFTAG_GROUP3OPTIONS, &g3opts);
		if ((g3opts & GROUP3OPT_2DENCODING) == DF_2DMR)
		    params.df = DF_2DMR;
		else
		    params.df = DF_1DMH;
	    } else
		params.df = DF_2DMMR;
	}

	/*
	 * Correct bit order of data if not what modem expects.
	 */
	uint16_t fillorder;
	TIFFGetFieldDefaulted(tif, TIFFTAG_FILLORDER, &fillorder);
	const u_char* bitrev =
	    TIFFGetBitRevTable(sendFillOrder != FILLORDER_LSB2MSB);
	/*
	 * Setup tag line processing.
	 */
	bool doTagLine = setupTagLineSlop(params);
	u_int ts = getTagLineSlop();
	/*
	 * Calculate total amount of space needed to read
	 * the image into memory (in its encoded format).
	 *
	 * It is tempting to want to use TIFFStripSize() instead
	 * of summing stripbytecounts, but the two are not equal.
	 */
	TIFFSTRIPBYTECOUNTS* stripbytecount;
	(void) TIFFGetField(tif, TIFFTAG_STRIPBYTECOUNTS, &stripbytecount);
	tstrip_t strip;
	u_long totdata = 0;
	for (strip = 0; strip < nstrips; strip++)
	    totdata += stripbytecount[strip];
	/*
	 * Read the image into memory.
	 */
	u_char* data = new u_char[totdata+ts];
	u_int off = ts;			// skip tag line slop area
	for (strip = 0; strip < nstrips; strip++) {
	    uint32_t sbc = stripbytecount[strip];
	    if (sbc > 0 && TIFFReadRawStrip(tif, strip, data+off, sbc) >= 0)
		off += (u_int) sbc;
	}
	totdata -= pageChop;		// deduct trailing white space not sent
	TIFFGetFieldDefaulted(tif, TIFFTAG_ROWSPERSTRIP, &rowsperstrip);
	if (rowsperstrip == (uint32_t) -1)
	    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &rowsperstrip);

	/*
	 * Image the tag line, if intended.
	 */
	u_char* dp;
	if (doTagLine) {
	    u_long totbytes = totdata;
	    dp = imageTagLine(data+ts, fillorder, params, totbytes);
	    // Because the whole image is processed with MMR, 
	    // totdata is then determined during encoding.
	    totdata = (params.df == DF_2DMMR) ? totbytes : totdata+ts - (dp-data);
	} else
	    dp = data;

	/*
	 * After a page chop rowsperstrip is no longer valid, as the strip will
	 * be shorter.  Therefore, convertPhaseCData (for the benefit of supporting
	 * the sending of JBIG NEWLEN markers) and correctPhaseCData (only in the 
	 * case of MMR data) deliberately update rowsperstrip.  Page-chopped and
	 * un-converted MH and MR data will not have updated rowsperstrip.
	 * However, this only amounts to a allocating more memory than is needed,
	 * and this is not consequential.
	 */

	if (conf.softRTFCC && params.df != newparams.df) {
	    switch (params.df) {
		case DF_1DMH:
		    protoTrace("Reading MH-compressed image file");
		    break;
		case DF_2DMR:
		    protoTrace("Reading MR-compressed image file");
		    break;
		case DF_2DMMR:
		    protoTrace("Reading MMR-compressed image file");
		    break;
	    }
	    dp = convertPhaseCData(dp, totdata, fillorder, params, newparams, rowsperstrip);
	    params = newparams;		// revert back
	}

	if (params.jp) {
#if defined(HAVE_JPEG) && ( defined(HAVE_LCMS) || defined(HAVE_LCMS2) )
	    /*
	     * The image is in raw RGB data.  We now need to compress
	     * with JPEG and put into the right colorspace (ITULAB).
	     */
	    uint32_t w, h;
	    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w);
	    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h);

	    FILE* out = Sys::tmpfile();
	    if (out) {
		char* kk = (char*)malloc(256);
		memset(kk, 0, 256);

		bool ok = convertRawRGBtoITULAB(dp, off, w, h, out, kk, 256);
		if (!ok || kk[0] != 0) {
		    emsg = fxStr::format("Error converting image to JPEG data: %s", kk);
		    protoTrace(emsg);
		    free(kk);
		    fclose(out);
		    return (false);
		}
		// JPEG compression succeeded, redirect pointers
		totdata = ftell(out);
		dp = (u_char*) malloc(totdata);
		rewind(out);
		if (fread(dp, 1, totdata, out) < 1) {
		    protoTrace("Failed storing the JPEG data.");
		}
		free(kk);
		fclose(out);
	    } else {
#endif
		emsg = "Could not open JPEG conversion output stream.";
		protoTrace(emsg);
		return (false);
#if defined(HAVE_JPEG) && ( defined(HAVE_LCMS) || defined(HAVE_LCMS2) )
	    }
#endif
	} else {
	    /*
	     * correct broken Phase C (T.4) data if neccessary 
	     */
	    if (params.df < DF_2DMMR)
		correctPhaseCData(dp, totdata, fillorder, params, rowsperstrip);
	}

	/*
	 * Send the page of data.  This is slightly complicated
	 * by the fact that we may have to add zero-fill before the
	 * EOL codes to bring the transmit time for each scanline
	 * up to the negotiated min-scanline time.
	 *
	 * Note that we blindly force the data to be in LSB2MSB bit
	 * order so that the EOL locating code works (if needed).
	 * This may result in two extraneous bit reversals if the
	 * modem wants the data in MSB2LSB order, but for now we'll
	 * avoid the temptation to optimize.
	 */
	if (!params.jp && params.df <= DF_2DMMR && fillorder != FILLORDER_LSB2MSB) {
	    TIFFReverseBits(dp, totdata);
	}

	/* For debugging purposes we may want to write the image-data to file. */
	if (conf.saverawimage) imagefd = Sys::open("/tmp/out.fax", O_RDWR|O_CREAT|O_EXCL);

	u_int minLen = params.minScanlineSize();
	if (minLen > 0) {			// only in non-ECM
	    /*
	     * Client requires a non-zero min-scanline time.  We
	     * comply by zero-padding scanlines that have <minLen
	     * bytes of data to send.  To minimize underrun we
	     * do this padding in a strip-sized buffer.
	     */
	    u_char* fill = new u_char[minLen*rowsperstrip];
	    u_char* eoFill = fill + minLen*rowsperstrip;
	    u_char* fp = fill;

	    u_char* bp = dp;
	    u_char* ep = dp+totdata;
	    u_long w = 0xffffff;

            /*
             * Immediately copy leading EOL into the fill buffer,
             * because it has not to be padded. Note that leading
             * EOL is not byte-aligned, and so we also copy 4 bits
             * if image data. But that's OK, and may only lead to
             * adding one extra zero-fill byte to the first image
             * row.
             */
            *fp++ = *bp++;
            *fp++ = *bp++;

	    /*
	     * The modem buffers data.  We need to keep track of how much
	     * data has been sent to the modem and how much time has 
	     * elapsed in order to know a proper dataTimeout setting
	     * because the modem may have a lot of data buffered to it
	     * and we can't always rely on a constrained pipe and limited
	     * buffer to the modem to keep our needed wait-time to under
	     * the 60-second setting that dataTimeout presently has.
	     */
	    time_t start = Sys::now();
	    long pagedatasent = 0;

	    do {
		u_char* bol = bp;
                bool foundEOL;
		do {
		    w = (w<<8) | *bp++;
                    foundEOL = EOLcode(w);
                } while (!foundEOL && bp < ep);
		/*
                 * We're either after an EOL code or at the end of data.
		 * If necessary, insert zero-fill before the last byte
		 * in the EOL code so that we comply with the
		 * negotiated min-scanline time.
		 */
		u_int lineLen = bp - bol;
		if ((fp + fxmax(lineLen, minLen) >= eoFill) && (fp-fill != 0)) {
		    /*
		     * Not enough space for this scanline, flush
		     * the current data and reset the pointer into
		     * the zero fill buffer.
		     */
		    pagedatasent += fp-fill;
		    setDataTimeout(pagedatasent, Sys::now() - start, params.br);
		    rc = sendPageData(fill, fp-fill, bitrev, (params.ec != EC_DISABLE), emsg);
		    fp = fill;
		    if (!rc)			// error writing data
			break;
		}
		if (lineLen >= minLen*rowsperstrip) {
		    /*
		     * The fill buffer is smaller than this
		     * scanline alone.  Flush this scanline
		     * also.  lineLen is greater than minLen.
		     */
		    pagedatasent += fp-fill;
		    setDataTimeout(pagedatasent, Sys::now() - start, params.br);
		    rc = sendPageData(bol, lineLen, bitrev, (params.ec != EC_DISABLE), emsg);
		    if (!rc)			// error writing
			break;
		} else {
		    memcpy(fp, bol, lineLen);	// first part of line
		    fp += lineLen;
		    if (lineLen < minLen) {		// must zero-fill
			u_int zeroLen = minLen - lineLen;
                	if ( foundEOL ) {
                	    memset(fp-1, 0, zeroLen);	// zero padding
			    fp += zeroLen;
			    fp[-1] = bp[-1];		// last byte in EOL
			} else {
			    /*
			     * Last line does not contain EOL
			     */
			    memset(fp, 0, zeroLen);	// zero padding
			    fp += zeroLen;
			}
		    }
		}
	    } while (bp < ep);
	    /*
	     * Flush anything that was not sent above.
	     */
	    if (fp > fill && rc) {
		pagedatasent += fp-fill;
		setDataTimeout(pagedatasent, Sys::now() - start, params.br);
		rc = sendPageData(fill, fp-fill, bitrev, (params.ec != EC_DISABLE), emsg);
	    }
	    delete[] fill;
	} else {
	    /*
	     * No EOL-padding needed, just jam the bytes.
	     * We need to set a timeout appropriate to the data size and bitrate.
	     */
	    setDataTimeout(totdata, 0, params.br);
	    rc = sendPageData(dp, (u_int) totdata, bitrev, (params.ec != EC_DISABLE), emsg);
	}
	delete[] data;
	if (imagefd > 0) {
	    Sys::close(imagefd);
	    imagefd = 0;
	}
    }
    if (!wasSSLFax && !repeatPhaseB && (rc || abortRequested()))
	rc = sendRTC(params, ppmcmd, rowsperstrip, emsg);
    protoTrace("SEND end page");
    if (params.ec == EC_DISABLE) {
	// these were already done by ECM protocol
	if (!wasSSLFax && !isSSLFax && rc) {
	    /*
	     * Wait for transmit buffer to empty.
	     */
	    ATResponse r;
	    while ((r = atResponse(rbuf, getDataTimeout())) == AT_OTHER)
		;
	    rc = (r == AT_OK);
	    if (r == AT_NOCARRIER) {
		/*
		 * The NO CARRIER result here is not per-spec.  However,
		 * some modems capable of detecting hangup conditions will
		 * use this to indicate a disconnection.  Because we did
		 * not check for modem responses during the entire data
		 * transfer we flush the modem input so as to avoid reading
		 * any modem responses related to misinterpreted Phase C
		 * data that occurred after the hangup.
		 */
		flushModemInput();
	    }
	}
	if (flowControl == FLOW_XONXOFF)
	    setXONXOFF(FLOW_NONE, FLOW_NONE, ACT_DRAIN);
    }
    if (wasSSLFax) {
	/*
	 * Same reasoning here as in sendPageData().
	 */
	wasSSLFax = false;
	rc = true;
	suppressSSLFax = true;	// since we're likely to get RTN and go back to Phase B let's avoid restarting SSL Fax
    }
    if (!rc && (emsg == "")) {
	emsg = "Unspecified Transmit Phase C error {E149}";	// XXX
    	protoTrace(emsg);
    }
    return (rc);
}

/*
 * Send the post-page-message and wait for a response.
 */
bool
Class1Modem::sendPPM(u_int ppm, HDLCFrame& mcf, fxStr& emsg)
{
    for (int t = 0; t < 3; t++) {
	traceFCF("SEND send", ppm);
	// don't use CRP here because it isn't well-received
	if (transmitFrame(ppm|FCF_SNDR)) {
	    if (recvFrame(mcf, FCF_SNDR, conf.t4Timer, false, false, true, ppm)) return (true);
	}
	if (gotEOT) break;	// skip remaining attempts, as they'd be futile
	if (abortRequested())
	    return (false);
	switchingPause(emsg);
    }
    switch (ppm) {
	case FCF_MPS:
	    emsg = "No response to MPS repeated 3 tries {E150}";
	    break;
	case FCF_EOP:
	    emsg = "No response to EOP repeated 3 tries {E151}";
	    break;
	case FCF_EOM:
	    emsg = "No response to EOM repeated 3 tries {E152}";
	    break;
	default:
	    emsg = "No response to PPM repeated 3 tries {E153}";
	    break;
    }
    protoTrace(emsg);
    return (false);
}

/*
 * Terminate a send session.
 */
void
Class1Modem::sendEnd()
{
    if (!wasModemError() && !gotEOT) {
	fxStr emsg;
	(void) switchingPause(emsg);
	if (!gotEOT) transmitFrame(FCF_DCN|FCF_SNDR);		// disconnect
	setInputBuffering(true);
    }
}

/*
 * Abort an active send session.
 */
void
Class1Modem::sendAbort()
{
    // nothing to do--DCN gets sent in sendEnd
}
