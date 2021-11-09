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
bool breakpoint[65536];
bool cassette_enabled = true;

FILE *cassette_file;
FILE *input_file;

extern void reset6502();
extern void exec6502(uint32_t);
extern void step6502();
extern void nmi6502();
extern volatile uint16_t pc;
extern volatile uint8_t a, x, y, sp, status;
extern volatile uint32_t clockticks6502;

int load_mem(char *filename, bool read_only);
int load_syms(char *filename);
int kbhit(bool);
void reset_term();
long current_time_millis();
void do_step();
void check_pc();
void handle_kb();
void show_display();
void read_string(char *, int);
void debug_step();
void disassemble(uint16_t, uint16_t);
uint16_t next_inst_addr(uint16_t);
int find_symbol(char *, uint16_t *);

uint8_t read6502(uint16_t);
void write6502(uint16_t, uint8_t);

uint8_t char_pending = 0;
uint8_t reading_file = 0;

char input_line[512];

int max_ram = 4096;

int baud = 0;
clock_t baud_clock_ticks;
clock_t next_char_time;
bool send_ready;

bool debugging = false;
bool debug_run_to_breakpoint = false;
uint16_t temp_breakpoint = 0;

int columns = 0;
int curr_col = 0;

struct sym_node {
    char *name;
    uint16_t value;
    struct sym_node *left;
    struct sym_node *right;
};

struct sym_node *sym_tree = NULL;

