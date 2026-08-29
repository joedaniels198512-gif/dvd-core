MiSTer DVD Player — Appliance A2.2 (OSD F0 dispatch)

Replace only:

  /media/fat/MiSTer_DVD_Appliance

RBF, supervisor, and player are unchanged from A2.1.

One-file install:

  cp MiSTer_DVD_Appliance /media/fat/MiSTer_DVD_Appliance
  chmod +x /media/fat/MiSTer_DVD_Appliance

Do NOT replace /media/fat/MiSTer_DVD or MiSTer_DVD_Player.rbf.
Do NOT copy into /media/fat/DVD.

Play ISO... now intercepts at MENU_FILE_SELECT2 (the actual confirm)
and still blocks user_io_file_tx in MENU_GENERIC_FILE_SELECTED.

OSD lines are appended to /media/fat/DVD_Appliance/logs/appliance.log
as well as the Main console.

Expected after selecting an ISO:

  APPLIANCE OSD: ISO selected path=/media/usb0/games/DVD-Player-Appliance/...
  APPLIANCE OSD: ISO path sent (no FPGA transfer) path=...
  APPLIANCE: ISO request path=...
