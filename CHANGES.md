# Changes from silverf0x/RpcView 14d5e1a

This branch keeps the GPL-3.0 license. It builds with Visual Studio 2026 and Qt 6.8
on current Windows 11.

## Build

- CMake targets Qt 6 Widgets and C++17. The Qt 5 / QtWin extras path is gone.
- `Qt/Qt.h` includes the Qt 6 headers the widgets use.
- `RpcView/WinIcon.h` replaces `QtWin::fromHICON`.
- Widgets that used `QRegExp` now use `QRegularExpression`.

## Windows 11 runtime

- RpcCore4 accepts the Windows 11 `rpcrt4.dll` file version used here, so RpcView loads
  RpcCore4 instead of stopping on an unknown runtime.
- `ntdll.dll` is loaded with `GetModuleHandleA`. The UNICODE build was calling the wide
  API with a narrow string and crashing at startup.

## Procedure names

- The default symbol path is `srv*C:\Symbols*https://msdl.microsoft.com/download/symbols`.
  It is used when Options, Configure Symbols, has not been saved, so that path does not
  need to be configured. `C:\Symbols` is the local cache. The part after the second star
  is the Microsoft symbol server. Configure Symbols shows this path until you save a
  different one.
- Added: the bottom-left bar names the PDB that is downloading and a percent from 0 to 100.
  In the screenshot it reads `Downloading AudioSrv.pdb` at 37%.
- Click an interface after its PDB is on disk and the Procedures pane shows the real function
  names. `RAILaunchProcessWithIdentity` is an example from `appinfo.dll`.
- One dbghelp session stays up for the life of the window. The interface click path does
  not call `SymCleanup`, which was clearing names after a few clicks.
- A module is loaded from its DLL path. Loading only the PDB path could show a name and
  then fail the address lookup.
- Missing PDBs download on a background thread with WinHTTP. A compressed or failed download
  falls back to a hidden `RpcView.exe /symfetch` process.

## Download PDBs menu

- The menu sits between Filter and Help. Download All PDBs asks Yes or No. No is the default.
- Yes queues only PDB files that are not already in the symbol cache.
- The scan runs off the UI thread. Auto-refresh stops while that dialog is open and while
  the scan runs, so the window does not show as not responding.
