#include "mra.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "utils.h"

static void store_node(char **dest, const char *text) {
    if (*dest) free(*dest);
    if (text)
        *dest = strndup(text, 1024);
    else
        *dest = 0;
}

int read_patch(XMLNode *node, t_patch *patch) {
    int j;

    memset(patch, 0, sizeof(t_patch));
    for (j = 0; j < node->n_attributes; j++) {
        if (strncmp(node->attributes[j].name, "offset", 7) == 0) {
            // offset can be decimal or hexa with 0x prefix
            patch->offset = strtoul(node->attributes[j].value, NULL, 0);
        }
    }
    if (node->text != NULL) {
        char *trimmed_text = str_trimleft(node->text);
        if (*trimmed_text) {
            if (parse_hex_string(trimmed_text, &(patch->data), &(patch->data_length))) {
                printf("warning: failed to decode patch data. Data dropped.\n");
            }
        }
    }
}

void get_pattern_from_map(char *map, uint8_t **pattern, int *map_index) {
    int i, j;
    int n = strnlen(map, 256);
    char first_found = 0;

    *pattern = (char *)malloc(n + 1);

    for (i = n - 1, j = 0; i >= 0; i--) {
        if (map[i] != '0') {
            if (!first_found) {
                first_found = -1;
                (*map_index) = n - i - 1;
            }
            (*pattern)[j++] = map[i] - 1;
        }
    }
    (*pattern)[j] = '\0';

    if (trace > 0) printf("map=0x%s => pattern=\"%s\"\n", map, *pattern);
}

static void free_parts(t_part *parts, int num) {
    int i;

    for (i = 0; i < num; i++) {
        if (parts[i].is_group) {
            free_parts(parts[i].g.parts, parts[i].g.n_parts);
        } else {
            if (parts[i].p.name)    free(parts[i].p.name);
            if (parts[i].p.zip)     free(parts[i].p.zip);
            if (parts[i].p.pattern) free(parts[i].p.pattern);
            if (parts[i].p.data)    free(parts[i].p.data);
        }
    }
    free(parts);
}

int read_part(XMLNode *node, t_part *part, int parent_index) {
    int j;

    memset(part, 0, sizeof(t_part));
    part->p._map_index = parent_index;
    
    for (j = 0; j < node->n_attributes; j++) {
        if (strncmp(node->attributes[j].name, "crc", 4) == 0) {
            // CRC is read as hexa no matter what
            part->p.crc32 = strtoul(node->attributes[j].value, NULL, 16);
        } else if (strncmp(node->attributes[j].name, "name", 5) == 0) {
            store_node(&part->p.name, node->attributes[j].value);
        } else if (strncmp(node->attributes[j].name, "zip", 4) == 0) {
            store_node(&part->p.zip, node->attributes[j].value);
        } else if (strncmp(node->attributes[j].name, "repeat", 7) == 0) {
            // repeat can be decimal or hexa with 0x prefix
            part->p.repeat = strtoul(node->attributes[j].value, NULL, 0);
        } else if (strncmp(node->attributes[j].name, "offset", 7) == 0) {
            // offset can be decimal or hexa with 0x prefix
            part->p.offset = strtoul(node->attributes[j].value, NULL, 0);
        } else if (strncmp(node->attributes[j].name, "length", 7) == 0 ||
                   strncmp(node->attributes[j].name, "size", 5) == 0) {
            // length/size can be decimal or hexa with 0x prefix
            part->p.length = strtoul(node->attributes[j].value, NULL, 0);
        } else if (strncmp(node->attributes[j].name, "pattern", 8) == 0) {
            part->p.pattern = strndup(node->attributes[j].value, 256);
        } else if (strncmp(node->attributes[j].name, "map", 4) == 0) {
            get_pattern_from_map(node->attributes[j].value, &(part->p.pattern), &(part->p._map_index));
        } else {
            printf("warning: unknown attribute for regular part: %s\n", node->attributes[j].name);
        }
    }
    if (node->text != NULL) {
        char *trimmed_text = str_trimleft(node->text);
        if (*trimmed_text) {
            if (part->p.name) {
                printf("warning: part %s has a name and data. Data dropped.\n", part->p.name);
            } else {
                if (parse_hex_string(trimmed_text, &(part->p.data), &(part->p.data_length))) {
                    printf("warning: failed to decode part data. Data dropped.\n");
                } else {
                }
            }
        }
    }
    return 0;
}

