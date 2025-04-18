/*	$Id: faxQueueApp.c++ 1177 2013-07-28 01:07:28Z faxguy $ */
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
#include "Sys.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <limits.h>
#include <sys/file.h>
#include <tiffio.h>

#include "Dispatcher.h"

#include "FaxMachineInfo.h"
#include "FaxAcctInfo.h"
#include "FaxRequest.h"
#include "FaxTrace.h"
#include "FaxRecvInfo.h"
#include "Timeout.h"
#include "UUCPLock.h"
#include "DialRules.h"
#include "RE.h"
#include "Modem.h"
#include "Trigger.h"
#include "faxQueueApp.h"
#include "HylaClient.h"
#include "MemoryDecoder.h"
#include "FaxSendInfo.h"
#include "PageSize.h"
#include "SendFaxClient.h"
#include "config.h"

/*
 * HylaFAX Spooling and Command Agent.
 */

const fxStr faxQueueApp::sendDir	= FAX_SENDDIR;
const fxStr faxQueueApp::docDir		= FAX_DOCDIR;
const fxStr faxQueueApp::clientDir	= FAX_CLIENTDIR;

fxStr strTime(time_t t)	{ return fxStr(fmtTime(t)); }

#define	JOBHASH(pri)	(((pri) >> 4) & (NQHASH-1))

faxQueueApp::SchedTimeout::SchedTimeout()
{
    started = false;
    pending = false;
    lastRun = Sys::now() - 1;
}

faxQueueApp::SchedTimeout::~SchedTimeout() {}

void
faxQueueApp::SchedTimeout::timerExpired(long, long)
{
    if (pending && lastRun <= Sys::now()) pending = false;
    if (faxQueueApp::instance().scheduling() ) {
	start(0);
	return;
    }
    faxQueueApp::instance().runScheduler();
    started = false;
}

void
faxQueueApp::SchedTimeout::start(u_short s)
{
    /*
     * If we don't throttle the scheduler then large
     * queues can halt the system with CPU consumption.
     * So we keep the scheduler from running more than
     * once per second.
     */
    if (!started && Sys::now() > lastRun) {
	started = true;
	pending = false;
	Dispatcher::instance().startTimer(s, 1, this);
	lastRun = Sys::now() + s;
    } else {
	if (!pending && lastRun <= Sys::now()) {
	    /*
	     * The scheduler is either running now or has been run
	     * within the last second and there are no timers set
	     * to trigger another scheduler run.  So we set a
	     * timer to go off in one second to avoid a stalled
	     * run queue.
	     */
	    Dispatcher::instance().startTimer(s + 1, 0, this);
	    lastRun = Sys::now() + 1 + s;
	    pending = true;
	}
    }
}

faxQueueApp* faxQueueApp::_instance = NULL;

faxQueueApp::faxQueueApp()
    : configFile(FAX_CONFIG)
{
    fifo = -1;
    quit = false;
    dialRules = NULL;
    inSchedule = false;
    setupConfig();

    fxAssert(_instance == NULL, "Cannot create multiple faxQueueApp instances");
    _instance = this;
}

faxQueueApp::~faxQueueApp()
{
    HylaClient::purge();
    delete dialRules;
}

faxQueueApp& faxQueueApp::instance() { return *_instance; }

void
faxQueueApp::initialize(int argc, char** argv)
{
    updateConfig(configFile);		// read config file
    faxApp::initialize(argc, argv);

    for (GetoptIter iter(argc, argv, getOpts()); iter.notDone(); iter++)
	switch (iter.option()) {
	case 'c':			// set configuration parameter
	    readConfigItem(iter.optArg());
	    break;
	}

    logInfo("%s", HYLAFAX_VERSION);
    logInfo("%s", "Copyright (c) 1990-1996 Sam Leffler");
    logInfo("%s", "Copyright (c) 1991-1996 Silicon Graphics, Inc.");

    scanForModems();
}

void
faxQueueApp::open()
{
    faxApp::open();
    scanQueueDirectory();
    Modem::broadcast("HELLO");		// announce queuer presence
    scanClientDirectory();		// announce queuer presence
    pokeScheduler();
}

void
faxQueueApp::blockSignals()
{
    sigset_t block;
    sigemptyset(&block);
    sigaddset(&block, SIGCHLD);
    sigprocmask(SIG_BLOCK, &block, NULL);
}

void
faxQueueApp::releaseSignals()
{
    sigset_t release;
    sigemptyset(&release);
    sigaddset(&release, SIGCHLD);
    sigprocmask (SIG_UNBLOCK, &release, NULL);
}

/*
 * Scan the spool area for modems.  We can't be certain the
 * modems are actively working without probing them; this
 * work is done simply to buildup the internal database for
 * broadcasting a ``HELLO'' message.  Later on, individual
 * modems are enabled for use based on messages received
 * through the FIFO.
 */
void
faxQueueApp::scanForModems()
{
    DIR* dir = Sys::opendir(".");
    if (dir == NULL) {
	logError("Could not scan directory for modems");
	return;
    }
    fxStr fifoMatch(fifoName | ".");
    for (dirent* dp = readdir(dir); dp; dp = readdir(dir)) {
	if (dp->d_name[0] != fifoName[0])
	    continue;
	if (!strneq(dp->d_name, fifoMatch, fifoMatch.length()))
	    continue;
	if (Sys::isFIFOFile(dp->d_name)) {
	    fxStr devid(dp->d_name);
	    devid.remove(0, fifoMatch.length()-1);	// NB: leave "."
	    if (Sys::isRegularFile(FAX_CONFIG | devid)) {
		devid.remove(0);			// strip "."
		(void) Modem::getModemByID(devid);	// adds to list
	    }
	}
    }
    closedir(dir);
}

/*
 * Scan the spool directory for queue files and
 * enter them in the queues of outgoing jobs.
 */
void
faxQueueApp::scanQueueDirectory()
{
    DIR* dir = Sys::opendir(sendDir);
    if (dir == NULL) {
	logError("Could not scan %s directory for outbound jobs",
		(const char*)sendDir);
	return;
    }
    for (dirent* dp = readdir(dir); dp; dp = readdir(dir)) {
	if (dp->d_name[0] == 'q')
	{
	    submitJob(&dp->d_name[1], true);
	    long s, us;
	    s = 0; us = 10;
	    Dispatcher::instance().dispatch(s, us);
	}
    }
    closedir(dir);
}

/*
 * Scan the client area for active client processes
 * and send a ``HELLO message'' to notify them the
 * queuer process has restarted.  If no process is
 * listening on the FIFO, remove it; the associated
 * client state will be purged later.
 */
void
faxQueueApp::scanClientDirectory()
{
    DIR* dir = Sys::opendir(clientDir);
    if (dir == NULL) {
	logError("Could not scan %s directory for clients",
		(const char*) clientDir);
	return;
    }
    for (dirent* dp = readdir(dir); dp; dp = readdir(dir)) {
	if (!isdigit(dp->d_name[0]))
	    continue;
	fxStr fifo(clientDir | "/" | dp->d_name);
	if (Sys::isFIFOFile((const char*) fifo))
	    if (!HylaClient::getClient(fifo).send("HELLO", 6))
		Sys::unlink(fifo);
    }
    closedir(dir);
}

/*
 * Process a job.  Prepare it for transmission and
 * pass it on to the thread that does the actual
 * transmission work.  The job is marked ``active to
 * this destination'' prior to preparing it because
 * preparation may involve asynchronous activities.
 * The job is placed on the active list so that it
 * can be located by filename if necessary.
 */
void
faxQueueApp::processJob(Job& job, FaxRequest* req, DestInfo& di)
{
    JobStatus status;
    FaxMachineInfo& info = di.getInfo(job.dest);

    Job* bjob = job.bfirst();	// first job in batch
    Job* cjob = &job;		// current job
    FaxRequest* creq = req;	// current request
    Job* njob = NULL;		// next job
    
    for (; cjob != NULL; cjob = njob) {
	creq = cjob->breq;
	njob = cjob->bnext;
	cjob->commid = "";			// set on return
	req->notice = "";			// Clear for new procssing
	di.active(*cjob);
	setActive(*cjob);			// place job on active list
	updateRequest(*creq, *cjob);
	if (!prepareJobNeeded(*cjob, *creq, status)) {
	    if (status != Job::done) {
		if (cjob->bprev == NULL)
		    bjob = njob;
		cjob->state = FaxRequest::state_failed;
		deleteRequest(*cjob, creq, status, true);
		setDead(*cjob);
	    }
	} else {
	    numPrepares++;
	    if (prepareJobStart(*cjob, creq, info))
		return;
	    else if (cjob->bprev == NULL)
		bjob = njob;
	}
    }
    if (bjob != NULL)
	sendJobStart(*bjob, bjob->breq);
    else
	di.hangup();	// needed because runScheduler() did a di.call()

}

/*
 * Check if the job requires preparation that should be
 * done in a fork'd copy of the server.  A sub-fork is
 * used if documents must be converted or a continuation
 * cover page must be crafted (i.e. the work may take
 * a while).
 */
bool
faxQueueApp::prepareJobNeeded(Job& job, FaxRequest& req, JobStatus& status)
{
    if (!req.items.length()) {
	req.notice = "Job contains no documents {E323}";
	status = Job::rejected;
	jobError(job, "SEND REJECT: %s", (const char*) req.notice);
	return (false);
    }
    for (u_int i = 0, n = req.items.length(); i < n; i++)
	switch (req.items[i].op) {
	case FaxRequest::send_postscript:	// convert PostScript
	case FaxRequest::send_pcl:		// convert PCL
	case FaxRequest::send_tiff:		// verify&possibly convert TIFF
	case FaxRequest::send_pdf:		// convert PDF
	    return (true);
	case FaxRequest::send_poll:		// verify modem is capable
	    if (!job.modem->supportsPolling()) {
		req.notice = "Modem does not support polling {E324}";
		status = Job::rejected;
		jobError(job, "SEND REJECT: %s", (const char*) req.notice);
		return (false);
	    }
	    break;
	}
    status = Job::done;
    return (req.cover != "");			// need continuation cover page
}

/*
 * Handler used by job preparation subprocess
 * to pass signal from parent queuer process.
 * We mark state so job preparation will be aborted
 * at the next safe point in the procedure.
 */
void
faxQueueApp::prepareCleanup(int s)
{
    int old_errno = errno;
    signal(s, fxSIGHANDLER(faxQueueApp::prepareCleanup));
    faxQueueApp::instance().abortSignal = s;
    faxQueueApp::instance().abortPrepare = true;
    errno = old_errno;
}

/*
 * Start job preparation in a sub-fork.  The server process
 * forks and sets up a Dispatcher handler to reap the child
 * process.  The exit status from the child is actually the
 * return value from the prepareJob method; this and a
 * reference to the original Job are passed back into the
 * server thread at which point the transmit work is actually
 * initiated.
 */
bool
faxQueueApp::prepareJobStart(Job& job, FaxRequest* req,
    FaxMachineInfo& info)
{
    traceQueue(job, "PREPARE START");
    abortPrepare = false;
    abortSignal = 0;
    pid_t pid = fork();
    switch (pid) {
    case 0:				// child, do work
	/*
	 * NB: There is a window here where the subprocess
	 * doing the job preparation can have the old signal
	 * handlers installed when a signal comes in.  This
	 * could be fixed by using the appropriate POSIX calls
	 * to block and unblock signals, but signal usage is
	 * quite tenuous (i.e. what is and is not supported
	 * on a system), so rather than depend on this
	 * functionality being supported, we'll just leave
	 * the (small) window in until it shows itself to
	 * be a problem.
	 */
	signal(SIGTERM, fxSIGHANDLER(faxQueueApp::prepareCleanup));
	signal(SIGINT, fxSIGHANDLER(faxQueueApp::prepareCleanup));
	_exit(prepareJob(job, *req, info));
	/*NOTREACHED*/
    case -1:				// fork failed, sleep and retry
	if (job.isOnList()) job.remove(); // Remove from active queue
	delayJob(job, *req, "Could not fork to prepare job for transmission {E340}",
	    Sys::now() + random() % requeueInterval);
	delete req;
	return false;
    default:				// parent, setup handler to wait
	job.startPrepare(pid);
	delete req;			// must reread after preparation
	job.breq = NULL;
	Trigger::post(Trigger::JOB_PREP_BEGIN, job);
	return true;
    }
}

/*
 * Handle notification from the sub-fork that job preparation
 * is done.  The exit status is checked and interpreted as the
 * return value from prepareJob if it was passed via _exit.
 */
void
faxQueueApp::prepareJobDone(Job& job, int status)
{
    traceQueue(job, "PREPARE DONE");
    Trigger::post(Trigger::JOB_PREP_END, job);
    if (status&0xff) {
	logError("JOB %s: bad exit status %#x from sub-fork",
	    (const char*) job.jobid, status);
	status = Job::failed;
    } else
	status >>= 8;
    if (job.suspendPending) {		// co-thread waiting
	job.suspendPending = false;
	releaseModem(job);
    } else {
	FaxRequest* req = readRequest(job);
	if (!req) {
	    // NB: no way to notify the user (XXX)
	    logError("JOB %s: qfile vanished during preparation",
		(const char*) job.jobid);
	    setDead(job);
	} else {
	    bool processnext = false;
	    bool startsendjob = false;
	    Job* targetjob = &job;
	    if (status == Job::done) {		// preparation completed successfully
		job.breq = req;
		startsendjob = (job.bnext == NULL);
		processnext = !startsendjob;
		if (processnext) {
		    targetjob = job.bnext;
		}
	    } else {
		/*
		 * Job preparation did not complete successfully.
		 *
		 * If there is more than one job in this batch, then remove this job
		 * and adjust the batch accordingly.
		 */
		if (job.bnext == NULL) {	// no jobs left in batch
		    targetjob = job.bprev;
		    startsendjob = (targetjob != NULL);
		} else {			// more jobs in batch
		    targetjob = job.bnext;
		    processnext = true;
		}
		/*
		 * If there are other jobs in the batch, we have to be
		 * careful to *not* release the modem, otherwise faxq will
		 * schedule new jobs on this modem while the rest of the jobs
		 * in the batch are still using it.
		 */
		if (startsendjob || processnext)
			job.modem = NULL;
		if (status == Job::requeued) {
		    if (job.isOnList()) job.remove();
		    delayJob(job, *req, "Could not fork to prepare job for transmission {E340}",
			Sys::now() + random() % requeueInterval);
		    delete req;
		} else {
		    deleteRequest(job, req, status, true);
		    setDead(job);
		}
	    }
	    if (processnext) {
		processJob(*targetjob, targetjob->breq, destJobs[targetjob->dest]);
	    } else if (startsendjob)
		sendJobStart(*targetjob->bfirst(), targetjob->bfirst()->breq);
	    else {
		/*
		 * This job may be blocking others.
		 */
		DestInfo& di = destJobs[job.dest];
		di.hangup();			// undo runScheduler()'s di.call() 
		unblockDestJobs(di);		// release any blocked jobs
		pokeScheduler();
	    }
	}
    }
    if (numPrepares) numPrepares--;
    if (numPrepares < maxConcurrentPreps) {
	/*
	 * The number of simultaneous job preparations is
	 * low enough to allow another job preparation.
	 */
	pokeScheduler();
    }
}

/*
 * Document Use Database.
 *
 * The server minimizes imaging operations by checking for the
 * existence of compatible, previously imaged, versions of documents.
 * This is done by using a file naming convention that includes the
 * source document name and the remote machine capabilities that are
 * used for imaging.  The work done here (and in other HylaFAX apps)
 * also assumes certain naming convention used by hfaxd when creating
 * document files.  Source documents are named:
 *
 *     doc<docnum>.<type>
 *
 * where <docnum> is a unique document number that is assigned by
 * hfaxd at the time the document is stored on the server.  Document
 * references by a job are then done using filenames (i.e. hard
 * links) of the form:
 *
 *	doc<docnum>.<type>.<jobid>
 *
 * where <jobid> is the unique identifier assigned to each outbound
 * job.  Then, each imaged document is named:
 *
 *	doc<docnum>.<type>;<encoded-capabilities>
 *
 * where <encoded-capabilities> is a string that encodes the remote
 * machine's capabilities.
 *
 * Before imaging a document the scheduler checks to see if there is
 * an existing file with the appropriate name.  If so then the file
 * is used and no preparation work is needed for sending the document.
 * Otherwise the document must be converted for transmission; this
 * result is written to a file with the appropriate name.  After an
 * imaged document has been transmitted it is not immediately removed,
 * but rather the scheduler is informed that the job no longer holds
 * (needs) a reference to the document and the scheduler records this
 * information so that when no jobs reference the original source
 * document, all imaged forms may be expunged.  As documents are
 * transmitted the job references to the original source documents are
 * converted to references to the ``base document name'' (the form
 * without the <jobid>) so that the link count on the inode for this
 * file reflects the number of references from jobs that are still
 * pending transmission.  This means that the scheduler can use the
 * link count to decide when to expunge imaged versions of a document.
 *
 * Note that the reference counts used here do not necessarily
 * *guarantee* that a pre-imaged version of a document will be available.
 * There are race conditions under which a document may be re-imaged
 * because a previously imaged version was removed.
 *
 * A separate document scavenger program should be run periodically
 * to expunge any document files that might be left in the docq for
 * unexpected reasons.  This program should read the set of jobs in
 * the sendq to build a onetime table of uses and then remove any
 * files found in the docq that are not referenced by a job.
 */

/*
 * Remove a reference to an imaged document and if no
 * references exist for the corresponding source document,
 * expunge all imaged versions of the document.
 */
void
faxQueueApp::unrefDoc(const fxStr& file)
{
    /*
     * Convert imaged document name to the base
     * (source) document name by removing the
     * encoded session parameters used for imaging.
     */
    u_int l = file.nextR(file.length(), ';');
    if (l == 0) {
	logError("Bogus document handed to unrefDoc: %s", (const char*) file);
	return;
    }
    fxStr doc = file.head(l-1);
    /*
     * Add file to the list of pending removals.  We
     * do this before checking below so that the list
     * of files will always have something on it.
     */
    fxStr& files = pendingDocs[doc];
    if (files.find(0, file) == files.length())		// suppress duplicates
	files.append(file | " ");
    if (tracingLevel & FAXTRACE_DOCREFS)
	logInfo("DOC UNREF: %s files %s",
	    (const char*) file, (const char*) files);
    /*
     * The following assumes that any source document has
     * been renamed to the base document name *before* this
     * routine is invoked (either directly or via a msg
     * received on a FIFO).  Specifically, if the stat
     * call fails we assume the file does not exist and
     * that it is safe to remove the imaged documents.
     * This is conservative and if wrong will not break
     * anything; just potentially cause extra imaging
     * work to be done.
     */
    struct stat sb;
    if (Sys::stat(doc, sb) < 0 || sb.st_nlink == 1) {
	if (tracingLevel & FAXTRACE_DOCREFS)
	    logInfo("DOC UNREF: expunge imaged files");
	/*
	 * There are no additional references to the
	 * original source document (all references
	 * should be from completed jobs that reference
	 * the original source document by its basename).
	 * Expunge imaged documents that were waiting for
	 * all potential uses to complete.
	 */
	l = 0;
	do {
	    fxStr filename = files.token(l, ' ');
	    (void) Sys::unlink(filename);
	    (void) Sys::unlink(filename|".color");
	} while (l < files.length());
	pendingDocs.remove(doc);
    }
}

#include "class2.h"

/*
 * Prepare a job by converting any user-submitted documents
 * to a format suitable for transmission.
 */
