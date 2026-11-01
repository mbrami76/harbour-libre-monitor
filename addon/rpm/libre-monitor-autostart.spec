# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami
#
# The optional add-on of Libre Monitor (REQ-PKG-03 to REQ-PKG-06): no code, no scriptlets, two user
# systemd units and the symlink that enables the timer with the user session, the way Jolla ships
# osupdate-check.timer. It is distributed outside Harbour, for example on
# OpenRepos, and needs the main package. Built on its own: sfdk build <tree>/addon.
%define lm_userunitdir /usr/lib/systemd/user

Name:       libre-monitor-autostart
Summary:    Starts the Libre Monitor service with the session and after a crash
Version:    0.1
Release:    1
License:    GPL-3.0-or-later
URL:        https://michelebrami.it/LibreMonitor
Packager:   Michele Brami <info@michelebrami.it>
BuildArch:  noarch
Requires:   harbour-libre-monitor
Requires:   systemd
Requires:   dbus

%description
Optional add-on of Libre Monitor. A user timer starts the monitoring service
about 20 seconds after the phone is unlocked for the first time and starts it
again within about a minute if it stops while the phone is awake (up to about
4 minutes while it sleeps). Without the add-on, monitoring starts when the
application is opened. Contains no program code.

%prep

%build

%install
SRC=%{_sourcedir}/..
install -D -m 644 $SRC/libre-monitor-autostart.service %{buildroot}%{lm_userunitdir}/libre-monitor-autostart.service
install -D -m 644 $SRC/libre-monitor-autostart.timer %{buildroot}%{lm_userunitdir}/libre-monitor-autostart.timer
mkdir -p %{buildroot}%{lm_userunitdir}/user-session.target.wants
ln -s ../libre-monitor-autostart.timer %{buildroot}%{lm_userunitdir}/user-session.target.wants/libre-monitor-autostart.timer

%files
%defattr(-,root,root,-)
%{lm_userunitdir}/libre-monitor-autostart.service
%{lm_userunitdir}/libre-monitor-autostart.timer
%{lm_userunitdir}/user-session.target.wants/libre-monitor-autostart.timer
