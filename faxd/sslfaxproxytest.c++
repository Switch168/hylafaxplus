#include <stdio.h>
#include <stdlib.h>
#include <tiffio.h>

#include "config.h"
#include "ClassModem.h"
#if defined(HAVE_SSL)
#include "sslfax.h"
#endif

const u_char* bitrev = TIFFGetBitRevTable(false);

int
main(int argc, char* argv[])
{
#if defined(HAVE_SSL)
    if (argc != 3) {
	fprintf(stderr, "usage: %s info passcode\n", argv[0]);
	exit(-1);
    }

    SSLFax sslfax;
    SSLFaxProcess sslFaxProcess = sslfax.startClient(argv[1], argv[2], bitrev, 1000);
    printf("%s\n", (const char*) sslFaxProcess.emsg);

    char buf[1024];
    int r;
    fxStr info;
    do {
	r = sslfax.read(sslFaxProcess, buf, 1, 0, 60000);
	if (r > 0 && buf[0] != 0) info.append( buf[0]&0xFF );
    } while (r > 0 && (buf[0]&0xFF) != 0);
    printf("Got info: %s\n", (const char*) info);

    int atpos = info.next(0, '@');
    fxStr passcode = info.extract(0, atpos);
    fxStr hostport = info.extract(atpos+1, info.length()-atpos-1);
    printf("Connecting to %s with passcode %s\n", (const char*) hostport, (const char*) passcode);

    SSLFaxProcess sslFaxProcess2 = sslfax.startClient(hostport, passcode, bitrev, 1000);
    printf("%s\n", (const char*) sslFaxProcess2.emsg);

    u_char test[7] = { 0xFF, 0x13, 0x84, 0xEA, 0x7D, 0x10, 0x03 };  // a CFR signal

    do {

	if (sslfax.write(sslFaxProcess2, test, 7, bitrev, 0, 60000, false) < 1) {
	    printf("%s\n", (const char*) sslFaxProcess2.emsg);
	}
	int cc = 0;
	while (cc < 7) {
	    int cr = sslfax.read(sslFaxProcess, buf, 1024, 0, 60000);
	    if (cr > 0) cc += cr;
	}
	printf("%.2X/%.2X %.2X/%.2X %.2X/%.2X %.2X/%.2X %.2X/%.2X %.2X/%.2X %.2X/%.2X\n", buf[0]&0xFF, test[0], buf[1]&0xFF, test[1], buf[2]&0xFF, test[2], buf[3]&0xFF, test[3], buf[4]&0xFF, test[4], buf[5]&0xFF, test[5], buf[6]&0xFF, test[6]);

	if (sslfax.write(sslFaxProcess, test, 7, bitrev, 0, 60000, false) < 1) {
	    printf("%s\n", (const char*) sslFaxProcess.emsg);
	}
        cc = 0;
	while (cc < 7) {
	    int cr = sslfax.read(sslFaxProcess2, buf, 1024, 0, 60000);
	    if (cr > 0) cc += cr;
	}
	printf("%.2X/%.2X %.2X/%.2X %.2X/%.2X %.2X/%.2X %.2X/%.2X %.2X/%.2X %.2X/%.2X\n", buf[0]&0xFF, test[0], buf[1]&0xFF, test[1], buf[2]&0xFF, test[2], buf[3]&0xFF, test[3], buf[4]&0xFF, test[4], buf[5]&0xFF, test[5], buf[6]&0xFF, test[6]);

    } while (true);
#endif
}
