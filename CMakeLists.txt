cmake_minimum_required(VERSION 3.20)

project(Minix)


add_compile_options(-std=gnu99 -v)
set(CMAKE_VERBOSE_MAKEFILE on )


# set sysroot
SET(MACHINE_ARCH i386)

#copy include 

file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/include
     DESTINATION ${CMAKE_BINARY_DIR}
     PATTERN "Makefile" EXCLUDE
     )

message(${MACHINE_ARCH})

add_subdirectory(commands)
#add_subdirectory(servers)
#add_subdirectory(kernel)

