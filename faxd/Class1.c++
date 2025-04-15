/*	$Id: Class1.c++ 1024 2010-11-04 19:44:29Z faxguy $ */
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
 */
#include "Sys.h"
#include "Class1.h"
#include "ModemConfig.h"
#include "HDLCFrame.h"
#include "t.30.h"
#include "tiffio.h"
#include "config.h"
#if defined(HAVE_SSL)
#include "sslfax.h"
#endif

#include <stdlib.h>
#include <ctype.h>
#include <arpa/inet.h>

extern timeval operator-(timeval src1, timeval src2);

const char* Class1Modem::modulationNames[6] = {
    "v.21, chan 2",
    "v.27ter fallback mode",
    "v.27ter",
    "v.29",
    "v.17",
    "v.33",
};
/*
 * Modem capabilities are queried at startup and a
 * table based on this is created for each modem.
 * This information is used to negotiate T.30 session
 * parameters (e.g. signalling rate).
 *
 * NB: v.17 w/ long training is the same as v.33, at
 *     least at 12000 and 14400.
 */
const Class1Cap Class1Modem::basicCaps[15] = {
    {  3,  0,	 	0,		     V21,   false, 0 },  // v.21
    {  24, BR_2400,	DCSSIGRATE_2400V27,  V27FB, false, 1 },  // v.27 ter
    {  48, BR_4800,	DCSSIGRATE_4800V27,  V27,   false, 2 },  // v.27 ter
    {  72, BR_7200,	DCSSIGRATE_7200V29,  V29,   false, 3 },  // v.29
    {  73, BR_7200,	DCSSIGRATE_7200V17,  V17,   false, 4 },  // v.17
    {  74, BR_7200,	DCSSIGRATE_7200V17,  V17,   false, 5 },  // v.17 w/st
    {  96, BR_9600,	DCSSIGRATE_9600V29,  V29,   false, 6 },  // v.29
    {  97, BR_9600,	DCSSIGRATE_9600V17,  V17,   false, 7 },  // v.17
    {  98, BR_9600,	DCSSIGRATE_9600V17,  V17,   false, 8 },  // v.17 w/st
    { 121, BR_12000,	DCSSIGRATE_12000V33, V33,   false, 9 },  // v.33
    { 121, BR_12000,	DCSSIGRATE_12000V17, V17,   false, 10 }, // v.17
    { 122, BR_12000,	DCSSIGRATE_12000V17, V17,   false, 11 }, // v.17 w/st
    { 145, BR_14400,	DCSSIGRATE_14400V33, V33,   false, 12 }, // v.33
    { 145, BR_14400,	DCSSIGRATE_14400V17, V17,   false, 13 }, // v.17
    { 146, BR_14400,	DCSSIGRATE_14400V17, V17,   false, 14 }, // v.17 w/st
};
#define	NCAPS	(sizeof (basicCaps) / sizeof (basicCaps[0]))

const char* Class1Modem::tmCmdFmt = "AT+FTM=%u";
const char* Class1Modem::rmCmdFmt = "AT+FRM=%u";

Class1Modem::Class1Modem(FaxServer& s, const ModemConfig& c)
    : FaxModem(s,c)
    , thCmd("AT+FTH=3")
    , rhCmd("AT+FRH=3")
{
    messageReceived = false;
    memcpy(xmitCaps, basicCaps, sizeof (basicCaps));
    memcpy(recvCaps, basicCaps, sizeof (basicCaps));

    /*
     * Because the sending routines deliver data to the transmit functions
     * in segments, these must be globally available to spool outgoing data
     * until a complete ECM block can be assembled.
     *
     * Besides the contents of ecmBlock, ecmStuffedBlock must be able to
     * hold all sync flag bytes, stuffed bits, and RCP frames.
     */
    u_int fs = 256;
    if (conf.class1ECMFrameSize == 64) fs = 64;
    ecmFrame = (u_char*) malloc(fs + 4);
    fxAssert(ecmFrame != NULL, "ECM procedure error (frame).");
    ecmBlock = (u_char*) malloc((fs + 4) * 256);
    fxAssert(ecmBlock != NULL, "ECM procedure error (block).");
    ecmStuffedBlock = (u_char*) malloc(fs == 256 ? 83000 : 33000);
    fxAssert(ecmStuffedBlock != NULL, "ECM procedure error (stuffed block).");
    gotCTRL = false;
    repeatPhaseB = false;
    silenceHeard = false;
    imagefd = 0;
    triggerInterrupt = false;
    checkReadOnWrite = true;
}

Class1Modem::~Class1Modem()
{
    free(ecmFrame);
    free(ecmBlock);
    free(ecmStuffedBlock);
}

bool
Class1Modem::atCmd(const fxStr& cmd, ATResponse r, long ms)
{
    silenceHeard = false;
    return (ClassModem::atCmd(cmd, r, ms));
}

/*
 * Check if the modem is a Class 1 modem and,
 * if so, configure it for use.
 */
bool
Class1Modem::setupModem(bool isSend)
{
    if (!selectBaudRate(conf.maxRate, conf.flowControl, conf.flowControl))
	return (false);
    // Query service support information
    fxStr s;
    if (doQuery(conf.classQueryCmd, s, 5000) && FaxModem::parseRange(s, modemServices))
	traceBits(modemServices & SERVICE_ALL, serviceNames);
    if ((modemServices & serviceType) == 0)
	return (false);
    atCmd(classCmd);

    /*
     * Query manufacturer, model, and firmware revision.
     * We use the manufacturer especially as a key to
     * working around firmware bugs (yech!).
     */
    if (setupManufacturer(modemMfr)) {
	modemCapability("Mfr " | modemMfr);
	modemMfr.raisecase();
    }
    (void) setupModel(modemModel);
    (void) setupRevision(modemRevision);
    if (modemModel != "")
	modemCapability("Model " | modemModel);
    if (modemRevision != "")
	modemCapability("Revision " | modemRevision);

    /*
     * Get modem capabilities and calculate best signalling
     * rate, data formatting capabilities, etc. for use in
     * T.30 negotiations.
     */
    if (!class1Query(conf.class1TMQueryCmd, xmitCaps)) {
	serverTrace("Error parsing \"+FTM\" query response: \"%s\"", rbuf);
	return (false);
    }
    modemParams.br = 0;
    u_int i;
    for (i = 1; i < NCAPS; i++)
	if (xmitCaps[i].ok)
	    modemParams.br |= BIT(xmitCaps[i].br);
    nonV34br = modemParams.br;
    if (conf.class1EnableV34Cmd != "" && conf.class1ECMSupport) {
	// This is cosmetic, mostly, to state the modem supports V.34.
	// We could query the modem but that would require another
	// config option, so we just trust the enable command.
	primaryV34Rate = 0;
	u_short pos = conf.class1EnableV34Cmd.findR(conf.class1EnableV34Cmd.length(), "+F34=", 5) + 4;
	if (pos == 4) // Unknown class1EnableV34Cmd
	    modemCapability("Unable to determinate V.34 speed from class1EnableV34Cmd, V.34 faxing will be disabled!");
	else
	    primaryV34Rate = atoi(conf.class1EnableV34Cmd.extract(pos, conf.class1EnableV34Cmd.next(pos, ',') - pos));
	modemParams.br |= BIT(primaryV34Rate) - 1;
    }
    modemParams.wd = conf.class1PageWidthSupport;
    modemParams.ln = conf.class1PageLengthSupport;
    modemParams.df = BIT(DF_1DMH) | (conf.class1MRSupport ? BIT(DF_2DMR) : 0);
    modemParams.bf = BF_DISABLE;
    modemParams.st = ST_ALL;
    modemParams.jp = 0;
    pokeConfig(isSend);
    traceModemParams();
    /*
     * Receive capabilities are maintained separately from
     * transmit capabilities because we need to know more
     * than the signalling rate to formulate the DIS.
     */ 
    if (!class1Query(conf.class1RMQueryCmd, recvCaps)) {
	serverTrace("Error parsing \"+FRM\" query response: \"%s\"", rbuf);
	return (false);
    }
    u_int mods = 0;
    for (i = 1; i < NCAPS; i++)
	if (recvCaps[i].ok)
	    mods |= BIT(recvCaps[i].mod);
    switch (mods) {
    case BIT(V27FB):
	discap = DISSIGRATE_V27FB;
	break;
    case BIT(V27FB)|BIT(V27):
	discap = DISSIGRATE_V27;
	break;
    case BIT(V29):
	discap = DISSIGRATE_V29;
	break;
    case BIT(V27FB)|BIT(V27)|BIT(V29):
	discap = DISSIGRATE_V2729;
	break;
    case BIT(V27FB)|BIT(V27)|BIT(V29)|BIT(V33):
	discap = DISSIGRATE_V33;
	break;
    case BIT(V27FB)|BIT(V27)|BIT(V29)|BIT(V17):
    case BIT(V27FB)|BIT(V27)|BIT(V29)|BIT(V33)|BIT(V17):
	discap = DISSIGRATE_V17;
	break;
    }
    /*
     * T.30 specifies that HDLC frame data are in MSB bit
     * order except for CIG/TSI data which have LSB bit order.
     * We compose and interpret frame data in MSB bit order
     * and pass the frames through frameRev immediately before
     * transmitting and upon receiving to handle modem characteristics.
     *
     * Class1Modem::encodeTSI and Class1Modem::decodeTSI (below)
     * assume this work and process CIG/TSI data accordingly.
     */
    frameRev = TIFFGetBitRevTable(conf.frameFillOrder == FILLORDER_LSB2MSB);

    setupClass1Parameters();
    return (true);
}

/*
 * These are potentially dynamic modem settings that can be altered on-the-fly.
 */
void
Class1Modem::pokeConfig(bool isSend)
{
    modemParams.vr = conf.class1Resolutions;	// bitmapped by configuration
    if (conf.class1ECMSupport) {
	modemParams.ec = BIT(EC_DISABLE) | BIT(EC_ENABLE64) | BIT(EC_ENABLE256);

	if (conf.class1MMRSupport) modemParams.df |= BIT(DF_2DMMR);
	else modemParams.df &= ~BIT(DF_2DMMR);

	switch (conf.class1JBIGSupport) {
	    case FaxModem::JBIG_FULL:
		jbigSupported = true; 
		break;
	    case FaxModem::JBIG_SEND:
		jbigSupported = isSend;
		break;
	    case FaxModem::JBIG_RECV:
		jbigSupported = !isSend;
		break;
	    default:
		jbigSupported = false;
		break;
	}
	if (jbigSupported) modemParams.df |= BIT(DF_JBIG);
	else modemParams.df &= ~BIT(DF_JBIG);

	modemParams.jp = 0;
	if (conf.class1GreyJPEGSupport || conf.class1ColorJPEGSupport)
	    modemParams.jp |= BIT(JP_GREY);
	if (conf.class1ColorJPEGSupport)
	    modemParams.jp |= BIT(JP_COLOR);
    } else {
	modemParams.ec = BIT(EC_DISABLE);
	modemParams.df &= ~BIT(DF_2DMMR);
	modemParams.jp = 0;
    }
}