t_part *read_group(XMLNode *node, t_part *part) {
    int i, j;

    memset(part, 0, sizeof(t_part));
    part->is_group = -1;
    part->g.width = 8;            // default width = 8 bits
    part->g.is_interleaved = -1;  // groups are interleaved by default
    for (j = 0; j < node->n_attributes; j++) {
        if (strncmp(node->attributes[j].name, "width", 6) == 0) {
            part->g.width = atoi(node->attributes[j].value);
        } else if (strncmp(node->attributes[j].name, "output", 7) == 0) {
            part->g.width = atoi(node->attributes[j].value);
        } else if (strncmp(node->attributes[j].name, "repeat", 7) == 0) {
            part->g.repeat = atoi(node->attributes[j].value);
        } else if (strncmp(node->attributes[j].name, "interleaved", 12) == 0) {
            part->g.is_interleaved = atoi(node->attributes[j].value);
        } else {
            printf("warning: unsupported attribute for group: %s\n", node->attributes[j].name);
        }
    }
    if (node->text != NULL) {
        char *trimmed_text = str_trimleft(node->text);
        if (*trimmed_text) {
            printf("warning: groups cannot have embedded data. (%s)\n", node->text);
        }
    }
    return part;
}

static int cmp_part_map(const void *p1, const void *p2) {

    return ((t_part *)p1)->p._map_index - ((t_part *)p2)->p._map_index;
}

int read_parts(XMLNode *node, t_part **parts, int *n_parts) {
    int i;
    char *part_types[] = {"part", "group", "interleave"};
    t_part *part;

    for (i = 0; i < 3; i++)
        if (strncmp(node->tag, part_types[i], 20) == 0)
            break;

    if (i < 3) {
        (*n_parts)++;
        *parts = (t_part *)realloc(*parts, sizeof(t_part) * (*n_parts));
        part = (*parts) + (*n_parts) - 1;

        switch (i) {
            case 0:  // part
                read_part(node, part, (*n_parts) - 1);
                break;
            case 1:  // group
            case 2:  // interleave
            {
                t_part *group = read_group(node, part);
                for (i = 0; i < node->n_children; i++) {
                    read_parts(node->children[i], &(group->g.parts), &(group->g.n_parts));
                }
                qsort(group->g.parts, group->g.n_parts, sizeof(t_part), cmp_part_map);
            } break;
            default:  // won't happen
                break;
        }
    } else if (node->tag_type != TAG_COMMENT) {
        printf("warning: unexpected token in rom node: %s\n", node->tag);
        return -1;
    }

    return 0;
}

void read_rom(XMLNode *node, t_rom *rom) {
    int i, j;

    memset(rom, 0, sizeof(t_rom));

    for (j = 0; j < node->n_attributes; j++) {
        if (strncmp(node->attributes[j].name, "index", 6) == 0) {
            rom->index = atoi(node->attributes[j].value);
        } else if (strncmp(node->attributes[j].name, "zip", 4) == 0) {
            string_list_add(&rom->zip, node->attributes[j].value);
        } else if (strncmp(node->attributes[j].name, "md5", 4) == 0) {
            if (strncmp(node->attributes[j].value, "none", 256) != 0) {
                store_node(&rom->md5, node->attributes[j].value);
            }
        } else if (strncmp(node->attributes[j].name, "type", 5) == 0) {
            string_list_add(&rom->type, node->attributes[j].value);
        }
    }
    for (i = 0; i < node->n_children; i++) {
        if (strncmp(node->children[i]->tag, "patch", 6) == 0) {
            rom->n_patches++;
            rom->patches = (t_patch *)realloc(rom->patches, sizeof(t_patch) * rom->n_patches);
            read_patch(node->children[i], rom->patches + rom->n_patches - 1);
        } else {
            read_parts(node->children[i], &rom->parts, &rom->n_parts);
        }
    }
}

static void free_switches(t_switches *switches) {
    int i;
    if (switches->page_name) free(switches->page_name);
    for (i = 0; i < switches->n_dips; i++) {
        if (switches->dips[i].bits)   free(switches->dips[i].bits);
        if (switches->dips[i].name)   free(switches->dips[i].name);
        if (switches->dips[i].ids)    free(switches->dips[i].ids);
        if (switches->dips[i].values) free(switches->dips[i].values);
    }
    if (switches->dips) free(switches->dips);
}

