#include "arc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "globals.h"
#include "utils.h"

#define MAX_LINE_LENGTH 256
#define MAX_CONTENT_LENGTH 25
#define MAX_CONF_OPT_LENGTH 128
#define MAX_RBF_NAME_LENGTH 32
#define MAX_VALUES 16

int parse_values( const char *values, int *order ) {
    const char *p = values;
    const char *o = values;
    int i = 0;
    while(1) {
        if (*p == ',' || *p == 0) {
            if (i >= MAX_VALUES) {
                printf("error: more than '%d' dip values.\n", MAX_VALUES);
                return 0;
            }
            unsigned int v = strtol(o, NULL, 10);
            if (v >= MAX_VALUES) {
                printf("error in dip values: '%d' exceeds '%d'.\n", v, MAX_VALUES);
                return 0;
            }
            order[i++] = v;
            if (*p == 0) return i;
            o = p+1;
        }
        p++;
    }
}

char *substrcpy( char *d, const char *s, char idx ) {
    char p = 0;

    while(*s) {
        if((p == idx) && *s && (*s != ','))
            *d++ = *s;

        if(*s == ',')
            p++;

        s++;
    }
    *d = 0;
    return d;
}

void reorder_ids( const char *ids, const int *order, int orders, char *dest ) {
    char idx;
    for (int i = 0; i < orders; i++) {
        dest = substrcpy(dest, ids, order[i]);
        if (i != orders-1) *dest++=',';
    }
}

char *format_bits( t_mra* mra, t_dip *dip ) {
    char buffer[256] = "O";
    int start = 1;
    int n;

    int base = mra->switches.base;
    int page_id = mra->switches.page_id;

    if (page_id >= 1 && page_id <= 9) {
        snprintf(buffer, 256, "P%dO", page_id);
        start = 3;
    }
    n = start;

    char *token = dip->bits;

    // Parse bits first
    while (token = strtok(token, ",")) {
        char c = atoi(token) + (char)base;
        if (c > 61) {
            printf("error while parsing dip switch (%s): required bit position exceeds 61.\n", mra->setname);
            return NULL;
        }
        buffer[n++] = (c < 10) ? ('0' + c) : (c < 36) ? ('A' + c - 10) : ('a' + c - 36);
        token = NULL;
    }
    buffer[n] = '\0';

    if (!dip->ids) {
        if (n - 1 > start) {
            printf("error (%s) while parsing \"%s\" dip switch: number of bits > 1 but no ids defined.\n", mra->setname, dip->name);
            return NULL;
        }
        buffer[start - 1] = 'T';
    }

    return strdup(buffer);
}

int check_ids_len(t_dip *dip) {
    int nlen;
    int tlen;
    char copy[MAX_LINE_LENGTH];
    char *tok;

    nlen = strnlen(dip->name, MAX_LINE_LENGTH);
    strncpy(copy, dip->ids, MAX_LINE_LENGTH);
    tok = strtok(copy, ",");
    tlen = nlen;
    while (tok) {
        int j = strlen(tok);
        tlen += j+1;
        if (tlen > MAX_CONF_OPT_LENGTH) return 1;
        if (nlen + j > MAX_CONTENT_LENGTH) return 1;
        tok = strtok(NULL, ",");
    }
    return 0;
}