/*
 * Send the modem the Class 1 configuration commands.
 */
bool
Class1Modem::setupClass1Parameters()
{
    if (modemServices & serviceType) {
	setupFlowControl(flowControl);
	atCmd(conf.setupAACmd);
    }
    return (true);
}

void
Class1Modem::hangup()
{
#if defined(HAVE_SSL)
    if (isSSLFax) {
	// terminate SSL Fax connection
	SSLFax sslfax;
	sslfax.cleanup(sslFaxProcess);
	useSSLFax = false;
	isSSLFax = false;
	suppressSSLFax = false;
	usingSSLFaxProxy = false;
	setSSLFaxFd(0);
	sslWatchModem = false;
	wasSSLFax = true;
    }
#endif
    if (useV34) {
	// terminate V.34 channel
	u_char buf[2];
	buf[0] = DLE; buf[1] = EOT;		// <DLE><EOT>
	putModemData(buf, 2, -1, false);	// don't get stopped by data to be read
	// T.31-A1 states that OK is expected; however, the Si2435 responds "NO CARRIER".
	// We have to clear any data in the read buffer.  If we don't, then we may have trouble
	// sending ATH0 afterwards.
	if (!waitFor(AT_OK) && lastResponse != AT_NOCARRIER) waitFor(AT_OK);
    }
    atCmd(conf.onHookCmd, AT_OK, 5000);
}

/*
 * Setup receive-specific parameters.
 */
bool
Class1Modem::setupReceive()
{
    return (true);			// nothing to do
}

/*
 * Send the modem any commands needed to force use of
 * the specified flow control scheme.
 */
bool
Class1Modem::setupFlowControl(FlowControl fc)
{
    switch (fc) {
    case FLOW_NONE:	return atCmd(conf.class1NFLOCmd);
    case FLOW_XONXOFF:	return atCmd(conf.class1SFLOCmd);
    case FLOW_RTSCTS:	return atCmd(conf.class1HFLOCmd);
    }
    return (true);
}

char*
randomString(int len)
{
    static char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    int cs = sizeof(charset) - 1;
    char* rstr = (char*) malloc((len + 1) * sizeof(char));
    for (int i = 0; i < len; i++) rstr[i] = charset[rand() % cs];
    rstr[len] = '\0';
    return rstr;
}

/*
 * Place the modem into the appropriate state
 * for sending facsimile.
 */
bool
Class1Modem::faxService(bool enableV34, bool enableV17)
{
    if (!atCmd(classCmd)) return (false);
    if (conf.class1EnableV34Cmd != "" && enableV34)
	atCmd(conf.class1EnableV34Cmd);
    useV34 = false;	// only when V.8 handshaking is used
#if defined(HAVE_SSL)
    SSLFax sslfax;
    sslFaxProcess = sslfax.null();
#endif
    useSSLFax = false;
    isSSLFax = false;
    wasSSLFax = false;
    suppressSSLFax = false;
    usingSSLFaxProxy = false;
    fullDuplex = false;
    remoteCSAinfo = "";
    sslFaxPasscode = randomString(10);
    remoteCSAType = 0x00;
    sslRcvCC = sslRcvNext = sslRcvBit = sslGotByte = 0;
    sslSawBlockEnd = false;
    sslWatchModem = false;
    gotEOT = false;
    gotRTNC = false;
    return (setupFlowControl(flowControl));
}

/*
 * The modem is initialized. Now set it to ready for answer.
 */
bool
Class1Modem::ready(long ms)
{
    gotEOT = false;
    gotRTNC = false;
    useV34 = false;     // only when V.8 handshaking is used
#if defined(HAVE_SSL)
    SSLFax sslfax;
    sslFaxProcess = sslfax.null();
#endif
    useSSLFax = false;
    isSSLFax = false;
    wasSSLFax = false;
    suppressSSLFax = false;
    usingSSLFaxProxy = false;
    fullDuplex = false;
    sslRcvCC = sslRcvNext = sslRcvBit = sslGotByte = 0;
    sslSawBlockEnd = false;
    sslWatchModem = false;
    remoteCSAinfo = "";
    sslFaxPasscode = randomString(10);
    remoteCSAType = 0x00;
    if (conf.class1EnableV34Cmd != "" && conf.class1ECMSupport)   
        if (!atCmd(conf.class1EnableV34Cmd))
	    return (false);
    if (conf.class1AdaptRecvCmd != "")
	if (!atCmd(conf.class1AdaptRecvCmd))
	    return (false);
    return (FaxModem::ready(ms));
}


/*
 * Set the local subscriber identification.
 */
void
Class1Modem::setLID(const fxStr& number)
{
    encodeTSI(lid, number);
}

bool
Class1Modem::supportsPolling() const
{
    return (true);
}

/*
 * Construct a binary TSI/CIG string for transmission.  Note
 * that we do not enforce the specification that restricts
 * the alphabet to that of Table 3/T.30 (numeric digits,
 * hyphen, period, and space), but instead permit any
 * printable ASCII string of at most 20 characters to be
 * used.  Note also that the bit reversal is done with the
 * understanding that the encoded string is processed again
 * using frameRev (see above) when forming the transmitted
 * HDLC frame.
 */
void
Class1Modem::encodeTSI(fxStr& binary, const fxStr& ascii)
{
    u_int i, j;
    u_char buf[20];
    u_int n = fxmin(ascii.length(),(u_int) 20);
    for (i = 0, j = 0; i < n; i++) {
	int c = ascii[i];
	if (isprint(c) || c == ' ')
	    buf[j++] = frameRev[c];
    }
    /*
     * Now ``reverse copy'' the string.
     */
    binary.resize(20);
    for (i = 0; j > 0; i++, j--)
	binary[i] = buf[j-1];
    for (; i < 20; i++)
	binary[i] = frameRev[' '];	// blank pad remainder
}

/*
 * Encode an NSF string for transmission.
 */
void
Class1Modem::encodeNSF(fxStr& binary, const fxStr& ascii)
{
    u_int i, j;
    u_int n = ascii.length();
    binary.resize(n);
    for (i = 0, j = 0; i < n; i++) {
	int c = ascii[i];
	if (isprint(c) || c == ' ')
	    binary[j++] = frameRev[c];
    }
}

/*
 * Encode a CSA, TSA, CIA, IRA, or ISP string for transmission.
 *
 * The format is defined in T.30 5.3.6.2.12. The format provides
 * for multiple frames and long addresses (>77 bytes, >80 byte FIF),
 * but because we're only currently dealing with what is likely just
 * a short string, we don't get that complicated yet.
 *
 * T.30 doesn't specify how the address is to be formatted in the FIF,
 * whether that's IPv4, IPv6, email, or URL.  T.30 does suggest that
 * these features are intended for T.37 and T.38 uses (because to use
 * them bits 1 and/or 3 of the DIS/DCS must be set).  However, neither
 * T.37 nor T.38 mention these signals at all except for one reference
 * in T.38 to IRA/ISP, which only refers the reader back to T.30.
 *
 * So, it seems that manufacturers are left to their own methods in
 * formatting the FIF, and manufacturers are expected to collaborate
 * between themselves about how to interact with each other via these
 * signals.  Therefore, the net effect of DIS/DCS bits 1 and 3 is just
 * to enable use of CIA, TSA, and CSA.  (IRA/ISP is enabled by yet
 * other bits.)  There is an expectation that use of those bits would
 * suggest T.37 support and T.38 support, but because the FIF of CIA,
 * TSA, and CSA is not specified it is left to the DTE to encode or
 * decode the meaning and use of these signals so that others may
 * be able to interact with it.  This enables us to use bits 1 and 3
 * and, consequently, TSA and CSA, for SSL Fax.
 *
 * So, until we see how other manufacturers have implemented these,
 * we'll just format the address in a way that will be as transparent
 * as possible.  (So, we use URLs in a human-readable format.)
 *
 * The administrator is expected to configure the modem with
 * Class1SSLFaxInfo using a hostname and port number, in that order,
 * separated by a colon, like this:  "hostname:port".  If the
 * hostname is an IPv6 address it should be enclosed by square
 * [brackets].  encodeCSA will prepend Class1SSLFaxInfo with the
 * protocol (ssl) and a passcode with formatting.  The end-result
 * will be a URL in this format:
 *
 *        ssl://passcode@hostname:port
 *
 * We'll use a passcode of 10 characters (bytes) which will leave
 * 60 bytes for the Class1SSLFaxInfo.  Although, I suppose that
 * a hostname could be longer than this (perhaps just contrived),
 * it seems a reasonable limit.  If the limit should be expanded
 * we'll need to add support for multiple frames as indicated in
 * T.30 5.3.6.2.12.
 */
void
Class1Modem::encodeCSA(fxStr& binary, const fxStr& ascii, bool haspasscode)
{
    u_int n = ascii.length();
    if (n > 60) {
	// We can't support the length of the string.
	binary = "";
	return;
    }
    fxStr url = ascii;
    if (!haspasscode) {
	url.insert('@');
	url.insert(sslFaxPasscode);
    }
    url.insert("ssl://");
    n = url.length();
    u_int i, j = 0;
    binary.resize(n+3);
    binary[j++] = 0x00;			// no continuation
    binary[j++] = 0x40;			// URL address
    binary[j++] = frameRev[n];		// address length
    for (i = 0; i < n; i++) {
	int c = url[i];
	binary[j++] = frameRev[c];	// T.30 says LSB2MSB for this
    }
}

const fxStr&
Class1Modem::decodeCSA(fxStr& ascii, const HDLCFrame& binary)
{
    remoteCSAinfo = "";
    remoteCSAType = 0x00;

    u_int n = binary.getFrameDataLength();
    ascii = "";
    if (n > 3) {	// FCF+sequence+type+length
	n -= 4;
	remoteCSAType = binary[4];
	u_int len = binary[5]; len = frameRev[len];
	if (n != len) protoTrace("CSA/TSA address length mismatch.  Expected %d, got %d.", len, n);
	ascii.resize(n);
	u_int d = 0;
	for (; n > 0; n--) {
	    u_char b = binary[6+d];
	    ascii[d++] = frameRev[b];
	}
    }
    return ascii;
}

