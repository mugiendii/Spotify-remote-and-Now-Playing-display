"""
PlatformIO pre-build script: make edits to display_config.h actually rebuild.

THE PROBLEM
-----------
platformio.ini force-includes include/display_config.h with `-include`.
PlatformIO's SCons dependency scanner works by parsing `#include` directives
out of source files, so a header injected on the command line is invisible to
it. The result is silent and nasty: change a pin, run `pio run -t upload`,
and the old firmware is flashed because SCons believed nothing had changed.

THE FIX
-------
Hash the header and inject the digest as a preprocessor define. Any edit
changes the digest, which changes every compile command line, which is part
of the SCons build signature - so everything that could depend on the header
is rebuilt. It costs a full rebuild whenever pins change, which is exactly
what is wanted.

The digest is also compiled in, so the firmware can print it at boot and you
can confirm a board is running the configuration you think it is.
"""

import hashlib

Import("env")  # noqa: F821  (injected by SCons)

HEADER = env.subst("$PROJECT_INCLUDE_DIR/display_config.h")  # noqa: F821

try:
    with open(HEADER, "rb") as fh:
        digest = hashlib.sha256(fh.read()).hexdigest()[:8]
except OSError:
    # A missing header is reported far more clearly by the compiler than it
    # would be here; do not mask that error with one of our own.
    digest = "00000000"

env.Append(  # noqa: F821
    CPPDEFINES=[("DISPLAY_CONFIG_FINGERPRINT", "0x" + digest)]
)

print("display_config.h fingerprint: 0x%s" % digest)
