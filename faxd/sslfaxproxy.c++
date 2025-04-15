#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <tiffio.h>
#include <pthread.h>
#include <sys/time.h>

#include "config.h"
#if defined(HAVE_SSL)
#include "Sys.h"
#include "ClassModem.h"
#include "sslfax.h"

const u_char* bitrev = TIFFGetBitRevTable(false);

void
printlog(FILE *fp, const char *fmt, ...)
{
  va_list ap;
  char buf[32];
  struct timeval tv;

  gettimeofday(&tv, NULL);
  time_t tt = tv.tv_sec;
  strftime(buf, sizeof(buf), "[%Y-%m-%d %H:%M:%S", localtime(&tt));
  fprintf(fp, "%s.%.6ld] ", buf, tv.tv_usec);

  va_start(ap, fmt);
  vfprintf(fp, fmt, ap);
  va_end(ap);
  fflush(fp);
}

timeval currentTime() {
    timeval curTime;
    gettimeofday(&curTime, 0);
    return curTime;
}

/*
 * Data structure for pthread argument for SSLFax endpoint thread.
 */
struct sslFaxThreadData {
    int id, rfd, wfd;
    SSLFax* sslfax;
    SSLFaxProcess* sfp;
};

/*
 * Wrapper function for SSLFax endpoint thread.
 */