int main(int argc, char *argv[]) {

    for (int i=0; i < 65536; i++) {
        ram[i] = 0;
        rom[i] = false;
        breakpoint[i] = false;
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
            i++;
        } else if (!strcmp(argv[i], "-rom")) {
            if (i >= argc-1) {
                printf("Must specify at least one filename after -rom\n");
                exit(1);
            }
            char *start = argv[i+1];
            for (;;) {
                char *char_pos = strchr(start, ',');
                if (!char_pos) {
                    if (!load_mem(start, true)) {
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
            i++;
        } else if (!strcmp(argv[i], "-ram")) {
            if (i >= argc-1) {
                printf("Must specify at least one filename after -ram\n");
                exit(1);
            }
            char *start = argv[i+1];
            for (;;) {
                char *char_pos = strchr(start, ',');
                if (!char_pos) {
                    if (!load_mem(start, false)) {
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
            i++;
        } else if (!strcmp(argv[i], "-sym")) {
            if (i >= argc-1) {
                printf("Must specify at least one filename after -sym\n");
                exit(1);
            }
            char *start = argv[i+1];
            for (;;) {
                char *char_pos = strchr(start, ',');
                if (!char_pos) {
                    if (!load_syms(start)) {
                        exit(1);
                    }
                    break;
                }
                *char_pos = 0;
                if (!load_syms(start)) {
                    exit(1);
                }
                start = char_pos+1;
                char_pos++;
                if (!*char_pos) break;
            }
            i++;
        } else if (!strcmp(argv[i], "-d")) {
            debugging = true;
        } else if (!strcmp(argv[i], "-baud")) {
            if (i >= argc-1) {
                printf("Must specify a baud rate after -baud\n");
                exit(1);
            }
            if (sscanf(argv[i+1], "%d", &baud) == 0) {
                printf("Unable to parse baud rate %s\n",argv[i+1]);
                exit(1);
            }
            if (baud < 0) {
                printf("Baud rate cannot be negative\n");
                exit(1);
            }
            i++;
        } else if (!strcmp(argv[i], "-cols")) {
            if (i >= argc-1) {
                printf("Must specify a column count after -cols\n");
                exit(1);
            }
            if (sscanf(argv[i+1], "%d", &columns) == 0) {
                printf("Unable to parse column count %s\n",argv[i+1]);
                exit(1);
            }
            if (columns < 0) {
                printf("Column count cannot be negative\n");
                exit(1);
            }
            i++;
        } else {
            printf("Unknown argument: %s\n", argv[i]);
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

    send_ready = true;
    if (baud > 0) {
        baud_clock_ticks = 9l * CLOCKS_PER_SEC / (long) baud;
    }

    for (;;) {

        if (!send_ready) {
            clock_t curr_clock = clock();
            if (curr_clock >= next_char_time) {
                send_ready = true;
            }
        }

        do_step();

        // Check where the CPU is
        check_pc();

        // If a key has been hit, process it
        if (kbhit(false)) {
            handle_kb();
        }
        if (reading_file && !char_pending) {
            char ch;
            if (fread(&ch, 1, 1, input_file) < 1) {
                fclose(input_file);
                reading_file = 0;
                printf("File loaded.\n");
            } else {
                if (ch == 0x0a) {
                    ch = 0x0d;
                }
                char_pending = ch;
            }
        }
    }
}

void do_step() {
    if (debugging) {
        debug_step();
    } else {
        step6502();
    }
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

void reset_term() {
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
        snprintf(line, sizeof(line)-1, "/usr/local/share/froot-1/%s", filename);
        if ((in = fopen(line, "r")) == NULL) {
            fprintf(stderr, "Can't open file %s\n", filename);
            return 0;
        }
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

int load_syms(char *filename) {
    FILE *in;

    printf("Loading symbols from %s\n", filename);
    // Open the input file
    if ((in = fopen(filename, "r")) == NULL) {
        snprintf(line, sizeof(line)-1, "/usr/local/share/froot-1/%s", filename);
        if ((in = fopen(line, "r")) == NULL) {
            fprintf(stderr, "Can't open file %s\n", filename);
            return 0;
        }
    }

    // Read it line-by line
    while (fgets(line, sizeof(line), in)) {
        if (strlen(line) == 0) continue;
        if (strncmp(line, "sym", 3)) continue;
        
        char *name_start = strstr(line, "name=\"");
        if (!name_start) continue;
        char *val_start = strstr(line, "val=0x");
        if (!val_start) continue;

        char *name = name_start + 6;
        char *quote_pos = strchr(name, '"');
        if (quote_pos == NULL) continue;
        *quote_pos = 0;

        char *val_str = val_start + 6;
        char *comma_pos = strchr(val_str, ',');
        if (comma_pos == NULL) continue;
        *comma_pos = 0;

        int val;
        if (sscanf(val_str, "%x", &val) == 0) continue;

        struct sym_node *new_node = (struct sym_node *) malloc(sizeof(struct sym_node));
        new_node->name = malloc(strlen(name)+1);
        strcpy(new_node->name, name);
        new_node->value = val;
        new_node->left = NULL;
        new_node->right = NULL;

        if (sym_tree == NULL) {
            sym_tree = new_node;
        } else {
            struct sym_node *curr = sym_tree;
            while (curr != NULL ) {
                int res = strcmp(name, curr->name);
                if (res == 0) {
                    break;
                } else if (res < 0) {
                    if (curr->left == NULL) {
                        curr->left = new_node;
                        break;
                    } else {
                        curr = curr->left;
                    }
                } else {
                    if (curr->right == NULL) {
                        curr->right = new_node;
                        break;
                    } else {
                        curr = curr->right;
                    }
                }
            }
        }
    }
    fclose(in);
    return 1;
}

int find_symbol(char *symbol, uint16_t *value) {
    struct sym_node *curr;

    curr = sym_tree;
    while (curr != NULL) {
        int res = strcmp(symbol, curr->name);
        if (res == 0) {
            *value = curr->value;
            return 1;
        } else if (res < 0) {
            curr = curr->left;
        } else {
            curr = curr->right;
        }
    }
    return 0;
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
                char ch = cassette_read();
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
            int ch = cassette_read();
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
    } else if (ch == 4) {   // Ctrl-D
        debugging = true;
        printf("Debugging mode.\n");
    } else if (ch == 3) {           // Ctrl-C
        reset_term();
        exit(0);
    } else if (char_pending || reading_file) {
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
    } else if (ch == 12) {  // Ctrl-L
        printf("Load from file: ");
        reset_term();
        fgets(input_line, sizeof(input_line)-1, stdin);
        kbhit(true);
        int len = strlen(input_line);
        if ((len > 0) && (input_line[len-1] == '\n')) {
            input_line[len-1] = 0;
        }
        len = strlen(input_line);
        if (len > 0) {
            input_file = fopen(input_line, "r");
            if (input_file != NULL) {
                reading_file = 1;
            } else {
                printf("Unable to open file %s\n", input_line);
            }
        }
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
    } else if (((address & 0xff1f) == 0xd012) || ((address & 0xff1f) == 0xd013)) {
        if (send_ready || reading_file) {
            return 0x00; // Allow baud rate regulation
        } else {
            return 0x80;
        }
    } else {
//        printf("Returning %02x\n", ram[address]);
        return ram[address];
    }
}

/* Callback from the fake6502 library, handle writes to RAM or the RIOT chips */
void write6502(uint16_t address, uint8_t value) {
    if ((address & 0xff1f) == 0xd012) {
        if ((reading_file || send_ready) && (value & 0x80)) {
            char ch = value & 0x7f;
            if (columns > 0 && (ch == 10 || ch == 13)) {
                curr_col = 0;
            }
            if ((ch == 13) || (ch == 10)) {
                putchar(10);
            } else {
                putchar(ch);
                if ((columns > 0) && (++curr_col >= columns)) {
                    putchar(10);
                    curr_col = 0;
                }
            }
            fflush(stdout);

            if (!reading_file && (baud > 0)) {
                next_char_time = clock() + baud_clock_ticks;
                send_ready = false;
            }
        }
    } else if (!rom[address]) { // only write if mem not marked as rom
        ram[address] = value;
    }
}

int parse_addr_range(char *args, uint16_t *start, uint16_t *end, uint16_t default_size) {
    *start = 0;
    int start_len = 0;
    *end = 0;
    int end_len = 0;

    bool invalid_char = false;
    bool at_start = true;
    int add_to_start = false;
    while (*args) {
        char ch = *args++;
        if (((ch >= '0') && (ch <= '9')) ||
            ((ch >= 'a') && (ch <= 'f')) ||
            ((ch >= 'A') && (ch <= 'F'))) {
            uint8_t nybble = 0;
            if ((ch >= '0') && (ch <= '9')) {
                nybble = ch - '0';
            } else if ((ch >= 'a') && (ch <= 'f')) {
                nybble = 10 + ch - 'a';
            } else if ((ch >= 'A') && (ch <= 'F')) {
                nybble = 10 + ch - 'A';
            }
            if (at_start) {
                if (start_len >= 4) {
                    printf("Too many digits in start address.\n");
                    return 0;
                }
                *start = (*start << 4) + nybble;
                start_len++;
            } else {
                if (end_len >= 4) {
                    printf("Too many digits in end address.\n");
                    return 0;
                }
                *end = (*end << 4) + nybble;
                end_len++;
            }
        } else if ((ch == ' ') || (ch == '.') || (ch == ',') || (ch == '-') || (ch == '+')) {
            if (at_start) {
                at_start = false;
            }
            if (ch == '+') {
                add_to_start = true;
            }
        } else if (ch == '@') {
            char *sym = args;
            bool found = false;
            while (*args) {
                char ch = *args;
                if ((ch == ' ') || (ch == '.') || (ch == ',') || (ch == '-') || (ch == '+')) {
                    *args = 0;
                    uint16_t value;
                    if (find_symbol(sym, &value)) {
                        if (at_start) {
                            printf("Start = %04x\n", value);
                            *start = value;
                            at_start = 0;
                        } else {
                            printf("End = %04x\n", value);
                            *end = value;
                        }
                        found = true;
                    } else {
                        printf("Can't find symbol %s\n", sym);
                        return 0;
                    }
                    if (ch == '+') {
                        add_to_start = true;
                    }
                    at_start = 0;
                    args++;
                    break;
                }
                args++;
            }
            if (!found) {
                uint16_t value;
                if (find_symbol(sym, &value)) {
                    if (at_start) {
                        printf("Start = %04x\n", value);
                        *start = value;
                        *end = value + default_size;
                    } else {
                        printf("End = %04x\n", value);
                        *end = value;
                    }
                } else {
                    printf("Can't find symbol %s\n", sym);
                    return 0;
                }
            }
        } else {
            printf("Invalid character in disassembly range: %c\n", ch);
            return 0;
        }
    }
    if (at_start) {
        *end = *start + default_size;
    } else if (add_to_start) {
        printf("Adding to start\n");
        *end = *start + *end;
    }
    return 1;
}

void debug_step() {
    char status_str[9];

    if (!breakpoint[pc] && debug_run_to_breakpoint) {
        step6502();
        return;
    }
    debug_run_to_breakpoint = false;

    status_str[8] = 0;
    status_str[0] = status&0x80 ? 'N' : ' ';
    status_str[1] = status&0x40 ? 'V' : ' ';
    status_str[2] = ' ';
    status_str[3] = status&0x10 ? 'B' : ' ';
    status_str[4] = status&0x08 ? 'D' : ' ';
    status_str[5] = status&0x04 ? 'I' : ' ';
    status_str[6] = status&0x02 ? 'Z' : ' ';
    status_str[7] = status&0x01 ? 'C' : ' ';

    printf("pc = %04x  a=%02x  x=%02x  y=%02x  sp=%02x  status=%s\n",
            pc, a, x, y, sp, status_str);
    disassemble(pc, pc+1);

    if (pc == temp_breakpoint) {
        breakpoint[pc] = false;
        temp_breakpoint = 0;
    }

    reset_term();
    for (;;) {
        printf("Debug>");
        fgets(input_line, sizeof(input_line)-1, stdin);
        int len = strlen(input_line);
        while ((len > 0) && ((input_line[len-1] == '\n') || (input_line[len-1] == '\r'))) {
            input_line[len-1] = 0;
        }
        len = strlen(input_line);

        if (len == 0) {
            // Allow just hitting enter to work like "s"
            kbhit(true);
            step6502();
            return;
        }

        char *spacepos = strchr(input_line, ' ');
        char *args = NULL;
        if (spacepos) {
            *spacepos++ = 0;
            while (*spacepos == ' ') spacepos++;
            args = spacepos;
        }

        if (!strcmp(input_line, "s")) {
            kbhit(true);
            step6502();
            return;
        } else if (!strcmp(input_line, "n")) {
            if (temp_breakpoint != 0) {
                breakpoint[temp_breakpoint] = false;
                printf("Clearing temp breakpoint at %04x\n", temp_breakpoint);
            }
            temp_breakpoint = next_inst_addr(pc);
            breakpoint[temp_breakpoint] = true;
            kbhit(true);
            debug_run_to_breakpoint = true;
            step6502();
            return;
        } else if (!strcmp(input_line, "c")) {
            kbhit(true);
            debug_run_to_breakpoint = true;
            step6502();
            return;
        } else if (!strcmp(input_line, "b")) {
            if (args == NULL) {
                breakpoint[pc] = true;
                printf("Set breakpoint at %04x\n", pc);
            } else {
                if (args[0] == '@') {
                    uint16_t bp_addr;
                    if (!find_symbol(&args[1], &bp_addr)) {
                        printf("Can't find symbol %s\n", &args[1]);
                    } else {
                        breakpoint[bp_addr] = true;
                        printf("Set breakpoint at %04x\n", bp_addr);
                    }
                } else {
                    unsigned int bp_addr;
                    if (sscanf(args, "%x", &bp_addr) == 1) {
                        if (bp_addr >= 0x10000) {
                            printf("Breakpoint %0x out of range.\n", bp_addr);
                        } else {
                            breakpoint[bp_addr] = true;
                            printf("Set breakpoint at %04x\n", bp_addr);
                        }
                    } else {
                        printf("Can't parse breakpoint addr %s\n", args);
                    }
                }
            }
        } else if (!strcmp(input_line, "lb")) {
            bool found_breakpoint = false;
            for (int i=0; i < 65536; i++) {
                if (breakpoint[i]) {
                    printf("%04x\n", i);
                    found_breakpoint = true;
                }
            }
            if (!found_breakpoint) {
                printf("No breakpoints.\n");
            }
        } else if (!strcmp(input_line, "cb")) {
            if (args == NULL) {
                if (!breakpoint[pc]) {
                    printf("No current breakpoint at %04x\n", pc);
                } else {
                    breakpoint[pc] = false;
                    printf("Breakpoint cleared at %04x\n", pc);
                }
            } else {
                if (args[0] == '@') {
                    uint16_t bp_addr;
                    if (!find_symbol(&args[1], &bp_addr)) {
                        printf("Can't find symbol %s\n", &args[1]);
                    } else {
                        breakpoint[bp_addr] = false;
                        printf("Cleared breakpoint at %04x\n", bp_addr);
                    }
                } else {
                    unsigned int bp_addr;
                    if (sscanf(args, "%x", &bp_addr) == 1) {
                        if (bp_addr > 0x10000) {
                            printf("Breakpoint %0x out of range.\n", bp_addr);
                        } else {
                            breakpoint[bp_addr] = false;
                            printf("Cleared breakpoint at %04x\n", bp_addr);
                        }
                    } else {
                        printf("Can't parse breakpoint addr %s\n", args);
                    }
                }
            }
        } else if (!strcmp(input_line, "ca")) {
            int count = 0;
            for (int i=0; i < 65536; i++) {
                if (breakpoint[i]) {
                    count++;
                    breakpoint[i] = false;
                }
            }
            if (count == 0) {
                printf("No breakpoints to clear.\n");
            } else {
                printf("Cleared %d breakpoints.\n", count);
            }
        } else if (!strcmp(input_line, "d")) {
            uint16_t start_addr = 0;
            uint16_t end_addr = 0;
            if (args != NULL) {
                if (!parse_addr_range(args, &start_addr, &end_addr, 20)) {
                    continue;
                }
            } else {
                start_addr = pc;
                end_addr = pc + 20;
            }
            disassemble(start_addr, end_addr);
        } else if (!strcmp(input_line, "m")) {
            if (args == NULL) {
                printf("m command requires at least a starting address\n");
                continue;
            }
            uint16_t start_addr = 0;
            uint16_t end_addr = 0;
            if (!parse_addr_range(args, &start_addr, &end_addr, 64)) {
                continue;
            }
            int bytes_printed = 0;
            char ascii_rep[17];
            if (start_addr & 0xf != 0) {
                printf("%04x: ", start_addr & 0xfff0);
                for (int i=0; i < (start_addr & 0xf); i++) {
                    if (i == 8) {
                        printf("  ");
                    }
                    printf("   ");
                    ascii_rep[i] = ' ';
                }
                bytes_printed = start_addr & 0xf;
            }

            while (start_addr < end_addr) {
                if (bytes_printed == 0) {
                    printf("%04x: ", start_addr);
                }
                if (bytes_printed == 8) {
                    printf("  ");
                }
                printf("%02x ", ram[start_addr]);
                char ch = ram[start_addr] & 0x7f;
                if (ch < 32) {
                    ascii_rep[bytes_printed] = '.';
                } else {
                    ascii_rep[bytes_printed] = ch;
                }
                bytes_printed++;
                ascii_rep[bytes_printed] = 0;
                if (bytes_printed >= 16) {
                    printf("  %s\n", ascii_rep);
                    bytes_printed = 0;
                }
                start_addr++;
            }

            if (bytes_printed > 0) {
                while (bytes_printed < 16) {
                    if (bytes_printed == 8) {
                        printf("  ");
                    }
                    printf("   ");
                    bytes_printed++;
                }
                printf("  %s\n", ascii_rep);
            }
        } else if (!strcmp(input_line, "end")) {
            printf("End debugging mode.\n");
            debugging = false;
            kbhit(true);
            step6502();
            return;
        } else if (!strcmp(input_line, "h") || !strcmp(input_line, "help")) {
            printf("Debugging commands:\n");
            printf("s or <return> - step to next instruction\n");
            printf("n - step over next instruction (useful to not follow subroutines)\n");
            printf("c - continue running until a breakpoint is reached\n");
            printf("b [addr]  - set breakpoint at address (addr defaults to pc)\n");
            printf("cb [addr]  - set breakpoint at address (addr defaults to pc)\n");
            printf("ca - clear all breakpoints\n");
            printf("lb - list breakpoints\n");
            printf("d start [end] - disassemble starting at start, with optional end addr\n");
            printf("m start [end] - display memory starting at start, with optional end addr\n");
            printf("end - stop debugging\n");
            printf("h or help - this listing\n");
            continue;
        }
    }
}

enum addressing_modes {
    IMM, ABS, ABS_X, ABS_Y, ZP, ZP_X, IND, IND_X, IND_Y, REL, ACC, NONE
};

struct instruction {
    char *opcode;
    uint16_t addr_mode;
};

const struct instruction ILLEGAL = { "???", NONE };

struct instruction instruction_desc[] = {
/* 0 */    { "brk", NONE }, {"ora", IND_X}, ILLEGAL, ILLEGAL, ILLEGAL, {"ora", ZP}, {"asl", ZP}, ILLEGAL, {"php", NONE}, {"ora", IMM}, {"asl", ACC}, ILLEGAL, ILLEGAL, {"ora", ABS}, {"asl", ABS}, ILLEGAL,
/* 1 */    { "bpl", REL}, {"ora", IND_Y}, ILLEGAL, ILLEGAL, ILLEGAL, {"ora", ZP_X}, {"asl", ZP_X}, ILLEGAL, {"clc", NONE}, {"ora", ABS_Y}, ILLEGAL, ILLEGAL, ILLEGAL, {"ora", ABS_X}, {"asl", ABS_X}, ILLEGAL,
/* 2 */    { "jsr", ABS}, {"and", IND_X}, ILLEGAL, ILLEGAL, {"bit", ZP}, {"and", ZP}, {"rol", ZP}, ILLEGAL, {"plp", NONE}, {"and", IMM}, {"rol", ACC}, ILLEGAL, {"bit", ABS}, {"and", ABS}, {"rol", ABS}, ILLEGAL,
/* 3 */    { "bmi", REL}, {"and", IND_Y}, ILLEGAL, ILLEGAL, ILLEGAL, {"and", ZP_X}, {"rol", ZP_X}, ILLEGAL, {"sec", NONE}, {"and",ABS_Y}, ILLEGAL, ILLEGAL, ILLEGAL, {"and", ABS_X}, {"rol", ABS_X}, ILLEGAL,
/* 4 */    { "rti", NONE}, {"eor", IND_X}, ILLEGAL, ILLEGAL, ILLEGAL, {"eor", ZP}, {"lsr", ZP}, ILLEGAL, {"pha", NONE}, {"eor", IMM}, {"lsr", ACC}, ILLEGAL, {"jmp", ABS}, {"eor", ABS}, {"lsr", ABS}, ILLEGAL,
/* 5 */    { "bvc", REL}, {"eor", IND_Y}, ILLEGAL, ILLEGAL, ILLEGAL, {"eor", ZP_X}, {"lsr", ZP_X}, ILLEGAL, {"cli", NONE}, {"eor", ABS_Y}, ILLEGAL, ILLEGAL, ILLEGAL, {"eor", ABS_X}, {"lsr", ABS_X}, ILLEGAL,
/* 6 */    {"rts", NONE}, {"adc", IND_X}, ILLEGAL, ILLEGAL, ILLEGAL, {"adc", ZP}, {"ror", ZP}, ILLEGAL, {"pla", NONE}, {"adc", IMM}, {"ror", ACC}, ILLEGAL, {"jmp",IND}, {"adc", ABS}, {"ror", ABS}, ILLEGAL,
/* 7 */    {"bvs", REL}, {"adc",IND_Y}, ILLEGAL, ILLEGAL, ILLEGAL, {"adc",ZP_X}, {"ror", ZP_X}, ILLEGAL, {"sei", NONE}, {"adc", ABS_Y},ILLEGAL, ILLEGAL, ILLEGAL, {"adc",ABS_X}, {"ror",ABS_X}, ILLEGAL,
/* 8 */     ILLEGAL, {"sta",IND_X}, ILLEGAL, ILLEGAL, {"sty",ZP}, {"sta",ZP}, {"stx",ZP}, ILLEGAL, {"dey",NONE}, ILLEGAL,{"txa", NONE}, ILLEGAL, {"sty",ABS},{"sta",ABS},{"stx",ABS}, ILLEGAL,
/* 9 */     {"bcc", REL}, {"sta", IND_Y}, ILLEGAL, ILLEGAL, {"sty",ZP_X}, {"sta",ZP_X}, {"stx",ZP_X}, ILLEGAL, {"tya", NONE}, {"sta",ABS_Y}, {"txs",NONE},  ILLEGAL, ILLEGAL, {"sta",ABS_X}, ILLEGAL, ILLEGAL,
/* a */     {"ldy",IMM}, {"lda",IND_X},{"ldx",IMM}, ILLEGAL,{"ldy",ZP},{"lda",ZP},{"ldx",ZP}, ILLEGAL, {"tay", NONE}, {"lda", IMM}, {"tax", NONE},  ILLEGAL, {"ldy",ABS}, {"lda",ABS}, {"ldx",ABS},  ILLEGAL,
/* b */     {"bcs",REL},{"lda",IND_Y}, ILLEGAL, ILLEGAL,{"ldy",ZP_X},{"lda",ZP_X},{"ldx",ZP_X}, ILLEGAL,{"clv",NONE},{"lda",ABS_Y},{"tsx",NONE}, ILLEGAL,{"ldy",ABS_X},{"lda",ABS_X},{"ldy",ABS_Y}, ILLEGAL,
/* c */     {"cpy",IMM},{"cmp",IND_X}, ILLEGAL, ILLEGAL,{"cpy",ZP},{"cmp",ZP},{"dec",ZP}, ILLEGAL, {"iny",NONE},{"cmp",IMM},{"dex",NONE}, ILLEGAL,{"cpy",ABS},{"cmp",ABS},{"dec",ABS},ILLEGAL,
/* d */     {"bne",REL},{"cmp",IND_Y},ILLEGAL, ILLEGAL, ILLEGAL,{"cmp",ZP_X},{"dec",ZP_X}, ILLEGAL,{"cld",NONE},{"cmp",ABS_Y},ILLEGAL, ILLEGAL, ILLEGAL, {"cmp",ABS_X},{"dec",ABS_X}, ILLEGAL,
/* e */     {"cpx",IMM},{"sbc",IND_X}, ILLEGAL, ILLEGAL,{"cpx",ZP},{"sbc",ZP},{"inc",ZP}, ILLEGAL,{"inx",NONE},{"sbc",IMM},{"nop",NONE}, ILLEGAL,{"cpx",ABS},{"sbc",ABS},{"inc",ABS}, ILLEGAL,
/* f */     {"beq",REL},{"sbc",IND_Y},ILLEGAL, ILLEGAL, ILLEGAL,{"sbc",ZP_X},{"inc",ZP_X}, ILLEGAL,{"sed",NONE},{"sbc",ABS_Y},ILLEGAL, ILLEGAL, ILLEGAL,{"sbc",ABS_X},{"inc",ABS_X}, ILLEGAL
};

uint16_t instruction_size(uint8_t addr_mode) {
    switch (addr_mode) {
        case ABS:
        case ABS_X:
        case ABS_Y:
            return 3;
        case ZP:
        case ZP_X:
        case IND:
        case IND_X:
        case IND_Y:
        case IMM:
        case REL:
            return 2;
        case NONE:
        case ACC:
            return 1;
    }
}

uint16_t next_inst_addr(uint16_t loc) {
    uint8_t opcode = ram[loc];
    struct instruction inst = instruction_desc[opcode];
    uint8_t addr_mode = inst.addr_mode;
    return loc + instruction_size(addr_mode);
}

void disassemble(uint16_t from, uint16_t to) {
    while (from < to) {
        uint8_t opcode = ram[from];
        struct instruction inst = instruction_desc[opcode];
        uint8_t addr_mode = inst.addr_mode;
        uint8_t inst_size = instruction_size(addr_mode);

        printf("%04x: ", from);
        for (int i=0; i <= 2; i++) {
            if (i >= inst_size) {
                printf("   ");
            } else {
                printf("%02x ", ram[from+i]);
            }
        }
        printf(" %3.3s", inst.opcode);

        switch (addr_mode) {
            case ABS:
                printf(" $%04x\n", ram[from+1]+(ram[from+2]<<8));
                break;
            case ABS_X:
                printf(" $%04x,X\n", ram[from+1]+(ram[from+2]<<8));
                break;
            case ABS_Y:
                printf(" $%04x,Y\n", ram[from+1]+(ram[from+2]<<8));
                break;
            case ZP:
                printf(" $%02x\n", ram[from+1]);
                break;
            case ZP_X:
                printf(" $%02x,X\n", ram[from+1]);
                break;
            case IND:
                printf(" ($%02x)\n", ram[from+1]);
                break;
            case IND_X:
                printf(" ($%02x,X)\n", ram[from+1]);
                break;
            case IND_Y:
                printf(" ($%02x),Y\n", ram[from+1]);
                break;
            case IMM:
                printf(" #$%02x\n", ram[from+1]);
                break;
            case REL:
                printf(" $%04x\n", from+2+(char)ram[from+1]);
                break;
            case NONE:
                printf("\n");
                break;
            case ACC:
                printf(" A\n");
                break;
        }

        from += inst_size;
    }
}