void read_dip_switch(XMLNode *node, t_dip *dip_switch) {
    int i;

    memset(dip_switch, 0, sizeof(t_dip));
    for (i = 0; i < node->n_attributes; i++) {
        if (strncmp(node->attributes[i].name, "bits", 5) == 0) {
            store_node(&dip_switch->bits, node->attributes[i].value);
        } else if (strncmp(node->attributes[i].name, "name", 5) == 0) {
            store_node(&dip_switch->name, node->attributes[i].value);
        } else if (strncmp(node->attributes[i].name, "ids", 4) == 0) {
            store_node(&dip_switch->ids, node->attributes[i].value);
        } else if (strncmp(node->attributes[i].name, "values", 7) == 0) {
            store_node(&dip_switch->values, node->attributes[i].value);
        }
    }
}

int read_switches(XMLNode *node, t_switches *switches) {
    int i;

    free_switches(switches);
    memset(switches, 0, sizeof(t_switches));

    // Read all attributes
    for (i = 0; i < node->n_attributes; i++) {
        XMLAttribute *attr = &node->attributes[i];
        if (strncmp(attr->name, "base", 10) == 0) {
            switches->base = strtol(attr->value, NULL, 0);
        } else if (strncmp(attr->name, "default", 8) == 0) {
            int a, b, c, d, n;  // up to four values
            //Fix incorrect spaces
            char *temp=attr->value;
            while (*temp != '\0' ){
               if (*temp == ' ' ) {
                 *temp = ',';
               }
               temp++;
            } 
            n = sscanf(attr->value, "%X,%X,%X,%X", &a, &b, &c, &d);
            if (n-- > 0) switches->defaults |= a;
            if (n-- > 0) switches->defaults |= ((b & 0xff) << 8);
            if (n-- > 0) switches->defaults |= ((c & 0xff) << 16);
            if (n-- > 0) switches->defaults |= ((d & 0xff) << 24);
        } else if (strncmp(attr->name, "page_id", 8) == 0) {
            switches->page_id = strtol(attr->value, NULL, 0);
        } else if (strncmp(attr->name, "page_name", 10) == 0) {
            store_node(&switches->page_name, attr->value);
        }
    }

    // Read DIPs
    for (i = 0; i < node->n_children; i++) {
        XMLNode *child = node->children[i];
        if (strncmp(child->tag, "dip", 4) == 0) {
            switches->n_dips++;
            switches->dips = (t_dip *)realloc(switches->dips, sizeof(t_dip) * (switches->n_dips));
            read_dip_switch(child, switches->dips + switches->n_dips - 1);
        }
    }
    return 0;
}

void read_buttons(XMLNode *node, t_buttons *buttons) {
    int i;

    if (buttons->defaults) free(buttons->defaults);
    if (buttons->names) free(buttons->names);

    memset(buttons, 0, sizeof(t_buttons));
    for (i = 0; i < node->n_attributes; i++) {
        if (strncmp(node->attributes[i].name, "default", 8) == 0) {
            store_node(&buttons->defaults, node->attributes[i].value);
        } else if (strncmp(node->attributes[i].name, "names", 6) == 0) {
            store_node(&buttons->names, node->attributes[i].value);
        }
    }
}

void read_roms(XMLNode *node, t_rom **roms, int *n_roms) {
    int i;

    if (strncmp(node->tag, "rom", 4) == 0) {
        (*n_roms)++;
        *roms = (t_rom *)realloc(*roms, sizeof(t_rom) * (*n_roms));
        read_rom(node, (*roms) + (*n_roms) - 1);
    } else {
        for (i = 0; i < node->n_children; i++) {
            read_roms(node->children[i], roms, n_roms);
        }
    }
}


void read_rbf(XMLNode *node, t_rbf *rbf) {
    if (rbf->name) free(rbf->name);
    if (rbf->alt_name) free(rbf->alt_name);
    memset(rbf, 0, sizeof(t_rbf));

    for (int i = 0; i < node->n_attributes; i++) {
        XMLAttribute *attr = &node->attributes[i];
        if (strcmp(attr->name, "alt") == 0) {
            store_node(&rbf->alt_name, attr->value);
        }
    }
    store_node(&rbf->name, node->text);
}

