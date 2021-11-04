#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/ioctl.h> // For FIONREAD
#include <termios.h>
#include <stdbool.h>
#include <time.h>
#include <memory.h>
#include <ctype.h>
#include <unistd.h>

uint8_t ram[65536];
bool rom[65536];
bool cassette_enabled = true;

FILE *cassette_file;

extern void reset6502();
extern void exec6502(uint32_t);
extern void step6502();
extern void nmi6502();
extern volatile uint16_t pc;
extern volatile uint8_t a, x, y, status;
extern volatile uint32_t clockticks6502;

int load_mem(char *filename, bool read_only);
int kbhit(bool);
int reset_term();
long current_time_millis();
void do_step();
void check_pc();
void handle_kb();
void show_display();
void read_string(char *, int);

uint8_t read6502(uint16_t);
void write6502(uint16_t, uint8_t);

uint8_t char_pending;

char input_line[512];

int max_ram = 4096;

int main(int argc, char *argv[]) {

    for (int i=0; i < 65536; i++) {
        ram[i] = 0;
        rom[i] = false;
    }

    // Load the Woz monitor (at FF00)
    load_mem("monitor.rom", true);

    // Parse the command-line arguments
    for (int i=1; i < argc; i++) {
        if (!strcmp(argv[i], "-mem") || !strcmp(argv[i], "--mem")) {
            if (i >= argc-1) {
                printf("Must specify ram size (1k,2k, ... 64K or full)\n");
                exit(1);
            }
            if (!strcmp(argv[i+1], "full")) {
                max_ram = 65536;
            } else if (isdigit(argv[i+1][0]) && (argv[i+1][strlen(argv[i+1])-1] == 'k' || (argv[i+1][strlen(argv[i+1])-1] == 'K'))) {
                argv[i+1][strlen(argv[i+1])-1] = 0;
                int ram_size = atoi(argv[i+1]);
                if (ram_size < 1) {
                    printf("Ram size must be at least 1k\n");
                    exit(1);
                }
                if (ram_size > 64) {
                    printf("Ram size cannot be greater than 64k\n");
                    exit(1);
                }

                max_ram = 1024 * ram_size;
            }
            i++;
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "-help") ||
            !strcmp(argv[i], "--h") || !strcmp(argv[i], "--help")) {
            printf("Usage:  froot1 [-mem size] [-cassette y/n] [-rom file1,file2,etc.] [-ram file1,file2,etc.]\n  where size = 1k, 2k, .. 64k\n");
            printf("\nThe mem size currently specifies the amount of ram memory available,\n");
            printf("except for the 256 bytes at FF00-FFFF and D000-D100 for the P1A controller.\n");
            printf("If you specify 64K and load additional roms, those ROM locations will be marked\n");
            printf("as ROM, so you won't have a full 64K, but you don't have to work out how much\n");
            printf("RAM is left after loading ROMS. If the cassette interface is enabled (default=y),\n");
            printf("then locations C000-C200 are also marked as ROM. This emulator does not fully \n");
            printf("emulate the cassette interface from a timing perspective, but instead works with \n");
            printf("the cassette ROM to load/store each bit.\n");
            printf("ROM and RAM files should be text files with lines in the format:\n");
            printf("aaaa: dd dd dd dd dd dd dd dd\n");
            printf("where aaaa is a hex address, and each dd is a hex byte.\n");
            printf("The parser is currently limited to 16 bytes per row.\n");
            printf("Since ROM files are loaded first, if a ROM and RAM file have overlapping addresses,\n");
            printf("the ROM wins and the memory is marked as read-only\n");
            printf("The emulator will automatically load the monitor.rom file.\n");

            exit(0);
        } else if (!strcmp(argv[i], "-cassette")) {
            if (i >= argc-1) {
                printf("Must specify y or n for cassette\n");
                exit(1);
            }
            if ((argv[i+1][0] == 'y') || (argv[i+1][0] == 'Y')) {
                cassette_enabled = 1;
            } else if ((argv[i+1][0] == 'n') || (argv[i+1][0] == 'N')) {
                cassette_enabled = 0;
            } else {
                printf("Must specify y or n for cassette\n");
                exit(1);
            }
        } else if (!strcmp(argv[i], "-rom")) {
            if (i >= argc-1) {
                printf("Must specify at least one filename after -rom\n");
                exit(1);
            }
            char *start = argv[i+1];
            for (;;) {
                char *char_pos = strchr(argv[i+1], ',');
                if (!char_pos) {
                    if (!load_mem(argv[i+1], true)) {
                        exit(1);
                    }
                    break;
                }
                *char_pos = 0;
                if (!load_mem(start, true)) {
                    exit(1);
                }
                start = char_pos+1;
                char_pos++;
                if (!*char_pos) break;
            }
        } else if (!strcmp(argv[i], "-ram")) {
            if (i >= argc-1) {
                printf("Must specify at least one filename after -ram\n");
                exit(1);
            }
            char *start = argv[i+1];
            for (;;) {
                char *char_pos = strchr(argv[i+1], ',');
                if (!char_pos) {
                    if (!load_mem(argv[i+1], false)) {
                        exit(1);
                    }
                    break;
                }
                *char_pos = 0;
                if (!load_mem(start, false)) {
                    exit(1);
                }
                start = char_pos+1;
                char_pos++;
                if (!*char_pos) break;
            }
        }
    }

    if (cassette_enabled) {
        // If cassette is enabled, load the Woz cassette interface
        load_mem("wozaci.rom", true);
    }

    for (int i=max_ram; i < sizeof(ram); i++) {
        rom[i] = true;
    }

    // Reset the CPU
    reset6502();

    for (;;) {


        do_step();

        // Check where the CPU is
        check_pc();

        // If a key has been hit, process it
        if (kbhit(false)) {
            handle_kb();
        }
    }
}

