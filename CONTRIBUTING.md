# Contributing

Thanks for your interest in retro-de.

## Build and test

```sh
./configure --defaults
make
make check        # unit tests + full sessions in Xvfb (or Xwayland)
```

Please run `make check` before opening a pull request. For memory errors:

```sh
make clean
make CFLAGS="-std=c11 -g -O1 -fsanitize=address,undefined"
make check
```

## Code style

- C11 with POSIX.1-2008; no new hard dependencies.
- Must build without warnings under `-std=c11 -Wall -Wextra -pedantic`
  (gcc and clang).
- Tabs for indentation, return type on its own line (dwm style).
- `lib/retroui.h` is the first include of every `.c` file.
- Optional features degrade silently when their tool is missing.

## Pull requests

- Keep changes focused; one feature or fix per PR.
- Add or update tests in `tests/` when behaviour changes.
- Update `README.md` / `docs/` when you add options, keys or modules.
- See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for how the pieces fit,
  including how to add a back-end module or a theme.

## Reporting bugs

Open an issue with your OS and init system, the output of
`retro-about -t`, the `./configure` options used, and your `retro.conf`
if relevant.
