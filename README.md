Minix 3
======

MINIX 3 is a free, open-source, operating system designed to be highly reliable, flexible, and secure.
It is based on a tiny microkernel running in kernel mode with the rest of the operating system running as a number of isolated, protected, processes in user mode.
It runs on x86 and ARM CPUs, is compatible with NetBSD, and runs thousands of NetBSD packages. 

Building
--------

You can cross-build Minix 3 from most UNIX-like operating systems.
To build a cd image for X86, in the src directory:

    releasetools/x86_cdimage.sh

To build a ramdisk for X86, in the src directory:

    releasetools/x86_ramimage.sh

Additional Links
----------------

- [The Official Minix 3 website](https://www.minix3.org/)
- [The Official releases](https://wiki.minix3.org/doku.php?id=www:download:start)
