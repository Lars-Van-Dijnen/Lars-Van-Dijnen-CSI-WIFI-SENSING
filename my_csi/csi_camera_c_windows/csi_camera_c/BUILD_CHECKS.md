# Build and validation

Version 1.1.0, validated on 10 November 2026.

- Cross-compiled the complete recorder as C11 for x86-64 Windows using Zig 0.14.1.
- Compilation passed with `-Wall -Wextra -Wpedantic -Werror`.
- Confirmed that `capture.exe` is an x86-64 Windows console executable.
- Inspected imports: Windows Media Foundation, Win32, COM and Windows C runtime
  interfaces; no Python, OpenCV, FFmpeg or third-party runtime DLL dependencies.
- Executed the portable parser and timestamp arithmetic tests using GCC.
- Repeated those checks with AddressSanitizer and UndefinedBehaviorSanitizer.
  Leak detection was disabled because the execution environment uses tracing;
  the parser itself does not allocate heap memory.

Hardware feedback for v1.0.0: the HP HD Camera enumerated and supplied about 30
frames/second, but the program timed out waiting for its worker at shutdown,
leaving an incomplete MP4. The log reported 243 frames at 8.1 seconds in a
ten-second recording; the last periodic update alone does not indicate an early
capture stop.

Version 1.1.0 removes the concurrent source shutdown, copies camera samples into
encoder-owned buffers, selects software encoding, adds periodic CSV flushing,
and logs the finalization stage. The exact blocked Media Foundation call in
v1.0.0 could not be identified from the old log, so this patch also adds phase
diagnostics if the problem recurs.

The revised Windows executable cannot be run in this Linux build environment.
Its webcam/encoder shutdown still requires a retest on the HP HD Camera. Real
combined serial/video capture has not yet been tested on your hardware. The
host receipt timestamps have not been calibrated to physical camera exposure
or radio capture times.
