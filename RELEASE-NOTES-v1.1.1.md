UA-101 Driver 1.1.1.

For Macs with Apple silicon (M1 and later), macOS 14 Sonoma or later. Tested on macOS 27.2 on an M1 MacBook Pro.

- Remove Everything removes everything it may. Before, macOS's privacy protection stopped it at your Documents folder, and the rest wasn't removed. Now it goes on, and what macOS doesn't allow is listed in the last window with how to remove it yourself.
- Your own files (UA-101 Backups and UA-101 Reports) go to the Trash when you untick "Keep my backup files and reports", never deleted for good. Help Viewer's copy of the manual stays (macOS protects it; it does no harm), and the last window's "Show in Finder and Quit" opens its folder.
- The uninstaller removes its temporary copy of Core Audio's settings when all went well. Core Audio's lists of earlier default devices stay: they hold your other sound devices' choices.
- Every message window fits between the menu bar and the Dock; a long one scrolls, so its buttons can always be reached.
- Recall Memory: Continue comes on by itself when the short wait after a Recall, Save or Reset is over (before, it stayed grey until you picked another memory).
- Each release now shows the installer's SHA-256 checksum, to check your download (see the README's Install section).

Install: double-click `UA-101 Driver.pkg` (your browser may save it as `UA-101.Driver.pkg`), over 1.1 or 1.0, or on a clean Mac. It isn't signed through Apple's paid developer program, so the first time macOS won't open it: click Done, then System Settings → Privacy & Security → Open Anyway (see the README).

SHA-256 of UA-101 Driver.pkg: 2b90ffec9557837d0a0670c1147f6d15e538e1c96e561655670003fa6ba4bafc
