MiSTer DVD Player v0.2.0 Beta 2

Install:
Extract this archive to the root of the MiSTer SD card.

Then add this to MiSTer.ini (do not replace your existing file):

[DVD-Player]
main=MiSTer_DVD

Do not replace stock /media/fat/MiSTer with MiSTer_DVD.

Core:
DVD Player

ISO folders:

SD:
/media/fat/games/DVD-Player/

USB:
/games/DVD-Player/

Example:
/media/usb0/games/DVD-Player/Movie.iso

MiSTer uses the USB core folder when it exists, otherwise SD.
This package does not create USB folders.

Encrypted DVDs:

libdvdcss is not included.

Users must source:

libdvdcss.so.2

and place it at:

/media/fat/DVD/lib/libdvdcss.so.2

Unencrypted discs and ISO playback do not require it.

Source, documentation, known issues, and compatibility reports:

https://github.com/joedaniels198512-gif/dvd-core

Corresponding source for the binaries in this package is published
as a separate GitHub Release asset:

MiSTer_DVD_Player_0.2.0-beta.2-source.zip

Licence texts for redistributed components are in LICENSES/.
See SOURCE_OFFER.txt.