void do_step() {
    step6502();
}

void set_raw() {
    static const int STDIN = 0;

    // Use termios to turn off line buffering
    struct termios term;
    tcgetattr(STDIN, &term);
    term.c_lflag &= ~ICANON;
    term.c_lflag &= ~ECHO;
    term.c_iflag &= ~ICRNL;
    tcsetattr(STDIN, TCSANOW, &term);
    setbuf(stdin, NULL);
}

int kbhit(bool init) {
    static bool initflag = false;
    static const int STDIN = 0;

    // If raw mode hasn't been turned on yet, turn it on
    if (init || !initflag) {
        set_raw();
        initflag = true;
    }

    // Return the number of bytes available to read
    int nbbytes;
    ioctl(STDIN, FIONREAD, &nbbytes);  // 0 is STDIN
    return nbbytes;
}

int reset_term() {
    static const int STDIN = 0;

    // Use termios to turn on line buffering
    struct termios term;
    tcgetattr(STDIN, &term);
    term.c_lflag |= ICANON;
    term.c_lflag |= ECHO;
    term.c_iflag |= ICRNL;
    tcsetattr(STDIN, TCSANOW, &term);
    setbuf(stdin, NULL);
}

char line[1024];

int load_mem(char *filename, bool read_only) {
    FILE *in;

    // Open the input file
    if ((in = fopen(filename, "r")) == NULL) {
        fprintf(stderr, "Can't open file %s\n", filename);
        return 0;
    }

    // Read it line-by line
    while (fgets(line, sizeof(line), in)) {
        if (strlen(line) == 0) continue;
        
        int addr = 0;
        int pos = 0;
        int len = strlen(line);
        unsigned char row[16];

        int addr_len = 0;
        int byte_len = 0;
        int curr_byte = 0;
        bool got_colon = false;
        int byte_count = 0;

        // Parse to the end of the line
        while (pos < len) {
            char ch = line[pos++];

            // Is the next char a hex digit?
            if (((ch >= '0') && (ch <= '9')) ||
                ((ch >= 'a') && (ch <= 'f')) ||
                ((ch >= 'A') && (ch <= 'F'))) {

                // Have we parsed the 4-digit address but not seen a : yet?
                if ((addr_len == 4) && !got_colon) {
                    fprintf(stderr, "No : after 4-digit address in %s at line %s\n", filename, line);
                    fclose(in);
                    return 0;
                }
                unsigned char nybble;

                // Convert the digit to a nybble
                if ((ch >= '0') && (ch <= '9')) {
                    nybble = ch - '0';
                } else if ((ch >= 'a') && (ch <= 'f')) {
                    nybble = 10 + ch - 'a';
                } else if ((ch >= 'A') && (ch <= 'F')) {
                    nybble = 10 + ch - 'A';
                }

                // If we are still in the address, add the nybble to the address
                if (addr_len < 4) {
                    addr = (addr << 4) + nybble;
                    addr_len++;
                } else {
                    // If we already have 16 bytes on this line and see another digit, that's an error
                    if (byte_count == 16) {
                        fprintf(stderr, "Got more than 16 bytes in %s at line %s\n", filename, line);
                        fclose(in);
                        return 0;
                    }
                    // Add the nybble to the current byte
                    curr_byte = (curr_byte << 4) + nybble;
                    byte_len++;

                    // If we have a complete byte, add it to the row
                    if (byte_len == 2) {
                        row[byte_count] = curr_byte;
                        byte_count++;
                        curr_byte = 0;
                        byte_len = 0;
                    }
                }
            } else if (ch == ':') {
                if (addr_len < 4) {
                    fprintf(stderr, "Got : before 4-digit address in %s at %s\n", filename, line);
                    fclose(in);
                    return 0;
                } else if (got_colon) {
                    fprintf(stderr, "Got extra : in %s at %s\n", filename, line);
                    fclose(in);
                    return 0;
                }

                got_colon = true;
            } else {
            }
        }

        // Write the row into ram and update the rom flag appropriately
        for (int i=0; i < byte_count; i++) {
            if (!rom[addr+i]) {
                ram[addr+i] = row[i];
                rom[addr+i] = read_only;
            }
        }
    }
    fclose(in);
    return 1;
}

