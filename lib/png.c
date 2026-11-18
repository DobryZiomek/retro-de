/*
 * libretroui: inflate (RFC 1951), zlib and gzip wrappers, and a PNG
 * decoder - no zlib, no libpng.  Used for PNG icons and wallpapers and for
 * .tar.gz theme packages.
 *
 * The inflater follows the canonical-Huffman scheme of the RFC: code
 * lengths -> counts per length -> symbols sorted by code, decoded one bit
 * at a time.  Slow next to zlib, but icons and wallpapers are small.
 */
#include "retroui.h"

#include <stdlib.h>
#include <string.h>

#define MAXBITS 15
#define MAX_OUT ((size_t)256 << 20)    /* refuse to grow past 256 MiB    */

typedef struct Inf {
	const unsigned char *in;
	size_t inlen, inpos;
	unsigned bitbuf;
	int bitcnt;
	unsigned char *out;
	size_t outlen, outcap, limit;
	int err;
} Inf;

typedef struct Huff {
	short count[MAXBITS + 1];
	short symbol[320];
} Huff;

static int
bits(Inf *s, int need)
{
	long val = s->bitbuf;

	while (s->bitcnt < need) {
		if (s->inpos >= s->inlen) {
			s->err = 1;
			return 0;
		}
		val |= (long)s->in[s->inpos++] << s->bitcnt;
		s->bitcnt += 8;
	}
	s->bitbuf = (unsigned)(val >> need);
	s->bitcnt -= need;
	return (int)(val & ((1L << need) - 1));
}

static int
put(Inf *s, unsigned char c)
{
	if (s->outlen >= s->limit) {
		s->err = 1;
		return -1;
	}
	if (s->outlen == s->outcap) {
		s->outcap = s->outcap ? s->outcap * 2 : 65536;
		if (s->outcap > s->limit)
			s->outcap = s->limit;
		s->out = rui_realloc(s->out, s->outcap);
	}
	s->out[s->outlen++] = c;
	return 0;
}

static int
stored(Inf *s)
{
	unsigned len;

	s->bitbuf = 0;
	s->bitcnt = 0;
	if (s->inpos + 4 > s->inlen)
		return -1;
	len = s->in[s->inpos] | (unsigned)s->in[s->inpos + 1] << 8;
	if ((s->in[s->inpos + 2] ^ 0xff) != (len & 0xff) ||
	    (s->in[s->inpos + 3] ^ 0xff) != (len >> 8))
		return -1;
	s->inpos += 4;
	if (s->inpos + len > s->inlen)
		return -1;
	while (len--)
		if (put(s, s->in[s->inpos++]) < 0)
			return -1;
	return 0;
}

static int
decode(Inf *s, const Huff *h)
{
	int len, code = 0, first = 0, index = 0;

	for (len = 1; len <= MAXBITS; len++) {
		int count;

		code |= bits(s, 1);
		if (s->err)
			return -1;
		count = h->count[len];
		if (code - count < first)
			return h->symbol[index + (code - first)];
		index += count;
		first += count;
		first <<= 1;
		code <<= 1;
	}
	return -1;
}

/* 0 complete, >0 incomplete (allowed for single codes), <0 oversubscribed */
static int
construct(Huff *h, const short *length, int n)
{
	short offs[MAXBITS + 1];
	int symbol, len, left;

	for (len = 0; len <= MAXBITS; len++)
		h->count[len] = 0;
	for (symbol = 0; symbol < n; symbol++)
		h->count[length[symbol]]++;
	if (h->count[0] == n)
		return 0;
	left = 1;
	for (len = 1; len <= MAXBITS; len++) {
		left <<= 1;
		left -= h->count[len];
		if (left < 0)
			return left;
	}
	offs[1] = 0;
	for (len = 1; len < MAXBITS; len++)
		offs[len + 1] = (short)(offs[len] + h->count[len]);
	for (symbol = 0; symbol < n; symbol++)
		if (length[symbol] != 0)
			h->symbol[offs[length[symbol]]++] = (short)symbol;
	return left;
}

static const short lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19,
	23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const short lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
	2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const short dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49,
	65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
	6145, 8193, 12289, 16385, 24577 };
