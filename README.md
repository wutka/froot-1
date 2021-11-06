# Froot-1 - A Linux terminal Apple-1 Emulator
This is an emulator for the Apple-1 computer. It was written for Linux,
but should also run on OSX, I have tested it on Mac OS Big Sur.

The emulator runs from the command-line and generally works
like the Apple-1, which had a simple text-based interface.
Since the Apple-1 didn't support lowercase letters, the emulator
automatically converts lowercase keystrokes to upper-case.

## Special Keys
There are a few special keys recognized by the emulator:\
Control-R  Reset button\
Control-C  Exit the emulator\
Control-L  Load a text file as input to the Apple-1\

The Control-L option is useful if you have a Basic program as a
text file and you want to load it. Copy&paste doesn't work very well
with the single-character input.

## Command-line Options
The original Apple-1 came with 4K of RAM and that is the default
for Froot-1. If you want more memory, you can use `-mem nnk`,
where *nn* is the number of kilobytes of memory up to 64 to make
the full memory range available (minus what is used by the monitor at
FF00 and, the
cassette interface, unless disabled, at C100-C300, and the D0xx
addresses for communicating with the PIA chip). You can also specify
`full` for the memory size to get 64k.

To disable the cassette interface, use `-cassette n`. 

To load a ROM file, use `-rom file` or `-rom file1,file2,...,filen`.
You can also load a file in ROM format into RAM instead of ROM with
`-ram file` or `-ram file1,file2,...,filen`.

To drop immediately into the debugger, use `-d`.

You can simulate a baud rate with `-baud nnn`. A baud rate of 0
means that there is no baud rate limitation, which is the default.
## Woz Monitor

The original Woz monitor program is loaded starting at location FF00
and runs automatically when the system boots, or at reset. 
The monitor program used '?' as a delete character,
but I have patched the ROM to use backspace as the delete character
(just a 1 byte change from DF to 88 at location FF10). That is the
only change from the original.

The monitor is described in the
[Apple-1 Operation
Manual](https://archive.org/download/Apple-1_Operation_Manual_1976_Apple_a/Apple-1_Operation_Manual_1976_Apple_a.pdf)

## Woz Basic
Although it is not loaded by default, you can load Woz Basic,
the predecessor to the Apple \]\[ Integer Basic, from wozbasic.rom
by adding it to the command-line:
```shell
froot1 -rom wozbasic.rom
```

If Woz basic is loaded, you can type "E000R" from the monitor to
run it.

The features of Woz basic are described in
[Preliminary Apple Basic Users
Manual](https://archive.org/download/Preliminary_Apple_Basic_Users_Manual_1976-10_Apple/Preliminary_Apple_Basic_Users_Manual_1976-10_Apple.pdf)

Here is an example session with Woz Basic in which I use control-R
to reset and jump back to the monitor, then run the cassette interface
and save the basic program as described in the manual. I then quit
out of the program and start it again, using the cassette interface
to load my program back, and then I go back into basic. When you
load a basic program this way, make sure you start basic with E2B3R
instead of the usual E000R so it will keep the program that was
loaded.
```
$ froot1 -rom wozbasic.rom
\\
E000R

E000: 4C
>10 PRINT "HELLO FROM FROOT-1"
>20 END
>RESET
\\
C100R

C100: A9\*
4A.00FFW800.FFFW
Cassette save to file (enter=cancel): hello.bas
Cassette finished.
\\

$ froot1 -rom wozbasic.rom
\\
C100R

C100: A9\*
4A.00FFR800.FFFR
Cassette file to read (enter=cancel): hello.bas
Cassette finished.
\\
E2B3R

E2B3: 20
>LIST
   10 PRINT "HELLO FROM FROOT-1"
      
   20 END 

>
```

## Cassette Interface
Unless you disable the cassette interface with `-cassette n`, the
emulator will load the original Apple-1 cassette interface, which
is documented in the
[Apple-1 Cassette Interface
manual](https://archive.org/download/Apple-1_Cassette_Interface_1977_Apple/Apple-1_Cassette_Interface_1977_Apple.pdf)

The Apple-1 cassette interface is very timing-dependent and since I
am unlikely to want to store things on a real cassette, the program
lets you read/write files. It still uses the original cassette code,
but the emulator takes a bunch of shortcuts to fake things. In
particular, it looks at the program counter to see if it is running
the Woz cassette interface. If you were to copy this code and run it
from another part of memory, the cassette interface would not work.

When you tell the cassette interface to read or write memory, the
emulator will prompt you for a filename. As with the cassette, if you
read/write multiple ranges of memory, all those ranges will be
read from/written to the same file. If you write multiple ranges,
you need to make sure you read from those same ranges, or at least
the same size.

## Debugger
The Froot-1 emulator includes a built-in debugger. To enter the
debugger, either start the emulator with `-d` or hit control-D at
any point to drop into the debugger.

The debugger prints the current pc, registers, and current instruction
whenever it steps to another instruction or hits a breakpoint.

The following commands are available within the debugger:\
s or \<return\> - step to next instruction\
n - step over next instruction (useful to not follow subroutines)\
c - continue running until a breakpoint is reached\
b [addr]  - set breakpoint at address (addr defaults to pc)\
cb [addr]  - set breakpoint at address (addr defaults to pc)\
ca - clear all breakpoints\
lb - list breakpoints\
d start [end] - disassemble starting at start, with optional end addr\
m start [end] - display memory starting at start, with optional end
addr\
end - stop debugging\
h or help - a list of available debugger commands

## Implementation Details
The bulk of the work of this program is performed by Mike Chambers'
fake6502 emulator code, which I also used in my
[KIM-1 Emulator](https://github.com/wutka/kim1-emulator).

I was able to get this emulator working in a couple of hours thanks
to Mike's work.

The rest of the emulator was copied from my KIM-1 emulator and
stripped down since the interface for the Apple-1 is a simple terminal
and not the keypad+LED of the KIM-1.

I have included a couple of utilities to convert back and forth between
pure binary files and the ROM files that the emulator uses, which
are of the format:
```
aaaa: dd dd dd dd dd dd dd dd
```
Where *aaaa* is a 4-digit hex address and each *dd* is a 2-digit hex
value. The parser expects there to be 8 bytes per line and will complain
if not.

### bin2rom
If you have a binary file that you want to convert to a ROM file,
you just run bin2rom giving it the original file, the name of the new
ROM file to write, and the starting address where the binary file
should be loaded. For example, I found the Woz Basic file in a file
called `apple1basic.bin`. I converted it to `wozbasic.rom` with
this command:
```
bin2rom apple1basic.bin wozbasic.rom e000
```

You can also convert a ROM file into a binary file with rom2bin. In
this case, you don't need to supply an address, just the name of the
ROM file and the name of the file to write. For example, to re-create
apple1basic.bin from wozbasic.rom, do:
```
rom2bin wozbasic.rom applebasic.bin
```