JobStatus
faxQueueApp::prepareJob(Job& job, FaxRequest& req,
    const FaxMachineInfo& info)
{
    /*
     * Select imaging parameters according to requested
     * values, client capabilities, and modem capabilities.
     * Note that by this time we believe the modem is capable
     * of certain requirements needed to transmit the document
     * (based on the capabilities passed to us by faxgetty).
     */
    Class2Params params;
    
    /*
     * User requested vres (98 or 196) and usexvres (1=true or 0=false)
     */
    int vres = req.resolution;
    int usexvres = req.usexvres;
    /*
     * System overrides in JobControl:
     * VRes: we check for vres = 98 or vres = 196 in JobControl;
     *       if vres is not set getVRes returns 0.
     * UseXVres: we check for usexvres = 0 or usexvres = 1 in JobControl;
     *           if usexvres is not set getUseXVRes retuns -1.
     */
    if (job.getJCI().getVRes() == 98)
	vres = 98;
    else if (job.getJCI().getVRes() == 196)
	vres = 196;
    if (job.getJCI().getUseXVRes() == 0)
	usexvres = 0;
    else if (job.getJCI().getUseXVRes() == 1)
	usexvres = 1;

    /*
     * Here we overwrite the req file with modifications from JobControl
     */
    if (req.usesslfax && job.getJCI().getUseSSLFax() == false) {
	req.usesslfax = false;
	updateRequest(req, job);
    }

    // use the highest resolution the client supports
    params.vr = VR_NORMAL;

    if (usexvres) {
	if (info.getSupportsVRes() & VR_200X100 && job.modem->supportsVR(VR_200X100))
	    params.vr = VR_200X100;
	if (info.getSupportsVRes() & VR_FINE && job.modem->supportsVR(VR_FINE))
	    params.vr = VR_FINE;
	if (info.getSupportsVRes() & VR_200X200 && job.modem->supportsVR(VR_200X200))
	    params.vr = VR_200X200;
	if (info.getSupportsVRes() & VR_R8 && job.modem->supportsVR(VR_R8))
	    params.vr = VR_R8;
	if (info.getSupportsVRes() & VR_200X400 && job.modem->supportsVR(VR_200X400))
	    params.vr = VR_200X400;
	if (info.getSupportsVRes() & VR_300X300 && job.modem->supportsVR(VR_300X300))
	    params.vr = VR_300X300;
	if (info.getSupportsVRes() & VR_R16 && job.modem->supportsVR(VR_R16))
	    params.vr = VR_R16;
    } else {				// limit ourselves to normal and fine
	if (vres > 150) {
	    if (info.getSupportsVRes() & VR_FINE && job.modem->supportsVR(VR_FINE))
		params.vr = VR_FINE;
	}
    }
    /*
     * Follow faxsend and use unlimited page length whenever possible.
     */
    useUnlimitedLN = (info.getMaxPageLengthInMM() == (u_short) -1);

    if (job.getJCI().getPageSize().length()) {
	PageSizeInfo* psi = PageSizeInfo::getPageSizeByName(job.getJCI().getPageSize());
	params.setPageWidthInMM(
	    fxmin((u_int) psi->width(), (u_int) info.getMaxPageWidthInMM()));
	params.setPageLengthInMM(
	    fxmin((u_int) psi->height(), (u_int) info.getMaxPageLengthInMM()));
    } else {
	params.setPageWidthInMM(
	    fxmin((u_int) req.pagewidth, (u_int) info.getMaxPageWidthInMM()));
	params.setPageLengthInMM(
	    fxmin((u_int) req.pagelength, (u_int) info.getMaxPageLengthInMM()));
    }

    /*
     * Generate MMR or 2D-encoded facsimile if:
     * o the server is permitted to generate it,
     * o the modem is capable of sending it,
     * o the remote side is known to be capable of it, and
     * o the user hasn't specified a desire to send 1D data.
     */
    int jcdf = job.getJCI().getDesiredDF();
    if (jcdf != -1) req.desireddf = jcdf;
    if (req.usecolor) params.jp = JP_COLOR;
    if (req.usecolor && req.desireddf > DF_JBIG) {
	// code for JPEG-only fax...
	params.df = (u_int) -1;
    } else if (req.desireddf == DF_2DMMR && (req.desiredec != EC_DISABLE) && 
	use2D && job.modem->supportsMMR() &&
	 (! info.getCalledBefore() || info.getSupportsMMR()) )
	    params.df = DF_2DMMR;
    else if (req.desireddf > DF_1DMH) {
	params.df = (use2D && job.modem->supports2D() &&
	    (! info.getCalledBefore() || info.getSupports2DEncoding()) ) ?
		DF_2DMR : DF_1DMH;
    } else
	params.df = DF_1DMH;

    /*
     * Restrict parameter selection for destinations with poor audio quality.
     */
    u_int dataSent = info.getDataSent() + info.getDataSent1() + info.getDataSent2();
    u_int dataMissed = info.getDataMissed() + info.getDataMissed1() + info.getDataMissed2();
    if (class1RestrictPoorDestinations && dataSent && dataMissed * 100 / dataSent > class1RestrictPoorDestinations) {
	params.jp = JP_NONE;
	params.vr = VR_NORMAL;
	traceJob(job, "This destination exhibits poor call audio quality.  Restricting resolution and color support.");
    }

    /*
     * Check and process the documents to be sent
     * using the parameter selected above.
     */
    JobStatus status = Job::done;
    bool updateQFile = false;
    fxStr tmp;		// NB: here to avoid compiler complaint
    u_int i = 0;
    while (i < req.items.length() && status == Job::done && !abortPrepare) {
	FaxItem& fitem = req.items[i];
	switch (fitem.op) {
	case FaxRequest::send_postscript:	// convert PostScript
	case FaxRequest::send_pcl:		// convert PCL
	case FaxRequest::send_tiff:		// verify&possibly convert TIFF
        case FaxRequest::send_pdf:		// convert PDF
	    tmp = FaxRequest::mkbasedoc(fitem.item) | ";" | params.encodePage();
	    status = convertDocument(job, fitem, tmp, params, req.notice);
	    if (status == Job::done) {
		/*
		 * Insert converted file into list and mark the
		 * original document so that it's saved, but
		 * not processed again.  The converted file
		 * is sent, while the saved file is kept around
		 * in case it needs to be returned to the sender.
		 */
		fitem.op++;			// NB: assumes order of enum
		req.insertFax(i+1, tmp);
	    } else {
		Sys::unlink(tmp);		// bail out
		Sys::unlink(tmp|".color");
	    }
	    updateQFile = true;
	    break;
	}
	i++;
    }
    if (status == Job::done && !abortPrepare) {
	if (req.pagehandling == "" && !abortPrepare) {
	    /*
	     * Calculate/recalculate the per-page session parameters
	     * and check the page count against the max pages.  We
	     * do this before generating continuation any cover page
	     * to prevent any skippages setting from trying to skip
	     * the continuation cover page.
	     */
	    if (!preparePageHandling(job, req, info, req.notice)) {
		status = Job::rejected;		// XXX
		req.notice.insert("Document preparation failed: ");
	    }
	    updateQFile = true;
	}
	if (req.cover != "" && !abortPrepare) {
	    /*
	     * Generate a continuation cover page if necessary.
	     * Note that a failure in doing this is not considered
	     * fatal; perhaps this should be configurable?
	     */
	    if (updateQFile) 
		updateRequest(req, job);	// cover-page generation may look at the file
	    if (makeCoverPage(job, req, params))
		req.nocountcover++;
	    updateQFile = true;
	    /*
	     * Recalculate the per-page session parameters.
	     */
	    if (!preparePageHandling(job, req, info, req.notice)) {
	 	status = Job::rejected;		// XXX
		req.notice.insert("Document preparation failed: ");
	    }
	}    
    }
    if (abortPrepare)
	logError("CAUGHT SIGNAL %d, ABORT JOB PREPARATION", abortSignal);
    if (updateQFile)
	updateRequest(req, job);
    return (status);
}

/*
 * Prepare the job for transmission by analysing
 * the page characteristics and determining whether
 * or not the page transfer parameters will have
 * to be renegotiated after the page is sent.  This
 * is done before the call is placed because it can
 * be slow and there can be timing problems if this
 * is done during transmission.
 */
bool
faxQueueApp::preparePageHandling(Job& job, FaxRequest& req,
    const FaxMachineInfo& info, fxStr& emsg)
{
    /*
     * Figure out whether to try chopping off white space
     * from the bottom of pages.  This can only be done
     * if the remote device is thought to be capable of
     * accepting variable-length pages.
     */
    u_int pagechop;
    if (info.getMaxPageLengthInMM() == (u_short)-1) {
	pagechop = req.pagechop;
	if (pagechop == FaxRequest::chop_default)
	    pagechop = pageChop;
    } else
	pagechop = FaxRequest::chop_none;
    u_int maxPages = job.getJCI().getMaxSendPages();
    /*
     * Scan the pages and figure out where session parameters
     * will need to be renegotiated.  Construct a string of
     * indicators to use when doing the actual transmission.
     *
     * NB: all files are coalesced into a single fax document
     *     if possible
     */
    Class2Params params;		// current parameters
    Class2Params next;			// parameters for ``next'' page
    TIFF* tif = NULL;			// current open TIFF image
    req.totpages = req.npages;		// count pages previously transmitted
    u_int docpages;
    for (u_int i = 0;;) {
	if (!tif || TIFFLastDirectory(tif)) {
	    /*
	     * Locate the next file to be sent.
	     */
	    if (tif)			// close previous file
		TIFFClose(tif), tif = NULL;
	    if (i >= req.items.length()) {
		req.pagehandling.append('P');		// EOP
		req.skippages = 0;
		return (true);
	    }
	    i = req.findItem(FaxRequest::send_fax, i);
	    if (i == fx_invalidArrayIndex) {
		req.pagehandling.append('P');		// EOP
		req.skippages = 0;
		return (true);
	    }
	    const FaxItem& fitem = req.items[i];
	    tif = TIFFOpen(fitem.item, "r");
	    if (tif == NULL) {
		emsg = "Can not open document file " | fitem.item | " {E314}";
		if (tif)
		    TIFFClose(tif);
		return (false);
	    }
	    if (fitem.dirnum != 0 && !TIFFSetDirectory(tif, fitem.dirnum)) {
		emsg = fxStr::format(
		    "Can not set directory %u in document file %s {E315}"
		    , fitem.dirnum
		    , (const char*) fitem.item
		);
		if (tif)
		    TIFFClose(tif);
		return (false);
	    }
	    docpages = fitem.dirnum;
	    i++;			// advance for next find
	} else {
	    /*
	     * Read the next TIFF directory.
	     */
	    if (!TIFFReadDirectory(tif)) {
		emsg = fxStr::format(
		    "Error reading directory %u in document file %s {E316}"
		    , TIFFCurrentDirectory(tif)
		    , TIFFFileName(tif)
		);
		if (tif)
		    TIFFClose(tif);
		return (false);
	    }
	    docpages++;
	}
	if (++req.totpages > maxPages) {
	    emsg = fxStr::format("Too many pages in submission; max %u {E317}",
		maxPages);
	    if (tif)
		TIFFClose(tif);
	    return (false);
	}
	if (req.skippages) {
	    req.totpages--;
	    req.skippages--;
	    req.skippedpages++;
	    if (req.skippages == 0) {
		req.items[i-1].dirnum = docpages + 1;		// mark it
	    }
	    if (TIFFLastDirectory(tif)) {
		req.renameSaved(i-1);
		req.items.remove(i-1);
	    }
	} else {
	    next = params;
	    setupParams(tif, next, info);
	    if (params.df != (u_int) -1 || params.jp != (u_int) -1) {
		/*
		 * The pagehandling string has:
		 * 'M' = EOM, for when parameters must be renegotiated
		 * 'S' = MPS, for when next page uses the same parameters
		 * 'P' = EOP, for the last page to be transmitted
		 */
		if (next != params) {
		    /*
		     * There is no reason to switch from VR_NORMAL to VR_200X100 or from VR_FINE
		     * to VR_200X200 or from VR_R8 to VR_200X400 or vice-versa because they are
		     * essentially the same thing.
		     */
		    Class2Params save;
		    save = next;
		    if (((next.vr == VR_NORMAL)  && (params.vr == VR_200X100)) ||
		        ((next.vr == VR_200X100) && (params.vr == VR_NORMAL))  ||
		        ((next.vr == VR_FINE)    && (params.vr == VR_200X200)) ||
		        ((next.vr == VR_200X200) && (params.vr == VR_FINE))    ||
		        ((next.vr == VR_R8)      && (params.vr == VR_200X400)) ||
		        ((next.vr == VR_200X400) && (params.vr == VR_R8))) {
			next.vr = params.vr;
			if (next != params) next = save;	// only ignore VR difference if there are no other differences
		    }
		}
		if (next != params) {
		    fxStr thismsg = "Document format change between pages requires EOM";
		    if (next.vr != params.vr) thismsg = fxStr::format("%s; VR differs - this page: %d, next page: %d", (const char*) thismsg, params.vr, next.vr);
		    if (next.br != params.br) thismsg = fxStr::format("%s; BR differs - this page: %d, next page: %d", (const char*) thismsg, params.br, next.br);
		    if (next.wd != params.wd) thismsg = fxStr::format("%s; WD differs - this page: %d, next page: %d", (const char*) thismsg, params.wd, next.wd);
		    if (next.ln != params.ln) thismsg = fxStr::format("%s; LN differs - this page: %d, next page: %d", (const char*) thismsg, params.ln, next.ln);
		    if (next.df != params.df) thismsg = fxStr::format("%s; DF differs - this page: %d, next page: %d", (const char*) thismsg, params.df, next.df);
		    if (next.ec != params.ec) thismsg = fxStr::format("%s; EC differs - this page: %d, next page: %d", (const char*) thismsg, params.ec, next.ec);
		    if (next.bf != params.bf) thismsg = fxStr::format("%s; BF differs - this page: %d, next page: %d", (const char*) thismsg, params.bf, next.bf);
		    if (next.st != params.st) thismsg = fxStr::format("%s; ST differs - this page: %d, next page: %d", (const char*) thismsg, params.st, next.st);
		    if (next.jp != params.jp) thismsg = fxStr::format("%s; JP differs - this page: %d, next page: %d", (const char*) thismsg, params.jp, next.jp);
		    traceJob(job, (const char*) thismsg);
		}
		req.pagehandling.append(next == params ? 'S' : 'M');
	    }
	    /*
	     * Record the session parameters needed by each page
	     * so that we can set the initial session parameters
	     * as needed *before* dialing the telephone.  This is
	     * to cope with Class 2 modems that do not properly
	     * implement the +FDIS command.
	     */
	    req.pagehandling.append(next.encodePage());
	    /*
	     * If page is to be chopped (i.e. sent with trailing white
	     * space removed so the received page uses minimal paper),
	     * scan the data and, if possible, record the amount of data
	     * that should not be sent.  The modem drivers will use this
	     * information during transmission if it's actually possible
	     * to do the chop (based on the negotiated session parameters).
	     */
	    if (pagechop == FaxRequest::chop_all ||
	       (pagechop == FaxRequest::chop_last && TIFFLastDirectory(tif)))
	        preparePageChop(req, tif, next, req.pagehandling);
	    params = next;
	}
    }
}

/*
 * Select session parameters according to the info
 * in the TIFF file.  We setup the encoding scheme,
 * page width & length, and vertical-resolution
 * parameters.
 */
void
faxQueueApp::setupParams(TIFF* tif, Class2Params& params, const FaxMachineInfo& info)
{
    params.jp = 0;
    uint16_t compression = 0;
    (void) TIFFGetField(tif, TIFFTAG_COMPRESSION, &compression);
    if (compression == COMPRESSION_NONE) {
	params.jp = JP_COLOR;
	params.df = (u_int) -1;
    } else if (compression == COMPRESSION_CCITTFAX4) {
	params.df = DF_2DMMR;
    } else {
	uint32_t g3opts = 0;
	TIFFGetField(tif, TIFFTAG_GROUP3OPTIONS, &g3opts);
	params.df = (g3opts&GROUP3OPT_2DENCODING ? DF_2DMR : DF_1DMH);
    }

    uint32_t w;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w);
    params.setPageWidthInPixels((u_int) w);

    /*
     * Try to deduce the vertical resolution of the image
     * image.  This can be problematical for arbitrary TIFF
     * images 'cuz vendors sometimes don't give the units.
     * We, however, can depend on the info in images that
     * we generate 'cuz we're careful to include valid info.
     */
    float yres, xres;
    if (TIFFGetField(tif, TIFFTAG_YRESOLUTION, &yres) && TIFFGetField(tif, TIFFTAG_XRESOLUTION, &xres)) {
	uint16_t resunit;
	TIFFGetFieldDefaulted(tif, TIFFTAG_RESOLUTIONUNIT, &resunit);
	if (resunit == RESUNIT_CENTIMETER) {
	    yres *= 25.4;
	    xres *= 25.4;
	}
	params.setRes((u_int) xres, (u_int) yres);
    } else {
	/*
	 * No resolution is specified, try
	 * to deduce one from the image length.
	 */
	uint32_t l;
	TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &l);
	// B4 at 98 lpi is ~1400 lines
	params.setRes(204, (l < 1450 ? 98 : 196));
    }

    /*
     * Select page length according to the image size and
     * vertical resolution.  Note that if the resolution
     * info is bogus, we may select the wrong page size.
     */
    if (info.getMaxPageLengthInMM() != (u_short)-1) {
	uint32_t h;
	TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h);
	params.setPageLengthInMM((u_int)(h / yres));
    } else
	params.ln = LN_INF;
}

void
faxQueueApp::preparePageChop(const FaxRequest& req,
    TIFF* tif, const Class2Params& params, fxStr& pagehandling)
{
    tstrip_t s = TIFFNumberOfStrips(tif)-1;
    TIFFSTRIPBYTECOUNTS* stripbytecount;
    (void) TIFFGetField(tif, TIFFTAG_STRIPBYTECOUNTS, &stripbytecount);
    u_int stripSize = (u_int) stripbytecount[s];
    if (stripSize == 0)
	return;
    u_char* data = new u_char[stripSize];
    if (TIFFReadRawStrip(tif, s, data, stripSize) >= 0) {
	uint16_t fillorder;
	TIFFGetFieldDefaulted(tif, TIFFTAG_FILLORDER, &fillorder);

	MemoryDecoder dec(data, stripSize);
	dec.scanPageForBlanks(fillorder, params);

	float threshold = req.chopthreshold;
	if (threshold == -1)
	    threshold = pageChopThreshold;
	u_int minRows = 0;
	switch(params.vr) {
	    case VR_NORMAL:
	    case VR_200X100:
		minRows = (u_int) (98. * threshold);
		break;
	    case VR_FINE:
	    case VR_200X200:
		minRows = (u_int) (196. * threshold);
		break;
	    case VR_300X300:
		minRows = (u_int) (300. * threshold);
		break;
	    case VR_R8:
	    case VR_R16:
	    case VR_200X400:
		minRows = (u_int) (391. * threshold);
		break;
	}
	if (dec.getLastBlanks() > minRows)
	{
	    pagehandling.append(fxStr::format("Z%04x",
		fxmin((unsigned)0xFFFF, stripSize - (u_int)(dec.getEndOfPage() - data))));
	}
    }
    delete [] data;
}

/*
 * Convert a document into a form suitable
 * for transmission to the remote fax machine.
 */
