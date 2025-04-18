/*	$Id: sendfax.c++ 1176 2013-07-24 05:20:02Z faxguy $ */
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
#include "config.h"		//for BR_14400 definition
#include "class2.h"		//for BR_14400 definition
#include "SendFaxClient.h"
#include "FaxDB.h"
#include "Sys.h"
#include "config.h"
#include <ctype.h>		// isspace()

#if HAS_LOCALE
extern "C" {
#include <locale.h>
}
#endif

class sendFaxApp : public SendFaxClient {
private:
    fxStr	appName;		// for error messages
    fxStr	stdinTemp;		// temp file for collecting from pipe
    FaxDB*	db;

    static fxStr dbName;

    void addDestination(const char* cp);
    void addDestinationsFromFile(const char* filename);
    void copyToTemporary(int fin, fxStr& tmpl);
    void fatal(const char* fmt ...);
    void usage();
public:
    sendFaxApp();
    ~sendFaxApp();

    int run(int argc, char** argv);
};

fxStr sendFaxApp::dbName("~/.faxdb");

sendFaxApp::sendFaxApp()
{
    db = NULL;
}

sendFaxApp::~sendFaxApp()
{
    if (stdinTemp != "") Sys::unlink(stdinTemp);
    delete db;
}

int
sendFaxApp::run(int argc, char** argv)
{
    extern int optind;
    extern char* optarg;
    int c;
    char *owner = NULL;
    char *pass = NULL;
    bool optionsUsed = true;

    appName = argv[0];
    u_int l = appName.length();
    appName = appName.tokenR(l, '/');

    resetConfig();
    readConfig(FAX_SYSCONF);
    readConfig(FAX_LIBDATA "/sendfax.conf");
    readConfig(FAX_USERCONF);

    bool waitForJob = false;
    int verbose = 0;
    SendFaxJob& proto = getProtoJob();
    db = new FaxDB(tildeExpand(dbName));
    while ((c = Sys::getopt(argc, argv, "a:b:B:c:C:d:e:f:F:h:i:I:k:M:o:O:P:r:s:S:t:T:u:U:V:W:x:X:y:Y:z:123lmnpvwADEGNR")) != -1) {
        if (c != 'h')
            optionsUsed = false;
        switch (c) {
        case '1':			// restrict to 1D-encoded data
            proto.setDesiredDF(0);
            break;
        case '2':			// restrict to 2D-encoded data
            proto.setDesiredDF(1);
            break;
        case '3':			// restrict to MMR-encoded data
            proto.setDesiredDF(3);
            break;
        case 'a':			// time at which to transmit job
            proto.setSendTime(optarg);
                break;
        case 'A':			// archive job
            proto.setDoneOp("archive");
            break;
        case 'b':			// minimum transfer speed
            proto.setMinSpeed(optarg);
            break;
        case 'B':			// desired transmit speed
            proto.setDesiredSpeed(optarg);
            break;
        case 'C':			// cover page: template file
            proto.setCoverTemplate(optarg);
            break;
        case 'c':			// cover page: comment field
            proto.setCoverComments(optarg);
            break;
        case 'D':			// notify when done
            proto.setNotification("when done");
            break;
        case 'd':			// destination name and number
            optionsUsed = true;
            addDestination(optarg);
            break;
	case 'e':			// sender's Caller*ID Name
	    proto.setFaxName(optarg);
	    break;
        case 'E':			// disable use of ECM
            proto.setDesiredEC(EC_DISABLE);
            break;
        case 'F':			// override tag line format string
            proto.setTagLineFormat(optarg);
            break;
        case 'f':			// sender's identity
            setFromIdentity(optarg);
            break;
        case 'G':			// extended resolutions
            proto.setUseXVRes(true);
            break;
        case 'h':			// server's host
            setHost(optarg);
            break;
        case 'I':			// fixed retry time
            proto.setRetryTime(optarg);
            break;
        case 'i':			// user-specified job identifier
            proto.setJobTag(optarg);
            break;
        case 'k':			// time to kill job
            proto.setKillTime(optarg);
            break;
        case 'l':			// low resolution
            proto.setVResolution(98.);
            break;
        case 'M':			// desired min-scanline time
            proto.setDesiredMST(optarg);
            break;
        case 'm':			// fine resolution
            proto.setVResolution(196.);
            break;
        case 'n':			// no cover sheet
            proto.setAutoCoverPage(false);
            break;
        case 'N':			// no notification
            proto.setNotification("none");
            break;
	case 'o':			// specify owner:pass
	    {
		char* pp = strchr(optarg, ':');
		if (pp && *(pp + 1) != '\0') {
		    *pp = '\0';
		    pass = pp + 1;
		}
	    }
	    owner = optarg;
	    break;
	case 'O':
	    {
		char* op = strchr(optarg, ':');
		if (op && *(op + 1) != '\0') {
		    *op = '\0';
		    if (strncmp(optarg, "skippedpages", 12) == 0) {
			proto.setSkippedPages(atoi(++op));
		    } else if (strncmp(optarg, "skippages", 9) == 0) {
			proto.setSkipPages(atoi(++op));
		    } else if (strncmp(optarg, "nocountcover", 12) == 0) {
			proto.setNoCountCover(atoi(++op));
		    } else if (strncmp(optarg, "probeonly", 9) == 0) {
			proto.setProbeOnly(atoi(++op));
		    } else if (strncmp(optarg, "serverdocover", 13) == 0) {
			proto.setServerDoCover(FaxConfig::getBoolean(++op));
		    } else if (strncmp(optarg, "usecolor", 8) == 0) {
			if (strncmp(++op, "only", 4) == 0) {
			    proto.setUseColor(true);
			    proto.setDesiredDF(6);
			} else
			    proto.setUseColor(FaxConfig::getBoolean(op));
		    } else if (strncmp(optarg, "usesslfax", 9) == 0) {
			proto.setUseSSLFax(FaxConfig::getBoolean(++op));
		    } else if (strncmp(optarg, "ignoremodembusy", 15) == 0) {
			proto.setIgnoreModemBusy(FaxConfig::getBoolean(++op));
		    } else if (strncmp(optarg, "timezone", 8) == 0) {
			proto.setTimeZone(++op);
		    } else if (strncmp(optarg, "config", 6) == 0) {
			readConfigItem(++op);
		    }
		}
	    }
	    break;
        case 'p':			// submit polling request
            addPollRequest();
            break;
        case 'P':			// set scheduling priority
            proto.setPriority(optarg);
            break;
        case 'r':			// cover sheet: regarding field
            proto.setCoverRegarding(optarg);
            break;
        case 'R':			// notify when requeued or done
            proto.setNotification("when requeued");
            break;
        case 's':			// set page size
            proto.setPageSize(optarg);
            break;
        case 'S':			// set TSI
            proto.setTSI(optarg);
            break;
        case 't':			// times to retry sending
            proto.setMaxRetries(atoi(optarg));
            break;
        case 'T':			// times to dial telephone
            proto.setMaxDials(atoi(optarg));
            break;
	case 'u':			// sender's Caller*ID Number (for tagline also)
	    proto.setFaxNumber(optarg);
	    break;
        case 'U':			// cover page: sender's voice number
            proto.setCoverFromVoice(optarg);
            break;
        case 'v':			// verbose mode
            verbose++;
            setvbuf(stdout, NULL, _IOLBF, BUFSIZ);
            SendFaxClient::setVerbose(true);	// type rules & basic operation
            FaxClient::setVerbose(verbose > 1);	// protocol tracing
            break;
        case 'V':			// cover sheet: voice number field
            proto.setCoverVoiceNumber(optarg);
            break;
        case 'w':			// wait for job to complete
            waitForJob = true;
            break;
        case 'W':			// cover page: sender's fax number
            proto.setCoverFromFax(optarg);
            break;
        case 'x':			// cover page: to's company
            proto.setCoverCompany(optarg);
            break;
        case 'X':			// cover page: sender's company
            proto.setCoverFromCompany(optarg);
            break;
        case 'y':			// cover page: to's location
            proto.setCoverLocation(optarg);
            break;
        case 'Y':			// cover page: sender's location
            proto.setCoverFromLocation(optarg);
            break;
        case 'z':			// destinations from file
            optionsUsed = true;
            addDestinationsFromFile(optarg);
            break;
        case '?':
            usage();
            /*NOTREACHED*/
        }
    }
    if (getNumberOfJobs() == 0) {
        fprintf(stderr, "%s: No destination specified.\n",
            (const char*) appName);
        usage();
    }
    if (!optionsUsed) {
	fprintf(stderr, "%s: Unused options after last destination.\n",
	    (const char*) appName);
	usage();
    }
    if (optind < argc) {
        for (; optind < argc; optind++) {
            addFile(argv[optind]);
        }
    } else if (getNumberOfPollRequests() == 0) {
        copyToTemporary(fileno(stdin), stdinTemp);
        addFile(stdinTemp);
    }
    bool status = false;
    fxStr emsg;
    int retval = -1;
    if (callServer(emsg)) {
        status = login(owner, pass, emsg)
            && prepareForJobSubmissions(emsg)
            && submitJobs(emsg);
        if (status && waitForJob) {
            if (getNumberOfJobs() > 1) {
                printWarning("can only wait for one job (right now),"
                    " waiting for job %s.", (const char*) getCurrentJob());
            }
	    int waits = 0;
	    time_t waitstart = Sys::now();
	    fxStr rjobid = getCurrentJob();
	    while (!jobWait(rjobid) && waits++ < 5) {
		/*
		 * Our wait failed, and excepting a fault on the proxy this
		 * can only mean that the connection was lost.  Reconnect
		 * and re-start our wait.
		 */
		time_t sleeptime = waitstart + 60 - Sys::now();
		if (sleeptime < 1) sleeptime = 2;
		if (verbose) printWarning("lost connection to server, attempt %d.  Attempting reconnection in %d seconds.", waits, sleeptime);
		hangupServer();
		sleep(sleeptime);
		waitstart = Sys::now();
		status = false;
		if (callServer(emsg)) {
		    status = login(owner, pass, emsg) && setCurrentJob(rjobid);
		    if (status && verbose) printWarning("reconnected to server.  Resuming wait for job %s.", (const char*) rjobid);
		}
	    }
	    /* For the benefit of those watching in verbose... */
	    fxStr r;
            command((const char*) fxStr::format("JOB %s", (const char*) rjobid));
            jobParm("conntime");
            jobParm("duration");
            jobParm("npages");
            jobParm("totpages");
            jobParm("commid");
            jobParm("ntries");
            jobParm("status");
            emsg = getLastResponse(); emsg.remove(0, emsg.length() > 4 ? 4 : emsg.length());
            if (emsg.length()) {
		status = false;
                jobParm("errorcode");
                r = getLastResponse(); r.remove(0, r.length() > 5 ? 5 : r.length());
                retval = atoi((const char*) r);
            } else {
		retval = 0;
		status = true;
	    }
            jobParm("state");
            r = getLastResponse(); r.remove(0, r.length() > 4 ? 4 : r.length());
            // Due to the jobWait we should only ever see "DONE" and "FAILED"...
            if (r != "DONE") {
		status = false;
            }
        } else if (status) {
	    retval = 0;
	}
        hangupServer();
    }
    if (!status) {
	printError("%s", (const char*) emsg);
	if (!retval) retval = -1;
    }
    return (retval);
}

