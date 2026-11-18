# RPM spec for retro-de: Fedora, RHEL/Alma/Rocky 8+, openSUSE.
#   rpmbuild -bb retro-de.spec   (with the source tarball in SOURCES/)
%global debug_package %{nil}

Name:           retro-de
Version:        1.3
Release:        1
Summary:        Small CDE / Windows 95 style desktop environment for X11
License:        MIT
URL:            https://github.com/DobryZiomek/retro-de
Source0:        retro-de-%{version}.tar.gz

BuildRequires:  gcc make pkgconfig
BuildRequires:  pkgconfig(x11) pkgconfig(xft) pkgconfig(xinerama)
BuildRequires:  pkgconfig(xrandr) pkgconfig(xext)

%description
retro-de is a window manager, front panel, desktop icons, Style Manager,
file manager, program groups, calculator and network / Bluetooth / display
tools, written in plain C on Xlib and Xft.  It needs no D-Bus, no toolkit
and no particular init system.

%package dbus
Summary:        D-Bus session bus stand-in for retro-de
Requires:       %{name} = %{version}-%{release}

%description dbus
retro-dbus speaks the D-Bus protocol so programs that need a session bus
work without dbus installed; it also routes desktop notifications and the
file chooser / open-URI portals to retro-de.

%prep
%setup -q

%build
make %{?_smp_mflags} PREFIX=%{_prefix} LIBDIR=%{_libdir}/retro-de

%install
make install install-dbus PREFIX=%{_prefix} LIBDIR=%{_libdir}/retro-de \
    DESTDIR=%{buildroot}

%files
%license LICENSE
%doc README.md
%{_bindir}/retro
%{_bindir}/retro-*
%exclude %{_bindir}/retro-dbus
%{_datadir}/retro-de
%{_libdir}/retro-de
%{_includedir}/retro-de
%{_datadir}/xsessions/retro.desktop

%files dbus
%license LICENSE
%{_bindir}/retro-dbus