JobStatus
faxQueueApp::convertDocument(Job& job,
    const FaxItem& req,
    const fxStr& outFile,
    const Class2Params& params,
    fxStr& emsg)
{
    JobStatus status;
    /*
     * Open/create the target file and lock it to guard against
     * concurrent jobs imaging the same document with the same
     * parameters.  The parent will hold the open file descriptor
     * for the duration of the imaging job.  Concurrent jobs will
     * block on flock and wait for the imaging to be completed.
     * Previously imaged documents will be flock'd immediately
     * and reused without delays after verifying that they were
     * last modified *after* the source image.
     *
     * NB: There is a race condition here.  One process may create
     * the file but another may get the shared lock above before
     * the exclusive lock below is captured.  If this happens
     * then the exclusive lock will block temporarily, but the
     * process with the shared lock may attempt to send a document
     * before it's preparation is completed.  We could add a delay
     * before the shared lock but that would slow down the normal
     * case and the window is small--so let's leave it there for now.
     */
    struct stat sin;
    struct stat sout;
    if (Sys::stat(outFile, sout) == 0 && Sys::stat(req.item, sin) == 0) {
	if (sout.st_mtime < sin.st_mtime) {
	    /*
	     * It appears that the target file exists and is 
	     * older than the source image.  (Thus the old target is an image 
	     * file from a previous job.)  This can happen, for example,
	     * if faxqclean isn't being run frequently-enough and faxq
	     * for some reason did not delete the old target file after its job 
	     * completion.  Thus, we delete the old file before continuing.
	     */
	     jobError(job, "Removing old image file: %s (run faxqclean more often)", (const char*) outFile);
	     (void) Sys::unlink(outFile);
	     (void) Sys::unlink(outFile|".color");
	}
    }
    int fd = Sys::open(outFile, O_RDWR|O_CREAT|O_EXCL, 0600);
    int colorfd = -1;
    if (params.jp)
	colorfd = Sys::open(outFile|".color", O_RDWR|O_CREAT|O_EXCL, 0600);
    if (fd == -1) {
	if (errno == EEXIST) {
	    /*
	     * The file already exist, flock it in case it's
	     * being created (we'll block until the imaging
	     * is completed).  Otherwise, the document imaging
	     * has already been completed and we can just use it.
	     */
	    fd = Sys::open(outFile, O_RDWR);	// NB: RDWR for flock emulations
	    if (fd != -1) {
		if (flock(fd, LOCK_SH) == -1) {
		    status = Job::format_failed;
		    emsg = "Unable to lock shared document file {E318}";
		} else
		    status = Job::done;
		(void) Sys::close(fd);		// NB: implicit unlock
	    } else {
		/*
		 * This *can* happen if document preparation done
		 * by another job fails (e.g. because of a time
		 * limit or a malformed PostScript submission).
		 */
		status = Job::format_failed;
		emsg = "Unable to open shared document file {E319}";
	    }
	} else {
	    status = Job::format_failed;
	    emsg = "Unable to create document file {E320}";
	}
	/*
	 * We were unable to open, create, or flock
	 * the file.  This should not happen.
	 */
	if (status != Job::done)
	    jobError(job, "CONVERT DOCUMENT: %s: %m", (const char*) emsg);
    } else {
	(void) flock(fd, LOCK_EX);		// XXX check for errors?
	if (params.jp) (void) flock(colorfd, LOCK_EX);
	/*
	 * Imaged document does not exist, run the document converter
	 * to generate it.  The converter is invoked according to:
	 *   -i jobid		jobid number
	 *   -o file		output (temp) file
	 *   -r <res>		output resolution (dpi)
	 *   -w <pagewidth>	output page width (pixels)
	 *   -l <pagelength>	output page length (mm)
	 *   -m <maxpages>	max pages to generate
	 *   -1|-2|-3		1d, 2d, or 2d-mmr encoding
	 */
	fxStr rbuf = fxStr::format("%u", params.verticalRes());
	fxStr wbuf = fxStr::format("%u", params.pageWidth());
	fxStr lbuf = fxStr::format("%d", params.pageLength());
	fxStr mbuf = fxStr::format("%u", job.getJCI().getMaxSendPages());
	const char* argv[30];
	int ac = 0;
	switch (req.op) {
	case FaxRequest::send_postscript: argv[ac++] = ps2faxCmd; break;
	case FaxRequest::send_pdf:	  argv[ac++] = pdf2faxCmd; break;
	case FaxRequest::send_pcl:	  argv[ac++] = pcl2faxCmd; break;
	case FaxRequest::send_tiff:	  argv[ac++] = tiff2faxCmd; break;
	}
	argv[ac++] = "-i"; argv[ac++] = (const char*)job.jobid;
	argv[ac++] = "-o"; argv[ac++] = outFile;
	argv[ac++] = "-r"; argv[ac++] = (const char*)rbuf;
	argv[ac++] = "-w"; argv[ac++] = (const char*)wbuf;
	argv[ac++] = "-l"; argv[ac++] = (const char*)lbuf;
	argv[ac++] = "-m"; argv[ac++] = (const char*)mbuf;
	if (useUnlimitedLN) argv[ac++] = "-U";
	if (params.jp == JP_COLOR)
	    argv[ac++] = "-color";
	// When df is -1, then it's a color-only job.
	if (params.df != (u_int) -1) {
	    if (params.df == DF_2DMMR)
		argv[ac++] = "-3";
	    else
		argv[ac++] = params.df == DF_1DMH ? "-1" : "-2";
	}
	argv[ac++] = req.item;
	argv[ac] = NULL;
	// XXX the (char* const*) is a hack to force type compatibility
	status = runConverter(job, argv[0], (char* const*) argv, emsg);
	if (status == Job::done) {
	    /*
	     * Many converters exit with zero status even when
	     * there are problems so scan the the generated TIFF
	     * to verify the integrity of the converted data.
	     *
	     * NB: We must reopen the file instead of using the
	     *     previously opened file descriptor in case the
	     *     converter creates a new file with the given
	     *     output filename instead of just overwriting the
	     *     file created above.  This can easily happen if,
	     *     for example, the converter creates a link from
	     *     the input file to the target (e.g. tiff2fax
	     *     does this when no conversion is required).
	     */
	    TIFF* tif = TIFFOpen(outFile, "r");
	    if (tif) {
		while (!TIFFLastDirectory(tif))
		    if (!TIFFReadDirectory(tif)) {
			status = Job::format_failed;
			emsg = "Converted document is not valid TIFF {E321}";
			break;
		    }
		TIFFClose(tif);
	    } else {
		status = Job::format_failed;
		emsg = "Could not reopen converted document to verify format {E322}";
	    }
	    if (status == Job::done)	// discard any debugging output
		emsg = "";
	    else
		jobError(job, "CONVERT DOCUMENT: %s", (const char*) emsg);
	} else if (status == Job::rejected)
	    jobError(job, "SEND REJECT: %s", (const char*) emsg);
	(void) Sys::close(fd);		// NB: implicit unlock
	if (params.jp) (void) Sys::close(colorfd);	// NB: implicit unlock
    }
    return (status);
}

static void
closeAllBut(int fd)
{
    for (int f = Sys::getOpenMax()-1; f >= 0; f--)
	if (f != fd)
	    Sys::close(f);
}

/*
 * Startup a document converter program in a subprocess
 * with the output returned through a pipe.  We could just use
 * popen or similar here, but we want to detect fork failure
 * separately from others so that jobs can be requeued instead
 * of rejected.
 */
JobStatus
faxQueueApp::runConverter(Job& job, const char* app, char* const* argv, fxStr& emsg)
{
    fxStr cmdline(argv[0]);
    for (u_int i = 1; argv[i] != NULL; i++)
	cmdline.append(fxStr::format(" %s", argv[i]));
    traceQueue(job, "CONVERT DOCUMENT: %s", (const char*)cmdline);
    JobStatus status;
    int pfd[2];
    if (pipe(pfd) >= 0) {
	int fd;
	pid_t pid = fork();
	switch (pid) {
	case -1:			// error
	    jobError(job, "CONVERT DOCUMENT: fork: %m");
	    status = Job::requeued;	// job should be retried
	    Sys::close(pfd[1]);
	    break;
	case 0:				// child, exec command
	    if (pfd[1] != STDOUT_FILENO)
		dup2(pfd[1], STDOUT_FILENO);
	    /*
	     * We're redirecting application stdout and stderr back through
	     * the pipe to the faxq parent, but we don't want faxq stdin
	     * to be available to the application, so we close all except
	     * for stdout, redirect stderr to stdout, and redirect stdin
	     * to devnull.  We could, perhaps, just close stdin outright,
	     * but some applications and libc versions require stdin to not
	     * be closed.
	     */
	    closeAllBut(STDOUT_FILENO);
	    dup2(STDOUT_FILENO, STDERR_FILENO);
	    fd = Sys::open(_PATH_DEVNULL, O_RDWR);
	    if (fd != STDIN_FILENO)
	    {
		    dup2(fd, STDIN_FILENO);
		    Sys::close(fd);
	    }
	    setRealIDs();		// prevent root from operating on user-supplied files
	    Sys::execv(app, argv);
	    sleep(3);			// XXX give parent time to catch signal
	    _exit(255);
	    /*NOTREACHED*/
	default:			// parent, read from pipe and wait
	    Sys::close(pfd[1]);
	    if (runConverter1(job, pfd[0], emsg)) {
		int estat = -1;
		(void) Sys::waitpid(pid, estat);
		if (estat)
		    jobError(job, "CONVERT DOCUMENT: exit status %#x", estat);
		switch (estat) {
		case 0:			 status = Job::done; break;
	        case (254<<8):		 status = Job::rejected; break;
		case (255<<8): case 255: status = Job::no_formatter; break;
		default:		 status = Job::format_failed; break;
		}
	    } else {
		kill(pid, SIGTERM);
		(void) Sys::waitpid(pid);
		status = Job::format_failed;
	    }
	    break;
	}
	Sys::close(pfd[0]);
    } else {
	jobError(job, "CONVERT DOCUMENT: pipe: %m");
	status = Job::format_failed;
    }
    return (status);
}

/*
 * Replace unprintable characters with ``?''s.
 */
static void
cleanse(char buf[], int n)
{
    while (--n >= 0)
	if (!isprint(buf[n]) && !isspace(buf[n]))
	    buf[n] = '?';
}

/*
 * Run the interpreter with the configured timeout and
 * collect the output from the interpreter in case there
 * is an error -- this is sent back to the user that
 * submitted the job.
 */
bool
faxQueueApp::runConverter1(Job& job, int fd, fxStr& output)
{
    int n;
    Timeout timer;
    timer.startTimeout(postscriptTimeout*1000);
    char buf[1024];
    while ((n = Sys::read(fd, buf, sizeof (buf))) > 0 && !timer.wasTimeout()) {
	cleanse(buf, n);
	output.append(buf, n);
    }
    timer.stopTimeout();
    if (timer.wasTimeout()) {
	jobError(job, "CONVERT DOCUMENT: job time limit exceeded");
	if (output.length()) output.append("\n");
	output.append("[Job time limit exceeded]");
	return (false);
    } else
	return (true);
}

/*
 * Generate a continuation cover page and insert it in
 * the array of files to be sent.  Note that we assume
 * the cover page command generates PostScript which we
 * immediately image, discarding the PostScript.  We
 * could have the cover page command script do this, but
 * then it would need to know how to invoke the PostScript
 * imager per the job characteristics.  Note that we could
 * optimize things here by updating the pagehandling and
 * page counts for the job instead of resetting pagehandling
 * so that everything just gets recalculated from scratch.
 */
bool
faxQueueApp::makeCoverPage(Job& job, FaxRequest& req, const Class2Params& params)
{
    bool ok = true;
    FaxItem fitem(FaxRequest::send_postscript, 0, fxStr::null, req.cover);
    fxStr cmd(coverCmd
	| quote | quoted(req.qfile)             | enquote
	| quote | quoted(contCoverPageTemplate) | enquote
	| quote | fitem.item                    | enquote
    );
    traceQueue(job, "COVER PAGE: %s", (const char*)cmd);

    const char* argv[5];
    argv[0] = (const char*) coverCmd;
    argv[1] = (const char*) req.qfile;
    argv[2] = (const char*) contCoverPageTemplate;
    argv[3] = (const char*) fitem.item;
    argv[4] = NULL;
    if (runCmd(coverCmd, argv, true)) {
	fxStr emsg;
	fxStr tmp = fitem.item | ";" | params.encodePage();
	if (convertDocument(job, fitem, tmp, params, emsg)) {
	    req.insertFax(0, tmp);
	    req.cover = tmp;			// needed in sendJobDone
	    req.pagehandling = "";		// XXX force recalculation
	} else {
	    jobError(job, "SEND: No continuation cover page, "
		" document conversion failed: %s", (const char*) emsg);
	    ok = false;
	}
	Sys::unlink(fitem.item);
    } else {
	jobError(job,
	    "SEND: No continuation cover page, generation cmd failed");
	ok = false;
    }
    return (ok);
}

const fxStr&
faxQueueApp::pickCmd(const FaxRequest& req)
{
    if (req.jobtype == "pager")
	return (sendPageCmd);
    if (req.jobtype == "uucp")
	return (sendUUCPCmd);
    return (sendFaxCmd);			// XXX gotta return something
}

/*
 * Setup the argument vector and exec a subprocess.
 * This code assumes the command and dargs strings have
 * previously been processed to insert \0 characters
 * between each argument string (see crackArgv below).
 */
static void
doexec(const char* cmd, const fxStr& dargs, const char* devid, const char* files, int nfiles)
{
#define	MAXARGS	128
    const char* av[MAXARGS];
    int ac = 0;
    const char* cp = strrchr(cmd, '/');
    // NB: can't use ?: 'cuz of AIX compiler (XXX)
    if (cp)
	av[ac++] = cp+1;			// program name
    else
	av[ac++] = cmd;
    cp = strchr(cmd,'\0');
    const char* ep = strchr(cmd, '\0');
    while (cp < ep && ac < MAXARGS-4) {		// additional pre-split args
	av[ac++] = ++cp;
	cp = strchr(cp,'\0');
    }
    cp = dargs;
    ep = cp + dargs.length();
    while (cp < ep && ac < MAXARGS-4) {		// pre-split dargs
	av[ac++] = cp;
	cp = strchr(cp,'\0')+1;
    }
    av[ac++] = "-m"; av[ac++] = devid;

    if (! (MAXARGS > ac + nfiles))
    {
	sleep(1);
    	logError("%d files requires %d arguments, max %d", nfiles, ac+nfiles+1, MAXARGS);
	return;
    }
    while (files)
    {
	av[ac++] = files;
	files = strchr(files, ' ');
	/*
	 * We can be naster with memory here - we're exec()ing right way
	 */
	if (files)
	    *(char*)files++ = '\0';
    }

    av[ac] = NULL;
    Sys::execv(cmd, (char* const*) av);
}
#undef MAXARGS

static void
join(fxStr& s, const fxStr& a)
{
    const char* cp = a;
    const char* ep = cp + a.length();
    while (cp < ep) {
	s.append(' ');
	s.append(cp);
	cp = strchr(cp,'\0')+1;
    }
}

static fxStr
joinargs(const fxStr& cmd, const fxStr& dargs)
{
    fxStr s;
    join(s, cmd);
    join(s, dargs);
    return s;
}

void
faxQueueApp::sendJobStart(Job& job, FaxRequest* req)
{
    Job* cjob;
    int nfiles = 1;

    job.start = Sys::now();			// start of transmission
    fxStr files = job.file;
    for (cjob = job.bnext; cjob != NULL; cjob = cjob->bnext) {
	files = files | " " | cjob->file;
	cjob->start = job.start;
	// XXX start deadman timeout on active jobs
	nfiles++;
    }
    
    const fxStr& cmd = pickCmd(*req);
    fxStr dargs(job.getJCI().getArgs());

    DestInfo& di = destJobs[job.dest];
    if (di.getCalls() > 1 && job.getJCI().getMaxConcurrentCalls() == 0) {
	/*
	 * Tell faxsend/pagesend to not increment dials counters if busy signal.
	 */
	if(dargs != "") dargs.append('\0');
	dargs.append("-B");
    }

    int fd;
    pid_t pid = fork();
    switch (pid) {
    case 0:				// child, startup command
	/*
	 * faxq doesn't pay attention to faxsend through stdout/stderr,
	 * and we don't want faxq stdin to be available to the application,
	 * either.  So we close all and redirect stdin to devnull.  We 
	 * could, perhaps, just close stdin outright, but some applications
	 * and libc versions require stdin to not be closed.
	 */
	closeAllBut(-1);		// NB: close 'em all
	fd = Sys::open(_PATH_DEVNULL, O_RDWR);
	if (fd != STDIN_FILENO)
	{
	    dup2(fd, STDIN_FILENO);
	    Sys::close(fd);
	}
	doexec(cmd, dargs, job.modem->getDeviceID(), files, nfiles);
	sleep(10);			// XXX give parent time to catch signal
	_exit(127);
	/*NOTREACHED*/
    case -1:				// fork failed, sleep and retry
	/*
	 * We were unable to start the command because the
	 * system is out of processes.  Take the jobs off the
	 * active list and requeue them for a future time. 
	 * If it appears that the we're doing this a lot,
	 * then lengthen the backoff.
	 */
	Job* njob;
	for (cjob = &job; cjob != NULL; cjob = njob) {
	    njob = cjob->bnext;
	    req = cjob->breq;
	    cjob->remove();			// Remove from active queue
	    delayJob(*cjob, *req, "Could not fork to start job transmission {E341}",
		cjob->start + random() % requeueInterval);
	    delete req;
	}
	break;
    default:				// parent, setup handler to wait
	// joinargs puts a leading space so this looks funny here
	traceQueue(job, "CMD START%s -m %s %s (PID %d)"
	    , (const char*) joinargs(cmd, dargs)
	    , (const char*) job.modem->getDeviceID()
	    , (const char*) files
	    , pid
	);
	job.startSend(pid);
	for (cjob = &job; cjob != NULL; cjob = njob) {
	    cjob->pid = pid;
	    njob = cjob->bnext;
	    Trigger::post(Trigger::SEND_BEGIN, *cjob);
	    delete cjob->breq;		// discard handle (NB: releases lock)
	    cjob->breq = NULL;
	}
	break;
    }
}

void
faxQueueApp::sendJobDone(Job& job, int status)
{
    traceQueue(job, "CMD DONE: exit status %#x", status);
    if (status&0xff)
	logError("Send program terminated abnormally with exit status %#x", status);

    if (numProxyJobs && status&0x8000) numProxyJobs--;	// 0x8000 indicates proxied job

    Job* cjob;
    Job* njob;
    DestInfo& di = destJobs[job.dest];
    if (status&0x8000) {
	di.proxyHangup();
    } else {
	di.hangup();				// do before unblockDestJobs
    }
    if (job.modem) releaseModem(job);		// done with modem
    FaxRequest* req = readRequest(job);
    if (req && (req->status != send_done && req->status != send_reformat)) {
	// prevent turnaround-redialing, delay any blocked jobs
	time_t newtts = req->tts;
	while ((cjob = di.nextBlocked())) {
	    FaxRequest* blockedreq = readRequest(*cjob);
	    if (blockedreq) {
		/*
		 * If the error was call-related such as the errors "Busy", "No answer",
		 * or "No carrier" then that error could legitimately be applied to 
		 * all of the blocked jobs, too, as there would have been no difference 
		 * between the jobs in this respect.  So we make use of the 
		 * "ShareCallFailures" configuration here to control this.  This helps
		 * large sets of jobs going to the same destination fail sooner than 
		 * they would individually and consume less modem attention.
		 */
		if ((req->errorcode == "E001" && (shareCallFailures.find(0, "busy") < shareCallFailures.length() || shareCallFailures == "always")) ||
		    (req->errorcode == "E002" && (shareCallFailures.find(0, "nocarrier") < shareCallFailures.length() || shareCallFailures == "always")) ||
		    (req->errorcode == "E003" && (shareCallFailures.find(0, "noanswer") < shareCallFailures.length() || shareCallFailures == "always")) ||
		    (req->errorcode == "E004" && (shareCallFailures.find(0, "nodialtone") < shareCallFailures.length() || shareCallFailures == "always"))) {
		    blockedreq->status = send_retry;
		    blockedreq->totdials++;
		    delayJob(*cjob, *blockedreq, (const char*) req->notice, newtts);
		    delete blockedreq;
		} else {
		    delayJob(*cjob, *blockedreq, "Delayed by prior call {E342}", newtts);
		    delete blockedreq;
		}
	    }
	}
    } else {
	unblockDestJobs(di, 1);		// force one to unblock
    }
    for (cjob = &job; cjob != NULL; cjob = njob) {
	njob = cjob->bnext;
	if (cjob != &job) req = readRequest(*cjob);	// the first was already read
	if (!req) {
	    time_t now = Sys::now();
	    time_t duration = now - job.start;
	    logError("JOB %s: SEND FINISHED: %s; but job file vanished",
		(const char*) cjob->jobid, fmtTime(duration));
	    setDead(*cjob);
	    continue;
	}
	sendJobDone(*cjob, req);
    }
    pokeScheduler();
}

