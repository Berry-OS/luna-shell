%global build_date %(TZ=Asia/Tokyo date +%Y%m%d)

Name:           luna-shell
Version:        %{build_date}
Release:        b1
Summary:        Lightweight Wayland compositor and Luna desktop shell

License:        MPL-2.0
URL:            https://github.com/Berry-OS/luna-shell
# Produced by `make dist` / `make rpm` (includes vendored Cargo crates).
Source0:        %{name}-%{version}.tar

BuildRequires:  cargo
BuildRequires:  rust
BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  desktop-file-utils
BuildRequires:  systemd-rpm-macros
BuildRequires:  pkgconfig(dbus-1)
BuildRequires:  pkgconfig(egl)
BuildRequires:  pkgconfig(gbm)
BuildRequires:  pkgconfig(gl)
BuildRequires:  pkgconfig(glesv2)
BuildRequires:  pkgconfig(libdrm)
BuildRequires:  pkgconfig(libinput)
BuildRequires:  pkgconfig(udev)
BuildRequires:  pkgconfig(wayland-client)
BuildRequires:  pkgconfig(wayland-egl)
BuildRequires:  pkgconfig(x11)
BuildRequires:  pkgconfig(xkbcommon)
BuildRequires:  pkgconfig(openssl)
Requires:       bash
%{?systemd_requires}

%description
Luna Desktop is a lightweight desktop session consisting of a Rust Wayland
compositor, a KMS/Wayland shell, a clipboard manager, and a tray notification
service.  This package uses the distribution-provided Wayland client library.


%prep
%autosetup -p1
# Distro rust/cargo (BuildRequires) must be used; rustup toolchains cannot
# be downloaded during an offline rpmbuild.
rm -f rust-toolchain.toml rust-toolchain
test -d vendor
test -f .cargo/config.toml


%build
# Crates are vendored into Source0 by `make dist`.  Stay offline in rpmbuild.
export CARGO_NET_OFFLINE=true
export CARGO_PROFILE_RELEASE_OPT_LEVEL=s
export CFLAGS="%{optflags} -Os"
export CXXFLAGS="%{optflags} -Os"
# The aggregate Makefile target creates target/release symlinks alongside the
# Cargo build, so run it serially to avoid racing directory creation.
# Berry %{optflags} is `-Os -ffast-math`.  Do not pass them to luna-shell;
# the Makefile still appends -O2 -fno-fast-math as a backstop.
%{__make} V=1 build-desktop-system PROFILE=release \
    SHELL_CFLAGS="-pthread \
        $(pkg-config --cflags libdrm xkbcommon wayland-client dbus-1 openssl)"


%install
install -Dpm0755 luna-session %{buildroot}%{_bindir}/luna-session
install -Dpm0755 target/release/luna-compositor \
    %{buildroot}%{_bindir}/luna-compositor
install -Dpm0755 luna-shell %{buildroot}%{_bindir}/luna-shell
install -Dpm0755 luna-clipboard %{buildroot}%{_bindir}/luna-clipboard
install -Dpm0755 luna-tray-notify %{buildroot}%{_bindir}/luna-tray-notify

install -Dpm0644 tray/luna-tray-notify.desktop \
    %{buildroot}%{_datadir}/applications/luna-tray-notify.desktop

install -d %{buildroot}%{_datadir}/luna-desktop/shell
install -pm0644 skins/default/layout.html \
    %{buildroot}%{_datadir}/luna-desktop/shell/luna-shell.html
install -pm0644 skins/default/style.css \
    %{buildroot}%{_datadir}/luna-desktop/shell/luna-shell.css
install -pm0644 ui/luna-ui/luna-ui.h ui/luna-ui/cssparser.h \
    %{buildroot}%{_datadir}/luna-desktop/shell/

cp -a cursors %{buildroot}%{_datadir}/luna-desktop/
cp -a skins %{buildroot}%{_datadir}/luna-desktop/

# Make the symbol fonts available to fontconfig without keeping a duplicate
# copy under the skin directory.
install -d %{buildroot}%{_datadir}/fonts/luna
install -pm0644 skins/fonts/LunaSymbols-*.otf \
    %{buildroot}%{_datadir}/fonts/luna/
install -d %{buildroot}%{_datadir}/fonts/luna/web
install -pm0644 skins/fonts/web/Inter-Regular.ttf skins/fonts/web/Manrope-Regular.ttf \
    %{buildroot}%{_datadir}/fonts/luna/web/
rm -rf %{buildroot}%{_datadir}/luna-desktop/skins/fonts
ln -s ../../fonts/luna \
    %{buildroot}%{_datadir}/luna-desktop/skins/fonts
ln -s ../fonts/luna \
    %{buildroot}%{_datadir}/luna-desktop/fonts

# Optional kiosk unit is kept in the sources; Berry uses berry-dm.
#install -Dpm0644 luna-desktop.service \
#    %{buildroot}%{_unitdir}/luna-desktop.service

install -Dpm0644 udev/70-luna-shell.rules \
    %{buildroot}%{_udevrulesdir}/70-luna-shell.rules

desktop-file-validate \
    %{buildroot}%{_datadir}/applications/luna-tray-notify.desktop


%post
# luna-compositor opens DRM/evdev directly.  berry-dm runs the session as
# the logged-in user, so that user must be in video/input.
if command -v usermod >/dev/null 2>&1; then
    for u in berry; do
        getent passwd "$u" >/dev/null 2>&1 || continue
        for g in video input render; do
            getent group "$g" >/dev/null 2>&1 || continue
            usermod -aG "$g" "$u" >/dev/null 2>&1 || :
        done
    done
fi
if [ -d /run/udev ]; then
    udevadm control --reload-rules >/dev/null 2>&1 || :
    udevadm trigger --subsystem-match=input --subsystem-match=drm >/dev/null 2>&1 || :
fi
# Berry's display manager launches luna-session.  Do not leave a kiosk unit
# enabled that would grab tty1 first.
if [ -f /etc/berry-dm.conf ]; then
    if [ -d /run/systemd/system ]; then
        systemctl disable --now luna-desktop.service >/dev/null 2>&1 || :
    fi
    rm -f %{_sysconfdir}/systemd/system/multi-user.target.wants/luna-desktop.service
fi

#%preun
#%systemd_preun luna-desktop.service

#%postun
#%systemd_postun luna-desktop.service


%files
#%license LICENSE
#%doc README.md README.ja.md
%{_bindir}/luna-session
%{_bindir}/luna-compositor
%{_bindir}/luna-shell
%{_bindir}/luna-clipboard
%{_bindir}/luna-tray-notify
%{_datadir}/applications/luna-tray-notify.desktop
%{_datadir}/fonts/luna/
%{_datadir}/luna-desktop/
%{_udevrulesdir}/70-luna-shell.rules
#%{_unitdir}/luna-desktop.service


%changelog
* Fri Aug 14 2026 Yuichiro Nakada <berry@berry-lab.net> - 20260814-b1
- Create for Berry Linux
