# retro-bus

retro-de's message bus, used instead of D-Bus. It is one small process
(`retro-bus`) listening on a Unix socket. It routes lines of text between
programs and never looks inside the arguments.

## Finding the bus

1. `$RETRO_BUS`, if set. `retro-session` always sets it for everything it
   starts.
2. Otherwise `${TMPDIR:-/tmp}/retro-de-UID/bus-DISPLAY`. `DISPLAY=:0.0`
   gives `bus-0`, and `host:10` gives `bus-host_10`. If `TMPDIR` is longer
   than 40 characters, `/tmp` is used instead so the path fits in a
   `sockaddr_un`.

The directory is created with mode 0700, and retro-bus refuses to use it
if it is not yours. The socket is reachable only by your own user, so the
bus checks nothing else. `retro-msg path` prints the address.

## Wire format

There is one message per line, ending in `\n`. A message is a verb, then
words separated by single spaces. The last field is free text and runs to
the end of the line. In that text a backslash, a newline, a tab and a
carriage return are written as `\\`, `\n`, `\t` and `\r`.

**Words** (names, methods, topics, ids) are 1 to 128 characters from
`A-Z a-z 0-9 . _ : @ + -`. Patterns passed to `listen` may also use `*`
and `?`.

**Arguments** are fields separated by tabs (sent escaped as `\t`).
`retro-msg call svc m a "b c"` sends the two fields `a` and `b c`.

### From a client to the bus

| Line | Meaning |
|---|---|
| `call ID SERVICE METHOD [ARGS]` | Call a method. With ID `-`, no answer is sent. |
| `reply ID [TEXT]` | Answer a call the bus forwarded to you. |
| `error ID [TEXT]` | Answer it with an error. |
| `emit TOPIC [ARGS]` | Broadcast an event. |

### From the bus to a client

| Line | Meaning |
|---|---|
| `call ID SENDER SERVICE METHOD [ARGS]` | A call for one of your names. Answer it with `reply ID` or `error ID`. |
| `reply ID [TEXT]`, `error ID [TEXT]` | The answer to your call `ID`. |
| `event SENDER TOPIC [ARGS]` | An event that matches one of your patterns. |

SENDER is the sender's first claimed name, or `:N` (a number that is never
reused) if it has none. A call can be sent to `:N` as well.

If a service disconnects while it still owes answers, each caller gets
`error ID service gone`. A call to a name nobody owns gets
`error ID no service NAME`.

## The `bus` service

| Method | Answer |
|---|---|
| `ping` | `pong` |
| `version` | the retro-de version |
| `name NAME` | claims NAME for this connection (up to 16 names), or `error name taken` |
| `release NAME` | gives it back |
| `listen PATTERN` | start receiving events whose topic matches (globs, not case sensitive) |
| `unlisten PATTERN` | stop |
| `list` | the claimed names, one per line |

The bus emits `bus.join NAME` and `bus.leave NAME` as names come and go.

## A session in plain text

```
$ nc -U "$RETRO_BUS"
call 1 bus listen notify.*
reply 1
call 2 notify show Hello\tFrom netcat
reply 2 7
event notify notify.closed 7\texpired
```

## Services of the desktop

| Service | Program | Methods |
|---|---|---|
| `wm` | retro-wm | each action from `keys.conf` by name, with its argument (`workspace 2`, `snap left`, `dnd on`, `move 10 0`); `windows` (one line per window: id, workspace or -1 for all, class, title, and `*` for the focused one); `focus ID`; `close_window ID`; `actions` |
| `notify` | retro-bar | `show SUMMARY [BODY [SECONDS [ICON [URGENCY [ACTIONS [APP [REPLACES]]]]]]]` returns the notification's number. SECONDS is empty for `notification_time` or 0 to stay until clicked. ICON is a pictogram name (`terminal`, `folder`, `info` ...), an image path or an icon name (PNG, SVG, XPM). URGENCY is `low`, `normal` or `critical`. ACTIONS is `key:Label\|key2:Label2`, where the key `default` means a click on the popup itself. REPLACES is the number of a notification to update in place. Also `close ID`, `history` (one line each: id, time, app, summary, body), `clear`, `count` (total and unread) and `center` (opens the list). |
| `launcher` | retro-bar | `run COMMAND` (also the `@terminal`, `@browser`, `@open` ... forms), `open FILE-OR-URL` (browser for URLs, file manager for folders, editor for other files) |

| Event | Arguments |
|---|---|
| `wm.focus` | window id, title |
| `wm.workspace` | number, from 1 |
| `wm.dnd` | `on` or `off` |
| `notify.clicked` | number |
| `notify.action` | number, action key |
| `notify.closed` | number, then `expired`, `dismissed` or `closed` |

retro-dbus (the optional D-Bus stand-in) uses `notify` and `launcher` for
`org.freedesktop.Notifications` and the OpenURI portal, so D-Bus programs
end up in the same places.

## From C

Programs linked with libretroui use `lib/bus.c`. `rui_wait()` watches the
socket and reconnects every 2 seconds while a claimed name or a pattern is
waiting for the bus.

```c
static void
on_bus(const RuiBusMsg *m, void *ud)
{
	if (!strcmp(m->kind, "call") && !strcmp(m->name, "hello"))
		rui_bus_reply(m->id, 1, "hi");
}

rui_bus_handler(on_bus, NULL);
rui_bus_claim("myapp");                 /* kept across bus restarts */
rui_bus_listen("wm.workspace");
rui_bus_emit("myapp.started", "");
ans = rui_bus_call("notify", "show", "Title\tBody", 2000, &ok);
```

## From the shell

```sh
retro-msg serve clock 'f() { case $1 in now) date ;; *) exit 1 ;; esac; }; f'
retro-msg call clock now
```

`serve` runs the command once per call, with the method and the fields as
`$1`, `$2` and so on, and with `RETRO_MSG_FROM` and `RETRO_MSG_SERVICE` set.
The command's output is the reply. A non-zero exit status turns it into an
error.
