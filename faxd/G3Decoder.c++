/*	$Id: G3Decoder.c++ 139 2006-04-15 07:04:20Z faxguy $ */
/*
 * Copyright (c) 1994-1996 Sam Leffler
 * Copyright (c) 1994-1996 Silicon Graphics, Inc.
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
 * Group 3 Facsimile Reader Support.
 */
#include "G3Decoder.h"

/*
 * These macros glue the G3Decoder state to
 * the state expected by Frank Cringle's decoder.
 */
#define	DECLARE_STATE_EOL()						\
    uint32_t BitAcc;			/* bit accumulator */		\
    int BitsAvail;			/* # valid bits in BitAcc */	\
    int EOLcnt				/* # EOL codes recognized */
#define	DECLARE_STATE()							\
    DECLARE_STATE_EOL();						\
    int a0;				/* reference element */		\
    int RunLength;			/* length of current run */	\
    tiff_runlen_t* pa;			/* place to stuff next run */	\
    tiff_runlen_t* thisrun;			/* current row's run array */	\
    const hf_TIFFFaxTabEnt* TabEnt
#define	DECLARE_STATE_2D()						\
    DECLARE_STATE();							\
    int b1;				/* next change on prev line */	\
    tiff_runlen_t* pb			/* next run in reference line */
/*
 * Load any state that may be changed during decoding.
 */
#define	CACHE_STATE() do {						\
    BitAcc = data;							\
    BitsAvail = bit;							\
    EOLcnt = this->EOLcnt;						\
} while (0)
/*
 * Save state possibly changed during decoding.
 */
#define	UNCACHE_STATE() do {						\
    bit = BitsAvail;							\
    data = BitAcc;							\
    this->EOLcnt = EOLcnt;						\
} while (0)

/*
 * Override default definitions for the TIFF library.
 * We redirect the logic to call nextByte for each
 * input byte we need.  Note that we don't need to check
 * for EOF because the input decoder does a longjmp.
 */
#define NeedBits8(n,eoflab) do {					\
    if (BitsAvail < (n)) {						\
	BitAcc |= nextByte()<<BitsAvail;				\
	BitsAvail += 8;							\
    }									\
} while (0)
#define NeedBits16(n,eoflab) do {					\
    if (BitsAvail < (n)) {						\
	BitAcc |= nextByte()<<BitsAvail;				\
	if ((BitsAvail += 8) < (n)) {					\
	    BitAcc |= nextByte()<<BitsAvail;				\
	    BitsAvail += 8;						\
	}								\
    }									\
} while (0)

#include "tif_fax3.h"

G3Decoder::G3Decoder() {}
G3Decoder::~G3Decoder() {}

void
G3Decoder::setupDecoder(u_int recvFillOrder, bool is2d, bool isg4)
{
    /*
     * The G3 decoding state tables are constructed for
     * data in LSB2MSB bit order.  Received data that
     * is not in this order is reversed using the
     * appropriate byte-wide bit-reversal table.
     */
    is2D = is2d;
    isG4 = isg4;
    bitmap = TIFFGetBitRevTable(recvFillOrder != FILLORDER_LSB2MSB);
    data = 0;					// not needed
    bit = 0;					// force initial read
    EOLcnt = 0;					// no initial EOL
    RTCrun = 0;					// reset run count
    RTCrow = -1;				// reset RTC row number
    rowref = 0;					// reset row count
    curruns = refruns = NULL;
}

void
G3Decoder::setRuns(tiff_runlen_t* cr, tiff_runlen_t* rr, int w)
{
    curruns = cr;
    if ((refruns = rr)) {
	refruns[0] = w;
	refruns[1] = 0;
    }
}

void G3Decoder::raiseEOF()	{ siglongjmp(jmpEOF, 1); }
void G3Decoder::raiseRTC()	{ siglongjmp(jmpRTC, 1); }

/*
 * Decode h rows that are w pixels wide and return
 * the decoded data in raster.
 */
