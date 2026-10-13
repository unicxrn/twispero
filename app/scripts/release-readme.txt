@NAME@ @VERSION@

An app for watching Twitch on a jailbroken PS5: live streams with chat,
past broadcasts and clips, played on the console's own video decoder.


WHAT YOU NEED

- A jailbroken PS5 running etaHEN with kstuff.
- ShadowMount+, which turns a folder in /data/homebrew into a tile on the
  home screen.
- A way to copy files to the console, such as an FTP client and etaHEN's FTP
  server (port 1337 by default).

It was tested on firmware 13.60. Other versions may work, but nobody has
tried them yet.


INSTALL

1. Unzip this file. You get a folder called @TITLE_ID@.
2. Copy that folder into /data/homebrew/ on the PS5.
3. Give ShadowMount+ a few seconds to find it. A tile called "@NAME@"
   appears next to your games.
4. Start it like any game.

To update, close the app first, then replace the folder with the new one.
Your login and settings are kept in /data/ps5twitch, so they stay.

To remove it, close the app and delete /data/homebrew/@TITLE_ID@. Delete
/data/ps5twitch as well if you want your login and settings gone too.


CLOSING THE APP

Open the side menu (Left), pick Quit and wait for "Safe to close". Then close
the app from the PS button menu. Closing it with the PS button while a stream
plays can crash the console.


YOUR LOGIN

Your Twitch login is saved in /data/ps5twitch/auth.json. The jailbreak's FTP
server has no password, so anyone who can reach the console on your network
can read that file. Don't leave the console open to a network you don't trust.


LICENCE

This app is free software under the GNU General Public License, version 3 or
later. LICENSE has the full text. NOTICE lists the projects it is built on,
and assets/ holds their licences.

Source code: @SOURCE@

This app isn't affiliated with Twitch or Sony.