void
sendFaxApp::usage()
{
    fxFatal("usage: %s [options] [files]\n"
        "(Read the manual page; it's too complicated)", (const char*) appName);
}

/*
 * Add a destination; parse ``person@number#subaddress'' syntax.
 * T.33 Appendix I suggests that ``#'' be used to tag a subaddress.
 */
void
sendFaxApp::addDestination(const char* cp)
{
    fxStr recipient;
    const char* tp = strchr(cp, '@');
    if (tp) {
	recipient = fxStr(cp, tp-cp);
	cp = tp+1;
    } else {
	recipient = "";
    }
    fxStr subaddress;
    size_t sublen = 0;
    const char* ap = strchr(cp, '#');
    if (ap) {
	ap = ap+1;
	subaddress = fxStr(ap);
	sublen = strlen(subaddress) + 1;
    } else {
	subaddress = "";
    }
    fxStr dest(cp, strlen(cp) - sublen);
    if (db && dest.length() > 0) {
        fxStr name;
        FaxDBRecord* r = db->find(dest, &name);
        if (r) {
            if (recipient == "")
            recipient = name;
            dest = r->find(FaxDB::numberKey);
        }
    }
    if (dest.length() == 0) {
        fatal("Null destination for \"%s\"", cp);
    }
    SendFaxJob& job = addJob();
    job.setDialString(dest);
    job.setCoverName(recipient);
    job.setSubAddress(subaddress);
    if(job.getDesiredSpeed() > BR_14400 && job.getDesiredEC() == EC_DISABLE) {
        printWarning("ECM disabled, limiting job to 14400 bps.");
        job.setDesiredSpeed(BR_14400);
    }
}