void*
sslFaxThread(void* sd)
{
    sslFaxThreadData *s = (sslFaxThreadData*) sd;

    if (fcntl(s->rfd, F_SETFL, fcntl(s->rfd, F_GETFL, 0) | O_NONBLOCK) == -1) {
	printlog(stderr, "Unable to set pipe read descriptor to non-blocking.\n");
        return (NULL);
    }

    char buf[1024];
    int cc;
    int loops = 0;
    while ((cc = s->sslfax->read(*(s->sfp), buf, 1024, s->rfd, 60000, true, true)) != 0) {
	if (cc > 0) {
	    // Write read buf data to pipe.
	    cc = Sys::write(s->wfd, buf, cc);
	    if (cc <= 0) {
		s->sfp->emsg = "Error writing to pipe";
		break;
	    }
	} else if (cc == -1) {
	    // Data is waiting to be read from pipe.
	    while ((cc = Sys::read(s->rfd, buf, 1024)) > 0) {
		loops = 0;
		cc = s->sslfax->write(*(s->sfp), (u_char*) buf, cc, bitrev, 0, 60000, false, true);
		if (cc <= 0) {
		    s->sfp->emsg = "Error writing to SSL Fax client";
		    goto done;
		}
	    }
	    if (cc <= 0) {
		if (loops++ > 100 || cc == 0 || errno != EAGAIN) {
		    s->sfp->emsg = "Error reading from pipe";
		    break;
		}
	    }
	} else {
	    // Some error occurred.
	    break;
	}
    }
done:
    Sys::close(s->wfd);
    Sys::close(s->rfd);
    return (NULL);
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

void
pairingThread(SSLFaxProcess* sfp, const char* infohost, const char* pemfile)
{
    // we have a client connection for proxy-server service.  Now start up the other end...

    SSLFax sslfax;
    SSLFaxProcess sslFaxProcess2 = sslfax.startServer(":1", pemfile);	// "1" indicates randomly assigned port

    if (sslFaxProcess2.server) {
	struct sockaddr_in data_addr;
	socklen_t dlen = sizeof (data_addr);
	if (getsockname(sslFaxProcess2.server, (struct sockaddr *) &data_addr, &dlen) < 0) {
	    printlog(stderr, "getsockname(ctrl): %s\n", strerror(errno));
	}

	srand((unsigned int) currentTime().tv_usec % ntohs(data_addr.sin_port));
	const char* passcode = randomString(10);

	fxStr info = fxStr::format("%s@%s:%d", passcode, infohost, ntohs(data_addr.sin_port));
	printlog(stdout, "Listening for paired client on: %s\n", (const char*) info);
	const char* infostr = info;
	if (sslfax.write(*sfp, (const u_char*) infostr, info.length(), bitrev, 0, 60000, false) < 1) {
	    printlog(stderr, "%s\n", (const char*) sfp->emsg);
	}
	u_char buf[1];
	buf[0] = 0;
	if (sslfax.write(*sfp, buf, 1, bitrev, 0, 60000, false) < 1) {
	    printlog(stderr, "%s\n", (const char*) sfp->emsg);
	}

	sslfax.acceptClient(sslFaxProcess2, passcode, 0, 120000);	// 2 min timeout for connection
	printlog(stdout, "SSL Fax accept paired client: %s\n", (const char*) sslFaxProcess2.emsg);

	if (sslFaxProcess2.ssl) {
	    // Make a pretty-looking log entry describing the communication path.
	    printlog(stdout, "%s <--> %s <--> %s\n", (const char*) sfp->addr, (const char*) info, (const char*) sslFaxProcess2.addr);

	    /*
	     * We now have two SSLFax connections, one to each client, and we need simply exchange data
	     * between them.  So, we just read bytes from each connection and write them to the other.
	     * To do both concurrently we'll start up a thread and set of pipes for each and pass data
	     * between the two threads.
	     */
	    int pfd1[2];	// read communication pipe for sslFaxProcess1 ("a")
	    int pfd2[2];	// read communication pipe for sslFaxProcess2 ("b")
	    if (pipe(pfd1) >= 0 && pipe(pfd2) >= 0) {
		pthread_t at;
		struct sslFaxThreadData *a = (struct sslFaxThreadData *) malloc(sizeof(struct sslFaxThreadData));
		a->id = 1;
		a->rfd = pfd1[0];
		a->wfd = pfd2[1];
		a->sslfax = &sslfax;
		a->sfp = sfp;
		pthread_t bt;
		struct sslFaxThreadData *b = (struct sslFaxThreadData *) malloc(sizeof(struct sslFaxThreadData));
		b->id = 2;
		b->rfd = pfd2[0];
		b->wfd = pfd1[1];
		b->sslfax = &sslfax;
		b->sfp = &sslFaxProcess2;
		if (pthread_create(&at, NULL, &sslFaxThread, (void *) a) != 0) {
		    printlog(stderr, "Error starting thread a.\n");
		}
		if (pthread_create(&bt, NULL, &sslFaxThread, (void *) b) != 0) {
		    printlog(stderr, "Error starting thread b.\n");
		}

		pthread_join(at, NULL);
		pthread_join(bt, NULL);

		sslfax.cleanup(sslFaxProcess2, false);

		printlog(stdout, "Terminating connection with client on: %s\n", (const char*) info);
	    } else {
		printlog(stderr, "Error starting communication pipes.\n");
	    }
	} else {
	    // Connection failure message would have been shown above.
	}
    } else {
	printlog(stderr, "Error creating pairing SSL Fax service: %s\n", (const char*) sslFaxProcess2.emsg);
    }
    return;
}

int
main(int argc, char* argv[])
{
    if (argc != 4 && argc != 5) {
	printlog(stderr, "usage: %s port passcode infohost [pemfile]\n", argv[0]);
	exit(-1);
    }
    printlog(stdout, "%s started\n", argv[0]);
    const char* pemfile;
    if (argc == 5) pemfile = argv[4];
    else pemfile = "/var/spool/hylafax/etc/ssl.pem";

    char portstr[7];
    snprintf(portstr, sizeof(portstr), ":%s", argv[1]);

    bool repeat = true;

    SSLFax sslfax;
    SSLFaxProcess sfp = sslfax.startServer(portstr, pemfile);

    do {
	SSLFaxProcess sslFaxProcess;
	sslFaxProcess.ctx = sfp.ctx;
	sslFaxProcess.ssl = NULL;
	sslFaxProcess.emsg = sfp.emsg;
	sslFaxProcess.server = sfp.server;
	sslFaxProcess.client = 0;

	if (sslFaxProcess.server && sslfax.acceptClient1(sslFaxProcess, 0, true)) {
	    printlog(stdout, "%s\n", (const char*) sslFaxProcess.emsg);

	    switch (fork()) {
		case 0:		/* child */
		{
		    sslfax.acceptClient2(sslFaxProcess, argv[2], 0, 2000, true);
		    if (!sslFaxProcess.ssl) {
		    	printlog(stdout, "Error accepting SSL Fax client (%s): %s\n", sslFaxProcess.addr.length() ? (const char*) sslFaxProcess.addr : "no client connection", (const char*) sslFaxProcess.emsg);
		    } else {
			printlog(stdout, "SSL Fax accept client: %s\n", (const char*) sslFaxProcess.emsg);
			pairingThread(&sslFaxProcess, argv[3], pemfile);
		    }
		    sslfax.cleanup(sslFaxProcess, true);
		    exit(0);
		}
		case -1:	/* fork failure */
		    printlog(stderr, "Error forking for SSL Fax service.\n");
		    break;
		default:	/* parent */
		    close(sslFaxProcess.client);	// close parent thread's handle on the forked client
		    break;
	    }

	} else {
	    printlog(stderr, "Error creating primary SSL Fax service (%s): %s\n", sslFaxProcess.addr.length() ? (const char*) sslFaxProcess.addr : "no client connection", (const char*) sslFaxProcess.emsg);
	    sleep(60);
	    if (!sslFaxProcess.server) {
		sfp.emsg = "";
		sfp = sslfax.startServer(portstr, pemfile);
	    }
	}
	int wstatus;
	while (waitpid(-1, &wstatus, WNOHANG) > 0);	// clean up defunct child processes
    } while (repeat);

    exit (0);
}
#else
int
main(int argc, char* argv[])
{
    fprintf(stderr, "%s not available due to lack of SSL Fax support.\n", argv[0]);
    exit (0);
}
#endif