void
faxQueueApp::sendJobDone(Job& job, FaxRequest* req)
{
    time_t now = Sys::now();
    time_t duration = now - job.start;
    job.bnext = NULL; job.bprev = NULL;		// clear any batching
    job.commid = req->commid;			// passed from subprocess

    Trigger::post(Trigger::SEND_END, job);

    if (req->status == 127) {
	req->notice = "Send program terminated abnormally; unable to exec " |
	    pickCmd(*req) | "{E343}";
	req->status = send_failed;
	logError("JOB %s: %s",
		(const char*)job.jobid, (const char*)req->notice);
    }
    if (req->status == send_reformat) {
	/*
	 * Job requires reformatting to deal with the discovery
	 * of unexpected remote capabilities (either because
	 * the capabilities changed or because the remote side
	 * has never been called before and the default setup
	 * created a capabilities mismatch).  Purge the job of
	 * any formatted information and reset the state so that
	 * when the job is retried it will be reformatted according
	 * to the updated remote capabilities.
	 */
	Trigger::post(Trigger::SEND_REFORMAT, job);
	u_int i = 0;
	while (i < req->items.length()) {
	    FaxItem& fitem = req->items[i];
	    if (fitem.op == FaxRequest::send_fax) {
		req->items.remove(i);
		continue;
	    }
	    if (fitem.isSavedOp())
		fitem.op--;			// assumes order of enum
	    i++;
	}
	req->pagehandling = "";			// force recalculation
	req->status = send_retry;		// ... force retry
    }
    /*
     * If the job did not finish and it is due to be
     * suspended (possibly followed by termination),
     * then treat it as if it is to be retried in case
     * it does get rescheduled.
     */
    if (req->status != send_done && job.suspendPending) {
	req->notice = "Job interrupted by user {E344}";
	req->status = send_retry;
    }
    if (job.killtime == 0 && !job.suspendPending && req->status == send_retry) {
	/*
	 * The job timed out during the send attempt.  We
	 * couldn't do anything then, but now the job can
	 * be cleaned up.  Not sure if the user should be
	 * notified of the requeue as well as the timeout?
	 */
	req->notice = "Kill time expired {E325}";
	updateRequest(*req, job);
	job.state = FaxRequest::state_failed;
	deleteRequest(job, req, Job::timedout, true);
	setDead(job);
    } else if (req->status == send_retry) {
	/*
	 * If a continuation cover page is required for
	 * the retransmission, fixup the job state so
	 * that it'll get one when it's next processed.
	 */
	if (req->cover != "") {
	    /*
	     * Job was previously setup to get a continuation
	     * cover page.  If the generated cover page was not
	     * sent, then delete it so that it'll get recreated.
	     */
	    if (req->items[0].item == req->cover) {
		Sys::unlink(req->cover);
		req->items.remove(0);
	    }
	} else if (req->useccover &&
	  req->npages > 0 && contCoverPageTemplate != "") {
	    /*
	     * Setup to generate a cover page when the job is
	     * retried.  Note that we assume the continuation
	     * cover page will be PostScript (though the
	     * type is not used anywhere just now).
	     */
	    req->cover = docDir | "/cover" | req->jobid | ".ps";
	}
	if (req->tts < now) {
	    /*
	     * Send failed and send app didn't specify a new
	     * tts, bump the ``time to send'' by the requeue
	     * interval, then rewrite the queue file.  This causes
	     * the job to be rescheduled for transmission
	     * at a future time.
	     */
	    req->tts = now + (req->retrytime != 0
		? req->retrytime
		: (requeueInterval>>1) + (random()%requeueInterval));
	    job.tts = req->tts;
	}
	/*
	 * Bump the job priority if is not bulk-style in which case
	 * we dip the job job priority.  This is intended to prevent
	 * non-bulk faxes from becoming buried by new jobs which
	 * could prevent a timely retry.  However, it is also intended
	 * to allow all bulk faxes to be attempted before retrying
	 * any that could not complete on the first attempt.  This 
	 * aids in timely delivery of bulk faxes as a group rather than
	 * preoccupation with individual jobs as is the case with 
	 * non-bulk style jobs.
	 */
	if (job.pri != 255 && job.pri > 190) job.pri++;
	else if (job.pri > 0) job.pri--;
	job.state = (req->tts > now) ?
	    FaxRequest::state_sleeping : FaxRequest::state_ready;
	updateRequest(*req, job);		// update on-disk status
	if (!job.suspendPending) {
	    if (job.isOnList()) job.remove();	// remove from active list
	    if (req->tts > now) {
		traceQueue(job, "SEND INCOMPLETE: requeue for %s; %s",
		    (const char*)strTime(req->tts - now), (const char*)req->notice);
		setSleep(job, req->tts);
		Trigger::post(Trigger::SEND_REQUEUE, job);
		if (job.getJCI().getNotify() != -1) {
		    if (job.getJCI().isNotify(FaxRequest::when_requeued))
			notifySender(job, Job::requeued);
		} else
		    if (req->isNotify(FaxRequest::when_requeued))
			notifySender(job, Job::requeued);
	    } else {
		traceQueue(job, "SEND INCOMPLETE: retry immediately; %s",
		    (const char*)req->notice);
		setReadyToRun(job, false);	// NB: job.tts will be <= now
	    }
	} else					// signal waiting co-thread
	    job.suspendPending = false;
	delete req;				// implicit unlock of q file
    } else {
	// NB: always notify client if job failed
	if (req->status == send_failed) {
	    job.state = FaxRequest::state_failed;
	    deleteRequest(job, req, Job::failed, true, fmtTime(duration));
	} else {
	    job.state = FaxRequest::state_done;
	    deleteRequest(job, req, Job::done, false, fmtTime(duration));
	}
	traceQueue(job, "SEND DONE: %s", (const char*)strTime(duration));
	Trigger::post(Trigger::SEND_DONE, job);
	setDead(job);
    }
}

/*
 * Job Queue Management Routines.
 */

/*
 * Begin the process to insert a job in the queue
 * of ready-to-run jobs.  We run JobControl, and when it's done
 * the job is placed on the ready-to-run queue.
 */
void
faxQueueApp::setReadyToRun(Job& job, bool wait)
{
    if (jobCtrlCmd.length()) {
	const char *app[3];
	app[0] = jobCtrlCmd;
	app[1] = job.jobid;
	app[2] = NULL;
	traceJob(job, "CONTROL");
	int pfd[2], fd;
	if (pipe(pfd) >= 0) {
	    pid_t pid = fork();
	    switch (pid) {
	    case -1:			// error - continue with no JCI
		jobError(job, "JOB CONTROL: fork: %m");
		Sys::close(pfd[0]);
		Sys::close(pfd[1]);
		// When fork fails we need to run ctrlJobDone, since there
		// will be no child signal to start it.
		ctrlJobDone(job, -1);
		break;
	    case 0:				// child, exec command
		if (pfd[1] != STDOUT_FILENO)
		    dup2(pfd[1], STDOUT_FILENO);
		/*
		 * We're redirecting application stdout back through the pipe
		 * to the faxq parent, but application stderr is not meaningful
		 * to faxq, and we don't want faxq stdin to be available to the 
		 * application, either.  However, some libc versions require 
		 * stdin to not be close, and some applications depend on stdin
		 * and stderr to be valid.  So we close all except for stdout, 
		 * and then redirect stdin and stderr to devnull.
		 */
		closeAllBut(STDOUT_FILENO);
		fd = Sys::open(_PATH_DEVNULL, O_RDWR);
		if (fd != STDIN_FILENO)
		{
		    dup2(fd, STDIN_FILENO);
		    Sys::close(fd);
		}
		fd = Sys::open(_PATH_DEVNULL, O_RDWR);
		if (fd != STDERR_FILENO)
		{
		    dup2(fd, STDERR_FILENO);
		    Sys::close(fd);
		}
		traceQueue(job, "JOB CONTROL: %s %s", app[0], app[1]);
		Sys::execv(app[0], (char * const*)app);
		sleep(1);			// XXX give parent time to catch signal
		traceQueue(job, "JOB CONTROL: failed to exec: %m");
		_exit(255);
		/*NOTREACHED*/
	    default:			// parent, read from pipe and wait
		{
		    Sys::close(pfd[1]);
		    int estat = -1;
		    char data[1024];
		    int n;
		    fxStr buf;
		    while ((n = Sys::read(pfd[0], data, sizeof(data))) > 0) {
			buf.append(data, n);
		    }
		    Sys::close(pfd[0]);
		    job.jci = new JobControlInfo(buf);
                    (void) Sys::waitpid(pid, estat);

		    /*
		     * JobControl modification of job priority must be
		     * handled before ctrlJobDone, as that's where the
		     * job is placed into runq based on the priority.
		     */
		    if (job.getJCI().getPriority() != -1) {
			job.pri = job.getJCI().getPriority();
			if (job.pri > 255) job.pri = 255;
			else if (job.pri < 0) job.pri = 0;
		    }

		    ctrlJobDone(job, estat);
		}
		break;
	    }
	} else {
	    // If our pipe fails, we can't run the child, but we still
	    // Need ctrlJobDone to be called to proceed this job
	    ctrlJobDone(job, -1);
	}
    } else {
    	ctrlJobDone(job, 0);
    }
}

/*
 * Insert the job into the runq.  We have finished
 * all the JobControl execution
 */
void
faxQueueApp::ctrlJobDone(Job& job, int status)
{
    if (status) {
	logError("JOB %s: bad exit status %#x from sub-fork",
	    (const char*) job.jobid, status);
    }
    blockSignals();
    JobIter iter(runqs[JOBHASH(job.pri)]);
    for (; iter.notDone() && (iter.job().pri < job.pri || 
      (iter.job().pri == job.pri && iter.job().tts <= job.tts)); iter++)
	;
    job.state = FaxRequest::state_ready;
    job.insert(iter.job());
    job.pid = 0;
    releaseSignals();
    traceJob(job, "READY");
    Trigger::post(Trigger::JOB_READY, job);
}

/*
 * Place a job on the queue of jobs waiting to run
 * and start the associated timer.
 */
void
faxQueueApp::setSleep(Job& job, time_t tts)
{
    blockSignals();
    JobIter iter(sleepq);
    for (; iter.notDone() && iter.job().tts <= tts; iter++)
	;
    job.insert(iter.job());
    job.startTTSTimer(tts);
    releaseSignals();
    traceJob(job, "SLEEP FOR %s", (const char*)strTime(tts - Sys::now()));
    Trigger::post(Trigger::JOB_SLEEP, job);
}

/*
 * Process a job that's finished.  The corpse gets placed
 * on the deadq and is reaped the next time the scheduler
 * runs.  If any jobs are blocked waiting for this job to
 * complete, one is made ready to run.
 */
void
faxQueueApp::setDead(Job& job)
{
    job.stopTTSTimer();
    if (job.state != FaxRequest::state_done 
      && job.state != FaxRequest::state_failed)
	job.state = FaxRequest::state_failed;
    job.suspendPending = false;
    traceJob(job, "DEAD");
    Trigger::post(Trigger::JOB_DEAD, job);
    removeDestInfoJob(job);
    if (job.isOnList())			// lazy remove from active list
	job.remove();
    job.insert(*deadq.next);		// setup job corpus for reaping
    if (job.modem)			// called from many places
	releaseModem(job);
    pokeScheduler();
}

/*
 * Place a job on the list of jobs actively being processed.
 */
void
faxQueueApp::setActive(Job& job)
{
    job.state = FaxRequest::state_active;
    traceJob(job, "ACTIVE");
    Trigger::post(Trigger::JOB_ACTIVE, job);
    job.insert(*activeq.next);
}

/*
 * Place a job on the list of jobs not being scheduled.
 */
void
faxQueueApp::setSuspend(Job& job)
{
    job.state = FaxRequest::state_suspended;
    traceJob(job, "SUSPEND");
    Trigger::post(Trigger::JOB_SUSPEND, job);
    job.insert(*suspendq.next);
}

/*
 * Create a new job entry and place them on the
 * appropriate queue.  A kill timer is also setup
 * for the job.
 */
bool
faxQueueApp::submitJob(FaxRequest& req, bool checkState)
{
    Job* job = new Job(req);
    traceJob(*job, "CREATE");
    Trigger::post(Trigger::JOB_CREATE, *job);
    return (submitJob(*job, req, checkState));
}

bool
faxQueueApp::submitJob(Job& job, FaxRequest& req, bool checkState)
{
    /*
     * Check various submission parameters.  We setup the
     * canonical version of the destination phone number
     * first so that any rejections that cause the notification
     * script to be run will return a proper value for the
     * destination phone number.
     */
    job.dest = canonicalizePhoneNumber(req.number);
    if (job.dest == "") {
	if (req.external == "")			// NB: for notification logic
	    req.external = req.number;
	rejectSubmission(job, req,
	    "REJECT: Unable to convert dial string to canonical format {E327}");
	return (false);
    }
    req.canonical = job.dest;
    time_t now = Sys::now();
    if (req.killtime <= now) {
	timeoutJob(job, req);
	return (false);
    }
    if (!Modem::modemExists(req.modem, true) && !ModemGroup::find(req.modem)) {
	rejectSubmission(job, req,
	    "REJECT: Requested modem " | req.modem | " is not registered {E328}");
	return (false);
    }
    if (req.items.length() == 0) {
	rejectSubmission(job, req, "REJECT: No work found in job file {E329}");
	return (false);
    }
    if (req.pagewidth > 303) {
	rejectSubmission(job, req,
	    fxStr::format("REJECT: Page width (%u) appears invalid {E330}",
		req.pagewidth));
	return (false);
    }
    /*
     * Verify the killtime is ``reasonable''; otherwise
     * select (through the Dispatcher) may be given a
     * crazy time value, potentially causing problems.
     */
    if (req.killtime-now > 365*24*60*60) {	// XXX should be based on tts
	rejectSubmission(job, req,
	    fxStr::format("REJECT: Job expiration time (%u) appears invalid {E331}",
		req.killtime));
	return (false);
    }
    if (checkState) {
	/*
	 * Check the state from queue file and if
	 * it indicates the job was not being
	 * scheduled before then don't schedule it
	 * now.  This is used when the scheduler
	 * is restarted and reading the queue for
	 * the first time.
	 *
	 * NB: We reschedule blocked jobs in case
	 *     the job that was previously blocking
	 *     it was removed somehow.
	 */
	switch (req.state) {
	case FaxRequest::state_suspended:
	    setSuspend(job);
	    return (true);
	case FaxRequest::state_done:
	case FaxRequest::state_failed:
	    setDead(job);
	    return (true);
	}
    }
    if (req.useccover && (req.serverdocover || req.skippedpages || req.skippages) && contCoverPageTemplate != "") {
	/*
	 * The user submitted a job with "skipped" pages.  This equates
	 * to a user-initiated resubmission.  Add a continuation coverpage
	 * if appropriate.
	 */
	req.cover = docDir | "/cover" | req.jobid | ".ps";
    }
    /*
     * Put the job on the appropriate queue
     * and start the job kill timer.
     */
    if (req.tts > now) {			// scheduled for future
	/*
	 * Check time-to-send as for killtime above.
	 */
	if (req.tts - now > 365*24*60*60) {
	    rejectSubmission(job, req,
		fxStr::format("REJECT: Time-to-send (%u) appears invalid {E332}",
		    req.tts));
	    return (false);
	}
	job.startKillTimer(req.killtime);
	job.state = FaxRequest::state_pending;
	setSleep(job, job.tts);
    } else {					// ready to go now
	job.startKillTimer(req.killtime);
	setReadyToRun(job, true);
	pokeScheduler();
    }
    updateRequest(req, job);
    return (true);
}

/*
 * Reject a job submission.
 */
void
faxQueueApp::rejectSubmission(Job& job, FaxRequest& req, const fxStr& reason)
{
    Trigger::post(Trigger::JOB_REJECT, job);
    req.status = send_failed;
    req.notice = reason;
    traceServer("JOB %s: ", (const char*)job.jobid, (const char*)reason);
    deleteRequest(job, req, Job::rejected, true);
    setDead(job);				// dispose of job
}

/*
 * Suspend a job by removing it from whatever
 * queue it's currently on and/or stopping any
 * timers.  If the job has an active subprocess
 * then the process is optionally sent a signal
 * and we wait for the process to stop before
 * returning to the caller.
 */
bool
faxQueueApp::suspendJob(Job& job, bool abortActive)
{
    if (job.suspendPending)			// already being suspended
	return (false);
    switch (job.state) {
    case FaxRequest::state_active:
	/*
	 * Job is being handled by a subprocess; optionally
	 * signal the process and wait for it to terminate
	 * before returning.  We disable the kill timer so
	 * that if it goes off while we wait for the process
	 * to terminate the process completion work will not
	 * mistakenly terminate the job (see sendJobDone).
	 */
	job.suspendPending = true;		// mark thread waiting
	if (abortActive)
	    (void) kill(job.pid, SIGTERM);	// signal subprocess
	job.stopKillTimer();
	while (job.suspendPending)		// wait for subprocess to exit
	    Dispatcher::instance().dispatch();
	/*
	 * Recheck the job state; it may have changed while
	 * we were waiting for the subprocess to terminate.
	 */
	if (job.state != FaxRequest::state_done &&
	  job.state != FaxRequest::state_failed)
	    break;
	/* fall thru... */
    case FaxRequest::state_done:
    case FaxRequest::state_failed:
	return (false);
    case FaxRequest::state_sleeping:
    case FaxRequest::state_pending:
	job.stopTTSTimer();			// cancel timeout
	/* fall thru... */
    case FaxRequest::state_suspended:
    case FaxRequest::state_ready:
	break;
    case FaxRequest::state_blocked:
	/*
	 * Decrement the count of job blocked to
	 * to the same destination.
	 */
	destJobs[job.dest].unblock(job);
	break;
    }

    /*
     * We must remove any DestInfo stuff this is recorded in
     * When the job is resubmitted (or killed), we don't know
     * when (could be hours/never), or even if the dest number
     * will be the same
     */
    removeDestInfoJob(job);
    if (job.isOnList()) job.remove();		// remove from old queue
    job.stopKillTimer();			// clear kill timer
    return (true);
}

/*
 * Suspend a job and place it on the suspend queue.
 * If the job is currently active then we wait for
 * it to reach a state where it can be safely suspended.
 * This control is used by clients that want to modify
 * the state of a job (i.e. suspend, modify, submit).
 */
bool
faxQueueApp::suspendJob(const fxStr& jobid, bool abortActive)
{
    Job* job = Job::getJobByID(jobid);
    if (job && suspendJob(*job, abortActive)) {
	setSuspend(*job);
	FaxRequest* req = readRequest(*job);
	if (req) {
	    updateRequest(*req, *job);
	    delete req;
	}
	return (true);
    } else
	return (false);
}

/*
 * Terminate a job in response to a command message.
 * If the job is currently running the subprocess is
 * sent a signal to request that it abort whatever
 * it's doing and we wait for the process to terminate.
 * Otherwise, the job is immediately removed from
 * the appropriate queue and any associated resources
 * are purged.
 */
bool
faxQueueApp::terminateJob(const fxStr& jobid, JobStatus why)
{
    Job* job = Job::getJobByID(jobid);
    if (job && suspendJob(*job, true)) {
	job->state = FaxRequest::state_failed;
	Trigger::post(Trigger::JOB_KILL, *job);
	FaxRequest* req = readRequest(*job);
	if (req) {
	    req->notice = "Job aborted by request {E345}";
	    deleteRequest(*job, req, why, why != Job::removed);
	}
	setDead(*job);
	return (true);
    } else
	return (false);
}

/*
 * Reject a job at some time before it's handed off to the server thread.
 */