/*
 * Add destinations from file, one destination per line
 */
void
sendFaxApp::addDestinationsFromFile(const char* filename)
{
    FILE* destfile;
    char dest[256];
    char *cp;

    if ((destfile = fopen(filename, "r")) != NULL) {
	while (fgets(dest, sizeof(dest), destfile)) {
            for (cp = strchr(dest, '\0'); cp>dest && isspace(cp[-1]); cp--);
            *cp='\0';
	    if (dest[0] != '#' && dest[0] != '\0')
		addDestination(dest);
	}
    } else {
	fatal("%s: no such file", filename);
    }

    fclose(destfile);
}

/*
 * Copy data from fin to a temporary file.
 */
void
sendFaxApp::copyToTemporary(int fin, fxStr& tmpl)
{
    const char* templ = _PATH_TMP "sndfaxXXXXXX";
    char* buff = strcpy(new char[strlen(templ) + 1], templ);
    int fd = Sys::mkstemp(buff);
    tmpl = buff;
    delete [] buff;
    if (fd < 0) {
        fatal("%s: Can not create temporary file", (const char*) tmpl);
    }
    int cc, total = 0;
    char buf[16*1024];
    while ((cc = Sys::read(fin, buf, sizeof (buf))) > 0) {
        if (Sys::write(fd, buf, cc) != cc) {
            Sys::unlink(tmpl);
            fatal("%s: write error", (const char*) tmpl);
        }
        total += cc;
    }
    Sys::close(fd);
    if (total == 0) {
        Sys::unlink(tmpl);
        tmpl = "";
        fatal("No input data; transmission aborted");
    }
}