/*
 * Do the inverse of encodeTSI; i.e. convert a binary
 * string of encoded digits into the equivalent ascii.
 * Note that as above (and per the spec) bytes are
 * presumed to be transmitted in LSB2MSB bit order and
 * then reversed on receipt according to the host bit
 * order.
 */
const fxStr&
Class1Modem::decodeTSI(fxStr& ascii, const HDLCFrame& binary)
{
    int n = binary.getFrameDataLength();
    if (n > 20)			// spec says no more than 20 digits
	n = 20;
    else n--;			// trim FCF
    ascii.resize(n);
    u_int d = 0;
    bool seenDigit = false;
    for (const u_char* cp = binary.getFrameData() + n-1; n > 0; cp--, n--) {
	/*
	 * Accept any printable ascii.
	 */
	u_char c = frameRev[*cp];
        if (isprint(c) || c == ' ') {
	    if (c != ' ')
		seenDigit = true;
	    if (seenDigit)
		ascii[d++] = c;
	}
    }
    ascii.resize(d);
    return ascii;
}

/*
 * Construct a binary PWD/SUB/SEP string for transmission.
 */
void
Class1Modem::encodePWD(fxStr& binary, const fxStr& ascii)
{
    encodeTSI(binary, ascii);
}

/*
 * Do the inverse of encodePWD; i.e. convert a binary
 * string of encoded digits into the equivalent ascii.
 */
const fxStr&
Class1Modem::decodePWD(fxStr& ascii, const HDLCFrame& binary)
{
    return decodeTSI(ascii, binary);
}

bool
Class1Modem::switchingPause(fxStr& emsg, u_int times)
{
    if (isSSLFax) return (true);	// SSL is full-duplex, so no need to pause
    if (useV34) return (true);		// V.34 is also full-duplex.
    /*
     * If class1SwitchingCmd is of the AT+FRS=n form we honor
     * the caller's indication for a multiplication of the 
     * configured silence detection.  This is primarily used
     * to avoid sending CRP as a receiver during a sender's TCF,
     * but it could also be used in places where we find a 
     * longer wait often necessary.
     */
    fxStr scmd = fxStr(conf.class1SwitchingCmd);
    if (times != 1) {
	fxStr ncmd = fxStr(scmd);
	ncmd.raiseatcmd();
	if (ncmd.length() > 7 && ncmd.head(7) == "AT+FRS=") {
	    int dur = atoi(ncmd.tail(ncmd.length()-7)) * times;
	    scmd = scmd.head(7) | fxStr(dur, "%d");
	}
    }
    if (!silenceHeard && !atCmd(scmd, AT_OK, 60*1000)) {
	emsg = "Failure to receive silence (synchronization failure). {E100}";
	protoTrace(emsg);
	if (lastResponse == AT_ERROR) gotEOT = true;
	if (wasTimeout()) abortReceive();
	return (false);
    }
    /*
     * We necessarily use class1SwitchingCmd rather frequently... 
     * sometimes this is programmed to be immediately before our signalling,
     * and sometimes this is programmed to be immediately after the remote
     * signalling.  That doesn't therefore give us very good control on 
     * ensuring that class1SwitchingCmd is not issued in duplicate in some
     * kinds of program flow.
     *
     * Ideally we'd be able to standardize on issuing class1SwitchingCmd
     * only in one case or another (but not both), but that's not particularly
     * an easy ideal to realize given the number of exceptions to that rule
     * that would neccesarily be required.
     *
     * So for now we just set this flag here, and we unset it in atCmd, and 
     * if the flag is set when we come back here, then we can avoid duplication.
     */
    silenceHeard = true;
    return (true);
}

/*
 * Pass data to modem, filtering DLE's and
 * optionally including the end-of-data
 * marker <DLE><ETX>.
 *
 * If the data transfer is aborted by the user
 * then we must take care to send <DLE><ETX>
 * in order to exit gracefully.
 */
bool
Class1Modem::sendClass1Data(const u_char* data, u_int cc,
    const u_char* bitrev, bool eod, long ms)
{
#if defined(HAVE_SSL)
    if (isSSLFax) {
	SSLFax sfp;
	int cs = sfp.write(sslFaxProcess, data, cc, bitrev, (sslWatchModem ? getModemFd() : 0), (ms ? ms : 60000), true);
	if (cs > 0) {	// SSLFax::write returns "1" on success.
	    if (eod) {
		u_char buf[2];
		buf[0] = bitrev[DLE];	// counteract later reversal
		buf[1] = bitrev[ETX];
		cs = sfp.write(sslFaxProcess, buf, 2, bitrev, getModemFd(), (ms ? ms : 60000), false);
	    }
	    if (cs > 0) {
		protoTrace("SSL Fax successfully wrote %d bytes", cc);
		return (true);
	    }
	}
	protoTrace("SSL FAX WRITE ERROR: %s", (const char*) sslFaxProcess.emsg);
	useSSLFax = false;
	isSSLFax = false;
	usingSSLFaxProxy = false;
	setSSLFaxFd(0);
	sslWatchModem = false;
	wasSSLFax = true;
	params.br = storedBitrate;

	if (useV34) {
	    /*
	     * Because in V.34-fax the modem continually demodulates and buffers whatever was there,
	     * and because we haven't been reading from the modem during the entire SSL Fax operation
	     * we could now have lots of stuff buffered, and because we don't know how long it's been
	     * in there we could be out of sync if we responded to it.  So, we flush it all in order
	     * to get in real-time sync again.
	     */
	    flushModemInput();
	}

	return (false);
    }
#endif

    bool ok = putModemDLEData(data, cc, bitrev, ms, conf.doPhaseCDebug, checkReadOnWrite);
    if (ok && !wasTimeout() && (eod || abortRequested())) {
	u_char buf[2];
	buf[0] = DLE;
	buf[1] = ETX;
	ok = putModemData(buf, 2, ms, checkReadOnWrite);
	return (ok && !abortRequested());
    } else
	return (ok);
}

/*
 * Receive timed out, abort the operation
 * and resynchronize before returning.
 */
void
Class1Modem::abortReceive()
{
    if (isSSLFax || (useV34 && !wasSSLFax)) return;	// nothing to do in V.34 or SSL Fax
    if (wasSSLFax) {
	// the abort already happened
	wasSSLFax = false;
	setTimeout(false);
	return;
    }
    bool b = wasTimeout();
    char c = CAN;			// anything other than DC1/DC3
    /*
     * If the modem handles abort properly, then just
     * wait for an OK result after sending the character
     * abort.  Otherwise, just send "AT" and wait for the
     * return "OK".
     */
    if (conf.class1RecvAbortOK) {
	putModem(&c, 1, 1);
	while (!waitFor(AT_OK, conf.class1RecvAbortOK) && lastResponse == AT_OTHER && !wasTimeout());
    } else {
	(void) atCmd("AT", AT_OK, 500);
	pause(100);
    }
    setTimeout(b);			// XXX putModem clobbers timeout state
}

/*
 * Request a primary rate renegotiation.
 */
bool
Class1Modem::renegotiatePrimary(bool constrain)
{
    u_char buf[4];
    u_short size = 0;
    buf[size++] = DLE;
    if (constrain) {
	// don't neotiate a faster rate
	if (primaryV34Rate == 1) buf[size++] = 0x70;	// 2400 bit/s
	else if (primaryV34Rate == 2 || primaryV34Rate == 3) {
	    /*
	     * V.34 bitspeed 2400 is only possible with symbolrate 2400, and
	     * as we have no way of determining the symbolrate, we cannot
	     * wisely instruct the modem to use 2400 bps unless the modem
	     * negotiates it independently.
	     */
	    buf[size++] = 0x71;				// 4800 bit/s
	} else buf[size++] = primaryV34Rate + 0x6D;	// drop 4800 bit/s
	buf[size++] = DLE;
    }
    buf[size++] = 0x6C;					// <DLE><pph>
    if (!putModemData(buf, size)) return (false);
    if (constrain)
	protoTrace("Request primary rate renegotiation (limit %u bit/s).", 
	    primaryV34Rate == 1 ? 2400 : primaryV34Rate > 3 ? (primaryV34Rate-2)*2400 : 4800);
    else
	protoTrace("Request primary rate renegotiation.");
    return (true);
}

/*
 * We need to be ambidextrous in reading data from either
 * the modem or the SSL Fax socket.
 */
int
Class1Modem::getModemChar(long ms, bool isquery)
{
    if (!isSSLFax) return (ClassModem::getModemChar(ms, isquery));
#if defined(HAVE_SSL)
    // We can ignore isquery for SSL Fax because there is no DCE debug needed for it.
    int buf[1];
    SSLFax sfp;
    int r = sfp.read(sslFaxProcess, buf, 1, (sslWatchModem ? getModemFd() : 0), (ms ? ms : 60000));
    if (r > 0) {
	if (getSSLFaxTracing()) protoTrace("--> SSL Fax <%u:%.2X>", r, (buf[0] & 0xFF));
	memmove(sslFaxPastData, sslFaxPastData+1, 10);
	sslFaxPastData[10] = buf[0] & 0xFF;
	return (buf[0] & 0xFF);
    }
    protoTrace("SSL FAX READ ERROR: %s", (const char*) sslFaxProcess.emsg);
    useSSLFax = false;
    isSSLFax = false;
    usingSSLFaxProxy = false;
    setSSLFaxFd(0);
    sslWatchModem = false;
    wasSSLFax = true;
    params.br = storedBitrate;

    if (useV34) {
	/*
	 * Because in V.34-fax the modem continually demodulates and buffers whatever was there,
	 * and because we haven't been reading from the modem during the entire SSL Fax operation
	 * we could now have lots of stuff buffered, and because we don't know how long it's been
	 * in there we could be out of sync if we responded to it.  So, we flush it all in order
	 * to get in real-time sync again.
	 */
	flushModemInput();
	r = 0;
    }

    if (r == -1) {
	/*
	 * The modem has data waiting.  This probably means that the SSL Fax connection failed and the
	 * other end started transmitting V.21 HDLC or Phase C data.  So, here we need to collect the
	 * CONNECT response and then pass along the first byte of the HDLC frame.
	 *
	 * If we get +FRH:3 then we were waiting for Phase C data, negotiated SSL Fax for some period
	 * of time, which later failed, and now we've detected V.21 HDLC.
	 */
	if (waitFor(AT_CONNECT, ms) || (lastResponse == AT_FRH3 && waitFor(AT_CONNECT, conf.t2Timer))) {
	    wasSSLFax = false;
	    return (ClassModem::getModemChar(ms, isquery));
	}
    }
#endif
    return (EOF);
}

