Anki Natural Taskbar Identity POC

This POC intentionally uses no explicit AUMID. It creates a separate clean Start Menu shortcut that points to Anki and verifies that the shortcut has PKEY_AppUserModel_ID=<NONE>.

Test:
1. Manually unpin older Anki/POC pins first.
2. Run as a normal user.
3. Click Prepare clean shortcut; log must say explicit AUMID=<NONE>.
4. On Windows 10 try SysPin/PTTB. On old Windows 11 try IPinnedList3. On current Windows 11 try IPinnedList3 and, if needed, Try both.
5. Click Launch Anki. PASS means the pinned icon becomes active and no second Anki icon appears.
6. Send %TEMP%\AnkiNaturalIdentityPOC.log.

The POC does not modify the existing Anki shortcut and does not call SetCurrentProcessExplicitAppUserModelID.