static const short dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5,
	6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static int
codes(Inf *s, const Huff *lencode, const Huff *distcode)
{
	int symbol;

	do {
		symbol = decode(s, lencode);
		if (symbol < 0)
			return -1;
		if (symbol < 256) {
			if (put(s, (unsigned char)symbol) < 0)
				return -1;
		} else if (symbol > 256) {
			int len, d;
			size_t dist;

			symbol -= 257;
			if (symbol >= 29)
				return -1;
			len = lbase[symbol] + bits(s, lext[symbol]);
			d = decode(s, distcode);
			if (d < 0 || d >= 30)
				return -1;
			dist = (size_t)(dbase[d] + bits(s, dext[d]));
			if (s->err || dist > s->outlen)
				return -1;
			while (len--)
				if (put(s, s->out[s->outlen - dist]) < 0)
					return -1;
		}
	} while (symbol != 256);
	return 0;
}

static int
fixed(Inf *s)
{
	static Huff lencode, distcode;
	static int built;

	if (!built) {
		short lengths[288];
		int i;

		for (i = 0; i < 144; i++)
			lengths[i] = 8;
		for (; i < 256; i++)
			lengths[i] = 9;
		for (; i < 280; i++)
			lengths[i] = 7;
		for (; i < 288; i++)
			lengths[i] = 8;
		construct(&lencode, lengths, 288);
		for (i = 0; i < 30; i++)
			lengths[i] = 5;
		construct(&distcode, lengths, 30);
		built = 1;
	}
	return codes(s, &lencode, &distcode);
}

static int
dynamic(Inf *s)
{
	static const short order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11,
		4, 12, 3, 13, 2, 14, 1, 15 };
	short lengths[320];
	Huff lencode, distcode;
	int nlen, ndist, ncode, index, err;

	nlen = bits(s, 5) + 257;
	ndist = bits(s, 5) + 1;
	ncode = bits(s, 4) + 4;
	if (s->err || nlen > 286 || ndist > 30)
		return -1;
	for (index = 0; index < ncode; index++)
		lengths[order[index]] = (short)bits(s, 3);
	for (; index < 19; index++)
		lengths[order[index]] = 0;
	if (s->err || construct(&lencode, lengths, 19) != 0)
		return -1;
	index = 0;
	while (index < nlen + ndist) {
		int symbol = decode(s, &lencode), len = 0;

		if (symbol < 0)
			return -1;
		if (symbol < 16) {
			lengths[index++] = (short)symbol;
			continue;
		}
		if (symbol == 16) {
			if (index == 0)
				return -1;
			len = lengths[index - 1];
			symbol = 3 + bits(s, 2);
		} else if (symbol == 17) {
			symbol = 3 + bits(s, 3);
		} else {
			symbol = 11 + bits(s, 7);
		}
		if (s->err || index + symbol > nlen + ndist)
			return -1;
		while (symbol--)
			lengths[index++] = (short)len;
	}
	if (lengths[256] == 0)
		return -1;
	err = construct(&lencode, lengths, nlen);
	if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1))
		return -1;
	err = construct(&distcode, lengths + nlen, ndist);
	if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1))
		return -1;
	return codes(s, &lencode, &distcode);
}

/* raw deflate data -> malloc'd bytes (*outlen), or NULL; *used tells how
 * much input was consumed */
unsigned char *
rui_inflate(const unsigned char *in, size_t inlen, size_t *outlen,
    size_t *used)
{
	Inf s;
	int last, r = 0;

	memset(&s, 0, sizeof s);
	s.in = in;
	s.inlen = inlen;
	s.limit = MAX_OUT;
	do {
		int type;

		last = bits(&s, 1);
		type = bits(&s, 2);
		if (s.err)
			r = -1;
		else if (type == 0)
			r = stored(&s);
		else if (type == 1)
			r = fixed(&s);
		else if (type == 2)
			r = dynamic(&s);
		else
			r = -1;
	} while (!last && r == 0 && !s.err);
	if (r < 0 || s.err) {
		free(s.out);
		return NULL;
	}
	if (!s.out)
		s.out = rui_malloc(1);
	*outlen = s.outlen;
	if (used)
		*used = s.inpos;
	return s.out;
}

