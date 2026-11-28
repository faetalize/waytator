#build-rpm.sh defines waytator_version, from meson.build, and waytator_commit_time, the time of the commit
#being built in seconds since 1970, at the top of its copy of this file

Name:           waytator
Version:        %{waytator_version}
Release:        1%{?dist}
Summary:        Fast screen capture and editing

License:        GPL-3.0-or-later
URL:            https://github.com/faetalize/waytator
Source0:        %{url}/archive/v%{version}/%{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  meson
BuildRequires:  gettext
BuildRequires:  desktop-file-utils
BuildRequires:  pkgconfig(gobject-2.0)
BuildRequires:  pkgconfig(gio-unix-2.0)
BuildRequires:  pkgconfig(gtk4) >= 4.18
BuildRequires:  pkgconfig(libadwaita-1) >= 1.6
BuildRequires:  pkgconfig(gtk4-layer-shell-0)
BuildRequires:  pkgconfig(wayland-client) >= 1.20
BuildRequires:  pkgconfig(wayland-protocols) >= 1.14
BuildRequires:  pkgconfig(wayland-scanner) >= 1.15
#from Fedora's own ffmpeg-free, so the package installs on a plain Fedora and with RPM Fusion's ffmpeg alike
BuildRequires:  pkgconfig(libavformat) >= 60
BuildRequires:  pkgconfig(libavcodec) >= 60
BuildRequires:  pkgconfig(libavfilter) >= 9
BuildRequires:  pkgconfig(libavutil) >= 58
BuildRequires:  pkgconfig(libswscale) >= 7
BuildRequires:  pkgconfig(libswresample) >= 4.10
BuildRequires:  pkgconfig(libdrm)
BuildRequires:  pkgconfig(libva)
BuildRequires:  pkgconfig(libpipewire-0.3) >= 0.3.50
BuildRequires:  pkgconfig(rubberband) >= 3.0
BuildRequires:  pkgconfig(gbm)
BuildRequires:  pkgconfig(egl)
#the tests that use a session bus start their own
BuildRequires:  dbus-daemon

#copying goes through wl-copy, OCR through tesseract
Requires:       wl-clipboard
Requires:       hicolor-icon-theme
Recommends:     tesseract

%description
Screenshot and record the screen, annotate images and edit videos, all in one
GTK app for Wayland desktops.

%prep
%autosetup

%build
#the source comes from a tarball, without a .git folder to read the commit time from
%meson -Dcommit_time=%{waytator_commit_time}
%meson_build

%install
%meson_install
%find_lang %{name}

%check
desktop-file-validate %{buildroot}%{_datadir}/applications/dev.faetalize.waytator.desktop
%meson_test

%files -f %{name}.lang
%license LICENSE
%doc README.md
%{_bindir}/waytator
%{_datadir}/applications/dev.faetalize.waytator.desktop
%{_datadir}/icons/hicolor/*/apps/dev.faetalize.waytator.png

%changelog
* %(date -u -d @%{waytator_commit_time} '+%%a %%b %%d %%Y') faetalize <oss@faetalize.dev> - %{waytator_version}-1
- Release %{waytator_version}
