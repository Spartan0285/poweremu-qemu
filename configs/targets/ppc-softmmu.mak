TARGET_ARCH=ppc
TARGET_BIG_ENDIAN=y
TARGET_KVM_HAVE_GUEST_DEBUG=y
# One host thread per vCPU, as ppc64 already has.  mac99 runs up to four
# 32-bit G4s now, and without this they take turns on a single host thread
# -- four CPUs slower than one.  The BAT and TLB fixes this sits on top of
# are the correctness half; see the target/ppc commits before it.
TARGET_SUPPORTS_MTTCG=y
TARGET_XML_FILES= gdb-xml/power-core.xml gdb-xml/power-fpu.xml gdb-xml/power-altivec.xml gdb-xml/power-spe.xml
TARGET_LONG_BITS=32