void begin_write_cassette() {
    // If we are already writing, don't prompt for another file
    // The Apple-1 cassette interface can write multiple address
    // ranges to the cassette
    if (cassette_file != NULL) return;
    reset_term();
    for (;;) {
        printf("Cassette save to file (enter=cancel): ");
        fgets(input_line, sizeof(input_line)-1, stdin);
        int len = strlen(input_line);
        if ((len > 0) && (input_line[len-1] == '\n')) {
            input_line[len-1] = 0;
        }
        len = strlen(input_line);
        if (len == 0) {
            printf("Cassette write aborted, will not write to file\n");
            cassette_file = NULL;
            break;
        }
        if ((cassette_file = fopen(input_line, "wb")) == NULL) {
            printf("Unable to open file %s for writing, try again\n", input_line);
            continue;
        }
        break;
    }
    // kbhit(true) puts the terminal back in raw mode
    kbhit(true);
}

void begin_read_cassette() {
    // If we are already writing, don't prompt for another file
    // The Apple-1 cassette interface can read multiple address
    // ranges from the cassette
    if (cassette_file != NULL) return;
    reset_term();
    for (;;) {
        printf("Cassette file to read (enter=cancel): ");
        fgets(input_line, sizeof(input_line)-1, stdin);
        int len = strlen(input_line);
        if ((len > 0) && (input_line[len-1] == '\n')) {
            input_line[len-1] = 0;
        }
        len = strlen(input_line);
        if (len == 0) {
            printf("Cassette read aborted, will not read from file\n");
            cassette_file = NULL;
            break;
        }
        if ((cassette_file = fopen(input_line, "rb")) == NULL) {
            printf("Unable to open file %s for reading, try again\n", input_line);
            continue;
        }
        break;
    }
    // kbhit(true) puts the terminal back in raw mode
    kbhit(true);
}

int cassette_read() {
    unsigned char ch;
    if (cassette_file == NULL) {
        return -1;
    }
    if (fread(&ch, 1, 1, cassette_file) == 0) {
        return -1;
    }
    return ch;
}

void cassette_write(unsigned char ch) {
    if (cassette_file != NULL) {
        fwrite(&ch, 1, 1, cassette_file);
    }
}