unsigned char *
rui_zlib_inflate(const unsigned char *in, size_t inlen, size_t *outlen)
{
	if (inlen < 6 || (in[0] & 0x0f) != 8 || ((in[0] << 8) | in[1]) % 31 ||
	    (in[1] & 0x20))
		return NULL;
	return rui_inflate(in + 2, inlen - 2, outlen, NULL);
}

/* gzip (RFC 1952), one member */
unsigned char *
rui_gunzip(const unsigned char *in, size_t inlen, size_t *outlen)
{
	size_t pos = 10;
	int flg;

	if (inlen < 18 || in[0] != 0x1f || in[1] != 0x8b || in[2] != 8)
		return NULL;
	flg = in[3];
	if (flg & 4) {                         /* FEXTRA */
		if (pos + 2 > inlen)
			return NULL;
		pos += 2 + (in[pos] | (size_t)in[pos + 1] << 8);
	}
	if (flg & 8)                           /* FNAME */
		while (pos < inlen && in[pos++])
			;
	if (flg & 16)                          /* FCOMMENT */
		while (pos < inlen && in[pos++])
			;
	if (flg & 2)                           /* FHCRC */
		pos += 2;
	if (pos >= inlen)
		return NULL;
	return rui_inflate(in + pos, inlen - pos, outlen, NULL);
}

/* ----------------------------------------------------------------- crc32 */

unsigned long
rui_crc32(unsigned long crc, const unsigned char *p, size_t n)
{
	static unsigned long table[256];
	static int have;

	if (!have) {
		unsigned long c;
		int i, k;

		for (i = 0; i < 256; i++) {
			c = (unsigned long)i;
			for (k = 0; k < 8; k++)
				c = c & 1 ? 0xedb88320UL ^ (c >> 1) : c >> 1;
			table[i] = c;
		}
		have = 1;
	}
	crc ^= 0xffffffffUL;
	while (n--)
		crc = table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
	return crc ^ 0xffffffffUL;
}

/* gzip with stored (uncompressed) deflate blocks: valid everywhere, made
 * without a compressor (theme packages are mostly PNG/XPM anyway) */
unsigned char *
rui_gzip_store(const unsigned char *in, size_t inlen, size_t *outlen)
{
	size_t nblocks = inlen / 65535 + 1, cap = 10 + inlen + nblocks * 5 + 8;
	unsigned char *out = rui_malloc(cap), *o = out;
	unsigned long crc = rui_crc32(0, in, inlen);
	size_t left = inlen;
	int i;

	memcpy(o, "\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\x03", 10);
	o += 10;
	do {
		size_t n = left > 65535 ? 65535 : left;

		*o++ = left == n ? 1 : 0;
		*o++ = (unsigned char)(n & 0xff);
		*o++ = (unsigned char)(n >> 8);
		*o++ = (unsigned char)(~n & 0xff);
		*o++ = (unsigned char)((~n >> 8) & 0xff);
		memcpy(o, in + (inlen - left), n);
		o += n;
		left -= n;
	} while (left);
	for (i = 0; i < 4; i++)
		*o++ = (unsigned char)(crc >> (8 * i));
	for (i = 0; i < 4; i++)
		*o++ = (unsigned char)((inlen & 0xffffffffUL) >> (8 * i));
	*outlen = (size_t)(o - out);
	return out;
}

/* ------------------------------------------------------------------- PNG */

static unsigned long
be32(const unsigned char *p)
{
	return (unsigned long)p[0] << 24 | (unsigned long)p[1] << 16 |
	    (unsigned long)p[2] << 8 | p[3];
}

static int
paeth(int a, int b, int c)
{
	int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);

	if (pa <= pb && pa <= pc)
		return a;
	return pb <= pc ? b : c;
}

