#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

char line[1024];

int main(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Please supply an input file and an output file\n");
        exit(1);
    }

    FILE *in;
    FILE *out;

    if ((in = fopen(argv[1], "r")) == NULL) {
        fprintf(stderr, "Can't open file %s\n", argv[1]);
        exit(1);
    }

    if ((out = fopen(argv[2], "wb")) == NULL) {
        fprintf(stderr, "Can't open file %s\n", argv[2]);
        exit(1);
    }

    while (fgets(line, sizeof(line), in)) {
        if (strlen(line) == 0) continue;
        
        int addr = 0;
        int pos = 0;
        int len = strlen(line);
        unsigned char row[8];

        int addr_len = 0;
        int byte_len = 0;
        int curr_byte = 0;
        bool got_colon = false;
        int byte_count = 0;

        while (pos < len) {
            char ch = line[pos++];
            if (((ch >= '0') && (ch <= '9')) ||
                ((ch >= 'a') && (ch <= 'f')) ||
                ((ch >= 'A') && (ch <= 'F'))) {
                if ((addr_len == 4) && !got_colon) {
                    fprintf(stderr, "No : after 4-digit address in %s at line %s\n", argv[1], line);
                    fclose(in);
                    return 0;
                }
                unsigned char nybble;
                if ((ch >= '0') && (ch <= '9')) {
                    nybble = ch - '0';
                } else if ((ch >= 'a') && (ch <= 'f')) {
                    nybble = 10 + ch - 'a';
                } else if ((ch >= 'A') && (ch <= 'F')) {
                    nybble = 10 + ch - 'A';
                }
                if (addr_len < 4) {
                    addr = (addr << 4) + nybble;
                    addr_len++;
                } else {
                    if (byte_count == 8) {
                        fprintf(stderr, "Got more than 8 bytes in %s at line %s\n", argv[1], line);
                        fclose(in);
                        return 0;
                    }
                    curr_byte = (curr_byte << 4) + nybble;
                    byte_len++;
                    if (byte_len == 2) {
                        row[byte_count] = curr_byte;
                        byte_count++;
                        curr_byte = 0;
                        byte_len = 0;
                    }
                }
            } else if (ch == ':') {
                if (addr_len < 4) {
                    fprintf(stderr, "Got : before 4-digit address in %s at %s\n", argv[1], line);
                    fclose(in);
                    return 0;
                } else if (got_colon) {
                    fprintf(stderr, "Got extra : in %s at %s\n", argv[1], line);
                    fclose(in);
                    return 0;
                }

                got_colon = true;
            } else {
            }
        }
        fwrite(row, 1, 8, out);
    }
    fclose(in);
    fclose(out);
}