void cassette_end() {
    if (cassette_file != NULL) {
        fclose(cassette_file);
        cassette_file = NULL;
    }
    printf("Cassette finished.\n");
}

/* check_pc is a hack to get the cassette interface to work.
 * It doesn't work yet. */
void check_pc() {
    if (cassette_enabled) {
        if (pc == 0xc170) { // ACI - WRITE, skip to WRNEXT
            ram[0x28] = x; // save X in SAVEINDEX, since we skip WHEADER, we need to do this
            begin_write_cassette();
            if (cassette_file == NULL) {
                pc = 0xc163; // Quit if no filename entered
            } else {
                pc = 0xc175;
            }
        } else if (pc == 0xc17c) {  // ACI - WBITLOOP
            cassette_write(a);
            pc = 0xc182; // skip write bit loop, jump to increment address
        } else if (pc == 0xc18d)  { // ACI - READ
            begin_read_cassette();
            if (cassette_file == NULL) {
                pc = 0xc163; // Quit if no filename entered
            } else {
                ram[0x28] = x; // save X in SAVEINDEX, since we skip WHEADER, we need to do this
                char ch = cassette_read(a);
                if (ch < 0) {
                    status = status | 1; // Set carry
                    pc = 0xc189;
                } else {
                    a = ch;
                    x = 0;
                    pc = 0xc1b1;  // save new byte
                }
            }
        } else if (pc == 0xc1a4) {  // ACI - RDBYTE
            int ch = cassette_read(a);
            if (ch < 0) {
                status = status | 1; // Set carry
                pc = 0xc189;
            } else {
                a = ch;
                x = 0;
                pc = 0xc1b1;  // save new byte
            }
        } else if (pc == 0xc189) {
            status = status | 1; // Set carry
        } else if (pc == 0xc163) {  // ACI - GOESC
            // Don't end the cassette operation until there was no more
            // import for the cassette monitor in case it is reading/writing
            // multiple memory ranges
            cassette_end();
        }
    }
}

/* Handle local keyboard interaction. */
void handle_kb() {
    char ch;
    int len;
    uint16_t addr, save_len;
    FILE *loadfile;

    ch = getchar();

    if (ch == 18) {                 // Ctrl-R
        printf("RESET\n");
        reset6502();
    } else if (char_pending) {
        // If the last character hasn't been processed, push this one back
        ungetc(ch, stdin);
    } else if (ch == 10) {
        // Convert a newline to carriage-return
        char_pending = 13;
    } else if (ch == 8 || ch == 0x7f) {
        // Backspace or delete were originally converted to 3f (?) because that's what
        // the Apple-1 uses for delete. I patched monitor.rom so that 8 is a backspace
        // instead of 3F
        char_pending = 8;
    } else if (ch == 3) {           // Ctrl-C
        reset_term();
        exit(0);
    } else if ((ch >= 'a') && (ch <= 'z')) {
        // Apple-1 only supported uppercase
        char_pending = ch - 'a' + 'A';
    } else {
        char_pending = ch;
    }
}

/* Callback from the fake6502 library, handle reads from RAM or the RIOT chips */
uint8_t read6502(uint16_t address) {
//    printf("reading %04x, pc = %04x\n", address, pc);
    if (address == 0xd011) {
        if (char_pending) {
            return 0x80;
        } else {
            return 0;
        }
    } else if (address == 0xd010) {
        uint8_t ch = char_pending;
        char_pending = 0;
        return 0x80 | ch;
    } else if (address == 0xd013) {
        return 0x80; // Always ready to write to display
    } else {
//        printf("Returning %02x\n", ram[address]);
        return ram[address];
    }
}

/* Callback from the fake6502 library, handle writes to RAM or the RIOT chips */
void write6502(uint16_t address, uint8_t value) {
    if ((address & 0xff1f) == 0xd012) {
        char ch = value & 0x7f;
        if (ch == 13) {
            putchar(10);
        } else {
            putchar(ch);
        }
        fflush(stdout);
    } else if (!rom[address]) { // only write if mem not marked as rom
        ram[address] = value;
    }
}