void
G3Decoder::decode(void* raster, u_int w, u_int h)
{
    u_int rowbytes = howmany(w, 8);
    if (curruns == NULL) {
	tiff_runlen_t runs[2*4864];		// run arrays for cur+ref rows
	setRuns(runs, runs+4864, w);
	while (h-- > 0) {
	    decodeRow(raster, w);
	    if (raster)
		raster = (u_char*) raster + rowbytes;
	}
    } else {
	while (h-- > 0) {
	    decodeRow(raster, w);
	    if (raster)
		raster = (u_char*) raster + rowbytes;
	}
    }
}

bool
G3Decoder::isNextRow1D()
{
    DECLARE_STATE_EOL();

    CACHE_STATE();
    SYNC_EOL(Nop);
    bool is1D;
    if (is2D) {
	NeedBits8(1, Nop);
	is1D = (GetBits(1) != 0);	// 1D/2D-encoding tag bit
	// NB: we don't clear the tag bit
    } else {
	is1D = true;
    }
    // now reset state for next decoded row
    BitAcc = (BitAcc<<1)|1;		// EOL bit
    BitsAvail++;
    EOLcnt = 1;				// mark state to indicate EOL recognized
    UNCACHE_STATE();
    return (is1D);
}

#define	unexpected(table, a0) do {		\
    invalidCode(table, a0);			\
    rowgood = false;				\
} while (0)
#define	extension(a0) do {			\
    invalidCode("2D", a0);			\
    rowgood = false;				\
} while (0)
#define	prematureEOF(a0)	// never happens 'cuz of longjmp
#define	SWAP(t,a,b)	{ t x; x = (a); (a) = (b); (b) = x; }

#define isAligned(p, t) ((((size_t)(p)) & (sizeof(t) - 1)) == 0)

#define FILL(n, cp)                                                            \
    for (int32_t ifill = 0; ifill < (n); ++ifill)                              \
    {                                                                          \
        (cp)[ifill] = 0xff;                                                    \
    }                                                                          \
    (cp) += (n);

#define ZERO(n, cp)                                                            \
    for (int32_t izero = 0; izero < (n); ++izero)                              \
    {                                                                          \
        (cp)[izero] = 0;                                                       \
    }                                                                          \
    (cp) += (n);

/*
 * Bit-fill a row according to the white/black
 * runs generated during G3/G4 decoding.
 * Lifted from libtiff-4.5.0 source.
 */
void hf_TIFFFax3fillruns(unsigned char *buf, uint32_t *runs, uint32_t *erun,
                       uint32_t lastx)
{
    static const unsigned char _fillmasks[] = {0x00, 0x80, 0xc0, 0xe0, 0xf0,
                                               0xf8, 0xfc, 0xfe, 0xff};
    unsigned char *cp;
    uint32_t x, bx, run;
    int32_t n, nw;
    int64_t *lp;

    if ((erun - runs) & 1)
        *erun++ = 0;
    x = 0;
    for (; runs < erun; runs += 2)
    {
        run = runs[0];
        if (x + run > lastx || run > lastx)
            run = runs[0] = (uint32_t)(lastx - x);
        if (run)
        {
            cp = buf + (x >> 3);
            bx = x & 7;
            if (run > 8 - bx)
            {
                if (bx)
                { /* align to byte boundary */
                    *cp++ &= 0xff << (8 - bx);
                    run -= 8 - bx;
                }
                if ((n = run >> 3) != 0)
                { /* multiple bytes to fill */
                    if ((n / sizeof(int64_t)) > 1)
                    {
                        /*
                         * Align to int64_tword boundary and fill.
                         */
                        for (; n && !isAligned(cp, int64_t); n--)
                            *cp++ = 0x00;
                        lp = (int64_t *)cp;
                        nw = (int32_t)(n / sizeof(int64_t));
                        n -= nw * sizeof(int64_t);
                        do
                        {
                            *lp++ = 0L;
                        } while (--nw);
                        cp = (unsigned char *)lp;
                    }
                    ZERO(n, cp);
                    run &= 7;
                }
                if (run)
                    cp[0] &= 0xff >> run;
            }
            else
                cp[0] &= ~(_fillmasks[run] >> bx);
            x += runs[0];
        }
        run = runs[1];
        if (x + run > lastx || run > lastx)
            run = runs[1] = lastx - x;
        if (run)
        {
            cp = buf + (x >> 3);
            bx = x & 7;
            if (run > 8 - bx)
            {
                if (bx)
                { /* align to byte boundary */
                    *cp++ |= 0xff >> bx;
                    run -= 8 - bx;
                }
                if ((n = run >> 3) != 0)
                { /* multiple bytes to fill */
                    if ((n / sizeof(int64_t)) > 1)
                    {
                        /*
                         * Align to int64_t boundary and fill.
                         */
                        for (; n && !isAligned(cp, int64_t); n--)
                            *cp++ = 0xff;
                        lp = (int64_t *)cp;
                        nw = (int32_t)(n / sizeof(int64_t));
                        n -= nw * sizeof(int64_t);
                        do
                        {
                            *lp++ = -1L;
                        } while (--nw);
                        cp = (unsigned char *)lp;
                    }
                    FILL(n, cp);
                    run &= 7;
                }
                /* Explicit 0xff masking to make icc -check=conversions happy */
                if (run)
                    cp[0] = (unsigned char)((cp[0] | (0xff00 >> run)) & 0xff);
            }
            else
                cp[0] |= _fillmasks[run] >> bx;
            x += runs[1];
        }
    }
    assert(x == lastx);
}
#undef ZERO
#undef FILL