void
faxQueueApp::rejectJob(Job& job, FaxRequest& req, const fxStr& reason)
{
    req.status = send_failed;
    req.notice = reason;
    traceServer("JOB %s: %s",
	    (const char*)job.jobid, (const char*)reason);
    job.state = FaxRequest::state_failed;
    Trigger::post(Trigger::JOB_REJECT, job);
    setDead(job);				// dispose of job
}

/*
 * Deal with a job that's blocked by a concurrent call.
 */
void
faxQueueApp::blockJob(Job& job, FaxRequest& req, const char* mesg)
{
    int old_state = job.state;
    job.state = FaxRequest::state_blocked;
    req.notice = mesg;
    updateRequest(req, job);
    traceQueue(job, "%s", mesg);
    if (old_state != FaxRequest::state_blocked) {
	if (job.getJCI().getNotify() != -1) {
	    if (job.getJCI().isNotify(FaxRequest::when_requeued))
		notifySender(job, Job::blocked);
	} else
	    if (req.isNotify(FaxRequest::when_requeued))
		notifySender(job, Job::blocked);
    }
    Trigger::post(Trigger::JOB_BLOCKED, job);
}

/*
 * Requeue a job that's delayed for some reason.
 */
void
faxQueueApp::delayJob(Job& job, FaxRequest& req, const char* mesg, time_t tts)
{
    job.state = FaxRequest::state_sleeping;
    fxStr reason(mesg);
    job.tts = tts;
    req.tts = tts;
    time_t delay = tts - Sys::now();
    req.notice = reason;
    updateRequest(req, job);
    traceQueue(job, "%s: requeue for %s",
	    (const char*)mesg, (const char*)strTime(delay));
    setSleep(job, tts);
    Trigger::post(Trigger::JOB_DELAYED, job);
    if (job.getJCI().getNotify() != -1) {
	if (job.getJCI().isNotify(FaxRequest::when_requeued))
	    notifySender(job, Job::requeued); 
    } else
	if (req.isNotify(FaxRequest::when_requeued))
	    notifySender(job, Job::requeued); 
    if (job.modem != NULL)
	releaseModem(job);
}

void
faxQueueApp::queueAccounting(Job& job, FaxRequest& req, const char* type)
{
    FaxAcctInfo ai;
    ai.jobid = (const char*) req.jobid;
    ai.jobtag = (const char*) req.jobtag;
    ai.user = (const char*) req.mailaddr;
    ai.start = Sys::now();
    ai.duration = 0;
    ai.conntime = 0;
    if (strstr(type, "PROXY")) {
	ai.duration = req.duration;
	ai.conntime = req.conntime;
	ai.commid = (const char*) req.commid;
	ai.device = (const char*) req.modemused;
    } else {
	ai.duration = 0;
	ai.conntime = 0;
	ai.commid = "";
	ai.device = "";
    }
    ai.dest = (const char*) req.external;
    ai.csi = "";
    ai.npages = req.npages;
    ai.params = 0;
    if (req.status == send_done)
	ai.status = "";
    else {
	ai.status = req.notice;
    }
    if (strstr(type, "SUBMIT"))
	ai.status = "Submitted";
    CallID empty_callid;
    ai.callid = empty_callid;
    ai.owner = (const char*) req.owner;
    ai.faxdcs = "";
    ai.jobinfo = fxStr::format("%u/%u/%u/%u/%u/%u/%u", 
	req.totpages, req.ntries, req.ndials, req.totdials, req.maxdials, req.tottries, req.maxtries);
    pid_t pid = fork();
    switch (pid) {
	case -1:			// error
	    if (!ai.record(type))
		logError("Error writing %s accounting record, dest=%s",
		    type, (const char*) ai.dest);
	    break;
	case 0:				// child
	    if (!ai.record(type))
		logError("Error writing %s accounting record, dest=%s",
		    type, (const char*) ai.dest);
	    _exit(255);
	    /*NOTREACHED*/
	default:			// parent
	    Dispatcher::instance().startChild(pid, this);
	    break;
    }
}

/*
 * Process the job whose kill time expires.  The job is
 * terminated unless it is currently being tried, in which
 * case it's marked for termination after the attempt is
 * completed.
 */
void
faxQueueApp::timeoutJob(Job& job)
{
    traceQueue(job, "KILL TIME EXPIRED");
    Trigger::post(Trigger::JOB_TIMEDOUT, job);
    if (job.state != FaxRequest::state_active) {
	if (job.isOnList())
	    job.remove();			// i.e. remove from sleep queue
	job.state = FaxRequest::state_failed;
	FaxRequest* req = readRequest(job);
	if (req) {
	    req->notice = "Kill time expired {E325}";
	    deleteRequest(job, req, Job::timedout, true);
	}
	setDead(job);
    } else
	job.killtime = 0;			// mark job to be removed
}

/*
 * Like above, but called for a job that times
 * out at the point at which it is submitted (e.g.
 * after the server is restarted).  The work here
 * is subtley different; the q file must not be
 * re-read because it may result in recursive flock
 * calls which on some systems may cause deadlock
 * (systems that emulate flock with lockf do not
 * properly emulate flock).
 */
void
faxQueueApp::timeoutJob(Job& job, FaxRequest& req)
{
    job.state = FaxRequest::state_failed;
    traceQueue(job, "KILL TIME EXPIRED");
    Trigger::post(Trigger::JOB_TIMEDOUT, job);
    req.notice = "Kill time expired {E325}";
    deleteRequest(job, req, Job::timedout, true);
    setDead(job);
}

/*
 * Resubmit an existing job or create a new job
 * using the specified job description file.
 */
bool
faxQueueApp::submitJob(const fxStr& jobid, bool checkState, bool nascent)
{
    Job* job = Job::getJobByID(jobid);
    if (job) {
	bool ok = false;
	if (job->state == FaxRequest::state_suspended) {
	    job->remove();			// remove from suspend queue
	    FaxRequest* req = readRequest(*job);// XXX need better mechanism
	    if (req) {
		job->update(*req);		// update job state from file
		ok = submitJob(*job, *req);	// resubmit to scheduler
		delete req;			// NB: unlock qfile
	    } else
		setDead(*job);			// XXX???
	} else if (job->state == FaxRequest::state_done ||
	  job->state == FaxRequest::state_failed)
	    jobError(*job, "Cannot resubmit a completed job");
	else
	    ok = true;				// other, nothing to do
	return (ok);
    }
    /*
     * Create a job from a queue file and add it
     * to the scheduling queues.
     */
    fxStr filename(FAX_SENDDIR "/" FAX_QFILEPREF | jobid);
    if (!Sys::isRegularFile(filename)) {
	logError("JOB %s: qfile %s is not a regular file: %s",
	    (const char*) jobid, (const char*) filename, strerror(errno));
	return (false);
    }
    bool status = false;
    int fd = Sys::open(filename, O_RDWR);
    if (fd >= 0) {
	if (flock(fd, LOCK_SH) >= 0) {
	    FaxRequest req(filename, fd);
	    /*
	     * There are four possibilities:
	     *
	     * 1. The queue file was read properly and the job
	     *    can be submitted.
	     * 2. There were problems reading the file, but
	     *    enough information was obtained to purge the
	     *    job from the queue.
	     * 3. The job was previously submitted and completed
	     *    (either with success or failure).
	     * 4. Insufficient information was obtained to purge
	     *    the job; just skip it.
	     */
	    bool reject;
	    if (req.readQFile(reject) && !reject &&
	      req.state != FaxRequest::state_done &&
	      req.state != FaxRequest::state_failed) {
		status = submitJob(req, checkState);
		if (nascent) queueAccounting(*job, req, "SUBMIT");
	    } else if (reject) {
		Job job(req);
		job.state = FaxRequest::state_failed;
		req.status = send_failed;
		req.notice = "Invalid or corrupted job description file {E326}";
		traceServer("JOB %s : %s", (const char*)jobid, (const char*) req.notice);
		// NB: this may not work, but we try...
		deleteRequest(job, req, Job::rejected, true);
	    } else if (req.state == FaxRequest::state_done ||
	      req.state == FaxRequest::state_failed) {
		logError("JOB %s: Cannot resubmit a completed job",
		    (const char*) jobid);
	    } else
		traceServer("%s: Unable to purge job, ignoring it",
			(const char*)filename);
	} else
	    logError("JOB %s: Could not lock job file; %m.",
		(const char*) jobid);
	Sys::close(fd);
    } else
	logError("JOB %s: Could not open job file; %m.", (const char*) jobid);
    return (status);
}

/*
 * Process the expiration of a job's time-to-send timer.
 * The job is moved to the ready-to-run queues and the
 * scheduler is poked.
 */
void
faxQueueApp::runJob(Job& job)
{
    if (job.state != FaxRequest::state_failed) {	// don't run a dead job corpus
	if (job.isOnList()) job.remove();
	setReadyToRun(job, true);
	FaxRequest* req = readRequest(job);
	if (req) {
	    updateRequest(*req, job);
	    delete req;
	}
    }
    /*
     * In order to deliberately batch jobs by using a common
     * time-to-send we need to give time for the other jobs'
     * timers to expire and to enter the run queue before
     * running the scheduler.  Thus the scheduler is poked
     * with a delay.
     */
    pokeScheduler(1);
}

/*
 * Process the DestInfo job-block list
 * for this job.  If the job is active and blocking other
 * jobs, we need to unblock...
 */
#define	isOKToCall(di, dci, n) \
    (di.getCalls()+n <= dci.getMaxConcurrentCalls())

void
faxQueueApp::unblockDestJobs(DestInfo& di, u_int force)
{
    /*
     * Check if there are blocked jobs waiting to run
     * and that there is now room to run one.  If so,
     * take jobs off the blocked queue and make them
     * ready for processing.
     */
    Job* jb;
    u_int n = 1, b = 1;
    while ((jb = di.nextBlocked())) {
	if (force || jb->getJCI().getMaxConcurrentCalls() == 0 || isOKToCall(di, jb->getJCI(), n)) {
	    FaxRequest* req = readRequest(*jb);
	    if (!req) {
		setDead(*jb);
		continue;
	    }
	    setReadyToRun(*jb, false);
	    if (!di.supportsBatching()) n++;
	    else if (++b > maxBatchJobs) {
		n++;
		b -= maxBatchJobs;
	    }
	    req->notice = "";
	    updateRequest(*req, *jb);
	    delete req;
	    if (force) force--;
	    /*
	     * We check isOKToCall again here now to avoid di.nextBlocked
	     * which would pull jb from the blocked list and then possibly
	     * require us to re-block it.
	     */
	    if (di.getBlocked() && !force && jb->getJCI().getMaxConcurrentCalls() != 0 && !isOKToCall(di, jb->getJCI(), n)) {
		traceQueue("Continue BLOCK on %d job(s) to %s, current calls: %d, max concurrent calls: %d", 
		    di.getBlocked(), (const char*) jb->dest, di.getCalls()+n-1, jb->getJCI().getMaxConcurrentCalls());
		break;
	    }
	} else {
	    /*
	     * unblockDestJobs was called, but a new
	     * call cannot be placed.  This would be
	     * unusual, but because di.nextBlocked
	     * removed jb from the di list, we need
	     * to put it back.
	     */
	    di.block(*jb);
	    traceQueue("Continue BLOCK on %d job(s) to %s, current calls: %d, max concurrent calls: %d", 
		di.getBlocked(), (const char*) jb->dest, di.getCalls()+n-1, jb->getJCI().getMaxConcurrentCalls());
	    break;
	}
    }
}

void
faxQueueApp::removeDestInfoJob(Job& job)
{
    DestInfo& di = destJobs[job.dest];
    di.done(job);			// remove from active destination list
    di.updateConfig();			// update file if something changed
    if (di.isEmpty()) {
	/*
	 * This is the last job to the destination; purge
	 * the entry from the destination jobs database.
	 */
	destJobs.remove(job.dest);
    }
}

/*
 * Compare two job requests to each other and to a selected
 * job to see if they can be batched together.
 */
bool
faxQueueApp::areBatchable(FaxRequest& reqa, FaxRequest& reqb, Job& job, Job& cjob)
{
    // make sure the job's modem is in the requested ModemGroup 
    if (!job.modem->isInGroup(reqb.modem)) return(false);
    // make sure cjob's TimeOfDay is for now
    time_t now = Sys::now();
    if ((cjob.getJCI().nextTimeToSend(now) != now) || (cjob.tod.nextTimeOfDay(now) != now)) return (false);
    return(true);
}

class MySendFaxClient : public SendFaxClient {
public:
    MySendFaxClient();
    ~MySendFaxClient();
};
MySendFaxClient::MySendFaxClient() {}
MySendFaxClient::~MySendFaxClient() {}

static bool
writeFile(void* ptr, const char* buf, int cc, fxStr&)
{
    int* fd = (int*) ptr;
    (void) Sys::write(*fd, buf, cc);
    return (true);
}

#define COMPLETE 2

/*
 * Send a fax job via a proxy HylaFAX server.  We do this by utilizing SendFaxJob jobWait.
 *
 * This does not currently work for pager requests or fax polls.
 *
 * Also not currently supported is job suspension or termination of the proxy job on the 
 * proxy host.  Therefore, actions such as faxrm or faxalter will not currently propagate
 * to the proxy.
 */
