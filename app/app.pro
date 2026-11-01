# The application name in TARGET must match the QML file, desktop file, icons and
# translation file names (see the notice in the IDE template).
TARGET = harbour-libre-monitor

CONFIG += sailfishapp

# One binary, three modes: the user interface, the
# headless service and the adapter process. The service and adapter code is in the static
# libraries below; this folder only decides the mode (src/main.cpp). QtDBus, QtSql and
# QtNetwork are needed by those libraries (on the Harbour list).
QT += dbus sql network quick

include(../common.pri)
useUiReal()
useDaemon()
useLibre()
useUi()
useCore()

# The device libraries of the phone ports. link_pkgconfig is added by the
# sailfishapp feature, which must see this list before it links; do not add it here.
!hostbuild {
    DEFINES += LM_PHONE_PORTS
    PKGCONFIG += keepalive nemonotifications-qt5 sailfishsecrets
}

SOURCES += src/main.cpp src/plotitem.cpp
HEADERS += src/plotitem.h

DISTFILES += qml/harbour-libre-monitor.qml \
    qml/cover/CoverPage.qml \
    qml/pages/*.qml \
    qml/components/* \
    qml/images/* \
    translations/*.ts \
    harbour-libre-monitor.desktop

# GPL text installed with the application, inside the one directory Harbour allows
license.files = $$_PRO_FILE_PWD_/../LICENSE
license.path = /usr/share/$${TARGET}
INSTALLS += license

# The launcher icon: the author's drawing exported at 86, 108, 128 and 172 px in
# design/export/launcher (design/MANIFEST.md), installed from there as
# /usr/share/icons/hicolor/<size>/apps/<package>.png. It replaces the template's icons of app/icons,
# which are gone, so the repository holds one copy only. The file is named after the package
# ($${TARGET}): the development package script renames the files of its own copy of the tree.
for(size, $$list(86x86 108x108 128x128 172x172)) {
    eval(launcher_$${size}.files = $$_PRO_FILE_PWD_/../design/export/launcher/$${size}/$${TARGET}.png)
    eval(launcher_$${size}.path = /usr/share/icons/hicolor/$${size}/apps)
    INSTALLS += launcher_$${size}
}
# The welcome page shows the application icon: the 172 px drawing, under a fixed name
# in the application's own folder, where the QML finds it whatever the package is called.
welcomeicon.path = /usr/share/$${TARGET}/icons/ui
welcomeicon.extra = install -D -m 644 $$_PRO_FILE_PWD_/../design/export/launcher/172x172/$${TARGET}.png $(INSTALL_ROOT)/usr/share/$${TARGET}/icons/ui/app-icon.png
INSTALLS += welcomeicon

# Icons of the alert notifications. All of them are the author's
# designs in design/export/notify/, a folder that the service only reads (design/MANIFEST.md): they are
# installed from there, never copied into the repository twice. The five alerts, the four "ended"
# entries and the three notices; replacing a design is one file with the name the catalogue gives it
# (core/service/notificationcatalog.cpp). the packaging packages the final set. The service names a file
# here as `app_icon` and never tints it.
DESIGNICONS = alert-ll alert-l alert-h alert-hh alert-nodata ended-ll ended-l ended-h ended-hh notice-1 notice-2 notice-3
for(icon, DESIGNICONS): alerticons.files += $$_PRO_FILE_PWD_/../design/export/notify/$${icon}.png
alerticons.path = /usr/share/$${TARGET}/icons
INSTALLS += alerticons

# The interface's drawings (design/MANIFEST.md): the round band badges (also the unknown
# badge, which stands for a reading that is not current) and the three own pictograms, white
# strokes that the pages tint like Silica's icons. Read from design/export/ui, which the user interface only
# reads, and installed in the application's icons/ui folder where the QML finds them (BandBadge.qml,
# Style.js pictogramSource); never copied into the repository twice.
UIDESIGN = badge-ll badge-l badge-n badge-h badge-hh badge-unknown state-problem state-guard sensor
for(f, UIDESIGN): uidesign.files += $$_PRO_FILE_PWD_/../design/export/ui/$${f}.svg
uidesign.path = /usr/share/$${TARGET}/icons/ui
INSTALLS += uidesign

# Identifier-based texts (REQ-UI-13): the source is English, written
# beside each qsTrId("id") as a //% comment; the English file is generated, Italian is the
# translation. A new language is one more .ts file in this list (procedure in the document).
CONFIG += sailfishapp_i18n sailfishapp_i18n_idbased

# The English file is listed too: it is the language shown when the phone's language has no
# translation, so it must be compiled like the others.
TRANSLATIONS += translations/harbour-libre-monitor.ts \
    translations/harbour-libre-monitor-it.ts

# The texts of the notifications the service posts (core/service/notificationtexts.cpp) are in the same
# translation files. The sailfishapp_i18n feature runs `lupdate -noobsolete` on src/ and qml/ ONLY, and
# rewrites the .ts files in the source tree at every package build: a message that lupdate does not find
# is dropped, and the package would then carry notification texts that are just their identifiers. The
# feature collects TRANSLATION_SOURCES when it is loaded, after this file, so the service's folder is
# added here (found out by a test that compiled the.ts files).
TRANSLATION_SOURCES += $$PWD/../core/service