/* undoes the filters of one (sub-)image in place; 0 ok */
static int
unfilter(unsigned char *data, int w, int h, int bpp_bits, size_t *pos,
    size_t len, unsigned char **rows)
{
	size_t stride = ((size_t)w * (size_t)bpp_bits + 7) / 8;
	int bpp = bpp_bits < 8 ? 1 : bpp_bits / 8, y;
	size_t x;
	unsigned char *prev = NULL;

	for (y = 0; y < h; y++) {
		unsigned char *row, ft;

		if (*pos + 1 + stride > len)
			return -1;
		ft = data[(*pos)++];
		row = data + *pos;
		*pos += stride;
		for (x = 0; x < stride; x++) {
			int a = x >= (size_t)bpp ? row[x - (size_t)bpp] : 0;
			int b = prev ? prev[x] : 0;
			int c = prev && x >= (size_t)bpp ? prev[x - (size_t)bpp] : 0;

			switch (ft) {
			case 0: break;
			case 1: row[x] = (unsigned char)(row[x] + a); break;
			case 2: row[x] = (unsigned char)(row[x] + b); break;
			case 3: row[x] = (unsigned char)(row[x] + ((a + b) >> 1)); break;
			case 4: row[x] = (unsigned char)(row[x] + paeth(a, b, c)); break;
			default: return -1;
			}
		}
		rows[y] = row;
		prev = row;
	}
	return 0;
}

typedef struct PngInfo {
	int w, h, depth, ctype;
	unsigned char pal[256][4];
	int npal;
	int trns[3], has_trns;             /* grey or RGB transparent colour */
} PngInfo;

static int
sample(const unsigned char *row, int x, int depth, int idx)
{
	if (depth == 8)
		return row[idx];
	if (depth == 16)
		return row[idx * 2] << 8 | row[idx * 2 + 1];
	{
		int per = 8 / depth, byte = idx / per, shift;

		(void)x;
		shift = 8 - depth * (idx % per + 1);
		return (row[byte] >> shift) & ((1 << depth) - 1);
	}
}

#define SC(v) (((v) * 255 + max / 2) / max)
static void
pixel(const PngInfo *pi, const unsigned char *row, int x, unsigned char *o)
{
	int d = pi->depth, max = (1 << d) - 1, ch;

	switch (pi->ctype) {
	case 0: {
		int g = sample(row, x, d, x);

		ch = SC(g);
		o[0] = o[1] = o[2] = (unsigned char)ch;
		o[3] = pi->has_trns && g == pi->trns[0] ? 0 : 255;
		break;
	}
	case 2: {
		int r = sample(row, x, d, x * 3), g = sample(row, x, d, x * 3 + 1),
		    b = sample(row, x, d, x * 3 + 2);

		o[0] = (unsigned char)(SC(r));
		o[1] = (unsigned char)(SC(g));
		o[2] = (unsigned char)(SC(b));
		o[3] = pi->has_trns && r == pi->trns[0] && g == pi->trns[1] &&
		    b == pi->trns[2] ? 0 : 255;
		break;
	}
	case 3: {
		int i = sample(row, x, d, x);

		if (i < pi->npal)
			memcpy(o, pi->pal[i], 4);
		else
			o[0] = o[1] = o[2] = o[3] = 0;
		break;
	}
	case 4:
		o[0] = o[1] = o[2] = (unsigned char)(SC(sample(row, x, d, x * 2)));
		o[3] = (unsigned char)(SC(sample(row, x, d, x * 2 + 1)));
		break;
	default:
		for (ch = 0; ch < 4; ch++)
			o[ch] = (unsigned char)(SC(sample(row, x, d, x * 4 + ch)));
		break;
	}
}

#undef SC

