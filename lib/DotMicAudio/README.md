# DotMic scoped USB audio compatibility patch

Source: Espressif Arduino-ESP32 3.3.9, `libraries/USB/src/USBAudioCard*`.
Original Apache-2.0 copyright and license notices are retained in each file.
This local copy does not modify the shared SDK or other projects.

Changes:
- Rename class/files to DotMicAudio, so only this local implementation is linked.
- Add an 8-byte UAC1 IAD to the full-speed microphone descriptor and its length.
  Group the AudioControl and AudioStreaming interfaces into one Windows function.
  The IAD uses Audio function subclass/protocol `0/0`; the AudioControl interface
  itself remains subclass `1` in its own standard interface descriptor.
  The CDC function already has an IAD; Windows does not apply legacy audio grouping
  when the composite configuration contains IADs.
- Name the function S3AI DotMic Microphone.
- The application uses PID `0xD07C` and a MAC-derived serial to make Windows
  create a fresh device instance after replacing the earlier broken descriptor.
- Read the 3-byte sampling frequency safely; reject rates other than the one advertised.

Evidence: Windows reported separate MI_02 and MI_03 MEDIA devices, both Code 10,
while CDC COM4 worked. Device logs showed `streaming=0` with `rxerr=0`.
Reference: https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/support-for-interface-collections

The patched descriptor has not yet been verified on the physical device.