/*
 * Decode a single row of pixels and return
 * the decoded data in the scanline buffer.
 */
bool
G3Decoder::decodeRow(void* scanline, u_int lastx)
{
    DECLARE_STATE_2D();
    bool rowgood = true;
    bool nullrow = false;

    CACHE_STATE();
    a0 = 0;
    RunLength = 0;
    pa = thisrun = curruns;
    bool is1D;
    if (isG4) {
	is1D = false;
    } else {
	SYNC_EOL(Nop);
	if (is2D) {
	    NeedBits8(1, Nop);
	    is1D = (GetBits(1) != 0);	// 1D/2D-encoding tag bit
	    ClrBits(1);
	} else
	    is1D = true;
    }
    if (!is1D) {
	pb = refruns;
	b1 = *pb++;
#define	badlength(a0,lastx) do {			\
    if (isG4 && RTCrow == -1)				\
	RTCrow = rowref;				\
    badPixelCount("2D", a0, lastx);			\
    rowgood = false;					\
} while (0)
	EXPAND2D(Nop2d);
    Nop2d:;
#undef	badlength
    } else {
#define	badlength(a0,lastx) do {			\
    nullrow = (a0 == 0);				\
    if (nullrow && ++RTCrun == 5 && RTCrow == -1)	\
	RTCrow = rowref-4;				\
    badPixelCount("1D", a0, lastx);			\
    rowgood = false;					\
} while (0)
	EXPAND1D(Nop1d);
    Nop1d:;
#undef	badlength
    }
    if (!nullrow)
	RTCrun = 0;
    if (scanline)
	hf_TIFFFax3fillruns((u_char*) scanline, thisrun, pa, lastx);
    if (is2D) {
	SETVAL(0);			// imaginary change for reference
	SWAP(tiff_runlen_t*, curruns, refruns);
    }
    rowref++;
    UNCACHE_STATE();
    return (rowgood);
}

#undef	SWAP
#undef	prematureEOF
#undef	extension
#undef	unexpected

/*
 * Return the next decoded byte of page data from
 * the input stream.  The byte is returned in the
 * bit order required by the G3 decoder.
 */
int
G3Decoder::nextByte()
{
    return bitmap[decodeNextByte()];
}

int G3Decoder::decodeNextByte() { raiseEOF(); return (0); }

void G3Decoder::invalidCode(const char*, int) {}
void G3Decoder::badPixelCount(const char*, int, int) {}
void G3Decoder::badDecodingState(const char*, int) {}