/* PNG bytes -> malloc'd RGBA (w*h*4), or NULL */
unsigned char *
rui_png_decode(const unsigned char *buf, size_t len, int *pw, int *ph)
{
	static const int channels[7] = { 1, 0, 3, 1, 2, 0, 4 };
	static const int ax[7] = { 0, 4, 0, 2, 0, 1, 0 },
	    ay[7] = { 0, 0, 4, 0, 2, 0, 1 }, dx[7] = { 8, 8, 4, 4, 2, 2, 1 },
	    dy[7] = { 8, 8, 8, 4, 4, 2, 2 };
	PngInfo pi;
	unsigned char *idat = NULL, *raw = NULL, *out = NULL, **rows = NULL;
	size_t ilen = 0, icap = 0, rawlen = 0, pos = 8, rp = 0;
	int interlace = 0, bpp, pass, ok = 0;

	memset(&pi, 0, sizeof pi);
	if (len < 33 || memcmp(buf, "\x89PNG\r\n\x1a\n", 8))
		return NULL;
	while (pos + 12 <= len) {
		unsigned long clen = be32(buf + pos);
		const unsigned char *type = buf + pos + 4, *d = buf + pos + 8;

		if (clen > len - pos - 12)
			goto done;
		if (!memcmp(type, "IHDR", 4) && clen >= 13) {
			pi.w = (int)be32(d);
			pi.h = (int)be32(d + 4);
			pi.depth = d[8];
			pi.ctype = d[9];
			interlace = d[12];
			if (pi.w <= 0 || pi.h <= 0 || pi.w > 16384 || pi.h > 16384 ||
			    (double)pi.w * pi.h > 64e6 || pi.ctype > 6 ||
			    !channels[pi.ctype] || d[10] || d[11] || interlace > 1 ||
			    !(pi.depth == 1 || pi.depth == 2 || pi.depth == 4 ||
			    pi.depth == 8 || pi.depth == 16) ||
			    (pi.ctype == 3 && pi.depth == 16) ||
			    (pi.ctype != 0 && pi.ctype != 3 && pi.depth < 8))
				goto done;
		} else if (!memcmp(type, "PLTE", 4)) {
			unsigned long i;

			pi.npal = (int)(clen / 3 > 256 ? 256 : clen / 3);
			for (i = 0; i < (unsigned long)pi.npal; i++) {
				memcpy(pi.pal[i], d + i * 3, 3);
				pi.pal[i][3] = 255;
			}
		} else if (!memcmp(type, "tRNS", 4)) {
			unsigned long i;

			if (pi.ctype == 3) {
				for (i = 0; i < clen && i < 256; i++)
					pi.pal[i][3] = d[i];
			} else if (pi.ctype == 0 && clen >= 2) {
				pi.trns[0] = d[0] << 8 | d[1];
				pi.has_trns = 1;
			} else if (pi.ctype == 2 && clen >= 6) {
				for (i = 0; i < 3; i++)
					pi.trns[i] = d[i * 2] << 8 | d[i * 2 + 1];
				pi.has_trns = 1;
			}
		} else if (!memcmp(type, "IDAT", 4)) {
			if (ilen + clen > icap) {
				icap = (ilen + clen) * 2;
				idat = rui_realloc(idat, icap);
			}
			memcpy(idat + ilen, d, clen);
			ilen += clen;
		} else if (!memcmp(type, "IEND", 4)) {
			break;
		}
		pos += 12 + clen;
	}
	if (!pi.w || !idat || !(raw = rui_zlib_inflate(idat, ilen, &rawlen)))
		goto done;
	bpp = channels[pi.ctype] * pi.depth;
	out = rui_calloc((size_t)pi.w * (size_t)pi.h, 4);
	rows = rui_malloc((size_t)pi.h * sizeof *rows);
	for (pass = interlace ? 0 : -1; pass < (interlace ? 7 : 0); pass++) {
		int sw, sh, x, y, ox = 0, oy = 0, sx = 1, sy = 1;

		if (pass < 0) {
			sw = pi.w;
			sh = pi.h;
		} else {
			ox = ax[pass];
			oy = ay[pass];
			sx = dx[pass];
			sy = dy[pass];
			sw = (pi.w - ox + sx - 1) / sx;
			sh = (pi.h - oy + sy - 1) / sy;
			if (sw <= 0 || sh <= 0)
				continue;
		}
		if (unfilter(raw, sw, sh, bpp, &rp, rawlen, rows) < 0)
			goto done;
		for (y = 0; y < sh; y++)
			for (x = 0; x < sw; x++)
				pixel(&pi, rows[y], x, out + (((size_t)(oy + y * sy) *
				    (size_t)pi.w) + (size_t)(ox + x * sx)) * 4);
	}
	ok = 1;
	*pw = pi.w;
	*ph = pi.h;
done:
	free(idat);
	free(raw);
	free(rows);
	if (!ok) {
		free(out);
		return NULL;
	}
	return out;
}