void
faxQueueApp::sendViaProxy(Job& job, FaxRequest& req)
{
    pid_t pid = fork();
    switch (pid) {
	case -1:			// error
	    logError("Error forking for proxy-send of job %s.", (const char*) job.jobid);
	    break;
	case 0:				// child
	    {
		Trigger::post(Trigger::SEND_BEGIN, job);

		MySendFaxClient* client = new MySendFaxClient;
		client->readConfig(FAX_SYSCONF);
		SendFaxJob& rjob = client->addJob();
		if (job.getJCI().getDesiredDF() != -1) req.desireddf = job.getJCI().getDesiredDF();
		// Since we want to send "now", and because we can't ensure that the remote clock matches ours
		// we deliberately do not do the following.  The remote will default to its "now".
		// rjob.setSendTime("now");
		rjob.setPriority(job.pri);
		// We use a special killtime "!" specifier to indicate that we're providing the raw 
		// LASTTIME in order to provide a killtime that is relative to the remote clock.
		rjob.setKillTime((const char*) fxStr::format("!%02d%02d%02d", 
		    (req.killtime - Sys::now())/(24*60*60), 
		   ((req.killtime - Sys::now())/(60*60))%24, 
		   ((req.killtime - Sys::now())/60)%60 ));
		rjob.setDesiredDF(req.desireddf);
		rjob.setMinSpeed(req.minbr);
		rjob.setDesiredSpeed(req.desiredbr);
		if (req.faxname != "") rjob.setFaxName(req.faxname);
		//rjob.setDesiredEC(req.desiredec);		// disabled for compatibility with HylaFAX 4.1.x servers
		if (job.getJCI().getProxyTagLineFormat().length()) {
		    rjob.setTagLineFormat(job.getJCI().getProxyTagLineFormat());
		} else {
		    if (req.desiredtl) rjob.setTagLineFormat(req.tagline);
		}
		if (req.timezone != "") rjob.setTimeZone(req.timezone);
		rjob.setUseXVRes(req.usexvres);
		client->setHost(job.getJCI().getProxy());
		if (job.getJCI().getProxyJobTag().length()) {
		    rjob.setJobTag(job.getJCI().getProxyJobTag());
		} else {
		    rjob.setJobTag(job.jobid);
		}
		rjob.setVResolution(req.resolution);
		rjob.setDesiredMST(req.desiredst);
		rjob.setAutoCoverPage(false);
		if (job.getJCI().getProxyMailbox().length())
		    rjob.setMailbox(job.getJCI().getProxyMailbox());
		if (job.getJCI().getProxyNotification().length())
		    rjob.setNotification((const char*) job.getJCI().getProxyNotification());
		else
		    rjob.setNotification("none");
		rjob.setSkippedPages(req.skippedpages);
		rjob.setSkipPages(req.skippages+req.npages);	// requires that the proxy be capable of skipping entire documents
		rjob.setNoCountCover(req.nocountcover);
		rjob.setUseColor(req.usecolor);
		rjob.setUseSSLFax(req.usesslfax);
		if (job.getJCI().getPageSize().length()) {
		    rjob.setPageSize(job.getJCI().getPageSize());
		} else {
		    PageSizeInfo* info = PageSizeInfo::getPageSizeBySize(req.pagewidth, req.pagelength);
		    if (info) rjob.setPageSize(info->abbrev());
		}
		if (job.getJCI().getProxyTSI().length()) {
		    rjob.setTSI(job.getJCI().getProxyTSI());
		} else {
		    if (req.tsi != "") rjob.setTSI(req.tsi);
		}
		int maxTries = 0;
		if (job.getJCI().getProxyTries() > 0)
		    maxTries = job.getJCI().getProxyTries();
		else
		    maxTries = req.maxtries - req.tottries;	// don't let the proxy repeat tries already made
		rjob.setMaxRetries(maxTries);
		int maxDials = 0;
		if (job.getJCI().getProxyDials() > 0)
		    maxDials = job.getJCI().getProxyDials();
		else
		    maxDials = req.maxdials - req.totdials;	// don't let the proxy repeat dials already made
		rjob.setMaxDials(maxDials);
		if (req.faxnumber != "") rjob.setFaxNumber(req.faxnumber);
		rjob.setDialString(req.number);
		for (u_int i = 0; i < req.items.length(); i++) {
		    switch (req.items[i].op) {
			case FaxRequest::send_tiff:
			case FaxRequest::send_tiff_saved:
			case FaxRequest::send_pdf:
			case FaxRequest::send_pdf_saved:
			case FaxRequest::send_postscript:
			case FaxRequest::send_postscript_saved:
			case FaxRequest::send_pcl:
			case FaxRequest::send_pcl_saved:
			    client->addFile(req.items[i].item);
			    break;
		    }
		}
		u_short prevPages = req.npages;		// queueAccounting() needs the pages sent only by the proxy, but updateRequest() needs the full count
		bool status = false;
		bool jobprepared = req.pagehandling.length();
		fxStr emsg;
		if (client->callServer(emsg)) {
		    status = client->login(job.getJCI().getProxyUser().length() ? job.getJCI().getProxyUser() : req.owner, 
					job.getJCI().getProxyPass().length() ? (const char*) job.getJCI().getProxyPass() : NULL, emsg) 
				&& client->prepareForJobSubmissions(emsg)
				&& client->submitJobs(emsg);
		    if (status) {
			fxStr r;
			fxStr rjobid = client->getCurrentJob();
			traceQueue(job, "submitted to proxy host %s as job %s", 
			    (const char*) job.getJCI().getProxy(), (const char*) rjobid);
			req.modemused = rjobid | "@" | job.getJCI().getProxy();
			req.notice = "delivered to proxy host " | job.getJCI().getProxy() | " as job " | rjobid;
			updateRequest(req, job);
			int waits = 0;
			time_t waitstart = Sys::now();
			while (!client->jobWait((const char*) rjobid) && waits++ < job.getJCI().getProxyReconnects()) {
			    /*
			     * Our wait failed, and excepting a fault on the proxy this
			     * can only mean that the connection was lost.  Reconnect
			     * and re-start our wait.
			     */
			    time_t sleeptime = waitstart + 60 - Sys::now();
			    if (sleeptime < 1) sleeptime = 2;
			    logError("PROXY SEND: (job %s) lost connection to %s, attempt %d.  Attempting reconnection in %d seconds.", (const char*) job.jobid, (const char*) job.getJCI().getProxy(), waits, sleeptime);
			    client->hangupServer();
			    sleep(sleeptime);
			    waitstart = Sys::now();
			    status = false;
			    if (client->callServer(emsg)) {
				status = client->login(job.getJCI().getProxyUser().length() ? job.getJCI().getProxyUser() : req.owner, 
					job.getJCI().getProxyPass().length() ? (const char*) job.getJCI().getProxyPass() : NULL, emsg) &&
					client->setCurrentJob(rjobid);
				if (status) {
				    logError("PROXY SEND: (job %s) reconnected to %s.  Resuming wait for job %s.", (const char*) job.jobid, (const char*) job.getJCI().getProxy(), (const char*) rjobid);
				    emsg = "";
				    waits = 0;
				}
			    }
			    if (emsg != "") logError("PROXY SEND: (job %s) server %s: %s", (const char*) job.jobid, (const char*) job.getJCI().getProxy(), (const char*) emsg);
			}
			/*
			 * There is a long wait here now... possibly VERY long.
			 * When jobWait is done, then we query the proxy
			 * for the various job data and update the request here.
			 */
			req.skippages = 0;
			if (status && client->command((const char*) fxStr::format("JOB %s", (const char*) rjobid)) == COMPLETE) {
			    if (client->jobParm("conntime")) {
				r = client->getLastResponse(); r.remove(0, r.length() > 4 ? 4 : r.length());
				req.conntime = atoi((const char*) r);
			    }
			    if (client->jobParm("duration")) {
				r = client->getLastResponse(); r.remove(0, r.length() > 4 ? 4 : r.length());
				req.duration = atoi((const char*) r);
			    }
			    int pagessent = 0;
			    if (client->jobParm("npages")) {
				r = client->getLastResponse(); r.remove(0, r.length() > 4 ? 4 : r.length());
				pagessent = atoi((const char*) r);
				// The following are for preparePageHandling below
				req.npages += pagessent;
				req.skippedpages += req.skippages;
				req.skippages = 0;
				req.pagehandling = "";
			    }
			    if (client->jobParm("totpages")) {
				r = client->getLastResponse(); r.remove(0, r.length() > 4 ? 4 : r.length());
				u_short totpages = atoi((const char*) r);
				if (totpages > req.totpages) req.totpages = totpages;	// if the formatting occurred on the proxy, we'll need to update
			    }
			    if (client->jobParm("commid")) {
				r = client->getLastResponse(); r.remove(0, r.length() > 4 ? 4 : r.length());
				req.commid = r;
			    }
			    if (client->jobParm("status")) {
				r = client->getLastResponse(); r.remove(0, r.length() > 4 ? 4 : r.length());
				req.notice = r;
			    } else {
				req.notice = "unknown status on proxy";
			    }
			    u_int tries = 0;
			    if (client->jobParm("ntries")) {
				r = client->getLastResponse(); r.remove(0, r.length() > 4 ? 4 : r.length());
				tries = atoi((const char*) r);
				if (tries < (u_int) maxTries && strstr((const char*) req.notice, "oo many attempts to send")) tries = maxTries;	// caught in a lie
				if (tries < (u_int) maxTries && strstr((const char*) req.notice, "oo many attempts to transmit")) tries = maxTries;	// caught in a lie
				/*
				 * Because ntries refers to the number of attempts to send the current page
				 * we need to determine if the page counter incremented or not and modify
				 * ntries accordingly.
				 */
				if (pagessent > 0) {
				    req.ntries = tries;
				} else {
				    req.ntries += tries;
				}
			    }
			    if (client->jobParm("tottries")) {
				r = client->getLastResponse(); r.remove(0, r.length() > 4 ? 4 : r.length());
				u_int tottries = atoi((const char*) r);
				req.tottries += tottries;
			    }
			    if (client->jobParm("ndials")) {
				r = client->getLastResponse(); r.remove(0, r.length() > 4 ? 4 : r.length());
				u_int dials = atoi((const char*) r);
				if (dials < (u_int) maxDials && strstr((const char*) req.notice, "oo many attempts to dial")) dials = maxDials;	// caught in a lie
				/*
				 * Because ndials refers to the number of consecutive failed attempts to place
				 * a call we need to determine if there were any tries made for this page and
				 * and modify ndials accordingly.
				 */
				if (tries > 0) {
				    req.ndials = dials;
				} else {
				    req.ndials += dials;
				}
			    }
			    if (client->jobParm("totdials")) {
				r = client->getLastResponse(); r.remove(0, r.length() > 4 ? 4 : r.length());
				u_int totdials = atoi((const char*) r);
				req.totdials += totdials;
			    }
			    if (req.notice.length() && client->jobParm("errorcode")) {
				r = client->getLastResponse(); r.remove(0, r.length() > 4 ? 4 : r.length());
				req.errorcode = r;
			    }
			    if (client->jobParm("state")) {
				r = client->getLastResponse(); r.remove(0, r.length() > 4 ? 4 : r.length());
				// Due to the jobWait we should only ever see "DONE" and "FAILED"...
				if (r == "DONE") {
				    job.state = FaxRequest::state_done;
				    req.status = send_done;
				} else {
				    job.state = FaxRequest::state_failed;
				    /*
				     * The JobRetry* configurations are modem-specific.  With a proxy involved the
				     * getProxyTries() and getProxyDials() are the analogous features.  If the configuration
				     * leaves them unset they are both -1.  Such a configuration delegates all authority for
				     * total job failure to the proxy.  So, if the proxy fails a job where our configuration
				     * has delegated all tries and dials to the proxy, then we must also fail the job else we
				     * will simply resubmit a job to the proxy that the proxy already has deemed as failed 
				     * as if it were rejected (even if maxdials or maxtries were not exceeded).  However, in 
				     * a situation where either configuration is not unset it means the opposite: that we are
				     * not delegating authority for full job failure to the proxy (e.g. it's only entrusted
				     * to handle one session at-a-time - or we want the proxy to make a minimum set of attempts
				     * on each submission).
				     */
				    if ((job.getJCI().getProxyTries() == -1 && job.getJCI().getProxyDials() == -1) || 
					req.ndials >= req.maxdials || req.ntries >= req.maxtries || 
					strstr((const char*) req.notice, "REJECT"))
					req.status = send_failed;
				    else
					req.status = send_retry;
				}
			    } else {
				logError("PROXY SEND: (job %s) unknown state for job %s on %s", (const char*) job.jobid, (const char*) rjobid, (const char*) job.getJCI().getProxy());
				job.state = FaxRequest::state_failed;
				req.status = send_retry;
			    }
			    if (req.commid.length()) {		// there's no sense in trying to retrieve the log if we don't know the commid number
				/*
				 * Try to get and store the session log from the proxy in a dedicated log subdirectory.
				 */
				int isdir = 0;
				fxStr proxyLogs = fxStr(FAX_LOGDIR) | fxStr("/" | job.getJCI().getProxy());
				struct stat sb;
				if (Sys::stat(proxyLogs, sb) != 0) {
				    mode_t logdirmode = job.getJCI().getProxyLogMode();
				    if (logdirmode & S_IREAD) logdirmode |= S_IEXEC;
				    if (logdirmode & S_IRGRP) logdirmode |= S_IXGRP;
				    if (logdirmode & S_IROTH) logdirmode |= S_IXOTH;
				    if (Sys::mkdir(proxyLogs, logdirmode) == 0) isdir = 1;
				} else {
				   isdir = 1;
				}
				if (isdir) {
				    fxStr proxylog = fxStr(proxyLogs | "/c" | req.commid);
				    int fd = Sys::open(proxylog, O_RDWR|O_CREAT|O_EXCL, job.getJCI().getProxyLogMode());
				    if (fd > 0) {
					client->setType(FaxClient::TYPE_I);
					if (!client->recvZData(writeFile, (void*) &fd, emsg, 0, fxStr("RETR log/c" | req.commid))) {
					    logError("PROXY LOG: (job %s) server %s response: %s", (const char*) job.jobid, (const char*) job.getJCI().getProxy(), (const char*) client->getLastResponse());
					}
					Sys::close(fd);
					if (emsg != "") logError("PROXY LOG: (job %s) server %s retrieval: %s", (const char*) job.jobid, (const char*) job.getJCI().getProxy(), (const char*) emsg);
				    } else {
					logError("PROXY LOG: (job %s) server: %s: %s: %s", (const char*) job.jobid, (const char*) job.getJCI().getProxy(), (const char*) proxylog, strerror(errno));
				    }
				}
			    } else {
				logError("PROXY LOG: (job %s) unknown commid number for job %s on %s", (const char*) job.jobid, (const char*) rjobid, (const char*) job.getJCI().getProxy());
			    }
			} else if (status) {
			    logError("PROXY SEND: (job %s) %s failed to identify job %s", (const char*) job.jobid, (const char*) job.getJCI().getProxy(), (const char*) rjobid);
			    job.state = FaxRequest::state_failed;
			    req.status = send_retry;
			    req.notice = "proxy lost job";
			}
		    }
		    client->hangupServer();
		}
		if (!status) {
		    // some error occurred in callServer() or login()
		    logError("PROXY SEND: (job %s) %s", (const char*) job.jobid, (const char*) emsg);
		    job.state = FaxRequest::state_failed;
		    req.status = send_retry;
		    req.notice = "cannot log into proxy";
		}
		if (req.pagehandling == "") {
		    DestInfo& di = destJobs[job.dest];
		    FaxMachineInfo& info = di.getInfo(job.dest);
		    // Don't run preparePageHandling if the job wasn't prepared on this server yet.
		    if (jobprepared && !preparePageHandling(job, req, info, req.notice)) {
			req.notice.insert("Document preparation failed: ");
		    }
		}
		updateRequest(req, job);
		req.npages -= prevPages;	// queueAccounting() only wants the pages sent by the proxy
		queueAccounting(job, req, "PROXY");
		_exit(req.status|0x80);	// 0x80 indicates proxied job

		/*NOTREACHED*/
	    }
	default:			// parent
	    DestInfo& di = destJobs[job.dest];
	    di.proxyCall();		// mark as called to correctly block other jobs
	    numProxyJobs++;
	    job.startSend(pid);
	    break;
    }
}

/*
 * Scan the list of jobs and process those that are ready
 * to go.  Note that the scheduler should only ever be
 * invoked from the dispatcher via a timeout.  This way we
 * can be certain there are no active contexts holding
 * references to job corpses (or other data structures) that
 * we want to reap.  To invoke the scheduler the pokeScheduler
 * method should be called to setup an immediate timeout that
 * will cause the scheduler to be invoked from the dispatcher.
 */
void
faxQueueApp::runScheduler()
{
    /*
     * Terminate the server if there are no jobs currently
     * being processed.  We must be sure to wait for jobs
     * so that we can capture exit status from subprocesses
     * and so that any locks held on behalf of outbound jobs
     * do not appear to be stale (since they are held by this
     * process).
     */
    if (quit && activeq.next == &activeq) {
	close();
	return;
    }
    fxAssert(inSchedule == false, "Scheduler running twice");
    inSchedule = true;
    /*
     * Reread the configuration file if it has been
     * changed.  We do this before each scheduler run
     * since we are a long-running process and it should
     * not be necessary to restart the process to have
     * config file changes take effect.
     */
    (void) updateConfig(configFile);
    /*
     * Scan the job queue and locate a compatible modem to
     * use in processing the job.  Doing things in this order
     * insures the highest priority job is always processed
     * first.
     */
    blockSignals();
    if (! quit) {
	for (u_int i = 0; i < NQHASH; i++) {
	    for (JobIter iter(runqs[i]); iter.notDone(); iter++) {

		if (numPrepares >= maxConcurrentPreps) {
		    /*
		     * Large numbers of simultaneous job preparations can cause problems.
		     * So if we're already preparing too many, we wait for them to finish
		     * before running the scheduler more.  This may prevent some jobs which
		     * are already prepared (i.e. jobs that failed to complete on a previous 
		     * attempt) from going out as soon as they could, but the delay should
		     * be minimal, and this approach prevents us from needing to run 
		     * prepareJobNeeded below, outside of prepareJob.
		     */
		    break;
		}
		Job& job = iter;
		if (job.bprev != NULL) {
		    /*
		     * The batching sub-loop below already allocated this job to a batch.
		     * Thus, this loop's copy of the run queue is incorrect.
		     */
		    pokeScheduler();
		    break;
		}
		fxAssert(job.modem == NULL, "Job on run queue holding modem");

		/*
		 * Read the on-disk job state and process the job.
		 * Doing all the processing below each time the job
		 * is considered for processing could be avoided by
		 * doing it only after assigning a modem but that
		 * would potentially cause the order of dispatch
		 * to be significantly different from the order
		 * of submission; something some folks care about.
		 */
		traceJob(job, "PROCESS");
		Trigger::post(Trigger::JOB_PROCESS, job);
		FaxRequest* req = readRequest(job);
		if (!req) {			// problem reading job state on-disk
		    setDead(job);
		    continue;
		}

		time_t tts;
		time_t now = Sys::now();
		/*
		 * A computer's clock can jump backwards.  For example, if 
		 * the system runs ntp and regularly syncs the system clock
		 * with some outside source it is possible that the local
		 * clock will move backwards.  We cannot die, then, simply
		 * because we find a job on the run queue that has a future 
		 * tts.  The possibility exists that it is due to some 
		 * adjustment in the system clock.
		 */
		if (job.tts > now) {
		    traceJob(job, "WARNING: Job tts is %d seconds in the future.  Proceeding anyway.", job.tts - now);
		    job.tts = now;
		}

		/*
		 * Do per-destination processing and checking.
		 */
		DestInfo& di = destJobs[job.dest];
		/*
		 * Constrain the maximum number of times the phone
		 * will be dialed and/or the number of attempts that
		 * will be made (and reject jobs accordingly).
		 */
		u_short maxdials = fxmin((u_short) job.getJCI().getMaxDials(),req->maxdials);
		if (req->totdials >= maxdials) {
		    rejectJob(job, *req, fxStr::format(
			"REJECT: Too many attempts to dial {E333}: %u, max %u",
			req->totdials, maxdials));
		    deleteRequest(job, req, Job::rejected, true);
		    continue;
		}
		u_short maxtries = fxmin((u_short) job.getJCI().getMaxTries(),req->maxtries);
		if (req->tottries >= maxtries) {
		    rejectJob(job, *req, fxStr::format(
			"REJECT: Too many attempts to transmit: %u, max %u {E334}",
			req->tottries, maxtries));
		    deleteRequest(job, req, Job::rejected, true);
		    continue;
		}
		// NB: repeat this check so changes in max pages are applied
		u_int maxpages = job.getJCI().getMaxSendPages();
		if (req->totpages > maxpages) {
		    rejectJob(job, *req, fxStr::format(
			"REJECT: Too many pages in submission: %u, max %u {E335}",
			req->totpages, maxpages));
		    deleteRequest(job, req, Job::rejected, true);
		    continue;
		}
		if (job.getJCI().getRejectNotice() != "") {
		    /*
		     * Calls to this destination are being rejected for
		     * a specified reason that we return to the sender.
		     */
		    rejectJob(job, *req, "REJECT: " | job.getJCI().getRejectNotice());
		    deleteRequest(job, req, Job::rejected, true);
		    continue;
		}
		if ((job.getJCI().getMaxConcurrentCalls() == 0 && di.isPendingConnection()) || 
			(job.getJCI().getMaxConcurrentCalls() != 0 && !isOKToCall(di, job.getJCI(), 1))) {
		    /*
		     * This job would exceed the max number of concurrent
		     * calls that may be made to this destination.  Put it
		     * on a ``blocked queue'' for the destination; the job
		     * will be made ready to run when one of the existing
		     * jobs terminates.
		     */
		    blockJob(job, *req, "Blocked by concurrent calls {E337}");
		    if (job.isOnList()) job.remove();	// remove from run queue
		    di.block(job);			// place at tail of di queue, honors job priority
		    delete req;
		} else if (((tts = job.getJCI().nextTimeToSend(now)) != now) || ((tts = job.tod.nextTimeOfDay(now)) != now)) {
		    /*
		     * This job may not be started now because of time-of-day
		     * restrictions.  Reschedule it for the next possible time.
		     */
		    if (job.isOnList()) job.remove();	// remove from run queue
		    delayJob(job, *req, "Delayed by time-of-day restrictions {E338}", tts);
		    delete req;
		} else if (staggerCalls && ((u_int) lastCall + staggerCalls) > (u_int) now) {
		    /*
		     * This job may not be started now because we last started
		     * another job too recently and we're staggering jobs.
		     * Reschedule it for the time when next okay.
		     */
		    if (job.isOnList()) job.remove();
		    delayJob(job, *req, "Delayed by outbound call staggering {E339}", lastCall + staggerCalls);
		    delete req;
		} else if (job.getJCI().getProxy() != "") {
		    if (numProxyJobs >= maxProxyJobs) {
			if (job.isOnList()) job.remove();
			delayJob(job, *req, "Delayed by limit on proxy connections {E346}", Sys::now() + random() % requeueInterval);
			delete req;
		    } else {
			/*
			 * Send this job through a proxy HylaFAX server.
			 */
			unblockDestJobs(di);			// this job may be blocking others
			pokeScheduler();
			if (job.isOnList()) job.remove();	// remove from run queue
			job.commid = "";
			job.start = now;
			req->notice = "";
			setActive(job);
			updateRequest(*req, job);
			sendViaProxy(job, *req);
			delete req; 
		    }
		} else if ((Modem::modemAvailable(job) || (allowIgnoreModemBusy && req->ignoremodembusy)) && assignModem(job, (allowIgnoreModemBusy && req->ignoremodembusy))) {
		    lastCall = now;
		    if (job.isOnList()) job.remove();	// remove from run queue
		    job.breq = req;
		    /*
		     * We have a modem and have assigned it to the
		     * job.  The job is not on any list; processJob
		     * is responsible for requeing the job according
		     * to the outcome of the work it does (which may
		     * take place asynchronously in a sub-process).
		     * Likewise the release of the assigned modem is
		     * also assumed to take place asynchronously in
		     * the context of the job's processing.
		     */
		    (void) di.getInfo(job.dest);	// must read file for supportsBatching
		    FaxMachineInfo info;
		    if (di.supportsBatching() && maxBatchJobs > 1
		    	&& (req->jobtype == "facsimile"
		    		|| (req->jobtype == "pager" 
		    			&& streq(info.getPagingProtocol(), "ixo")))) { 
					// fax and IXO pages only for now
			/*
			 * The destination supports batching and batching is enabled.  
			 * Continue down the queue and build an array of all processable 
			 * jobs to this destination allowed on this modem which are not 
			 * of a lesser priority than jobs to other destinations.
			 */
			unblockDestJobs(di);

			/*
			 * Since job files are passed to the send program as command-line
			 * parameters, our batch size is limited by that number of
			 * parameters.  64 should be a portable number.
			 */
			if (maxBatchJobs > 64) maxBatchJobs = 64;

			/*
			 * If the queue length becomes very large then scanning the queue
			 * for batching can become counter-productive as it consumes a 
			 * large amount of CPU attention spinning through the queue.  In
			 * all likelihood batching would not even be useful in those 
			 * scenarios, and the administrator simply has not attended to 
			 * disabling it.  So we prevent batching from traversing into deep 
			 * queues in able to prevent a large amount of unnecessary CPU 
			 * consumption.
			 */
			u_int qlencount = 0;

			Job* bjob = &job;	// Last batched Job
			Job* cjob = &job;	// current Job

			u_int batchedjobs = 1;
			for (u_int j = 0; batchedjobs < maxBatchJobs && j < NQHASH && qlencount < maxTraversal; j++) {
			    blockSignals();
			    for (JobIter joblist(runqs[j]); batchedjobs < maxBatchJobs && joblist.notDone() && qlencount < maxTraversal; joblist++) {
				qlencount++;
				if (joblist.job().dest != cjob->dest)
				    continue;
				cjob = joblist;
				if (job.jobid == cjob->jobid)
				    continue;	// Skip the current job
				fxAssert(cjob->tts <= Sys::now(), "Sleeping job on run queue");
				fxAssert(cjob->modem == NULL, "Job on run queue holding modem");
				FaxRequest* creq = readRequest(*cjob);
				if (!areBatchable(*req, *creq, job, *cjob)) {
				    delete creq;
				    continue;
				}
				if (iter.notDone() && &iter.job() == bjob)
				    iter++;

				traceJob(job, "ADDING JOB %s TO BATCH", (const char*) cjob->jobid);
				cjob->modem = job.modem;
				cjob->remove();
				bjob->bnext = cjob;
				cjob->bprev = bjob;
				bjob = cjob;
				cjob->breq = creq;
				batchedjobs++;
			    }
			    releaseSignals();
			}
			/*
			 * Jobs that are on the sleep queue with state_sleeping
			 * can be batched because the tts that the submitter requested
			 * is known to have passed already.  So we pull these jobs out
			 * of the sleep queue and batch them directly.
			 */
			blockSignals();
			for (JobIter sleepiter(sleepq); batchedjobs < maxBatchJobs && sleepiter.notDone() && qlencount < maxTraversal; sleepiter++) {
			    qlencount++;
			    cjob = sleepiter;
			    if (cjob->dest != job.dest || cjob->state != FaxRequest::state_sleeping)
				continue;
			    FaxRequest* creq = readRequest(*cjob);
			    if (!(req && areBatchable(*req, *creq, job, *cjob))) {
				delete creq;
				continue;
			    }

			    traceJob(job, "ADDING JOB %s TO BATCH", (const char*) cjob->jobid);
			    cjob->stopTTSTimer();
			    cjob->tts = now;
			    cjob->state = FaxRequest::state_ready;
			    cjob->remove();
			    cjob->modem = job.modem;
			    bjob->bnext = cjob;
			    cjob->bprev = bjob;
			    cjob->breq = creq;
			    bjob = cjob;
			    // This job was batched from sleeping, things have
			    // changed; Update the queue file for onlookers.
			    creq->tts = now;
			    updateRequest(*creq, *cjob);
			    batchedjobs++;
			}
			bjob->bnext = NULL;
			releaseSignals();
		    } else
			job.bnext = NULL;
		    di.call();			// mark as called to correctly block other jobs
		    processJob(job, req, di);
		} else if (job.state == FaxRequest::state_failed) {
		    rejectJob(job, *req, fxStr::format("REJECT: Modem is configured as exempt from accepting jobs {E336}"));
		    deleteRequest(job, req, Job::rejected, true);
		    continue;
		} else				// leave job on run queue
		    delete req;
	    }
	}
    }
    /*
     * Reap dead jobs.
     */
    for (JobIter iter(deadq); iter.notDone(); iter++) {
	Job* job = iter;
	job->remove();
	traceJob(*job, "DELETE");
	Trigger::post(Trigger::JOB_REAP, *job);
	delete job;
    }
    releaseSignals();
    /*
     * Reclaim resources associated with clients
     * that terminated without telling us.
     */
    HylaClient::purge();		// XXX maybe do this less often

    inSchedule = false;
}

