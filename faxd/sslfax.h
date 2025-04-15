/*
 * The code in this file was derived from sources taken from (1) GitHub user,
 * "mrwicks", on 9 Oct 2018.  That source, itself, was derived from work by
 * "Amlendra" published at Aticleworld on 21 May 2017 (2).  That work, then,
 * references programs (3) Copyright (c) 2000 Sean Walton and Macmillan
 * Publishers (The "Linux Socket Programming" book) and are licensed under
 * the GPL.
 *
 * 1. https://github.com/mrwicks/miscellaneous/tree/master/tls_1.2_example
 * 2. https://aticleworld.com/ssl-server-client-using-openssl-in-c/
 * 3. http://www.cs.utah.edu/~swalton/listings/sockets/programs/
 *
 * It is, therefore, presumed that this work is either under the* public
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
#ifndef __sslfax_H
#define __sslfax_H

#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <resolv.h>
#include <stdio.h>
#include <netdb.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#include "Str.h"

struct SSLFaxProcess {
    SSL_CTX *ctx;
    SSL *ssl;
    int server;
    int client;
    fxStr addr;		// address string
    fxStr emsg;		// error message
};

struct SSLFax {
public:
    int getAddressFamily(fxStr& address);
    SSLFaxProcess null();
    SSLFaxProcess startServer(fxStr info, fxStr pemFile);
    SSLFaxProcess startClient(fxStr info, fxStr passcode, const u_char* bitrev, long ms);
    void acceptClient(SSLFaxProcess& sfp, fxStr passcode, int modemFd, long ms);
    bool acceptClient1(SSLFaxProcess& sfp, long ms, bool sustain = false);
    void acceptClient2(SSLFaxProcess& sfp, fxStr passcode, int modemFd, long ms, bool sustain = false);
    void cleanup(SSLFaxProcess& sfp, bool sustain = false);
    int pending(SSLFaxProcess& sfp);
    int read(SSLFaxProcess& sfp, void *buf, size_t count, int modemFd, long ms, bool sustain = false, bool carryon = false);
    int write(SSLFaxProcess& sfp, const u_char *buf, u_int count, const u_char* bitrev, int modemFd, long ms, bool eod, bool sustain = false);
};

#endif