int
Class1Modem::getModemBit(long ms)
{
    if (!isSSLFax) return (ClassModem::getModemBit(ms));
    /*
     * Return bytes bit-by-bit in MSB2LSB order.
     * getModemChar() returns them in LSB2MSB.
     */
    if (sslRcvBit < 1) {
	sslRcvBit = 8;
	sslGotByte = getModemChar(ms);
	if (sslGotByte == 0x10) {          // strip stuffed DLE
	    sslGotByte = getModemChar(ms);
	    if (sslGotByte == 0x03) {
		sslSawBlockEnd = true;    // DLE+ETX
		sawBlockEnd();
	    }
	}
    }
    // enable this to simulate a VERY noisy connection
    // if (((int) Sys::now() & 1) && ((random() % 10000)/10000.0) > 0.95) return (1);
    if (sslGotByte == EOF) {
	sslSawBlockEnd = true;
	sawBlockEnd();
	return (EOF);
    } else if (sslGotByte & (0x80 >> --sslRcvBit)) return (1);
    else return (0);
}

int
Class1Modem::getModemDataChar()
{
    if (!isSSLFax) return (ClassModem::getModemChar(getDataTimeout()));
    return (getModemChar(getDataTimeout()));
}

/*
 * Wait for a <DLE><ctrl> response per T.31-A1 B.8.4.
 */
bool
Class1Modem::waitForDCEChannel(bool awaitctrl)
{
    time_t start = Sys::now();
    int c;
    fxStr garbage;
    bool gotresponse = false;
    gotRTNC = false;
    ctrlFrameRcvd = fxStr::null;
    do {
	c = getModemChar(60000);
	if (c == EOF) {
	    // possible SSL Fax connection
	    return (false);
	}
	if (c == DLE) {
	    /*
	     * With V.34-faxing we expect <DLE><command>
	     * Refer to T.31-A1 Table B.1.  Except for EOT
	     * these are merely indicators and do not require
	     * action.
	     */
	    c = getModemChar(60000);
	    switch (c) {
		case EOT:
		    protoTrace("EOT received (end of transmission)");
		    gotEOT = true;
		    recvdDCN = true;
		    return (false);
		case 0x69:
		    protoTrace("Control channel retrain");
		    // wait for the control channel to reappear
		    // should we reset the timeout setting?
		    gotresponse = waitForDCEChannel(true);
		    gotRTNC = true;
		    return ((awaitctrl ? gotresponse : false));
		case 0x6B:
		    protoTrace("Primary channel selected");
		    gotCTRL = false;
		    continue;
		case 0x6D:
		    protoTrace("Control channel selected");
		    gotCTRL = true;
		    continue;
		case 0x6E:			// 1200 bit/s
		case 0x6F:			// 2400 bit/s
		    // control rate indication
		    if (controlV34Rate != (c - 0x6D)) {
			controlV34Rate = (c - 0x6D);
			protoTrace("Control channel rate now %u bit/s", controlV34Rate*1200);
		    }
		    if (awaitctrl) gotresponse = true;
		    continue;
		case 0x70:			//  2400 bit/s
		case 0x71:			//  4800 bit/s
		case 0x72:			//  7200 bit/s
		case 0x73:			//  9600 bit/s
		case 0x74:			// 12000 bit/s
		case 0x75:			// 14400 bit/s
		case 0x76:			// 16800 bit/s
		case 0x77:			// 19200 bit/s
		case 0x78:			// 21600 bit/s
		case 0x79:			// 24000 bit/s
		case 0x7A:			// 26400 bit/s
		case 0x7B:			// 28800 bit/s
		case 0x7C:			// 31200 bit/s
		case 0x7D:			// 33600 bit/s
		    // primary rate indication
		    if (primaryV34Rate != (c - 0x6F)) {
			primaryV34Rate = (c - 0x6F);
			protoTrace("Primary channel rate now %u bit/s", primaryV34Rate*2400);
		    }
		    if (!awaitctrl) gotresponse = true;
		    continue;
		default:
		    // unexpected <DLE><command>, deem as garbage
		    garbage.append(DLE);
		    garbage.append(c);
		    break;
	    }
	} else garbage.append(c);
	if (gotCTRL && garbage.length() > 2 && (garbage[0] & 0xFF) == 0xFF && 
	    (garbage[garbage.length()-2] & 0xFF) == 0x10 &&
	    (garbage[garbage.length()-1] & 0xFF) == 0x03) {
	    // We got a control frame and won't get the channel change...
	    for (u_int i = 0; i < garbage.length()-2; i++) {
		if ((garbage[i] & 0xFF) == DLE && ++i < garbage.length()) {
		    /*
		     * We've got a frame and must apply T.31-A1 Table B.1.
		     */
		    if ((garbage[i] & 0xFF) == 0x07) {	// end of HDLC frame w/FCS error
			// what to do? send CRP? discard frame?
			continue;
		    }
		    switch (garbage[i] & 0xFF) {
			case DLE:	// <DLE><DLE> => <DLE>
			    ctrlFrameRcvd.append(DLE);
			    break;
			case SUB:	// <DLE><SUB> => <DLE><DLE>
			    ctrlFrameRcvd.append(DLE);
			    ctrlFrameRcvd.append(DLE);
			    break;
			case 0x51:	// <DLE><0x51> => <DC1>
			    ctrlFrameRcvd.append(DC1);
			    break;
			case 0x53:	// <DLE><0x53> => <DC3>
			    ctrlFrameRcvd.append(0x13);
			    break;
		    }
		} else
		    ctrlFrameRcvd.append(garbage[i] & 0xFF);
	    }
	    return (false);
	}
	fxStr rcpsignal;
	rcpsignal.append(0xFF);	rcpsignal.append(0x03);	rcpsignal.append(0x86);	rcpsignal.append(0x69);
	rcpsignal.append(0xCB);	rcpsignal.append(0x10);	rcpsignal.append(0x03);
	if (!gotCTRL && garbage == rcpsignal) {
	    // We anticipate getting "extra" RCP frames since we
	    // only look for one but usually we will see three.
	    garbage.cut(0, 7);
	}
    } while (!gotresponse && Sys::now()-start < 60);
    if (getHDLCTracing() && garbage.length()) {
	fxStr buf;
	u_int j = 0;
	for (u_int i = 0; i < garbage.length(); i++) {
	    if (j > 0)
		buf.append(' ');
	    buf.append(fxStr(garbage[i] & 0xFF, "%2.2X"));
	    j++;
	    if (j > 19) {
		protoTrace("--> [%u:%.*s]",
		    j, buf.length(), (const char*) buf);
		buf = "";
		j = 0;
	    }
	}
	if (j)
	    protoTrace("--> [%u:%.*s]",
		j, buf.length(), (const char*) buf);
    }
    return (gotresponse);
}

/*
 * Receive an HDLC frame. The frame itself must
 * be received within 3 seconds (per the spec).
 * If a carrier is received, but the complete frame
 * is not received before the timeout, the receive
 * is aborted.
 */
bool
Class1Modem::recvRawFrame(HDLCFrame& frame)
{
    /*
     * We need to be generous here ... given that
     * some frames can be long and some devices
     * can add lots of flags to the signalling.
     * Adhering to the spec too closely, 5 sec,
     * appears to be too conservative; if the modem
     * has said CONNECT, then it should be
     * responsible enough to carry-through with
     * things.  A timeout here is only needed to
     * reign-in modems that don't carry-through.
     */
    startTimeout(10000);
    /*
     * Strip HDLC frame flags. This is not needed,
     * (according to the standard DCE does the job),
     * be we leave this legacy code unchanged
     * for sure - D.B.
     */
    int c;
    fxStr garbage;

    for (;;) {
	c = getModemChar(0);
	if (c == 0xff || c == EOF)
	    break;
	if (!isSSLFax && useV34 && c == DLE) {
	    c = getModemChar(0);
	    switch (c) {
		case EOT:
		    protoTrace("EOT received (end of transmission)");
		    gotEOT = true;
		    recvdDCN = true;
		    return (false);
		case 0x69:
		    protoTrace("Control channel retrain");
		    // wait for the control channel to reappear
		    // should we reset the timeout setting?
		    waitForDCEChannel(true);
		    gotRTNC = true;
		    return (false);
		default:
		    // unexpected <DLE><command>, deem as garbage
		    garbage.append(DLE);
		    break;
	    }
	}
	garbage.append(c);
	if ( garbage.length() >= 2 && garbage.tail(2) == "\r\n") {
	    /*
	     * CR+LF received before address field.
	     * We expect result code (though it's possible
	     * that CR+LF is a part of garbage frame)
	     */
	    garbage = garbage.head(garbage.length() - 2);
	    break;
	}
    }

    if (getHDLCTracing() && garbage.length()) {
	fxStr buf;
	u_int j = 0;
	for (u_int i = 0; i < garbage.length(); i++) {
	    if (j > 0)
		buf.append(' ');
	    buf.append(fxStr(garbage[i] & 0xFF, "%2.2X"));
	    j++;
	    if (j > 19) {
		protoTrace("--> [%u:%.*s]",
		    j, buf.length(), (const char*) buf);
		buf = "";
		j = 0;
	    }
	}
	if (j)
	    protoTrace("--> [%u:%.*s]",
		j, buf.length(), (const char*) buf);
    }

    if (c == 0xff) {			// address field received
	do {
	    if (c == DLE) {
		c = getModemChar(0);
		if (c == ETX || c == EOF)
		    break;
		if (!isSSLFax && useV34) {
		    /*
		     * T.31-A1 Table B.1
		     * These indicate transparancy, shielding, or delimiting.
		     */
		    if (c == 0x07) {	// end of HDLC frame w/FCS error
			break;
		    }
		    switch (c) {
			case EOT:
			    protoTrace("EOT received (end of transmission)");
			    gotEOT = true;
			    recvdDCN = true;
			    return (false);
			case DLE:	// <DLE><DLE> => <DLE>
			    break;
			case SUB:	// <DLE><SUB> => <DLE><DLE>
			    frame.put(frameRev[DLE]);
			    break;
			case 0x51:	// <DLE><0x51> => <DC1>
			    c = DC1;
			    break;
			case 0x53:	// <DLE><0x53> => <DC3>
			    c = 0x13;
			    break;
		    }
		} else {
		    /*
		     * Some modems may not double-up the in-data DLEs like they should.
		     */
		    if (conf.class1ModemHasDLEBug) frame.put(frameRev[DLE]);
		    else if (c != DLE) {
			protoTrace("Odd.  Modem reported meaningless <DLE><0x%X>.  Possible DLE bug indication.", c);
		    }
		}
	    }
	    frame.put(frameRev[c]);
	} while ((c = getModemChar(0)) != EOF);
    }
    stopTimeout("receiving HDLC frame data");
    if (wasSSLFax) setTimeout(true);
    if (frame.getLength() > 0)
	traceHDLCFrame("-->", frame);
    if (wasTimeout()) {
	abortReceive();
	return (false);
    }
    /*
     * Now collect the "OK", "ERROR", or "FCERROR"
     * response telling whether or not the FCS was
     * legitimate.
     */
    if (!isSSLFax && !useV34 && !waitFor(AT_OK)) {
	if (lastResponse == AT_ERROR)
	    protoTrace("FCS error");
	return (false);
    }
    if (!isSSLFax && useV34 && c == 0x07) {
	protoTrace("FCS error");
	return (false);
    }
    if (frame.getFrameDataLength() < 1) {
	protoTrace("HDLC frame too short (%u bytes)", frame.getLength());
	return (false);
    }
    if ((isSSLFax || conf.class1ValidateV21Frames) && !frame.checkCRC()) {
	protoTrace("FCS error (calculated)");
	return (false);
    }
    if ((frame[1]&0xf7) != 0xc0) {
	protoTrace("HDLC frame with bad control field %#x", frame[1]);
	if (frame[1] == 0xff) {
	    /*
	     * The frame passed CRC checking, so it's very likely not corrupt data.
	     * Try to cope with senders that include superfluous 0xff and 0x00 bytes
	     * between the flag and control fields.  They seem to always start with
	     * an initial extra 0xff.
	     */
	    u_short i = 2;
	    while (i < frame.getLength() && (frame[i] == 0xff || frame[i] == 0x00)) i++;
	    if ((frame[i]&0xf7) == 0xc0) {
		protoTrace("Trying to cope with poor HDLC frame formatting.");
		HDLCFrame frametmp(conf.class1FrameOverhead);
		frametmp.put(0xff);
		while (i < frame.getLength()) frametmp.put(frame[i++]);
		frame.reset();
		i = 0;
		while (i < frametmp.getLength()) frame.put(frametmp[i++]);
		// The CRC will be bad for the reconstructed frame, but that shouldn't matter now.
	    } else {
		return (false);
	    }
	} else {
	    return (false);
	}
    }
    frameRcvd = "";
    for (u_int i = 0; i < frame.getLength(); i++) frameRcvd.append(frame[i]);
    frame.setOK(true);
    return (true);
}

