/*
 * retro-dbus: the D-Bus wire format (marshalling of the basic and container
 * types, and message headers), in either byte order.
 */
#include "dbus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------- writer */

void
db_init(DBuf *b, int big)
{
	memset(b, 0, sizeof *b);
	b->big = big;
}

void
db_free(DBuf *b)
{
	free(b->p);
	memset(b, 0, sizeof *b);
}

static void
grow(DBuf *b, size_t n)
{
	if (b->len + n > b->cap) {
		while (b->len + n > b->cap)
			b->cap = b->cap ? b->cap * 2 : 256;
		b->p = rui_realloc(b->p, b->cap);
	}
}

void
db_pad(DBuf *b, size_t align)
{
	while (b->len % align) {
		grow(b, 1);
		b->p[b->len++] = 0;
	}
}

void
db_byte(DBuf *b, unsigned v)
{
	grow(b, 1);
	b->p[b->len++] = (unsigned char)v;
}

void
db_u32(DBuf *b, uint32_t v)
{
	int i;

	db_pad(b, 4);
	grow(b, 4);
	for (i = 0; i < 4; i++)
		b->p[b->len + (size_t)i] = (unsigned char)(v >> (b->big ? 24 - 8 * i :
		    8 * i));
	b->len += 4;
}

void
db_u64(DBuf *b, uint64_t v)
{
	int i;

	db_pad(b, 8);
	grow(b, 8);
	for (i = 0; i < 8; i++)
		b->p[b->len + (size_t)i] = (unsigned char)(v >> (b->big ? 56 - 8 * i :
		    8 * i));
	b->len += 8;
}

void
db_i32(DBuf *b, int32_t v)
{
	db_u32(b, (uint32_t)v);
}

void
db_bool(DBuf *b, int v)
{
	db_u32(b, v ? 1 : 0);
}

void
db_str(DBuf *b, const char *s)
{
	size_t n = strlen(s);

	db_u32(b, (uint32_t)n);
	grow(b, n + 1);
	memcpy(b->p + b->len, s, n + 1);
	b->len += n + 1;
}

void
db_sig(DBuf *b, const char *s)
{
	size_t n = strlen(s);

	db_byte(b, (unsigned)n);
	grow(b, n + 1);
	memcpy(b->p + b->len, s, n + 1);
	b->len += n + 1;
}

/* the array length is filled in by db_array_end; it counts from the first
 * element, after that element's alignment padding */
size_t
db_array_begin(DBuf *b, size_t elem_align)
{
	size_t mark;

	db_u32(b, 0);
	mark = b->len;
	db_pad(b, elem_align);
	return mark << 8 | (b->len - mark);   /* where, and the padding */
}

void
db_array_end(DBuf *b, size_t mark)
{
	size_t at = mark >> 8, pad = mark & 0xff;
	uint32_t n = (uint32_t)(b->len - at - pad);
	int i;

	for (i = 0; i < 4; i++)
		b->p[at - 4 + (size_t)i] = (unsigned char)(n >> (b->big ? 24 - 8 * i :
		    8 * i));
}

void
db_variant_str(DBuf *b, const char *sig, const char *s)
{
	db_sig(b, sig);
	if (sig[0] == 'g')
		db_sig(b, s);
	else
		db_str(b, s);
}

void
db_variant_u32(DBuf *b, uint32_t v)
{
	db_sig(b, "u");
	db_u32(b, v);
}

void
db_variant_bool(DBuf *b, int v)
{
	db_sig(b, "b");
	db_bool(b, v);
}

/* --------------------------------------------------------------- reader */

void
dr_init(DRd *r, const unsigned char *p, size_t len, int big)
{
	r->p = p;
	r->len = len;
	r->pos = 0;
	r->big = big;
	r->err = 0;
}

void
dr_align(DRd *r, size_t a)
{
	size_t np = (r->pos + a - 1) / a * a;

	if (np > r->len)
		r->err = 1;
	else
		r->pos = np;
}

unsigned
dr_byte(DRd *r)
{
	if (r->err || r->pos >= r->len) {
		r->err = 1;
		return 0;
	}
	return r->p[r->pos++];
}

