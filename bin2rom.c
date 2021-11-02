#include <stdio.h>
#include <stdlib.h>

void print_row(FILE *out, int addr, unsigned char *row) {
    fprintf(out, "%04X: %02X %02X %02X %02X %02X %02X %02X %02x\n",
            addr, row[0], row[1], row[2], row[3], row[4],
            row[5], row[6], row[7]);
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        printf("Please supply an input filename, an output filename, and a starting address\n");
        exit(1);
    }

    FILE *in;
    FILE *out;

    if ((in = fopen(argv[1], "rb")) == NULL) {
        fprintf(stderr, "Unable to open file %s\n", argv[1]);
        exit(1);
    }

    if ((out = fopen(argv[2], "w")) == NULL) {
        fprintf(stderr, "Unable to open file %s\n", argv[2]);
        exit(1);
    }

    int addr;

    sscanf(argv[3], "%x", &addr);

    if ((addr < 0) || (addr > 0xff80)) {
        fprintf(stderr, "Address %x is out of range\n", addr);
        exit(1);
    }

    unsigned char row[8];
    unsigned char ch;
    int row_pos = 0;

    while (fread(&ch, 1, 1, in) == 1) {
        row[row_pos++] = ch;
        if (row_pos == 8) {
            if (addr > 0xff80) {
                fprintf(stderr, "Address %04x is out of range\n", addr);
                exit(1);
            }
            print_row(out, addr, row);
            row_pos = 0;
            addr += 8;
        }
    }
    if (row_pos > 0) {
        print_row(out, addr, row);
    }
    fclose(in);
    fclose(out);
}
