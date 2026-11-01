Name:       harbour-libre-monitor

Summary:    Caregiver App to monitor glucose sensors and get alerts
Version:    1.0.0
Release:    1
License:    GPLv3+
URL:        https://michelebrami.it/LibreMonitor
Packager:   Michele Brami <info@michelebrami.it>
Source0:    %{name}-%{version}.tar.bz2
Requires:   sailfishsilica-qt5 >= 0.10.9
BuildRequires:  pkgconfig(sailfishapp) >= 1.0.2
BuildRequires:  pkgconfig(Qt5Core)
BuildRequires:  pkgconfig(Qt5Qml)
BuildRequires:  pkgconfig(Qt5Quick)
BuildRequires:  desktop-file-utils

%description
Libre Monitor lets a caregiver follow the glucose readings of one or more
people through the LibreLinkUp follower service, with a plot of the last
hours and alerts for low and high values. Free software under the GNU
General Public License, version 3 or any later version. A supplementary monitor, not a medical
device: not to be relied on for therapy decisions.


%prep
%setup -q -n %{name}-%{version}

%build

%qmake5 

%make_build


%install
%qmake5_install


desktop-file-install --delete-original         --dir %{buildroot}%{_datadir}/applications                %{buildroot}%{_datadir}/applications/*.desktop

# The version stamp: one line, "<Version>-<Release>", the same value that
# common.pri reads from this spec into LM_APP_VERSION, so the two cannot differ. The running service
# compares it with its own version: a different stamp means a newer package is installed, a missing
# one that the package was removed.
printf '%%s\n' '%{version}-%{release}' > %{buildroot}%{_datadir}/%{name}/version
chmod 644 %{buildroot}%{_datadir}/%{name}/version

%files
%defattr(-,root,root,-)
%{_bindir}/%{name}
%{_datadir}/%{name}
%{_datadir}/applications/%{name}.desktop
%{_datadir}/icons/hicolor/*/apps/%{name}.png
