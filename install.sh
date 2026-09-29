#!/bin/sh
#
# Builds R Sound Editor and installs it into the Deskbar's Applications menu.
# Run this on the Haiku machine itself -- nothing here needs a cross-compiler.
#
# Usage:
#   ./install.sh              build and install
#   ./install.sh --uninstall  remove it again
#   ./install.sh --build-only build in place, install nothing

set -e

APP="R Sound Editor"
SRC="RSoundEditor.cpp"
RDEF="RSoundEditor.rdef"
LIBS="-lbe -lmedia -ltracker -llocalestub -lroot"

APPS_DIR="$HOME/config/non-packaged/apps"
# Deskbar does not read ~/config/settings/deskbar/menu directly: it follows the
# search list in /boot/system/data/deskbar/menu_entries, and this is the
# user-writable entry in that list. A symlink here is what makes the app show
# up under Deskbar -> Applications.
MENU_DIR="$HOME/config/non-packaged/data/deskbar/menu/Applications"
DESKTOP_DIR="$HOME/Desktop"

cd "$(dirname "$0")"

if [ "$(uname -s)" != "Haiku" ]; then
	echo "This builds a Haiku application; run it on Haiku." >&2
	exit 1
fi

if [ "$1" = "--uninstall" ]; then
	rm -f "$APPS_DIR/$APP" "$MENU_DIR/$APP" "$DESKTOP_DIR/$APP"
	echo "Removed $APP."
	exit 0
fi

echo "Compiling..."
g++ -O2 -o "$APP" "$SRC" $LIBS

# The resource file is not optional. B_SINGLE_LAUNCH has to come from the app's
# resources rather than a check inside main(): the roster registers the team
# under the signature *before* the binary starts, so an app that asks "am I
# already running?" at startup finds itself and quits -- which looks exactly
# like the Deskbar entry doing nothing at all. Attributes are no substitute
# either; mimeset -F clears a signature attached with addattr.
echo "Attaching resources..."
rc -o RSoundEditor.rsrc "$RDEF"
xres -o "$APP" RSoundEditor.rsrc
mimeset -f "$APP"
rm -f RSoundEditor.rsrc

if [ "$1" = "--build-only" ]; then
	echo "Built ./$APP (not installed)."
	exit 0
fi

echo "Installing..."
mkdir -p "$APPS_DIR" "$MENU_DIR" "$DESKTOP_DIR"
# Stop a running copy first: overwriting the binary underneath it leaves the
# Deskbar entry pointing at a file the running team no longer matches.
quit application/x-vnd.RSoundEditor >/dev/null 2>&1 || true
sleep 2

# Never overwrite a copy that is still running. Replacing the file under a
# live process invalidates its code pages, and the result is several threads
# faulting at once for reasons that look nothing like the real cause.
# The bracket keeps this grep from matching its own command line.
if ps | grep -q "[R] Sound Editor"; then
	echo "$APP is still running -- close it and run this again." >&2
	exit 1
fi

cp -f "$APP" "$APPS_DIR/$APP"

# Tracker reads the icon from the file's attributes, not from the resources the
# linker wrote into the binary. mimeset is meant to copy one to the other, but
# recent Haiku no longer sniffs ELF files, so a freshly installed app shows a
# blank document icon until resattr does the copy.
resattr -O -o "$APPS_DIR/$APP" "$APPS_DIR/$APP" 2>/dev/null \
	|| echo "install.sh: resattr failed; the icon may show as a blank document" >&2
ln -sf "$APPS_DIR/$APP" "$MENU_DIR/$APP"
ln -sf "$APPS_DIR/$APP" "$DESKTOP_DIR/$APP"
rm -f "$APP"

echo "Installed to $APPS_DIR/$APP"
echo "Find it in Deskbar -> Applications -> $APP, or on the Desktop."