void read_nvram(XMLNode *node, t_nvram *nvram) {
    memset(nvram, 0, sizeof(t_nvram));
    for (int i = 0; i < node->n_attributes; i++) {
        XMLAttribute *attr = &node->attributes[i];
        if (strcmp(attr->name, "index") == 0) {
            nvram->index = strtol(attr->value, NULL, 0);
        } else if (strcmp(attr->name, "size") == 0) {
            nvram->size = strtol(attr->value, NULL, 0);
        }
    }
}

void read_root(XMLNode *root, t_mra *mra) {
    int i;

    for (i = 0; i < root->n_children; i++) {
        XMLNode *node = root->children[i];

        if (strncmp(node->tag, "name", 5) == 0) {
            store_node(&mra->name, node->text);
        } else if (strncmp(node->tag, "mratimestamp", 13) == 0) {
            store_node(&mra->mratimestamp, node->text);
        } else if (strncmp(node->tag, "mameversion", 12) == 0) {
            store_node(&mra->mameversion, node->text);
        } else if (strncmp(node->tag, "setname", 8) == 0) {
            store_node(&mra->setname, node->text);
        } else if (strncmp(node->tag, "year", 5) == 0) {
            store_node(&mra->year, node->text);
        } else if (strncmp(node->tag, "manufacturer", 13) == 0) {
            store_node(&mra->manufacturer, node->text);
        } else if (strncmp(node->tag, "rbf", 4) == 0) {
            read_rbf(node, &mra->rbf);
        } else if (strncmp(node->tag, "nvram", 5) == 0) {
            read_nvram(node, &mra->nvram);
        } else if (strncmp(node->tag, "category", 9) == 0) {
            if (node->text) string_list_add(&mra->categories, node->text);
        } else if (strncmp(node->tag, "switches", 9) == 0) {
            read_switches(node, &mra->switches);
        } else if (strncmp(node->tag, "buttons", 8) == 0) {
            read_buttons(node, &mra->buttons);
        }

    }
}

int mra_load(char *filename, t_mra *mra) {
    int res;
    XMLDoc *doc = &(mra->_xml_doc);
    XMLNode *root = NULL;

    memset(mra, 0, sizeof(t_mra));

    XMLDoc_init(doc);
    res = XMLDoc_parse_file(filename, doc);
    if (res != 1 || doc->i_root < 0) {
        printf("%s is not a valid xml file\n", filename);
        return -1;
    }
    root = doc->nodes[doc->i_root];
    if (strncmp(root->tag, "misterromdescription", 20) != 0) {
        printf("%s is not a valid MRA file\n", filename);
        return -1;
    }

    read_root(root, mra);
    read_roms(root, &mra->roms, &mra->n_roms);
    XMLDoc_free(doc);

    return 0;
}

void dump_part(t_part *part) {
    int i;

    if (part->is_group) {
        printf("**** group start\n");
        printf("    is_interleaved: %s\n", part->g.is_interleaved ? "true" : "false");
        printf("    width: %u\n", part->g.width);
        printf("    repeat: %d\n", part->g.repeat);
        for (i = 0; i < part->g.n_parts; i++) {
            printf("[%d]: \n", i);
            dump_part(part->g.parts + i);
        }
        printf("**** group end\n");
    } else {
        if (part->p.crc32) printf("    crc32: %08x\n", part->p.crc32);
        if (part->p.name) printf("    name: %s\n", part->p.name);
        if (part->p.zip) printf("    zip: %s\n", part->p.zip);
        if (part->p.pattern) {
            printf("    pattern: %s\n", part->p.pattern);
            printf("    _map_index: %d\n", part->p._map_index);
        }
        if (part->p.repeat) printf("    repeat: %u (0x%04x)\n", part->p.repeat, part->p.repeat);
        if (part->p.offset) printf("    offset: %u (0x%04x)\n", part->p.offset, part->p.offset);
        if (part->p.length) printf("    length: %u (0x%04x)\n", part->p.length, part->p.length);
        if (part->p.data_length) printf("    data_length: %lu\n", part->p.data_length);
    }
}