int write_arc(t_mra *mra, char *filename) {
    FILE *out;
    char buffer[MAX_LINE_LENGTH + 1];
    int i, n;
    int mod = 0;

    /* let's be strict about mod:
        use only the first byte in a single part in a ROM with index = "1".
    */
    i = mra_get_rom_by_index(mra, 1, 0);
    if (i != -1 &&
        mra->roms[i].n_parts == 1 &&
        !mra->roms[i].parts[0].is_group &&
        mra->roms[i].parts[0].p.data_length >= 1) {
        mod = mra->roms[i].parts[0].p.data[0];
    }

    out = fopen(filename, "wb");
    if (out == NULL) {
        fprintf(stderr, "Couldn't open %s for writing!\n", filename);
        return -1;
    }

    n = snprintf(buffer, MAX_LINE_LENGTH, "[ARC]\n");
    if (n >= MAX_LINE_LENGTH) printf("%s:%d: warning: line was truncated while writing in ARC file!\n", __FILE__, __LINE__);
    fwrite(buffer, 1, n, out);
    // Write rbf
    if (mra->rbf.name) {
        char *rbf = str_toupper(mra->rbf.alt_name ? mra->rbf.alt_name : mra->rbf.name);
        if (strnlen(rbf, MAX_LINE_LENGTH) > MAX_RBF_NAME_LENGTH) printf("warning: RBF file name may be too long for MiST\n");

        n = snprintf(buffer, MAX_LINE_LENGTH, "RBF=%s\n", rbf);
        if (n >= MAX_LINE_LENGTH) printf("%s:%d: warning: line was truncated while writing in ARC file!\n", __FILE__, __LINE__);
        fwrite(buffer, 1, n, out);

        if (mod != -1) {
            n = snprintf(buffer, MAX_LINE_LENGTH, "MOD=%d\n", mod);
            if (n >= MAX_LINE_LENGTH) printf("%s:%d: warning: line was truncated while writing in ARC file!\n", __FILE__, __LINE__);
            fwrite(buffer, 1, n, out);
        }
    }
    n = snprintf(buffer, MAX_LINE_LENGTH, "NAME=%s\n", str_toupper(rom_basename));
    if (n >= MAX_LINE_LENGTH) printf("%s:%d: warning: line was truncated while writing in ARC file!\n", __FILE__, __LINE__);
    fwrite(buffer, 1, n, out);

    if (mra->switches.n_dips && mra->switches.defaults) {
        n = snprintf(buffer, MAX_LINE_LENGTH, "DEFAULT=0x%llX\n", mra->switches.defaults << mra->switches.base);
        fwrite(buffer, 1, n, out);
    }
    if (mra->switches.page_id && mra->switches.page_name) {
        n = snprintf(buffer, MAX_LINE_LENGTH, "CONF=\"P%d,%s\"\n", mra->switches.page_id, mra->switches.page_name);
        fwrite(buffer, 1, n, out);
    }

    for (i = 0; i < mra->switches.n_dips; i++) {
        t_dip *dip = mra->switches.dips + i;
        if (!strstr(str_tolower(dip->name), "unused")) {
            if (dip->ids) {
                if (check_ids_len(dip)) {
                    printf("warning (%s): dip_content too long for MiST (%s):\n\t%s\t%s\n\n", mra->setname, mra->name, dip->name, dip->ids);
                    continue;
                }
                int order[MAX_VALUES];
                char reordered_ids[MAX_LINE_LENGTH];
                int orders = 0;
                if (dip->values) orders = parse_values( dip->values, order );
                if (orders) reorder_ids( dip->ids, order, orders, reordered_ids );
                n = snprintf(buffer, MAX_LINE_LENGTH, "CONF=\"%s,%s,%s\"\n", format_bits( mra, dip ), dip->name, orders ? reordered_ids : dip->ids);
                strnlen(dip->ids, MAX_LINE_LENGTH);
            } else {
                n = snprintf(buffer, MAX_LINE_LENGTH, "CONF=\"%s,%s\"\n", format_bits( mra, dip ), dip->name);
            }
            if (n >= MAX_LINE_LENGTH) {
                printf("%s:%d: warning (%s): line was truncated while writing in ARC file!\n", __FILE__, __LINE__, mra->setname);
                continue;
            }
            fwrite(buffer, 1, n, out);
        } else {
            printf("warning (%s): \"%s\" dip setting skipped (unused)\n", mra->setname, dip->name);
        }
    }

    if (mra->buttons.names) {
        n = snprintf(buffer, MAX_LINE_LENGTH, "BUTTONS=\"%s\"\n", mra->buttons.names);
        if (n >= MAX_LINE_LENGTH) printf("%s:%d: warning: line was truncated while writing in ARC file!\n", __FILE__, __LINE__);
        fwrite(buffer, 1, n, out);
    }
    fclose(out);
    return 0;
}
