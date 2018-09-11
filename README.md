# PugOS for DOOM

This is a modified version of Harvard's Operating Systems class teaching OS done by Lucas Cassels and Max Levenson as our final project for the course. See our [DOOM repo](https://github.com/CS161/doom) for the DOOM port that runs on this OS!

Major OS additions:
- Implemented numerous Linux syscalls needed by DOOM, such as alloca, various string manipulation functions, free, lseek, etc.
- Added a compile-time switch (-GFX) for MMIO graphics interfacing via VGA mode 13h for a 320x200x8 screen
- Added support for programming the graphics card's color palette via the I/O port
- Created a system for de-activating debug messages by file and function (de-activate debug messages are compiled out of the executable) using macros and const functions
- The stack will automatically grow when it runs out of allocated memory
- Implemented real-time keyboard input for DOOM controls


## Dependencies

- QEMU
- GCC


## Running DOOM

Make sure you have fetched the DOOM submodule source, then run:
```
make GFX=1 run-doom
```
This should build both the kernel and DOOM, link them, and run the game!

