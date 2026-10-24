# Update the cubevox

You update the box yourself with a USB-C cable and one file. Nothing needs installing on the Mac. A slash in a screen text below separates the lines of the screen.

## What you need
1. The update file from the release page. It ends in `.uf2` and is named like `cubevox-v2.00.10.uf2`. The numbers change with each release.
2. A USB-C cable from the box to your Mac.
3. The box, switched on as usual.

## Steps
1. Download the update file from the release page. It is one file.
2. Plug the USB-C cable into the Mac. On the box, turn MENU to `Update firmware` and click. The screen says `Update the firmware?` / `You have v2.00.10.` / `Click to start.` / `Turn to cancel.` Check the version, then click again. Turn MENU instead to cancel.
3. The screen says `Drop the firmware on` / `the CUBEVOX drive.` / `You have v2.00.10.` / `Wait 60 s to cancel.` A drive named `CUBEVOX` appears on the desktop. Drag the update file onto it. Once the Mac finds the box, the last line changes to `Computer connected.` With no Mac connected, the box goes back to normal after 60 seconds.
4. The screen says `Writing the firmware.` / `Do not unplug.` Then it says `The update is done.` / `Restarting now.` The drive goes away by itself and the box restarts. The start screen shows the new version for 2 seconds. Your layout and settings are as before.
5. If the copy stops partway, unplug the box, plug it back in, and drag the file again. The box comes back in update mode on its own and says why.

## Other screens
The `CUBEVOX` drive is there whenever the screen says `Drop the firmware on` / `the CUBEVOX drive.`
1. `The update stopped.` / `Drop the firmware on` / `the CUBEVOX drive.` The last copy did not finish. Drag the file again.
2. `Startup failed.` / `Drop the firmware on` / `the CUBEVOX drive.` The box failed to start three times in a row, so it came back here by itself. Drag the file again.
3. `That is not a cubevox` / `firmware file.` / `Drop the firmware on` / `the CUBEVOX drive.` You dragged a file that is not a cubevox update. Drag the right one.
4. If the menu does not respond, hold MENU while you switch the power on. The box goes to the screen that says `Drop the firmware on`.

A bad file or a cable pulled halfway cannot lock you out. Update mode is always there for another try.

## The Mac's notice
After the box restarts, the Mac may say `Disk Not Ejected Properly`. That is expected. The box restarts itself at the end of the copy, so the drive disappears without an eject. Nothing is wrong. Finder may also leave hidden files on the drive. The box ignores them.

## Going back to an older version
Download the older file from the release page and drag it onto the drive the same way. Every release stays on the page. The box keeps one version at a time.

## Support
If something looks wrong, open the `CUBEVOX` drive, copy `DIAG.TXT` to your desktop, and send it to us.
