/* retro-dbus internals: the D-Bus wire format, the bus, built-in services */
#ifndef RETRO_DBUS_H
#define RETRO_DBUS_H

#include "../lib/retroui.h"

#include <stdint.h>
#include <poll.h>

enum { DM_CALL = 1, DM_RETURN, DM_ERROR, DM_SIGNAL };
enum { DF_NO_REPLY = 1, DF_NO_AUTO_START = 2 };
enum { HF_PATH = 1, HF_INTERFACE, HF_MEMBER, HF_ERROR_NAME, HF_REPLY_SERIAL,
       HF_DESTINATION, HF_SENDER, HF_SIGNATURE, HF_UNIX_FDS };

#define DBUS_MAX_MSG (64u << 20)

/* ------------------------------------------------------------- buffers */

typedef struct DBuf {
	unsigned char *p;
	size_t len, cap;
	int big;                           /* big-endian output             */
} DBuf;

void   db_init(DBuf *b, int big);
void   db_free(DBuf *b);
void   db_pad(DBuf *b, size_t align);
void   db_byte(DBuf *b, unsigned v);
void   db_u32(DBuf *b, uint32_t v);
void   db_u64(DBuf *b, uint64_t v);
void   db_i32(DBuf *b, int32_t v);
void   db_bool(DBuf *b, int v);
void   db_str(DBuf *b, const char *s);             /* s or o            */
void   db_sig(DBuf *b, const char *s);             /* g                 */
size_t db_array_begin(DBuf *b, size_t elem_align); /* returns the mark  */
void   db_array_end(DBuf *b, size_t mark);
void   db_variant_str(DBuf *b, const char *sig, const char *s);
void   db_variant_u32(DBuf *b, uint32_t v);
void   db_variant_bool(DBuf *b, int v);

/* -------------------------------------------------------------- reader */

typedef struct DRd {
	const unsigned char *p;
	size_t len, pos;
	int big, err;
} DRd;

void        dr_init(DRd *r, const unsigned char *p, size_t len, int big);
void        dr_align(DRd *r, size_t a);
unsigned    dr_byte(DRd *r);
uint32_t    dr_u32(DRd *r);
uint64_t    dr_u64(DRd *r);
const char *dr_str(DRd *r);                        /* s or o; in place  */
const char *dr_sig(DRd *r);                        /* g; in place      */
/* skips one complete type at *sig, advancing it; 0 ok */
int         dr_skip(DRd *r, const char **sig);
/* reads one complete type into a printable form (for strings and numbers;
 * "" for others), advancing sig */
int         dr_value_text(DRd *r, const char **sig, char *out, size_t n);
size_t      sig_align(char c);

/* ------------------------------------------------------------- messages */

typedef struct DMsg {
	int big, type, flags;
	uint32_t serial, reply_serial, nfds;
	char *path, *iface, *member, *error, *dest, *sender, *sig;
	unsigned char *body;               /* malloc'd                      */
	size_t body_len;
} DMsg;

/* parses one complete message (len bytes); 0 ok */
int    dmsg_parse(DMsg *m, const unsigned char *p, size_t len);
/* bytes needed for the message starting at p (>= 16 available), 0 if it
 * cannot be valid */
size_t dmsg_size(const unsigned char *p);
void   dmsg_free(DMsg *m);
/* serialises (in m->big byte order) into out (initialised by the caller) */
void   dmsg_write(const DMsg *m, DBuf *out);
int    valid_name(const char *s);          /* bus name, unique or well-known */
int    valid_path(const char *s);

/* ------------------------------------------------- the bus (retro-dbus.c) */

typedef struct Conn Conn;
typedef void (*BuiltinFn)(Conn *self, DMsg *m);

/* a connection living inside the daemon (no socket) */
Conn       *bus_builtin(BuiltinFn fn);
int         bus_own(Conn *c, const char *name);        /* 0 ok */
const char *bus_unique(const Conn *c);
/* sends a message from c (serial and sender are filled in) */
void        bus_send(Conn *c, DMsg *m);
void        bus_reply(Conn *c, const DMsg *call, const char *sig,
                      const DBuf *body);
void        bus_error(Conn *c, const DMsg *call, const char *name,
                      const char *text);
void        bus_signal(Conn *c, const char *dest, const char *path,
                       const char *iface, const char *member, const char *sig,
                       const DBuf *body);
extern const char *bus_address;               /* unix:path=...            */

/* --------------------------------------- built-in services (services.c) */

void services_init(void);
/* poll() integration: adds descriptors, returns how many; then handled */
int  services_fds(struct pollfd *p, int max, int *timeout_ms);
void services_handle(struct pollfd *p, int n);
void services_child(pid_t pid, int status);    /* a child process ended   */

#endif