uint32_t
dr_u32(DRd *r)
{
	uint32_t v = 0;
	int i;

	dr_align(r, 4);
	if (r->err || r->pos + 4 > r->len) {
		r->err = 1;
		return 0;
	}
	for (i = 0; i < 4; i++)
		v |= (uint32_t)r->p[r->pos + (size_t)i] << (r->big ? 24 - 8 * i : 8 * i);
	r->pos += 4;
	return v;
}

uint64_t
dr_u64(DRd *r)
{
	uint64_t v = 0;
	int i;

	dr_align(r, 8);
	if (r->err || r->pos + 8 > r->len) {
		r->err = 1;
		return 0;
	}
	for (i = 0; i < 8; i++)
		v |= (uint64_t)r->p[r->pos + (size_t)i] << (r->big ? 56 - 8 * i : 8 * i);
	r->pos += 8;
	return v;
}

const char *
dr_str(DRd *r)
{
	uint32_t n = dr_u32(r);
	const char *s;

	if (r->err || n > r->len - r->pos || r->pos + n >= r->len ||
	    r->p[r->pos + n] != 0) {
		r->err = 1;
		return "";
	}
	s = (const char *)r->p + r->pos;
	if (memchr(s, 0, n)) {
		r->err = 1;
		return "";
	}
	r->pos += n + 1;
	return s;
}

const char *
dr_sig(DRd *r)
{
	unsigned n = dr_byte(r);
	const char *s;

	if (r->err || r->pos + n >= r->len || r->p[r->pos + n] != 0) {
		r->err = 1;
		return "";
	}
	s = (const char *)r->p + r->pos;
	r->pos += n + 1;
	return s;
}

size_t
sig_align(char c)
{
	switch (c) {
	case 'y': case 'g': case 'v':
		return 1;
	case 'n': case 'q':
		return 2;
	case 'x': case 't': case 'd': case '(': case '{':
		return 8;
	default:
		return 4;
	}
}

/* the end of the single complete type at s, or NULL */
static const char *
type_end(const char *s)
{
	int depth = 0;

	do {
		switch (*s) {
		case 'a':
			s++;
			continue;
		case '(': case '{':
			depth++;
			break;
		case ')': case '}':
			depth--;
			break;
		case '\0':
			return NULL;
		default:
			break;
		}
		s++;
	} while (depth > 0);
	return s;
}

static int skip_depth;

int
dr_skip(DRd *r, const char **sig)
{
	const char *s = *sig;
	int rc = 0;

	if (++skip_depth > 64) {
		skip_depth--;
		return -1;
	}
	switch (*s) {
	case 'y':
		dr_byte(r);
		s++;
		break;
	case 'n': case 'q':
		dr_align(r, 2);
		if (r->pos + 2 > r->len)
			r->err = 1;
		else
			r->pos += 2;
		s++;
		break;
	case 'b': case 'i': case 'u': case 'h':
		dr_u32(r);
		s++;
		break;
	case 'x': case 't': case 'd':
		dr_u64(r);
		s++;
		break;
	case 's': case 'o':
		dr_str(r);
		s++;
		break;
	case 'g':
		dr_sig(r);
		s++;
		break;
	case 'v': {
		const char *vs = dr_sig(r);

		if (!r->err)
			rc = dr_skip(r, &vs);
		s++;
		break;
	}
	case 'a': {
		uint32_t n = dr_u32(r);
		const char *elem = s + 1, *end = type_end(elem);
		size_t start;

		if (!end) {
			rc = -1;
			break;
		}
		dr_align(r, sig_align(*elem));
		start = r->pos;
		if (r->err || n > r->len - r->pos) {
			r->err = 1;
			break;
		}
		while (!r->err && r->pos < start + n) {
			const char *e = elem;

			if (dr_skip(r, &e) < 0) {
				rc = -1;
				break;
			}
		}
		s = end;
		break;
	}
	case '(': case '{': {
		const char *end = type_end(s);

		if (!end) {
			rc = -1;
			break;
		}
		dr_align(r, 8);
		s++;
		while (!r->err && *s != ')' && *s != '}')
			if (dr_skip(r, &s) < 0) {
				rc = -1;
				break;
			}
		s = end;
		break;
	}
	default:
		rc = -1;
	}
	skip_depth--;
	*sig = s;
	return rc < 0 || r->err ? -1 : 0;
}