#include <signal.h>

static	sendFaxApp* app = NULL;

static void
cleanup()
{
    sendFaxApp* a = app;
    app = NULL;
    delete a;
}

static void
sigDone(int n)
{
    cleanup();
    exit(n);
}

int
main(int argc, char** argv)
{
    int r = 0;
#ifdef LC_CTYPE
    setlocale(LC_CTYPE, "");			// for <ctype.h> calls
#endif
#ifdef LC_TIME
    setlocale(LC_TIME, "");			// for strftime calls
#endif
    signal(SIGHUP, fxSIGHANDLER(sigDone));
    signal(SIGINT, fxSIGHANDLER(sigDone));
    signal(SIGTERM, fxSIGHANDLER(sigDone));
    signal(SIGCHLD, fxSIGHANDLER(SIG_DFL));     // by YC
    app = new sendFaxApp;
    r = app->run(argc, argv);
    if (r) sigDone(r);
    signal(SIGHUP, fxSIGHANDLER(SIG_IGN));
    signal(SIGINT, fxSIGHANDLER(SIG_IGN));
    signal(SIGTERM, fxSIGHANDLER(SIG_IGN));
    cleanup();
    return (r);
}

static void
vfatal(FILE* fd, const char* fmt, va_list ap)
{
    vfprintf(fd, fmt, ap);
    va_end(ap);
    fputs(".\n", fd);
    sigDone(-1);
}

void
fxFatal(const char* va_alist ...)
#define	fmt va_alist
{
    va_list ap;
    va_start(ap, fmt);
    vfatal(stderr, fmt, ap);
    /*NOTTEACHED*/
}
#undef fmt

void
sendFaxApp::fatal(const char* va_alist ...)
#define	fmt va_alist
{
    fprintf(stderr, "%s: ", (const char*) appName);
    va_list ap;
    va_start(ap, fmt);
    vfatal(stderr, fmt, ap);
    /*NOTTEACHED*/
}
#undef fmt