int mra_dump(t_mra *mra) {
    int i;

    if (mra->name) printf("name: %s\n", mra->name);
    if (mra->mratimestamp) printf("mratimestamp: %s\n", mra->mratimestamp);
    if (mra->mameversion) printf("mameversion: %s\n", mra->mameversion);
    if (mra->setname) printf("setname: %s\n", mra->setname);
    if (mra->year) printf("year: %s\n", mra->year);
    if (mra->manufacturer) printf("manufacturer: %s\n", mra->manufacturer);
    if (mra->rbf.name) printf("rbf name: %s\n", mra->rbf.name);
    if (mra->rbf.alt_name) printf("rbf alternative name: %s\n", mra->rbf.alt_name);
    for (i = 0; i < mra->categories.n_elements; i++) {
        printf("category[%d]: %s\n", i, mra->categories.elements[i]);
    }
    printf("switches: default=0x%llX, base=%d\n", mra->switches.defaults, mra->switches.base);
    printf("nb dips: %d\n", mra->switches.n_dips);
    for (i = 0; i < mra->switches.n_dips; i++) {
        printf("  dip[%d]: %s,%s,%s\n", i, mra->switches.dips[i].bits, mra->switches.dips[i].name, mra->switches.dips[i].ids);
    }
    printf("buttons: default=%s, names=%s\n", mra->buttons.defaults, mra->buttons.names);

    for (i = 0; i < mra->n_roms; i++) {
        int j;
        t_rom *rom = mra->roms + i;

        printf("\nrom[%d]:\n", i);
        printf("  index: %d\n", rom->index);
        if (rom->md5) printf("  md5: %s\n", rom->md5);
        if (rom->type.n_elements) printf("  ============\n");
        for (j = 0; j < rom->type.n_elements; j++) {
            printf("  type[%d]: %s\n", j, rom->type.elements[j]);
        }
        if (rom->zip.n_elements) printf("  ============\n");
        for (j = 0; j < rom->zip.n_elements; j++) {
            printf("  zip[%d]: %s\n", j, rom->zip.elements[j]);
        }
        if (rom->n_parts) printf("  ============\n");
        for (j = 0; j < rom->n_parts; j++) {
            printf("  part[%d]:\n", j);
            dump_part(rom->parts + j);
        }
        if (rom->n_patches) printf("  ============\n");
        for (j = 0; j < rom->n_patches; j++) {
            printf("  patch[%d]:\n", j);
            printf("    offset: %u (0x%08x)\n", rom->patches[j].offset, rom->patches[j].offset);
            printf("    data_length: %lu (0x%08lx)\n", rom->patches[j].data_length, rom->patches[j].data_length);
        }
    }
}

int mra_get_next_rom0(t_mra *mra, int start_index) {
    return mra_get_rom_by_index(mra, 0, start_index);
}

int mra_get_rom_by_index(t_mra *mra, int index, int start_pos) {
    int i;

    if (start_pos >= mra->n_roms) {
        return -1;
    }
    for (i = start_pos; i < mra->n_roms; i++) {
        if (mra->roms[i].index == index) {
            return i;
        }
    }
    return -1;  // ROM not found
}

void mra_free(t_mra *mra) {
    if (!mra) return;

    int i,j;

    if (mra->name) free(mra->name);
    if (mra->mratimestamp) free(mra->mratimestamp);
    if (mra->mameversion) free(mra->mameversion);
    if (mra->setname) free(mra->setname);
    if (mra->year) free(mra->year);
    if (mra->manufacturer) free(mra->manufacturer);
    if (mra->rbf.name) free(mra->rbf.name);
    if (mra->rbf.alt_name) free(mra->rbf.alt_name);
    if (mra->buttons.names) free(mra->buttons.names);
    if (mra->buttons.defaults) free(mra->buttons.defaults);

    for (i = 0; i < mra->n_roms; i++) {
        if (mra->roms[i].md5) free(mra->roms[i].md5);
        string_list_free(&mra->roms[i].zip);
        string_list_free(&mra->roms[i].type);
        if (mra->roms[i].parts) {
            free_parts(mra->roms[i].parts, mra->roms[i].n_parts);
        }
        if (mra->roms[i].patches) {
            for (j = 0; j < mra->roms[i].n_patches; j++) {
                if (mra->roms[i].patches[j].data) free(mra->roms[i].patches[j].data);
            }
            free(mra->roms[i].patches);
        }
    }
    if (mra->roms) free(mra->roms);

    free_switches(&mra->switches);

}
