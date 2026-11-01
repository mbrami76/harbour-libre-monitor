# Installing, upgrading and removing

This page is for people who install the package from a file. It uses the phone's shell (SSH or the terminal application) and `devel-su`. Never paste glucose values, patient names or account details into a bug report; the commands below print sizes, counts, states and times only.

## Shorthand

The commands below use these variables in the phone's shell. The service answers on the session bus under the application's name.

```bash
BUS=it.michelebrami.libre-monitor; OBJ=/it/michelebrami/libre_monitor; IF=it.michelebrami.libre_monitor.Service
call() { m=$1; shift; dbus-send --session --print-reply=literal --dest=$BUS $OBJ $IF.$m "$@"; }
DATA=~/.local/share/it.michelebrami/libre-monitor; CONF=~/.config/it.michelebrami/libre-monitor
```

## Install

1. Copy the package to the phone, for example with `scp harbour-libre-monitor-1.0.0-1.aarch64.rpm <user>@<phone>:`.
2. On the phone: `devel-su rpm -U harbour-libre-monitor-1.0.0-1.aarch64.rpm`. No output, or only a line about the desktop database, is normal.
3. Check with `rpm -q harbour-libre-monitor`.
4. Start the application from its icon. At the first start Sailfish asks for the permissions (Internet, Secrets, Documents); allow all three. Then follow "First start" in the README.

The package installs only the application binary, its desktop file, its icons and its data folder under `/usr/share`. It runs no scripts at install time. The monitoring service is the same binary, started by the system when the application or a notification asks for it.

## Upgrade

A new version can convert the stored data to a newer format at its first start. An older version cannot open the converted data. The only way back is the copy you take now, so make it before installing.

**Coming from a 0.1-x package.** Version 1.0.0 is the first public release; the 0.1-x packages were test packages. Install 1.0.0-1 over it with `rpm -U`. The upgrade from the 0.1-x test packages has been tried on an emulator, not on every phone, and the conversion of the stored data at the first start of 1.0.0 is one-way: the 0.1-x package cannot open the converted data. Keep a copy of the data folder (step 2) before you install, and keep the 0.1-x package file if you may want to go back.

1. Close the application (swipe its cover away) and stop the service, so that the databases are complete on disk: `call Stop`. After five seconds, `pgrep -f harbour-libre-monitor` should print nothing. If a process stays, wait ten seconds more; if it is still there, `kill <pid>` (a TERM is an orderly stop).
2. Copy the data and the configuration, and list the installed packages, then print sizes only:

   ```bash
   B=~/lm-backup-$(date +%Y%m%d-%H%M); mkdir -p "$B"
   cp -a "$DATA" "$B/data"; cp -a "$CONF" "$B/config"; rpm -qa | sort > "$B/packages.txt"
   du -sh "$B/data" "$B/config"; ls -l "$B/data" "$B/config" | awk '{print $5, $9}'
   ```

   You should see `store.db`, `journal.db` and `settings.json` in `data`, and `config.json` in `config`. If `store.db` or `config.json` is missing, the folder names differ on this phone: look in `~/.local/share` and `~/.config` and do not go on without a backup.
3. Keep the file of the version that is installed now: `rpm -q harbour-libre-monitor` tells you which one it is. You need it for the way back.
4. Install the new file: `devel-su rpm -U harbour-libre-monitor-1.0.0-1.aarch64.rpm` (or the file of a later version). A message that the package is already installed means the version is already there.
5. Start the application. You should see the overview with your patients, not the first-run page and not the account set-up. The About page shows the new version. Your account login is kept by the system outside these folders and is not touched by an upgrade.
6. Within about two minutes, `call Status | tr ',' '\n' | grep -E '"state"|"cause"|"stale"|last_success_at'` should show the service running and each patient `idle` or `polling`, not stale, with the last success a minute old.

If the first-run page or the account page appears, do not set up the account again. Close the application and go back as described next.

## The way back

1. `call Stop`, close the application and check that no process is left, as in step 1 of the upgrade.
2. Install the earlier package file: `devel-su rpm -U --oldpackage <the earlier file>`.
3. Start the application and check the state as in step 6 above. If it works, keep the data: it holds the readings collected since the upgrade. A version that predates a format change cannot open the converted data. In that case, and whenever the service does not start or the state shows `config-invalid`, stop it again and restore the copy:

   ```bash
   rm -rf "$DATA" "$CONF" && cp -a "$B/data" "$DATA" && cp -a "$B/config" "$CONF"
   ```

   Readings collected since the copy are then lost from the phone; the source still holds its own recent history.

## Remove

1. Remove the account in the application's settings first, so that no login stays in the secret storage. (From a terminal, the application's `--provision --remove` mode does the same; see its `--help`.)
2. `call Stop`, then `devel-su rpm -e harbour-libre-monitor`. `rpm -q harbour-libre-monitor` should say that the package is not installed.
3. The data folders stay in the home folder, because the package never deletes data. Delete them (`rm -rf "$DATA" "$CONF"`) only after making a copy that you want to keep. Old backups under `~/lm-backup-*` also hold readings; delete them when you no longer need them.

## The optional add-on

A second package, `libre-monitor-autostart`, contains no code. It installs a user timer that asks the system to start the monitoring service about twenty seconds after the session starts, and again within about a minute (up to about four minutes while the phone sleeps) if the service has died. Without it, monitoring starts when you open the application or tap the monitoring notice. The add-on is not part of the Harbour package; it needs the main package.