bool
Class1Modem::syncECMFrame()
{
    /*
     * We explicitly look for the first sync flag so as to
     * not get confused by any initial garbage or training.
     */
    int bit = 0;
    u_short ones = 0;

    // look for the first sync flag

    time_t start = Sys::now();
    startTimeout(60000);	// despite indications by T.4 A.3.1
    do {
	if ((unsigned) Sys::now()-start >= 60) {
	    protoTrace("Timeout awaiting synchronization sequence");
	    setTimeout(true);
	    return (false);
	}
	bit = getModemBit(0);
    } while (bit != EOF && bit != 0 && !didBlockEnd());
    if (bit != EOF && !didBlockEnd()) {
	do {
	    if ((unsigned) Sys::now()-start >= 60) {
		protoTrace("Timeout awaiting synchronization sequence");
		setTimeout(true);
		return (false);
	    }
	    if (bit == 0 || ones > 0xFF) ones = 0;
	    bit = getModemBit(0);
	    if (bit == EOF) break;
	    if (bit == 1) ones++;
	} while (!(ones == 6 && bit == 0) && !didBlockEnd());
    }
    stopTimeout("awaiting synchronization sequence");
    if (bit == EOF || didBlockEnd()) setTimeout(true);
    if (wasTimeout()) {
	return (false);
    }
    return (true);
}

/*
 * Receive an HDLC frame with ECM.  We must always be cautious
 * of "stuffed" zero-bits after five sequential one-bits between
 * flag sequences.  We assume this is called only after a
 * successfuly received complete flag sequence.
 *
 * Things are significantly more simple with V.34-fax ECM since
 * there is no synchronization or RCP frames.  In this case we
 * simply have to handle byte transparancy and shielding, looking
 * for byte-aligned frame delimiters.
 */
bool
Class1Modem::recvECMFrame(HDLCFrame& frame)
{
    fakedRCP = false;
    if (!isSSLFax && useV34) {
	int c;
	for (;;) {
	    c = getModemChar(60000);
	    if (wasTimeout()) {
		return (false);
	    }
	    if (c == DLE) {
		c = getModemChar(60000);
		if (wasTimeout()) {
		    return (false);
		}
		switch (c) {
		    case DLE:
			break;
		    case SUB:
			frame.put(frameRev[DLE]);
			break;
		    case 0x51:
			c = 0x11;
			break;
		    case 0x53:
			c = 0x13;
			break;
		    case ETX:
			if (frame.getLength() > 0)
			    traceHDLCFrame("-->", frame, true);
			if (frame.getLength() < 5) {		// RCP frame size
			    protoTrace("HDLC frame too short (%u bytes)", frame.getLength());
			    return (false);
			}
			if (frame[0] != 0xff) {
			    protoTrace("HDLC frame with bad address field %#x", frame[0]);
			    return (false);
			}
			if ((frame[1]&0xf7) != 0xc0) {
			    protoTrace("HDLC frame with bad control field %#x", frame[1]);
			    return (false);
			}
			return (true);
		    case 0x07:
			protoTrace("FCS error");
			return (false);
		    case 0x04:
			protoTrace("EOT received (end of transmission)");
			gotEOT = true;
			recvdDCN = true;
			return (false);
		    case 0x6D:
			protoTrace("Control channel selected");
			gotCTRL = true;
			return (false);
		    default:
			protoTrace("got <DLE><%X>", c);
			break;
		}
	    }
	    frame.put(frameRev[c]);
	}
    }

    int bit = getModemBit(60000);
    u_short ones = 0;

    // look for the last sync flag (possibly the previous one)

    // some senders use this as the time to do framing so we must wait longer than T.4 A.3.1 implies
    time_t start = Sys::now();
    while (bit != 1 && bit != EOF && !didBlockEnd()) {	// flag begins with zero, address begins with one
	do {
	    if ((unsigned) Sys::now()-start >= 300) {	// 5 minutes of synchronization is too much
		protoTrace("Timeout waiting for the last synchronization flag");
		return false;
	    }
	    if (bit == 0 || ones > 6) ones = 0;
	    bit = getModemBit(60000);
	    if (bit == 1) ones++;
	} while (!(ones == 6 && bit == 0 && bit != EOF) && !didBlockEnd());
	ones = 0;
	bit = getModemBit(60000);
    }

    // receive the frame, strip stuffed zero-bits, and look for end flag

    ones = 1;
    u_short bitpos = 7;
    u_int byte = (bit << bitpos);
    bool rcpframe = false;
    start = Sys::now();
    do {
	if ((unsigned) Sys::now()-start >= 30) {
	    protoTrace("Timeout receiving HDLC frame");
	    return (false);
	}
	bit = getModemBit(60000);
	if (bit == 1) {
	    ones++;
	}
	if (!(ones == 5 && bit == 0 && bit != EOF)) {	// not transparent stuffed zero-bits
	    bitpos--;
	    byte |= (bit << bitpos);
	    if (bitpos == 0) {			// fully populated byte
		frame.put(byte);
		bitpos = 8;
		byte = 0;
	    }
	}
	if (bit == 0) ones = 0;
	// don't wait for the terminating flag on an RCP frame
	if (frame[0] == 0xff && frame[1] == 0xc0 && frame[2] == FCF_RCP && frame.getLength() == 5 && frame.checkCRC()) {
	    protoTrace("RECV received RCP frame");
	    rcpframe = true;
	} else if (didBlockEnd()) {
	    // sometimes RCP frames are truncated by or are missing due to premature DLE+ETX characters, sadly
	    protoTrace("RECV assumed RCP frame with block end");
	    // force-feed a valid RCP frame with FCS
	    frame.reset();
	    frame.put(0xff); frame.put(0xc0); frame.put(0x61); frame.put(0x96); frame.put(0xd3);
	    rcpframe = true;
	    fakedRCP = true;
	}
    } while (ones != 6 && bit != EOF && !rcpframe && frame.getLength() < frameSize+6);
    if (ones == 6) bit = getModemBit(60000);			// trailing bit on flag
    if (!rcpframe && frame.getLength() < frameSize+6) {
	/*
	 * The HDLC frame was terminated early by a flag.  T.30 A.3.5 states that
	 * frame size cannot change during one page, and T.4 A.3.6.2 seems to provide
	 * for padding in order to get that last frame on a block to always line up
	 * on a byte and frame boundary.  However, the NOTE 2 there seemse to give
	 * leniency to that requirement, and in fact many senders will send short
	 * frames on the last frame of a block.  So we run a couple of additional
	 * checks here (in addition to FCS checking) to limit the remote chance
	 * of FCS actually checking out on corrupt data (although that may be very
	 * remote indeed).  We don't do these "trailing flag" tests on normal-sized
	 * frames because we deliberately don't look for a trailing flag when we
	 * get enough data.
	 */
	if (bit) {				// should have been zero
	    protoTrace("Bad HDLC terminating flag received.");
	    return (false);
	}
	if (byte != 0x7e) {			// trailing byte should be flag
	    protoTrace("HDLC frame not byte-oriented.  Trailing byte: %#x", byte);
	    return (false);
	}
    }
    traceHDLCFrame("-->", frame, true);
    if (bit == EOF) {
	protoTrace("EOF received.");
	return (false);
    }
    if (frame.getLength() < 5) {		// RCP frame size
	protoTrace("HDLC frame too short (%u bytes)", frame.getLength());
	return (false);
    }
    if (frame[0] != 0xff) {
	protoTrace("HDLC frame with bad address field %#x", frame[0]);
	return (false);
    }
    if ((frame[1]&0xf7) != 0xc0) {
	if (frame[1] == 0xff) {
	    protoTrace("HDLC frame with bad control field %#x, frame size: %d", frame[1], frame.getLength());
	} else if (frame[1] == 0x03 && frame.getLength() == 5 && frame[2] == 0x86) {
	    protoTrace("RECV received RCP frame (reversed)");
	    // Correct the reversed RCP frame
	    frame.reset();
	    frame.put(0xff); frame.put(0xc0); frame.put(0x61); frame.put(0x96); frame.put(0xd3);
	    return (true);
	} else {
	    protoTrace("HDLC frame with bad control field %#x", frame[1]);
	}
	return (false);
    }
    return (true);
}