bool
faxQueueApp::scheduling(void)
{
    return inSchedule;
}

/*
 * Attempt to assign a modem to a job.  If we are
 * unsuccessful and it was due to the modem being
 * locked for use by another program then we start
 * a thread to poll for the removal of the lock file;
 * this is necessary for send-only setups where we
 * do not get information about when modems are in
 * use from faxgetty processes.
 */
bool
faxQueueApp::assignModem(Job& job, bool ignorebusy)
{
    fxAssert(job.modem == NULL, "Assigning modem to job that already has one");

    bool retryModemLookup;
    do {
	retryModemLookup = false;
	Modem* modem = Modem::findModem(job, ignorebusy);
	if (modem) {
	    if (modem->getState() == Modem::EXEMPT) {
		job.state = FaxRequest::state_failed;
		return (false);
	    }
	    if (modem->assign(job, (modem->getState() == Modem::BUSY && ignorebusy))) {
		Trigger::post(Trigger::MODEM_ASSIGN, *modem);
		return (true);
	    }
	    /*
	     * Modem could not be assigned to job.  The
	     * modem is assumed to be ``removed'' from
	     * the list of potential modems scanned by
	     * findModem so we arrange to re-lookup a
	     * suitable modem for this job.  (a goto would
	     * be fine here but too many C++ compilers
	     * can't handle jumping past the above code...)
	     */
	    traceJob(job, "Unable to assign modem %s (cannot lock)",
		(const char*) modem->getDeviceID());
	    modem->startLockPolling(pollLockWait);
	    traceModem(*modem, "BUSY (begin polling)");
	    retryModemLookup = true;
	} else
	    traceJob(job, "No assignable modem located");
    } while (retryModemLookup);
    return (false);
}

/*
 * Release a modem assigned to a job.  The scheduler
 * is prodded since doing this may permit something
 * else to be processed.
 */
void
faxQueueApp::releaseModem(Job& job)
{
    Trigger::post(Trigger::MODEM_RELEASE, *job.modem);
    job.modem->release();
    pokeScheduler();
    Job* cjob;
    for (cjob = &job; cjob != NULL; cjob = cjob->bnext) {
	fxAssert(cjob->modem != NULL, "No assigned modem to release");
	cjob->modem = NULL;			// remove reference to modem
    }
}

/*
 * Poll to see if a modem's UUCP lock file is still
 * present.  If the lock has been removed then mark
 * the modem ready for use and poke the job scheduler
 * in case jobs were waiting for an available modem.
 * This work is only done when a modem is ``discovered''
 * to be in-use by an outbound process when operating
 * in a send-only environment (i.e. one w/o a faxgetty
 * process monitoring the state of each modem).
 */
void
faxQueueApp::pollForModemLock(Modem& modem)
{
    if (modem.lock->lock()) {
	modem.release();
	traceModem(modem, "READY (end polling)");
	pokeScheduler();
    } else
	modem.startLockPolling(pollLockWait);
}

/*
 * Set a timeout so that the job scheduler runs the
 * next time the dispatcher is invoked.
 */
void
faxQueueApp::pokeScheduler(u_short s)
{
    schedTimeout.start(s);
}

/*
 * Create a request instance and read the
 * associated queue file into it.
 */
FaxRequest*
faxQueueApp::readRequest(Job& job)
{
    int fd = Sys::open(job.file, O_RDWR);
    if (fd >= 0) {
	if (flock(fd, LOCK_EX) >= 0) {
	    FaxRequest* req = new FaxRequest(job.file, fd);
	    bool reject;
	    if (req->readQFile(reject)) {
		if (reject) {
		    jobError(job, "qfile contains one or more errors {E347}");
		    delete req;
		    return (NULL);
		}
		if (req->external == "")
		    req->external = job.dest;
		return (req);
	    }
	    jobError(job, "Could not read job file");
	    delete req;
	} else
	    jobError(job, "Could not lock job file: %m");
	Sys::close(fd);
    } else {
	// file might have been removed by another server
	if (errno != ENOENT)
	    jobError(job, "Could not open job file: %m");
    }
    return (NULL);
}

/*
 * Update the request instance with information
 * from the job structure and then write the
 * associated queue file.
 */
void
faxQueueApp::updateRequest(FaxRequest& req, Job& job)
{
    req.state = job.state;
    req.pri = job.pri;
    req.writeQFile();
}

/*
 * Delete a request and associated state.
 */
void
faxQueueApp::deleteRequest(Job& job, FaxRequest* req, JobStatus why,
    bool force, const char* duration)
{
    if (why != Job::done) queueAccounting(job, *req, "UNSENT");
    deleteRequest(job, *req, why, force, duration);
    delete req;
}

void
faxQueueApp::deleteRequest(Job& job, FaxRequest& req, JobStatus why,
    bool force, const char* duration)
{
    fxStr dest = FAX_DONEDIR |
	req.qfile.tail(req.qfile.length() - (sizeof (FAX_SENDDIR)-1));
    /*
     * Move completed jobs to the doneq area where
     * they can be retrieved for a period of time;
     * after which they are either removed or archived.
     */
    if (Sys::rename(req.qfile, dest) >= 0) {
	u_int i = 0;
	/*
	 * Remove entries for imaged documents and
	 * delete/rename references to source documents
	 * so the imaged versions can be expunged.
	 */
	while (i < req.items.length()) {
	    FaxItem& fitem = req.items[i];
	    if (fitem.op == FaxRequest::send_fax) {
		req.renameSaved(i);
		req.items.remove(i);
	    } else
		i++;
	}
	req.qfile = dest;			// moved to doneq
	job.file = req.qfile;			// ...and track change
	if (why == Job::done)
	    req.state = FaxRequest::state_done;	// job is definitely done
	else
	    req.state = FaxRequest::state_failed;// job is definitely done
	req.pri = job.pri;			// just in case someone cares
	req.tts = Sys::now();			// mark job termination time
	job.tts = req.tts;
	req.writeQFile();
	if (force) {
	    notifySender(job, why, duration);
	} else {
	    if (job.getJCI().getNotify() != -1) {
		if (job.getJCI().isNotify(FaxRequest::notify_any))
		    notifySender(job, why, duration);
	    } else
		if (req.isNotify(FaxRequest::notify_any))
		    notifySender(job, why, duration);
	}
    } else {
	/*
	 * Move failed, probably because there's no
	 * directory.  Treat the job the way we used
	 * to: purge everything.  This avoids filling
	 * the disk with stuff that'll not get removed;
	 * except for a scavenger program.
	 */
	jobError(job, "rename to %s failed: %s",
	    (const char*) dest, strerror(errno));
	req.writeQFile();
	if (force) {
	    notifySender(job, why, duration);
	} else {
	    if (job.getJCI().getNotify() != -1) {
		if (job.getJCI().isNotify(FaxRequest::notify_any))
		    notifySender(job, why, duration);
	    } else
		if (req.isNotify(FaxRequest::notify_any))
		    notifySender(job, why, duration);
	}
	u_int n = req.items.length();
	req.items.remove(0, n);
	Sys::unlink(req.qfile);
    }
}

/*
 * FIFO-related support.
 */

/*
 * Open the requisite FIFO special files.
 */
void
faxQueueApp::openFIFOs()
{
    fifo = openFIFO(fifoName, 0600, true);
    Dispatcher::instance().link(fifo, Dispatcher::ReadMask, this);
}

void
faxQueueApp::closeFIFOs()
{
    Sys::close(fifo), fifo = -1;
}

int faxQueueApp::inputReady(int fd)		{ return FIFOInput(fd); }

/*
 * Process a message received through a FIFO.
 */
void
faxQueueApp::FIFOMessage(const char* cp)
{
    if (tracingLevel & FAXTRACE_FIFO)
	logInfo("FIFO RECV \"%s\"", cp);
    if (cp[0] == '\0') {
	logError("Empty FIFO message");
	return;
    }
    const char* tp = strchr(++cp, ':');
    if (tp && (tp[1] != '\0'))
	FIFOMessage(cp[-1], fxStr(cp,tp-cp), tp+1);
    else
	FIFOMessage(cp[-1], fxStr::null, cp);
}

/*
 * Process a parsed FIFO message.
 *
 * If an application goes crazy, or if the FIFO overflows, then it's possible 
 * to see corrupt FIFO messages.  Thus, the previous parsing of the FIFO message
 * cannot be entirely trusted.  Here, "id" and "args" must be checked for size
 * before continued processing.  The downstream functions will need to make sure 
 * that the id and args are actually meaningful.
 */
void
faxQueueApp::FIFOMessage(char cmd, const fxStr& id, const char* args)
{
    bool status = false;
    switch (cmd) {
    case '+':				// modem status msg
	if (id.length()) FIFOModemMessage(id, args);
	return;
    case '*':				// job status msg from subproc's
	if (id.length()) FIFOJobMessage(id, args);
	return;
    case '@':				// receive status msg
	if (id.length()) FIFORecvMessage(id, args);
	return;
    case 'Q':				// quit
	if (!id.length()) {
	    traceServer("QUIT");
	    quit = true;
	    pokeScheduler();
	}
	return;				// NB: no return value expected
    case 'T':				// create new trigger 
	if (id.length()) {
	    traceServer("TRIGGER %s", args);
	    Trigger::create(id, args);
	}
	return;				// NB: trigger id returned specially

    /*
     * The remaining commands generate a response if
     * the client has included a return address.
     */
    case 'C':				// configuration control
	if (args[0] == '\0') return;
	traceServer("CONFIG %s", args);
	status = readConfigItem(args);
	break;
    case 'D':				// cancel an existing trigger
	if (args[0] == '\0') return;
	traceServer("DELETE %s", args);
	status = Trigger::cancel(args);
	break;
    case 'R':				// remove job
	if (args[0] == '\0') return;
	traceServer("REMOVE JOB %s", args);
	status = terminateJob(args, Job::removed);
	break;
    case 'K':				// kill job
	if (args[0] == '\0') return;
	traceServer("KILL JOB %s", args);
	status = terminateJob(args, Job::killed);
	break;
    case 'S':				// submit an outbound job
	if (args[0] == '\0') return;
	traceServer("SUBMIT JOB %s", args);
	status = submitJob(args, false, true);
	if (status)
	    pokeScheduler();
	break;
    case 'U':				// unreference file
	if (args[0] == '\0') return;
	traceServer("UNREF DOC %s", args);
	unrefDoc(args);
	status = true;
	break;
    case 'X':				// suspend job
	if (args[0] == '\0') return;
	traceServer("SUSPEND JOB %s", args);
	status = suspendJob(args, false);
	if (status)
	    pokeScheduler();
	break;
    case 'Y':				// interrupt job
	if (args[0] == '\0') return;
	traceServer("INTERRUPT JOB %s", args);
	status = suspendJob(args, true);
	if (status)
	    pokeScheduler();
	break;
    case 'N':				// noop
	status = true;
	break;
    case 'Z':
	showDebugState();
	break;
    default:
	logError("Bad FIFO cmd '%c' from client %s", cmd, (const char*) id);
	break;
    }
    if (id != fxStr::null) {
	char msg[3];
	msg[0] = cmd;
	msg[1] = (status ? '*' : '!');
	msg[2] = '\0';
	if (tracingLevel & FAXTRACE_FIFO)
	    logInfo("FIFO SEND %s msg \"%s\"", (const char*) id, msg);
	HylaClient::getClient(id).send(msg, sizeof (msg));
    }
}

void
faxQueueApp::notifyModemWedged(Modem& modem)
{
    fxStr dev(idToDev(modem.getDeviceID()));
    logError("MODEM %s appears to be wedged", (const char*)dev);
    fxStr cmd(wedgedCmd
	| quote | quoted(modem.getDeviceID()) | enquote
	| quote |                 quoted(dev) | enquote
    );
    traceServer("MODEM WEDGED: %s", (const char*) cmd);

    const char* argv[4];
    argv[0] = (const char*) wedgedCmd;
    argv[1] = (const char*) modem.getDeviceID();
    argv[2] = (const char*) dev;
    argv[3] = NULL;
    runCmd(wedgedCmd, argv, true, this);
}

void
faxQueueApp::FIFOModemMessage(const fxStr& devid, const char* msg)
{
    if (! (devid.length() > 0))
    {
	traceServer("Invalid modem FIFO message");
	return;
    }

    Modem& modem = Modem::getModemByID(devid);
    switch (msg[0]) {
    case 'R':			// modem ready, parse capabilities
	modem.stopLockPolling();
	if (msg[1] != '\0') {
	    modem.setCapabilities(msg+1);	// NB: also sets modem READY
	    traceModem(modem, "READY, capabilities %s", msg+1);
	} else {
	    modem.setState(Modem::READY);
	    traceModem(modem, "READY (no capabilities)");
	}
	Trigger::post(Trigger::MODEM_READY, modem);
	pokeScheduler();
	break;
    case 'B':			// modem busy doing something
	modem.stopLockPolling();
	traceModem(modem, "BUSY");
	modem.setState(Modem::BUSY);
	Trigger::post(Trigger::MODEM_BUSY, modem);
	break;
    case 'D':			// modem to be marked down
	modem.stopLockPolling();
	traceModem(modem, "DOWN");
	modem.setState(Modem::DOWN);
	Trigger::post(Trigger::MODEM_DOWN, modem);
	break;
    case 'E':			// modem exempt from sending use
	modem.stopLockPolling();
	traceModem(modem, "EXEMPT");
	modem.setState(Modem::EXEMPT);
	Trigger::post(Trigger::MODEM_EXEMPT, modem);
	// clear any pending jobs for this modem
	pokeScheduler();
	break;
    case 'N':			// modem phone number updated
	traceModem(modem, "NUMBER %s", msg+1);
	modem.setNumber(msg+1);
	break;
    case 'I':			// modem communication ID
	traceModem(modem, "COMID %s", msg+1);
	modem.setCommID(msg+1);
	break;
    case 'W':			// modem appears wedged
	// NB: modem should be marked down in a separate message
	notifyModemWedged(modem);
        Trigger::post(Trigger::MODEM_WEDGED, modem);
	break;
    case 'U':			// modem inuse by outbound job
	modem.stopLockPolling();
	traceModem(modem, "BUSY");
	modem.setState(Modem::BUSY);
	Trigger::post(Trigger::MODEM_INUSE, modem);
	break;
    case 'C':			// caller-ID information
	Trigger::post(Trigger::MODEM_CID, modem, msg+1);
	break;
    case 'd':			// data call begun
	Trigger::post(Trigger::MODEM_DATA_BEGIN, modem);
	break;
    case 'e':			// data call finished
	Trigger::post(Trigger::MODEM_DATA_END, modem);
	break;
    case 'v':			// voice call begun
	Trigger::post(Trigger::MODEM_VOICE_BEGIN, modem);
	break;
    case 'w':			// voice call finished
	Trigger::post(Trigger::MODEM_VOICE_END, modem);
	break;
    default:
	traceServer("FIFO: Bad modem message \"%s\" for modem %s",
		msg, (const char*)devid);
	break;
    }
}

void
faxQueueApp::FIFOJobMessage(const fxStr& jobid, const char* msg)
{
    Job* jp = Job::getJobByID(jobid);
    if (!jp) {
	traceServer("FIFO: JOB %s not found for msg \"%s\"",
	    (const char*) jobid, msg);
	return;
    }
    switch (msg[0]) {
    case 'c':			// call placed
	Trigger::post(Trigger::SEND_CALL, *jp);
	break;
    case 'C':			// call connected with fax
	Trigger::post(Trigger::SEND_CONNECTED, *jp);
	{
	    DestInfo& di = destJobs[(*jp).dest];
	    di.connected();
	    if ((*jp).getJCI().getMaxConcurrentCalls() == 0) {
	        unblockDestJobs(di, 1);		// release one blocked job
		pokeScheduler();
	    }
	}
	break;
    case 'd':			// page sent
	Trigger::post(Trigger::SEND_PAGE, *jp, msg+1);
	break;
    case 'D':			// document sent
	{ FaxSendInfo si; si.decode(msg+1); }
	Trigger::post(Trigger::SEND_DOC, *jp, msg+1);
	break;
    case 'p':			// polled document received
	Trigger::post(Trigger::SEND_POLLRCVD, *jp, msg+1);
	break;
    case 'P':			// polling operation done
	Trigger::post(Trigger::SEND_POLLDONE, *jp, msg+1);
	break;
    default:
	traceServer("FIFO: Unknown job message \"%s\" for job %s",
		msg, (const char*)jobid);
	break;
    }
}

void
faxQueueApp::FIFORecvMessage(const fxStr& devid, const char* msg)
{
    if (! (devid.length() > 0))
    {
	traceServer("Invalid modem FIFO message");
	return;
    }

    Modem& modem = Modem::getModemByID(devid);
    switch (msg[0]) {
    case 'B':			// inbound call started
	Trigger::post(Trigger::RECV_BEGIN, modem);
	break;
    case 'E':			// inbound call finished
	Trigger::post(Trigger::RECV_END, modem);
	break;
    case 'S':			// session started (received initial parameters)
	Trigger::post(Trigger::RECV_START, modem, msg+1);
	break;
    case 'P':			// page done
	Trigger::post(Trigger::RECV_PAGE, modem, msg+1);
	break;
    case 'D':			// document done
	Trigger::post(Trigger::RECV_DOC, modem, msg+1);
	break;
    default:
	traceServer("FIFO: Unknown recv message \"%s\" for modem %s",
		msg, (const char*)devid);
	break;
    }
}

/*
 * Configuration support.
 */

void
faxQueueApp::resetConfig()
{
    FaxConfig::resetConfig();
    dialRules = NULL;
    setupConfig();
}

