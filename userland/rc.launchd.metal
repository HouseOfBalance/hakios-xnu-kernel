# etc/rc for METAL=1: put our Metal plug-in's name on IOAcceleratorES (needs boot-arg metal=1), then
# hand process 1 to the system's launchd. The path is relative to /System/Library/Extensions, so the bundle
# stays in /hakios/lib and the system image is not touched.
/hakios/bin/tolog /hakios/bin/metal_props on ../../../hakios/lib/HakiosMetal HakiosMetalDevice
exec /sbin/launchd.system