bool
Class1Modem::endECMBlock()
{
    if (didBlockEnd()) return (true);	// some erroniously re-use bytes

    int c = getLastByte();		// some erroniously re-use bits	
    time_t start = Sys::now();
    setTimeout(false);
    do {
	if ((unsigned) Sys::now()-start >= 60) {	// 60 seconds of garbage after RCP is too much
	    protoTrace("Timeout waiting for DLE+ETX");
	    setTimeout(true);
	    return false;
	}
	if (c == DLE) {
	    c = getModemChar(30*1000);
	    if (c == ETX || c == EOF)
		break;
	}
    } while ((c = getModemChar(30*1000)) != EOF);
    if (c == EOF) return (false);
    else return (true);
}

#include "StackBuffer.h"

/*
 * Log an HLDC frame along with a time stamp (secs.10ms).
 */
void
Class1Modem::traceHDLCFrame(const char* direction, const HDLCFrame& frame, bool isecm)
{
    if (!isecm) {
	if (!getHDLCTracing()) return;
    } else {
	if (!getECMTracing()) return;
    }
    const char* hexdigits = "0123456789ABCDEF";
    fxStackBuffer buf;
    for (u_int i = 0; i < frame.getLength(); i++) {
	u_char b = frame[i];
	if (i > 0)
	    buf.put(' ');
	buf.put(hexdigits[b>>4]);
	buf.put(hexdigits[b&0xf]);
    }
    protoTrace("%s HDLC<%u:%.*s>", direction,
	frame.getLength(), buf.getLength(), (const char*) buf);
}

/*
 * Send a raw HDLC frame and wait for the modem response.
 * The modem is expected to be an HDLC frame sending mode
 * (i.e. +FTH=3 or similar has already be sent to the modem).
 * The T.30 max frame length is enforced with a 3 second
 * timeout on the send.
 */
bool
Class1Modem::sendRawFrame(HDLCFrame& frame)
{
    if (isSSLFax) {
	// We must generate FCS for SSL Fax; the modem firmware does it for us using modems.
	int fcs1 = (frame.getCRC() >> 8) & 0xff;	// 1st byte FCS
	int fcs2 = frame.getCRC() & 0xff;		// 2nd byte FCS
	frame.put(fcs1); frame.put(fcs2);
    }
    traceHDLCFrame("<--", frame);
    if (frame.getLength() < 3) {
	protoTrace("HDLC frame too short (%u bytes)", frame.getLength());
	return (false);
    }
    if (frame[0] != 0xff) {
	protoTrace("HDLC frame with bad address field %#x", frame[0]);
	return (false);
    }
    if ((frame[1]&0xf7) != 0xc0) {
	protoTrace("HDLC frame with bad control field %#x", frame[1]);
	return (false);
    }
    if (frame.getFCF() != FCF_CRP) {
	// We need to remember our last signal sent in order to respond properly to a CRP
	// signal from the other end or to respond properly if it appears that the other end
	// missed our last signal and we need to repeat it.  However, we don't ever want
	// to remember CRP, but, instead, remember the signal we sent prior to CRP.
	signalSent = "";
	for (u_int i = 0; i < frame.getLength(); i++) signalSent.append(frame[i]);
    }
    /*
     * sendFrame() is always called with a timeout set.
     * Let's keep it that way
     */
    return (sendClass1Data(frame, frame.getLength(), frameRev, true, 0) &&
	((useV34 || isSSLFax) ? true : waitFor(frame.moreFrames() ? AT_CONNECT : AT_OK, 0)));
}

/*
 * Send a single byte frame.
 */
bool
Class1Modem::sendFrame(u_char fcf, bool lastFrame)
{
    HDLCFrame frame(conf.class1FrameOverhead);
    frame.put(0xff);
    frame.put(lastFrame ? 0xc8 : 0xc0);
    frame.put(fcf);
    return (sendRawFrame(frame));
}

/*
 * Send a frame with DCS/DIS.
 */
bool
Class1Modem::sendFrame(u_char fcf, FaxParams& dcs_caps, bool lastFrame)
{
    HDLCFrame frame(conf.class1FrameOverhead);
    frame.put(0xff);
    frame.put(lastFrame ? 0xc8 : 0xc0);
    frame.put(fcf);

    int curByte = 0;
    //add dcs bytes an any optional bytes
    frame.put(dcs_caps.getByte(curByte));
    while (dcs_caps.hasNextByte(curByte)) {
        curByte += 1;
        frame.put(dcs_caps.getByte(curByte));
    }

    return (sendRawFrame(frame));
}

/*
 * Send a frame with TSI/CSI/PWD/SUB/SEP/PPR.
 */
bool
Class1Modem::sendFrame(u_char fcf, const fxStr& tsi, bool lastFrame)
{
    HDLCFrame frame(conf.class1FrameOverhead);
    frame.put(0xff);
    frame.put(lastFrame ? 0xc8 : 0xc0);
    frame.put(fcf);
    frame.put((const u_char*)(const char*)tsi, tsi.length());
    return (sendRawFrame(frame));
}

/*
 * Send a frame with NSF.
 */
bool
Class1Modem::sendFrame(u_char fcf, const u_char* code, const u_char* features, const fxStr& nsf, bool lastFrame)
{
    HDLCFrame frame(conf.class1FrameOverhead);
    frame.put(0xff);
    frame.put(lastFrame ? 0xc8 : 0xc0);
    frame.put(fcf);
    frame.put(code, 3);		// should be in LSBMSB already
    frame.put(features, 1);
    frame.put((const u_char*)(const char*)nsf, nsf.length());
    return (sendRawFrame(frame));
}

bool
Class1Modem::transmitFrame(fxStr& signal)
{
    HDLCFrame frame(conf.class1FrameOverhead);
    for (u_int i = 0; i < signal.length(); i++) frame.put(signal[i]);
    startTimeout(7550);
    bool frameSent = (useV34 || isSSLFax) ? true : atCmd(thCmd, AT_NOTHING, 0);
    if (frameSent) frameSent = (useV34 || isSSLFax) ? true : atResponse(rbuf, 0) == AT_CONNECT;
    if (frameSent) frameSent = sendRawFrame(frame);
    else if (lastResponse == AT_ERROR) gotEOT = true;	// on hook
    stopTimeout("sending HDLC frame");
    return (frameSent);
}

bool
Class1Modem::transmitFrame(u_char fcf, bool lastFrame)
{
    startTimeout(7550);
    bool frameSent = (useV34 || isSSLFax) ? true : atCmd(thCmd, AT_NOTHING, 0);
    if (frameSent) frameSent = (useV34 || isSSLFax) ? true : atResponse(rbuf, 0) == AT_CONNECT;
    if (frameSent) frameSent = sendFrame(fcf, lastFrame);
    else if (lastResponse == AT_ERROR) gotEOT = true;	// on hook
    stopTimeout("sending HDLC frame");
    return (frameSent);
}

bool
Class1Modem::transmitFrame(u_char fcf, FaxParams& dcs_caps, bool lastFrame)
{
    /*
     * The T.30 spec says no frame can take more than 3 seconds
     * (+/- 15%) to transmit.  But the DCE can take as much as 5
     * seconds to respond CONNECT or result OK, per T.31.
     */
    startTimeout(7550);
    bool frameSent = (useV34 || isSSLFax) ? true : atCmd(thCmd, AT_NOTHING, 0);
    if (frameSent) frameSent = (useV34 || isSSLFax) ? true : atResponse(rbuf, 0) == AT_CONNECT;
    if (frameSent) frameSent = sendFrame(fcf, dcs_caps, lastFrame);
    else if (lastResponse == AT_ERROR) gotEOT = true;	// on hook
    stopTimeout("sending HDLC frame");
    return (frameSent);
}

bool
Class1Modem::transmitFrame(u_char fcf, const fxStr& tsi, bool lastFrame)
{
    startTimeout(7550);
    bool frameSent = (useV34 || isSSLFax) ? true : atCmd(thCmd, AT_NOTHING, 0);
    if (frameSent) frameSent = (useV34 || isSSLFax) ? true : atResponse(rbuf, 0) == AT_CONNECT;
    if (frameSent) frameSent = sendFrame(fcf, tsi, lastFrame);
    else if (lastResponse == AT_ERROR) gotEOT = true;	// on hook
    stopTimeout("sending HDLC frame");
    return (frameSent);
}

bool
Class1Modem::transmitFrame(u_char fcf, const u_char* code, const u_char* features, const fxStr& nsf, bool lastFrame)
{
    startTimeout(7550);
    bool frameSent = (useV34 || isSSLFax) ? true : atCmd(thCmd, AT_NOTHING, 0);
    if (frameSent) frameSent = (useV34 || isSSLFax) ? true : atResponse(rbuf, 0) == AT_CONNECT;
    if (frameSent) frameSent = sendFrame(fcf, code, features, nsf, lastFrame);
    else if (lastResponse == AT_ERROR) gotEOT = true;	// on hook
    stopTimeout("sending HDLC frame");
    return (frameSent);
}

/*
 * Send data using the specified signalling rate.
 */
bool
Class1Modem::transmitData(int br, u_char* data, u_int cc,
    const u_char* bitrev, bool eod)
{
    if (flowControl == FLOW_XONXOFF)
	setXONXOFF(FLOW_XONXOFF, FLOW_NONE, ACT_FLUSH);
    fxStr tmCmd(br, tmCmdFmt);
    bool ok = atCmd(tmCmd, AT_CONNECT);
    if (ok) {

	// T.31 8.3.3 requires the DCE to report CONNECT at the beginning
	// of transmission of the training pattern rather than at the end.
	// We pause here to allow the remote's +FRM to result in CONNECT.
	// This delay will vary depending on the modem's adherence to T.31.
	pause(conf.class1TMConnectDelay);

	ok = sendClass1Data(data, cc, bitrev, eod, getDataTimeout());
	if (ok && eod) {
	    ok = false;
	    u_short attempts = 0;
	    lastResponse = AT_NOTHING;
	    while (!isSSLFax && !ok && lastResponse != AT_NOCARRIER && lastResponse != AT_FRH3 && attempts++ < 3) {
		ok = waitFor(AT_OK, 60*1000);	// wait up to 60 seconds for "OK"
	    }
	}
    } else if (lastResponse == AT_ERROR) gotEOT = true;	// on hook
    if (flowControl == FLOW_XONXOFF)
	setXONXOFF(FLOW_NONE, FLOW_NONE, ACT_DRAIN);
    return (ok);
}