#define	N(a)	(sizeof (a) / sizeof (a[0]))

faxQueueApp::stringtag faxQueueApp::strings[] = {
{ "logfacility",	&faxQueueApp::logFacility,	LOG_FAX },
{ "areacode",		&faxQueueApp::areaCode	},
{ "countrycode",	&faxQueueApp::countryCode },
{ "longdistanceprefix",	&faxQueueApp::longDistancePrefix },
{ "internationalprefix",&faxQueueApp::internationalPrefix },
{ "uucplockdir",	&faxQueueApp::uucpLockDir,	UUCP_LOCKDIR },
{ "uucplocktype",	&faxQueueApp::uucpLockType,	UUCP_LOCKTYPE },
{ "contcoverpage",	&faxQueueApp::contCoverPageTemplate },
{ "contcovercmd",	&faxQueueApp::coverCmd,		FAX_COVERCMD },
{ "notifycmd",		&faxQueueApp::notifyCmd,	FAX_NOTIFYCMD },
{ "ps2faxcmd",		&faxQueueApp::ps2faxCmd,	FAX_PS2FAXCMD },
{ "pdf2faxcmd",		&faxQueueApp::pdf2faxCmd,	FAX_PDF2FAXCMD },
{ "pcl2faxcmd",		&faxQueueApp::pcl2faxCmd,	FAX_PCL2FAXCMD },
{ "tiff2faxcmd",	&faxQueueApp::tiff2faxCmd,	FAX_TIFF2FAXCMD },
{ "sendfaxcmd",		&faxQueueApp::sendFaxCmd,
   FAX_LIBEXEC "/faxsend" },
{ "sendpagecmd",	&faxQueueApp::sendPageCmd,
   FAX_LIBEXEC "/pagesend" },
{ "senduucpcmd",	&faxQueueApp::sendUUCPCmd,
   FAX_LIBEXEC "/uucpsend" },
{ "wedgedcmd",		&faxQueueApp::wedgedCmd,	FAX_WEDGEDCMD },
{ "jobcontrolcmd",	&faxQueueApp::jobCtrlCmd,	"" },
{ "sharecallfailures",	&faxQueueApp::shareCallFailures,"none" },
};
faxQueueApp::numbertag faxQueueApp::numbers[] = {
{ "tracingmask",	&faxQueueApp::tracingMask,	// NB: must be first
   FAXTRACE_MODEMIO|FAXTRACE_TIMEOUTS },
{ "servertracing",	&faxQueueApp::tracingLevel,	FAXTRACE_SERVER },
{ "uucplocktimeout",	&faxQueueApp::uucpLockTimeout,	0 },
{ "postscripttimeout",	&faxQueueApp::postscriptTimeout, 3*60 },
{ "maxconcurrentjobs",	&faxQueueApp::maxConcurrentCalls, 1 },
{ "maxconcurrentcalls",	&faxQueueApp::maxConcurrentCalls, 1 },
{ "maxproxyjobs",	&faxQueueApp::maxProxyJobs,	(u_int) 64 },
{ "maxbatchjobs",	&faxQueueApp::maxBatchJobs,	(u_int) 64 },
{ "maxtraversal",	&faxQueueApp::maxTraversal,	(u_int) 256 },
{ "maxsendpages",	&faxQueueApp::maxSendPages,	(u_int) -1 },
{ "maxtries",		&faxQueueApp::maxTries,		(u_int) FAX_RETRIES },
{ "maxdials",		&faxQueueApp::maxDials,		(u_int) FAX_REDIALS },
{ "jobreqother",	&faxQueueApp::requeueInterval,	FAX_REQUEUE },
{ "polllockwait",	&faxQueueApp::pollLockWait,	30 },
{ "staggercalls",	&faxQueueApp::staggerCalls,	0 },
{ "maxconcurrentpreps",	&faxQueueApp::maxConcurrentPreps,	1 },
};

void
faxQueueApp::setupConfig()
{
    int i;

    for (i = N(strings)-1; i >= 0; i--)
	(*this).*strings[i].p = (strings[i].def ? strings[i].def : "");
    for (i = N(numbers)-1; i >= 0; i--)
	(*this).*numbers[i].p = numbers[i].def;
    tod.reset();			// any day, any time
    use2D = true;			// ok to use 2D data
    class1RestrictPoorDestinations = 0;	// no restrictions
    useUnlimitedLN = true;		// ok to use LN_INF
    allowIgnoreModemBusy = false;	// to allow jobs to ignore modem busy status
    uucpLockMode = UUCP_LOCKMODE;
    delete dialRules, dialRules = NULL;
    ModemGroup::reset();		// clear+add ``any modem'' class
    ModemGroup::set(MODEM_ANY, new RE(".*"), fxStr("0"));
    pageChop = FaxRequest::chop_last;
    pageChopThreshold = 3.0;		// minimum of 3" of white space
    lastCall = Sys::now() - 3600;
    numProxyJobs = numPrepares = 0;
}

void
faxQueueApp::configError(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlogError(fmt, ap);
    va_end(ap);
}

void
faxQueueApp::configTrace(const char* fmt, ...)
{
    if (tracingLevel & FAXTRACE_CONFIG) {
	va_list ap;
	va_start(ap, fmt);
	vlogError(fmt, ap);
	va_end(ap);
    }
}

static void
crackArgv(fxStr& s)
{
    u_int i = 0;
    do {
        while (i < s.length() && !isspace(s[i])) i++;
        if (i < s.length()) {
            s[i++] = '\0';
            u_int j = i;
            while (j < s.length() && isspace(s[j])) j++;
            if (j > i) {
                s.remove(i, j - i);
            }
        }
    } while (i < s.length());
    s.resize(i);
}

static void
tiffErrorHandler(const char* module, const char* fmt0, va_list ap)
{
    fxStr fmt = (module != NULL) ?
        fxStr::format("%s: Warning, %s.", module, fmt0)
        : fxStr::format("Warning, %s.", fmt0);
    vlogError(fmt, ap);
}

static void
tiffWarningHandler(const char* module, const char* fmt0, va_list ap)
{
    fxStr fmt = (module != NULL) ?
        fxStr::format("%s: Warning, %s.", module, fmt0)
        : fxStr::format("Warning, %s.", fmt0);
    vlogWarning(fmt, ap);
}

bool
faxQueueApp::setConfigItem(const char* tag, const char* value)
{
    u_int ix;
    if (findTag(tag, (const tags*) strings, N(strings), ix)) {
	(*this).*strings[ix].p = value;
	switch (ix) {
	case 0:	faxApp::setLogFacility(logFacility); break;
	}
	if (ix >= 8)
	    crackArgv((*this).*strings[ix].p);
    } else if (findTag(tag, (const tags*) numbers, N(numbers), ix)) {
	(*this).*numbers[ix].p = getNumber(value);
	switch (ix) {
	case 1:
	    tracingLevel &= ~tracingMask;
	    if (dialRules)
		dialRules->setVerbose((tracingLevel&FAXTRACE_DIALRULES) != 0);
	    if (tracingLevel&FAXTRACE_TIFF) {
		TIFFSetErrorHandler(tiffErrorHandler);
		TIFFSetWarningHandler(tiffWarningHandler);
	    } else {
		TIFFSetErrorHandler(NULL);
		TIFFSetWarningHandler(NULL);
	    }
	    break;
	case 2: UUCPLock::setLockTimeout(uucpLockTimeout); break;
	}
    } else if (streq(tag, "dialstringrules"))
	setDialRules(value);
    else if (streq(tag, "timeofday"))
	tod.parse(value);
    else if (streq(tag, "use2d"))
	use2D = getBoolean(value);
    else if (streq(tag, "class1restrictpoordestinations"))
	class1RestrictPoorDestinations = getNumber(value);
    else if (streq(tag, "allowignoremodembusy"))
	allowIgnoreModemBusy = getBoolean(value);
    else if (streq(tag, "uucplockmode"))
	uucpLockMode = (mode_t) strtol(value, 0, 8);
    else if (streq(tag, "modemgroup")) {
	const char* cp;
	for (cp = value; *cp && *cp != ':'; cp++)
	    ;
	if (*cp == ':') {
	    fxStr name(value, cp-value);
	    fxStr limit("0");
	    for (cp++; *cp && isspace(*cp); cp++)
		;
	    const char* cp2 = strchr(cp, ':');
	    if (cp2) {
		limit = fxStr(cp, cp2-cp);
		cp = cp2 + 1;
	    }
	    if (*cp != '\0') {
		RE* re = new RE(cp);
		if (re->getErrorCode() > REG_NOMATCH) {
		    fxStr emsg;
		    re->getError(emsg);
		    configError("Bad pattern for modem group \"%s\": %s: %s", (const char*) emsg,
			(const char*) name, re->pattern());
		} else
		    ModemGroup::set(name, re, limit);
	    } else
		configError("No regular expression for modem group");
	} else
	    configError("Missing ':' separator in modem group specification");
    } else if (streq(tag, "pagechop")) {
	if (streq(value, "all"))
	    pageChop = FaxRequest::chop_all;
	else if (streq(value, "none"))
	    pageChop = FaxRequest::chop_none;
	else if (streq(value, "last"))
	    pageChop = FaxRequest::chop_last;
    } else if (streq(tag, "pagechopthreshold")) {
	pageChopThreshold = atof(value);
    } else if (streq(tag, "unblock")) {
	DestInfo& di = destJobs[value];
	di.hangup();	// blindly force hangup indication
	unblockDestJobs(di, 1);
	pokeScheduler();
    } else if (streq(tag, "audithook")) {
        const char* cp;
	for (cp = value; *cp && *cp != ':'; cp++)
	    ;
	if (*cp == ':') {
	    fxStr cmd(value, cp-value);
	    for (cp++; *cp && isspace(*cp); cp++)
		;
	    if (*cp != '\0') {
	    	Trigger::setTriggerHook(cmd, cp);
	    } else
		configError("No trigger specification for audit hook");
	} else
	    configError("Missing ':' separator in audit hook specification");
    } else
	return (false);
    return (true);
}

/*
 * Subclass DialStringRules so that we can redirect the
 * diagnostic and tracing interfaces through the server.
 */
class MyDialStringRules : public DialStringRules {
private:
    virtual void parseError(const char* fmt ...);
    virtual void traceParse(const char* fmt ...);
    virtual void traceRules(const char* fmt ...);
public:
    MyDialStringRules(const char* filename);
    ~MyDialStringRules();
};
MyDialStringRules::MyDialStringRules(const char* f) : DialStringRules(f) {}
MyDialStringRules::~MyDialStringRules() {}

void
MyDialStringRules::parseError(const char* fmt ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlogError(fmt, ap);
    va_end(ap);
}
void
MyDialStringRules::traceParse(const char* fmt ...)
{
    if (faxQueueApp::instance().getTracingLevel() & FAXTRACE_DIALRULES) {
	va_list ap;
	va_start(ap, fmt);
	vlogInfo(fmt, ap);
	va_end(ap);
    }
}
void
MyDialStringRules::traceRules(const char* fmt ...)
{
    if (faxQueueApp::instance().getTracingLevel() & FAXTRACE_DIALRULES) {
	va_list ap;
	va_start(ap, fmt);
	vlogInfo(fmt, ap);
	va_end(ap);
    }
}

void
faxQueueApp::setDialRules(const char* name)
{
    delete dialRules;
    dialRules = new MyDialStringRules(name);
    dialRules->setVerbose((tracingLevel & FAXTRACE_DIALRULES) != 0);
    /*
     * Setup configuration environment.
     */
    dialRules->def("AreaCode", areaCode);
    dialRules->def("CountryCode", countryCode);
    dialRules->def("LongDistancePrefix", longDistancePrefix);
    dialRules->def("InternationalPrefix", internationalPrefix);
    if (!dialRules->parse()) {
	configError("Parse error in dial string rules \"%s\"", name);
	delete dialRules, dialRules = NULL;
    }
}

/*
 * Convert a dialing string to a canonical format.
 */
fxStr
faxQueueApp::canonicalizePhoneNumber(const fxStr& ds)
{
    if (dialRules)
	return dialRules->canonicalNumber(ds);
    else
	return ds;
}

/*
 * Create an appropriate UUCP lock instance.
 */
UUCPLock*
faxQueueApp::getUUCPLock(const fxStr& deviceName)
{
    return UUCPLock::newLock(uucpLockType,
	uucpLockDir, deviceName, uucpLockMode);
}

u_int faxQueueApp::getTracingLevel() const
    { return tracingLevel; }
u_int faxQueueApp::getMaxConcurrentCalls() const
    { return maxConcurrentCalls; }
u_int faxQueueApp::getMaxSendPages() const
    { return maxSendPages; }
u_int faxQueueApp::getMaxDials() const
    { return maxDials; }
u_int faxQueueApp::getMaxTries() const
    { return maxTries; }
time_t faxQueueApp::nextTimeToSend(time_t t) const
    { return tod.nextTimeOfDay(t); }

/*
 * Miscellaneous stuff.
 */

/*
 * Notify the sender of a job that something has
 * happened -- the job has completed, it's been requeued
 * for later processing, etc.
 */
void
faxQueueApp::notifySender(Job& job, JobStatus why, const char* duration)
{
    fxStr cmd(notifyCmd
	| quote |		 quoted(job.file) | enquote
	| quote | quoted(Job::jobStatusName(why)) | enquote
	| quote |		 quoted(duration) | enquote
    );
    fxStr jobwhy(Job::jobStatusName(why));
    char buf[30];
    const char* argv[6];
    argv[0] = (const char*) notifyCmd;
    argv[1] = (const char*) job.file;
    argv[2] = (const char*) jobwhy;
    argv[3] = duration;
    argv[4] = NULL;
    argv[5] = NULL;

    if (why == Job::requeued) {
	/*
	 * It's too hard to do localtime in an awk script,
	 * so if we may need it, we calculate it here
	 * and pass the result as an optional argument.
	 */
	strftime(buf, sizeof (buf), " '%H:%M'", localtime(&job.tts));
	cmd.append(buf);
	argv[4] = buf;
    }
    traceServer("NOTIFY: %s", (const char*) cmd);

    runCmd(notifyCmd, argv, true, this);
}

void
faxQueueApp::vtraceServer(const char* fmt, va_list ap)
{
    if (tracingLevel & FAXTRACE_SERVER)
	vlogInfo(fmt, ap);
}

void
faxQueueApp::traceServer(const char* fmt ...)
{
    if (tracingLevel & FAXTRACE_SERVER) {
	va_list ap;
	va_start(ap, fmt);
	vlogInfo(fmt, ap);
	va_end(ap);
    }
}

static void
vtraceJob(const Job& job, const char* fmt, va_list ap)
{
    static const char* stateNames[] = {
        "state#0", "suspended", "pending", "sleeping", "blocked",
	"ready", "active", "done", "failed"
    };
    time_t now = Sys::now();
    vlogInfo(
	  "JOB " | job.jobid
	| " (" | stateNames[job.state%9]
	| " dest " | job.dest
	| fxStr::format(" pri %u", job.pri)
	| " tts " | strTime(job.tts - now)
	| " killtime " | strTime(job.killtime - now)
	| "): "
	| fmt, ap);
}

void
faxQueueApp::traceQueue(const Job& job, const char* fmt ...)
{
    if (tracingLevel & FAXTRACE_QUEUEMGMT) {
	va_list ap;
	va_start(ap, fmt);
	vtraceJob(job, fmt, ap);
	va_end(ap);
    }
}

void
faxQueueApp::traceJob(const Job& job, const char* fmt ...)
{
    if (tracingLevel & FAXTRACE_JOBMGMT) {
	va_list ap;
	va_start(ap, fmt);
	vtraceJob(job, fmt, ap);
	va_end(ap);
    }
}

void
faxQueueApp::traceQueue(const char* fmt ...)
{
    if (tracingLevel & FAXTRACE_QUEUEMGMT) {
	va_list ap;
	va_start(ap, fmt);
	vlogInfo(fmt, ap);
	va_end(ap);
    }
}

void
faxQueueApp::traceModem(const Modem& modem, const char* fmt ...)
{
    if (tracingLevel & FAXTRACE_MODEMSTATE) {
	va_list ap;
	va_start(ap, fmt);
	vlogInfo("MODEM " | modem.getDeviceID() | ": " | fmt, ap);
	va_end(ap);
    }
}

void
faxQueueApp::jobError(const Job& job, const char* fmt ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlogError("JOB " | job.jobid | ": " | fmt, ap);
    va_end(ap);
}

void
faxQueueApp::showDebugState(void)
{
    traceServer("DEBUG: Listing modemes");
    for (ModemIter iter(Modem::list); iter.notDone(); iter++) {
	Modem& modem = iter;
	traceModem(modem, "STATE: %X", modem.getState());
    }

    traceServer("DEBUG: Listing destJobs with %d items", destJobs.size());
    for (DestInfoDictIter iter(destJobs); iter.notDone(); iter++)
    {
	const fxStr& dest(iter.key());
	const DestInfo& di(iter.value());
	traceServer("DestInfo (%p) to %s", &di, (const char*)dest);
    }


    for (int i = 0; i < NQHASH; i++)
    {
	traceServer("DEBUG: runqs[%d](%p) next %p", i, &runqs[i], runqs[i].next);
	for (JobIter iter(runqs[i]); iter.notDone(); iter++)
	{
	    Job& job(iter);
	    traceJob(job, "In run queue");
	}
    }

    traceServer("DEBUG: sleepq(%p) next %p", &sleepq, sleepq.next);
    for (JobIter iter(sleepq); iter.notDone(); iter++)
    {
	Job& job(iter);
	traceJob(job, "In sleep queue");
    }

    traceServer("DEBUG: suspendq(%p) next %p", &suspendq, suspendq.next);
    for (JobIter iter(suspendq); iter.notDone(); iter++)
    {
	Job& job(iter);
	traceJob(job, "In suspend queue");
    }

    traceServer("DEBUG: activeq(%p) next %p", &activeq, activeq.next);
    for (JobIter iter(activeq); iter.notDone(); iter++)
    {
	Job& job(iter);
	traceJob(job, "In active queue");
    }

    traceServer("DEBUG: inSchedule: %s", inSchedule ? "YES" : "NO");
}


void faxQueueApp::childStatus(pid_t pid, int status)
{
    // We don't do anything here - nothing to act on.
    //traceServer("Child exit status: %#o (%u)", status, pid);
}

static void
usage(const char* appName)
{
    faxApp::fatal("usage: %s [-q queue-directory] [-D]", appName);
}

static void
sigCleanup(int)
{
    faxQueueApp::instance().close();
    _exit(-1);
}

int
main(int argc, char** argv)
{
    faxApp::setupLogging("FaxQueuer");

    fxStr appName = argv[0];
    u_int l = appName.length();
    appName = appName.tokenR(l, '/');

    faxApp::setupPermissions();

    faxApp::setOpts("q:Dc:");

    bool detach = true;
    fxStr queueDir(FAX_SPOOLDIR);
    for (GetoptIter iter(argc, argv, faxApp::getOpts()); iter.notDone(); iter++)
	switch (iter.option()) {
	case 'q': queueDir = iter.optArg(); break;
	case 'D': detach = false; break;
	case '?': usage(appName);
	}
    if (Sys::chdir(queueDir) < 0)
	faxApp::fatal(queueDir | ": Can not change directory");
    if (!Sys::isRegularFile(FAX_ETCDIR "/setup.cache"))
	faxApp::fatal("No " FAX_ETCDIR "/setup.cache file; run faxsetup first");
    if (detach)
	faxApp::detachFromTTY();

    faxQueueApp* app = new faxQueueApp;

    signal(SIGTERM, fxSIGHANDLER(sigCleanup));
    signal(SIGINT, fxSIGHANDLER(sigCleanup));

    app->initialize(argc, argv);
    app->open();
    while (app->isRunning())
	Dispatcher::instance().dispatch();
    app->close();
    delete app;


    Modem::CLEANUP();
    delete &Dispatcher::instance();
    
    return 0;
}

