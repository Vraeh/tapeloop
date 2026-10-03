# Included at the end of the project() call when configuring the OBS sources.
#
# libobs-metal is written in Swift, but OBS 32.0 and 32.1 only enable the
# Swift language inside the mac-virtualcam plugin. The plugins are skipped here
# with ENABLE_PLUGINS=OFF, which leaves libobs-metal without a linker language.
# OBS 32.2 enables Swift globally, and enabling it twice is harmless.
enable_language(Swift)