/*
 * Wrapper for recvFrame.
 */
bool
Class1Modem::recvFrame(HDLCFrame& frame, u_char dir, long ms, bool readPending, bool docrp, bool usehooksensitivity, u_int echofcf, bool docng, bool handlestutter, u_int expectfcf)
{
    bool fr = recvFrame2(frame, dir, ms, readPending, docrp, usehooksensitivity, echofcf, docng, handlestutter, expectfcf);
    if (!docrp && !fr && !wasTimeout() && lastResponse == AT_OK && !frame.getLength()) {
	// Cope with broken behavior where they send somehing short, stop, and then send the actual signal.
	fr = recvFrame2(frame, dir, ms, readPending, docrp, usehooksensitivity, echofcf, docng, handlestutter, expectfcf);
    }
    return (fr);
}

/*
 * Receive an HDLC frame.  The timeout is against
 * the receipt of the HDLC flags; the frame itself must
 * be received within 3 seconds (per the spec).
 * If a carrier is received, but the complete frame
 * is not received before the timeout, the receive
 * is aborted.
 *
 * If the frame is received in error, then in instances
 * where the other end is likely to be able to receive
 * it, we can transmit CRP to get the other end to
 * retransmit the frame.
 */
bool
Class1Modem::recvFrame2(HDLCFrame& frame, u_char dir, long ms, bool readPending, bool docrp, bool usehooksensitivity, u_int echofcf, bool docng, bool handlestutter, u_int expectfcf)
{
    bool gotframe;
    u_short crpcnt = 0, rhcnt = 0;
    u_int onhooks = 0;
    gotCONNECT = true;
    time_t start = Sys::now();
    if (isSSLFax || useV34) {
	do {
	    if (crpcnt) traceFCF(dir == FCF_SNDR ? "SEND send" : "RECV send", FCF_CRP);
	    frame.reset();
	    gotframe = recvRawFrame(frame);
	} while (!gotframe && !gotRTNC && !gotEOT && docrp && crpcnt++ < 3 && !wasTimeout() && frame.getLength() && transmitFrame(dir|FCF_CRP));
	return (gotframe);
    }
    startTimeout(ms);
    if (!readPending) {
	/*
	 * Hopefully the modem is smart enough to *not* do +FCERROR after +FRH=3,
	 * as it is only a stumbling-block for us and cannot be beneficial.  But,
	 * in case it adheres blindly to the spec, we'll repeat ourselves here
	 * until we timeout or we do get the V.21 carrier.  We do slow the looping
	 * with a pause to prevent unwanted massive amounts of tracing.  The pause 
	 * needs to be short enough, though, that the modem will still pick up
	 * any V.21 signalling if it misses that much of the startup.
	 *
	 * T.31 8.3.6 states that the NO CARRIER result to AT+FRH=3 indicates 
	 * carrier loss.  Getting this result without first getting a CONNECT 
	 * response (indicating carrier detection) seems a bit undefined.  However,
	 * sample sessions T.31 I.1 and I.2 seem to give its use as a means to
	 * deliberately detect carrier loss since the OK result to AT+FRH=3 does
	 * not necessarily indicate carrier loss (even on final frames - although
	 * smart modem developers would make it thus).  Thus could serve a purpose
	 * similar to AT+FRS=1 as we haved used in Class1TCFRecvHackCmd (which is
	 * generally a bad idea).  In any case, if we're trying to detect a V.21
	 * HDLC frame and we get a NO CARRIER result immediately following AT+FRH=3
	 * it probably means that the previous carrier was still up, that we just
	 * saw its loss, and so we just need to re-issue AT+FRH=3.  See more below...
	 */
	do {
	    if (docng) {
		// Transmit CNG while listening for V.21 HDLC - e.g. after a procedure interrupt
		readPending = atCmd(conf.CNGCmd, AT_NOTHING, 0) && waitFor(AT_CONNECT, 0);
		docng = false;
	    } else {
		readPending = atCmd(rhCmd, AT_NOTHING, 0) && waitFor(AT_CONNECT, 0);
	    }
	    if (lastResponse == AT_FCERROR) pause(200);
	    if (lastResponse == AT_ERROR && !wasTimeout() && ++onhooks <= conf.class1HookSensitivity) {
		if (!usehooksensitivity) {
		    /*
		     * Class1HookSensitivity is used by the calling function, not us.
		     */
		    break;
		} else {
		    stopTimeout("");
		    startTimeout(ms);
		}
	    }
	} while (((u_int) Sys::now()-start < howmany(conf.t1Timer, 1000)) && !wasTimeout() &&
	    (lastResponse == AT_FCERROR || lastResponse == AT_NOCARRIER || lastResponse == AT_OK || (lastResponse == AT_ERROR && onhooks <= conf.class1HookSensitivity)));
    }
    if (readPending) {
        stopTimeout("waiting for HDLC flags");
        if (wasTimeout()){
            abortReceive();
            return (false);
        }
	fxStr emsg;
	bool gotecho = false;
	do {
	    if (crpcnt || rhcnt || gotecho) {
		if (rhcnt) crpcnt = 0;
		if (gotecho) {
		    gotecho = false; // reset
		} else if (crpcnt) {
		    rhcnt = 0;
		    traceFCF(dir == FCF_SNDR ? "SEND send" : "RECV send", FCF_CRP);
		}
		do {
		    lastResponse = AT_NOTHING;
		    startTimeout(ms);
		    if (!(atCmd(rhCmd, AT_NOTHING, 0) && waitFor(AT_CONNECT,0))) {
			stopTimeout("waiting for v.21 carrier");
			if (wasTimeout()) abortReceive();
			if (lastResponse == AT_ERROR) gotEOT = true;
			if (lastResponse != AT_NOCARRIER || rhcnt++ > 30) return (false);
		    }
		    stopTimeout("waiting for v.21 carrier");
		    /*
		     * The meaning of a direct NO CARRIER response/result to the AT+FRH=3 
		     * command can be interpreted two ways due to wording in ITU T.31.
		     * The more traditional interpretation is that it is out-of-spec: that
		     * a NO CARRIER response should only come after a CONNECT message.
		     * However, T.31 gives sample sessions that illustrate precisely this 
		     * kind of out-of-spec sequence:
		     *
		     * <-- AT+FRH=3
		     * --> NO CARRIER
		     *
		     * And the interpretation is clear to indicate that in this case the
		     * modem detected a carrier, but it apparently was not V.21 HDLC, and
		     * the NO CARRIER response indicates that the indeterminate carrier
		     * has dropped.  This functionality is more cumbersome than it is
		     * helpful (its value is not much different than that of an AT+FRS=1 
		     * command), and what it really means is that we just need to re-issue 
		     * the AT+FRH=3 command until we get something useful as this scenario
		     * will most likely occur on incidence of Phase C premature carrier loss.
		     */
		} while (lastResponse == AT_NOCARRIER);
	    }
	    frame.reset();
            gotframe = recvRawFrame(frame);
	    if ((echofcf && gotframe && frame.getFCF() == echofcf) || (docrp && crpcnt && crpcnt < 3 && frame.getRawFCF() == (FCF_CRP|dir))) {
		traceFCF(dir == FCF_SNDR ? "SEND echo" : "RECV echo", frame.getFCF());
		gotecho = true;
		gotframe = false;
		echofcf = 0;	// only expect one possible echo
	    }
	    if (!gotframe && (expectfcf == FCF_TSI || expectfcf == FCF_CSI)) {
		/*
		 * If the remote system's TSI+DCS or CSI+DIS signal is broken early enough in the
		 * signal, then it may be possible to restart our frame reception and pick up DCS
		 * or DIS which is acceptable because it's all that we really need to proceed.
		 */
		expectfcf = 0;
		gotecho = true;	// it wasn't echo, but we want the same response
	    }
	    /*
	     * Some modems aren't very particular about reporting CONNECT after AT+FRH=3.
	     * So, for such modems CONNECT may come with V.17 modulated audio or any noise
	     * whatsoever.  Those modems tend to report NO CARRIER after CONNECT, perhaps
	     * after <DLE><ETX> or some other garbage possibly.  NO CARRIER is an invalid
	     * modem response at that point, anyway.  So it is a good indicator for us.
	     * So we simply repeat AT+FRH=3 in that case.
	     */
	} while (((u_int) Sys::now()-start < howmany(conf.t1Timer, 1000)) && !gotframe && !wasTimeout() && 
	    (((conf.class1HasRHConnectBug || handlestutter) && !frame.getLength() && lastResponse == AT_NOCARRIER && rhcnt++ < 30) ||
	    gotecho ||
	    (docrp && crpcnt++ < 3 && switchingPause(emsg, 3) && transmitFrame(dir|FCF_CRP))));	/* triple switchingPause to avoid sending CRP during TCF */
	return (gotframe);
    } else {
	gotCONNECT = false;
	if (lastResponse == AT_ERROR) gotEOT = true;		// on hook
    }
    stopTimeout("waiting for v.21 carrier");
    if (wasTimeout()) abortReceive();
    return (false);
}

/*
 * Receive TCF data using the specified signalling rate.
 */
