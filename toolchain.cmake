# the name of the target operating system
set(CMAKE_SYSTEM_NAME Generic)

# which compilers to use for C and C++
set(CMAKE_C_COMPILER           /home/ludovic/Programmation/minix/master/obj.i386/tooldir.Linux-6.1.0-13-amd64-x86_64/bin/i586-elf32-minix-clang)
set(CMAKE_CXX_COMPILER         /home/ludovic/Programmation/minix/master/obj.i386/tooldir.Linux-6.1.0-13-amd64-x86_64/bin/i586-elf32-minix-clang++)

set(CMAKE_SYSROOT  /home/ludovic/Programmation/minix/master/obj.i386/destdir.i386)
# where is the target environment located
set(CMAKE_FIND_ROOT_PATH  
        ${CMAKE_BINARY_DIR}/include
    )

# adjust the default behavior of the FIND_XXX() commands:
# search programs in the host environment
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)

# search headers and libraries in the target environment
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)