int
dr_value_text(DRd *r, const char **sig, char *out, size_t n)
{
	char c = **sig;

	out[0] = '\0';
	if (c == 's' || c == 'o' || c == 'g') {
		rui_strlcpy(out, c == 'g' ? dr_sig(r) : dr_str(r), n);
		(*sig)++;
		return r->err ? -1 : 0;
	}
	if (c == 'u' || c == 'i' || c == 'b') {
		uint32_t v = dr_u32(r);

		if (c == 'i')
			snprintf(out, n, "%ld", (long)(int32_t)v);
		else
			snprintf(out, n, "%lu", (unsigned long)v);
		(*sig)++;
		return r->err ? -1 : 0;
	}
	if (c == 'y') {
		snprintf(out, n, "%u", dr_byte(r));
		(*sig)++;
		return r->err ? -1 : 0;
	}
	return dr_skip(r, sig);
}

/* ------------------------------------------------------------- messages */

int
valid_name(const char *s)
{
	size_t n = strlen(s);
	int dots = 0;

	if (!n || n > 255)
		return 0;
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;

		if (c == '.')
			dots++;
		else if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		    (c >= '0' && c <= '9') || c == '_' || c == '-' || c == ':'))
			return 0;
	}
	return dots > 0;
}

int
valid_path(const char *s)
{
	if (s[0] != '/')
		return 0;
	if (!s[1])
		return 1;
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;

		if (c == '/' && (s[1] == '/' || s[1] == '\0'))
			return 0;
		if (!(c == '/' || c == '_' || (c >= 'a' && c <= 'z') ||
		    (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
			return 0;
	}
	return 1;
}

size_t
dmsg_size(const unsigned char *p)
{
	DRd r;
	uint32_t body, fields;
	size_t total;

	if ((p[0] != 'l' && p[0] != 'B') || p[3] != 1)
		return 0;
	dr_init(&r, p, 16, p[0] == 'B');
	r.pos = 4;
	body = dr_u32(&r);
	dr_u32(&r);
	fields = dr_u32(&r);
	if (body > DBUS_MAX_MSG || fields > DBUS_MAX_MSG)
		return 0;
	total = 16 + ((size_t)fields + 7) / 8 * 8 + body;
	return total > DBUS_MAX_MSG ? 0 : total;
}

static char *
dup0(const char *s)
{
	return rui_strdup(s);
}

int
dmsg_parse(DMsg *m, const unsigned char *p, size_t len)
{
	DRd r;
	uint32_t fields_len, body_len;
	size_t fend;

	memset(m, 0, sizeof *m);
	if (len < 16 || (p[0] != 'l' && p[0] != 'B') || p[3] != 1)
		return -1;
	m->big = p[0] == 'B';
	m->type = p[1];
	m->flags = p[2];
	dr_init(&r, p, len, m->big);
	r.pos = 4;
	body_len = dr_u32(&r);
	m->serial = dr_u32(&r);
	fields_len = dr_u32(&r);
	if (r.err || m->type < 1 || m->type > 4 || !m->serial ||
	    16 + (size_t)fields_len > len)
		return -1;
	fend = 16 + fields_len;
	while (!r.err && r.pos < fend) {
		unsigned code;
		const char *vs;

		dr_align(&r, 8);
		if (r.pos >= fend)
			break;
		code = dr_byte(&r);
		vs = dr_sig(&r);
		if (r.err)
			break;
		switch (code) {
		case HF_PATH: case HF_INTERFACE: case HF_MEMBER: case HF_ERROR_NAME:
		case HF_DESTINATION: case HF_SENDER: {
			const char *v;
			char **dst = code == HF_PATH ? &m->path : code == HF_INTERFACE ?
			    &m->iface : code == HF_MEMBER ? &m->member : code ==
			    HF_ERROR_NAME ? &m->error : code == HF_DESTINATION ? &m->dest :
			    &m->sender;

			if (strcmp(vs, code == HF_PATH ? "o" : "s")) {
				r.err = 1;
				break;
			}
			v = dr_str(&r);
			if (!r.err && !*dst)
				*dst = dup0(v);
			break;
		}
		case HF_REPLY_SERIAL:
		case HF_UNIX_FDS:
			if (strcmp(vs, "u")) {
				r.err = 1;
				break;
			}
			if (code == HF_REPLY_SERIAL)
				m->reply_serial = dr_u32(&r);
			else
				m->nfds = dr_u32(&r);
			break;
		case HF_SIGNATURE:
			if (strcmp(vs, "g")) {
				r.err = 1;
				break;
			}
			if (!m->sig)
				m->sig = dup0(dr_sig(&r));
			else
				dr_sig(&r);
			break;
		default:                           /* unknown: skip it */
			dr_skip(&r, &vs);
			break;
		}
	}
	if (r.err) {
		dmsg_free(m);
		return -1;
	}
	r.pos = fend;
	dr_align(&r, 8);
	if (r.err || r.pos + body_len != len) {
		dmsg_free(m);
		return -1;
	}
	m->body_len = body_len;
	m->body = rui_malloc(body_len + 1);
	memcpy(m->body, p + r.pos, body_len);
	if (!m->sig)
		m->sig = dup0("");
	/* required fields */
	if ((m->type == DM_CALL && (!m->path || !m->member)) ||
	    (m->type == DM_SIGNAL && (!m->path || !m->iface || !m->member)) ||
	    (m->type == DM_ERROR && (!m->error || !m->reply_serial)) ||
	    (m->type == DM_RETURN && !m->reply_serial) ||
	    (m->path && !valid_path(m->path))) {
		dmsg_free(m);
		return -1;
	}
	return 0;
}

void
dmsg_free(DMsg *m)
{
	free(m->path);
	free(m->iface);
	free(m->member);
	free(m->error);
	free(m->dest);
	free(m->sender);
	free(m->sig);
	free(m->body);
	memset(m, 0, sizeof *m);
}

static void
field_str(DBuf *b, int code, const char *sig, const char *v)
{
	if (!v)
		return;
	db_pad(b, 8);
	db_byte(b, (unsigned)code);
	db_variant_str(b, sig, v);
}

void
dmsg_write(const DMsg *m, DBuf *b)
{
	size_t mark;

	db_byte(b, m->big ? 'B' : 'l');
	db_byte(b, (unsigned)m->type);
	db_byte(b, (unsigned)m->flags);
	db_byte(b, 1);
	db_u32(b, (uint32_t)m->body_len);
	db_u32(b, m->serial);
	mark = db_array_begin(b, 8);
	field_str(b, HF_PATH, "o", m->path);
	field_str(b, HF_INTERFACE, "s", m->iface);
	field_str(b, HF_MEMBER, "s", m->member);
	field_str(b, HF_ERROR_NAME, "s", m->error);
	if (m->reply_serial) {
		db_pad(b, 8);
		db_byte(b, HF_REPLY_SERIAL);
		db_variant_u32(b, m->reply_serial);
	}
	field_str(b, HF_DESTINATION, "s", m->dest);
	field_str(b, HF_SENDER, "s", m->sender);
	if (m->sig && *m->sig)
		field_str(b, HF_SIGNATURE, "g", m->sig);
	if (m->nfds) {
		db_pad(b, 8);
		db_byte(b, HF_UNIX_FDS);
		db_variant_u32(b, m->nfds);
	}
	db_array_end(b, mark);
	db_pad(b, 8);
	if (m->body_len) {
		if (b->len + m->body_len > b->cap) {
			b->cap = b->len + m->body_len;
			b->p = rui_realloc(b->p, b->cap);
		}
		memcpy(b->p + b->len, m->body, m->body_len);
		b->len += m->body_len;
	}
}