bool
Class1Modem::recvTCF(int br, HDLCFrame& buf, const u_char* bitrev, long ms)
{
    buf.reset();
    if (flowControl == FLOW_XONXOFF)
	setXONXOFF(FLOW_NONE, FLOW_XONXOFF, ACT_DRAIN);
    /*
     * Loop waiting for carrier or timeout.
     */
    bool readPending, gotCarrier;
    fxStr rmCmd(br, rmCmdFmt);
    startTimeout(ms);
    u_short attempts = 0;
    do {
	readPending = atCmd(rmCmd, AT_NOTHING, 0);
	gotCarrier = readPending && waitFor(AT_CONNECT, 0);
    } while (readPending && !gotCarrier && lastResponse == AT_FCERROR && ++attempts < conf.class1RMPersistence);
    /*
     * If carrier was recognized, collect the data.
     */
    bool gotData = false;
    if (gotCarrier) {
	int c = getModemChar(0);		// NB: timeout is to first byte
	stopTimeout("receiving TCF");
	if (c != EOF) {
	    buf.reset();
	    /*
	     * Use a  timer to receive the 1.5
	     * second TCF--perhaps if it is too long it
	     * won't permit us to send the nak in time?
	     *
	     * The initial timer needs to be longer than 1.5 seconds
	     * to support senders that may not start the zeros
	     * until a second or two after CONNECT.  This is
	     * also why we restart our timeout after the zeros
	     * start.
	     */
	    bool zerosstarted = false;
	    startTimeout(ms);
	    do {
		if (c == 0x00 && !zerosstarted) {
		    zerosstarted = true;
		    startTimeout(ms);
		}
		if (c == DLE) {
		    c = getModemChar(0);
		    if (c == ETX) {
			gotData = true;
			break;
		    }
		    if (c == EOF) {
			break;
		    }
		}
		buf.put(bitrev[c]);
		if (buf.getLength() > 10000) {
		    setTimeout(true);
		    break;
		}
	    } while ((c = getModemChar(0)) != EOF);
	}
    }
    stopTimeout("receiving TCF");
    /*
     * If the +FRM is still pending, abort it.
     */
    if (readPending && wasTimeout())
	abortReceive();
    if (flowControl == FLOW_XONXOFF)
	setXONXOFF(FLOW_NONE, FLOW_NONE, ACT_DRAIN);
    return (gotData);
}

/* 
 * Modem manipulation support.
 */

/*
 * Reset a Class 1 modem.
 */
bool
Class1Modem::reset(long ms)
{
    return (FaxModem::reset(ms) && setupClass1Parameters());
}

ATResponse
Class1Modem::atResponse(char* buf, long ms)
{
    if (FaxModem::atResponse(buf, ms) == AT_OTHER && strneq(buf, "+FCERROR", 8))
	lastResponse = AT_FCERROR;
    if (lastResponse == AT_OTHER && strneq(buf, "+FRH:3", 6))
	lastResponse = AT_FRH3;
    if (lastResponse == AT_OTHER && strneq(buf, "+FRCNG", 6))
	lastResponse = AT_FRCNG;
    if (lastResponse == AT_OTHER && strneq(buf, "+F34:", 5)) {
	/*
	 * V.8 handshaking was successful.  The rest of the
	 * session is governed by T.31 Amendment 1 Annex B.
	 * (This should only happen after ATA or ATD.)
	 *
	 * The +F34: response is interpreted according to T.31-A1 B.6.2.
	 */
	buf += 5;					// skip "+F34:" prefix
	primaryV34Rate = 0;
	while (!isdigit(buf[0])) buf++;		// move to digits
	do {
	    primaryV34Rate = primaryV34Rate*10 + (buf[0] - '0');
        } while (isdigit((++buf)[0]));
	controlV34Rate = 0;
	while (!isdigit(buf[0])) buf++;		// move to digits
	do {
	    controlV34Rate = controlV34Rate*10 + (buf[0] - '0');
        } while (isdigit((++buf)[0]));
	useV34 = true;
	protoTrace("V.8 handshaking succeeded, V.34-Fax (SuperG3) capability enabled.");
	protoTrace("Primary channel rate: %u bit/s, Control channel rate: %u bit/s.", primaryV34Rate*2400, controlV34Rate*1200);
	modemParams.br |= BIT(primaryV34Rate) - 1;
    }
    return (lastResponse);
}

/*
 * Wait (carefully) for some response from the modem.
 */
bool
Class1Modem::waitFor(ATResponse wanted, long ms)
{
    for (;;) {
	ATResponse response = atResponse(rbuf, ms);
	if (response == wanted)
	    return (true);
	switch (response) {
	case AT_TIMEOUT:
	case AT_EMPTYLINE:
	case AT_ERROR:
	case AT_NOCARRIER:
	case AT_NODIALTONE:
	case AT_NOANSWER:
	case AT_OFFHOOK:
	case AT_RING:
	    modemTrace("MODEM %s", ATresponses[response]);
	    /* fall thru... */
	case AT_OTHER:
	case AT_FCERROR:
	case AT_FRH3:
	case AT_FRCNG:
	case AT_OK:
	case AT_CONNECT:
	    return (false);
	}
    }
}

/*
 * Send queryCmd and get a range response.
 */
bool
Class1Modem::class1Query(const fxStr& queryCmd, Class1Cap caps[])
{
    char response[1024];
    if (queryCmd[0] == '!') {
	return (parseQuery(queryCmd.tail(queryCmd.length()-1), caps));
    }
    if (atCmd(queryCmd, AT_NOTHING) && atResponse(response) == AT_OTHER) {
	sync(5000);
	return (parseQuery(response, caps));
    }
    return (false);
}

/*
 * Map the DCS signalling rate to the appropriate
 * Class 1 capability entry.
 */
const Class1Cap*
Class1Modem::findSRCapability(u_short sr, const Class1Cap caps[])
{
    for (u_int i = NCAPS-1; i > 0; i--) {
	const Class1Cap* cap = &caps[i];
	if (cap->sr == sr) {
	    if (cap->mod == V17 && HasShortTraining(cap-1))
		cap--;
	    return (cap);
	}
    }
    // XXX should not happen...
    protoTrace("MODEM: unknown signalling rate %#x, using 9600 v.29", sr);
    return findSRCapability(DCSSIGRATE_9600V29, caps);
}

/*
 * Map the Class 2 bit rate to the best
 * signalling rate capability of the modem.
 */
const Class1Cap*
Class1Modem::findBRCapability(u_short br, const Class1Cap caps[])
{
    for (u_int i = NCAPS-1; i > 0; i--) {
	const Class1Cap* cap = &caps[i];
	if (cap->br == br && cap->ok) {
	    if (cap->mod == V17 && HasShortTraining(cap-1))
		cap--;
	    return (cap);
	}
    }
    protoTrace("MODEM: unsupported baud rate %#x", br);
    return NULL;
}

/*
 * Build upon the result from Class2Params.
 */
FaxParams
Class1Modem::modemDIS() const
{
    FaxParams dis_caps = FaxModem::modemDIS();

    // signalling rates
    for (u_short i = 0; i < 4; i++) dis_caps.setBit(11+i, (discap & (0x08>>i)));
    if (useV34) dis_caps.setBit(FaxParams::BITNUM_V8_CAPABLE, true);

    // internet fax (support for CIA, CSA, and TSA frames)
    if (conf.class1SSLFaxSupport) {
	dis_caps.setBit(FaxParams::BITNUM_T37, true);
	dis_caps.setBit(FaxParams::BITNUM_T38, true);
    }

    // preferred ECM frame size
    if (conf.class1ECMFrameSize == 64) dis_caps.setBit(FaxParams::BITNUM_FRAMESIZE_DIS, true);

    // we set both units preferences to allow the sender to choose
    dis_caps.setBit(FaxParams::BITNUM_METRIC_RES, true);
    dis_caps.setBit(FaxParams::BITNUM_INCH_RES, true);

    // we indicate both letter and legal page size support
    dis_caps.setBit(FaxParams::BITNUM_LETTER_SIZE, true);
    dis_caps.setBit(FaxParams::BITNUM_LEGAL_SIZE, true);

    // selective polling, subaddressing, password
    dis_caps.setBit(FaxParams::BITNUM_SEP, true);
    dis_caps.setBit(FaxParams::BITNUM_SUB, true);
    dis_caps.setBit(FaxParams::BITNUM_PWD, true);

    if (conf.class1ECMSupport) {
	// JBIG
	if (jbigSupported) {
	    dis_caps.setBit(FaxParams::BITNUM_JBIG_BASIC, true);
	    dis_caps.setBit(FaxParams::BITNUM_JBIG_L0, true);	// JBIG library can handle L0 = 1-Yd
	}
/* - disabled for now
	// JBIG grey/color requires JPEG grey/color
	if (conf.class1GreyJBIGSupport || conf.class1ColorJBIGSupport) {
	    dis_caps.setBit(FaxParams::BITNUM_JPEG, true);
	    dis_caps.setBit(FaxParams::BITNUM_JBIG, true);
	}
	if (conf.class1ColorJBIGSupport)
	    dis_caps.setBit(FaxParams::BITNUM_FULLCOLOR, true);
*/
	// JPEG
	if (conf.class1GreyJPEGSupport || conf.class1ColorJPEGSupport)
	    dis_caps.setBit(FaxParams::BITNUM_JPEG, true);
	if (conf.class1ColorJPEGSupport)
	    dis_caps.setBit(FaxParams::BITNUM_FULLCOLOR, true);
    }
    return dis_caps;
}

const char COMMA = ',';
const char SPACE = ' ';

/*
 * Parse a Class 1 parameter string.
 */
bool
Class1Modem::parseQuery(const char* cp, Class1Cap caps[])
{
    bool bracket = false, first = true;
    
    while (cp[0]) {
	if (cp[0] == SPACE) {		// ignore white space
	    cp++;
	    continue;
	}

        /* by a.pogoda@web.de, jan 21st 2002
         * workaround for modems sending (<item>,<item>,...), i.e. 
         * enclosed in brackets rather than just <item>,<item>,...
         * e.g. elsa microlink 56k internet II and maybo others
         */
        if (cp[0]=='(' && first && !bracket) {
          /* check whether the first non-space char is an 
           * opening bracket and skip it if true
           */ 
          bracket = true;
          cp++;
          continue;
        }
        else if (cp[0]==')' && !first && bracket) {
          /* if an opening bracket was scanned before and 
           * the current char is a closing one, skip it
           */
          bracket = false;
          cp++;
          continue;
        }
        else if (!isdigit(cp[0]))
	  return (false);

        /* state that we already scanned past the first char */
        first = false;
	int v = 0;
	do {
	    v = v*10 + (cp[0] - '0');
	} while (isdigit((++cp)[0]));
	int r = v;
	if (cp[0] == '-') {				// <low>-<high>
	    cp++;
	    if (!isdigit(cp[0])) {
		return (false);
	    }
	    r = 0;
	    do {
		r = r*10 + (cp[0] - '0');
	    } while (isdigit((++cp)[0]));
	}
	for (u_int i = 0; i < NCAPS; i++)
	    if (caps[i].value >= v && caps[i].value <= r) {
		caps[i].ok = true;
		break;
	    }
	if (cp[0] == COMMA)		// <item>,<item>...
	    cp++;
    }
    return (true);
}
