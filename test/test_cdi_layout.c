// Checks that every field the CDI describes sits at the address and size
// config_layout.h gives it, so the XML and the struct can't drift apart.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app/cdi_data.h"
#include "app/config_layout.h"

typedef enum { KIND_INT, KIND_STRING, KIND_EVENTID } kind_t;

typedef struct {
    unsigned offset;
    unsigned size;
    kind_t kind;
} field_t;

static field_t found[256];
static int nfound;

static unsigned attr(const char *tag_start, const char *name)
{
    const char *end = strchr(tag_start, '>');
    const char *a = strstr(tag_start, name);
    if (!a || a > end)
        return 0;
    return (unsigned)strtoul(a + strlen(name) + 2, NULL, 10);  // name='N'
}

// Walk the CDI: segments set the address, groups repeat their contents,
// int/string/eventid elements take space.
static void walk(void)
{
    unsigned pos = 0;
    struct { unsigned start; unsigned reps; } groups[4];
    int depth = 0;

    for (const char *p = cdi_xml; (p = strchr(p, '<')) != NULL; p++) {
        if (strncmp(p, "<segment", 8) == 0) {
            pos = attr(p, "origin");
        } else if (strncmp(p, "<group", 6) == 0) {
            unsigned reps = attr(p, "replication");
            groups[depth].start = pos;
            groups[depth].reps = reps ? reps : 1;
            depth++;
        } else if (strncmp(p, "</group>", 8) == 0) {
            depth--;
            unsigned stride = pos - groups[depth].start;
            // Record the remaining replicas' fields by copying the first
            int first = nfound;
            while (first > 0 && found[first - 1].offset >= groups[depth].start)
                first--;
            int n = nfound - first;
            for (unsigned r = 1; r < groups[depth].reps; r++)
                for (int k = 0; k < n; k++) {
                    found[nfound] = found[first + k];
                    found[nfound++].offset += r * stride;
                }
            pos = groups[depth].start + stride * groups[depth].reps;
        } else if (strncmp(p, "<int", 4) == 0 && (p[4] == ' ' || p[4] == '>')) {
            unsigned size = attr(p, "size");
            found[nfound++] = (field_t){ pos, size ? size : 1, KIND_INT };
            pos += size ? size : 1;
        } else if (strncmp(p, "<string", 7) == 0) {
            unsigned size = attr(p, "size");
            found[nfound++] = (field_t){ pos, size, KIND_STRING };
            pos += size;
        } else if (strncmp(p, "<eventid", 8) == 0) {
            found[nfound++] = (field_t){ pos, 8, KIND_EVENTID };
            pos += 8;
        }
    }
}

#define F(member, kind) { offsetof(config_layout_t, member), \
                          sizeof(((config_layout_t *)0)->member), kind }
#define S(member, kind) { offsetof(config_servo_t, member), \
                          sizeof(((config_servo_t *)0)->member), kind }

int main(void)
{
    static const field_t node_fields[] = {
        F(user_name, KIND_STRING),
        F(user_description, KIND_STRING),
        F(identify_enabled, KIND_INT),
        F(identify_timeout_minutes, KIND_INT),
    };
    static const field_t servo_fields[] = {
        S(throw_event, KIND_EVENTID),
        S(close_event, KIND_EVENTID),
        S(thrown_feedback, KIND_EVENTID),
        S(closed_feedback, KIND_EVENTID),
        S(center_event, KIND_EVENTID),
        S(thrown_position, KIND_INT),
        S(closed_position, KIND_INT),
        S(center_position, KIND_INT),
        S(throw_time, KIND_INT),
        S(hold_time, KIND_INT),
        S(enabled, KIND_INT),
        S(default_state, KIND_INT),
        S(reversed, KIND_INT),
        S(frog_output, KIND_INT),
        S(fascia_input, KIND_INT),
        S(description, KIND_STRING),
        S(reserved, KIND_STRING),
    };

    field_t want[256];
    int nwant = 0;
    for (size_t i = 0; i < sizeof(node_fields) / sizeof(node_fields[0]); i++)
        want[nwant++] = node_fields[i];
    for (int s = 0; s < CONFIG_SERVOS; s++)
        for (size_t i = 0; i < sizeof(servo_fields) / sizeof(servo_fields[0]); i++) {
            want[nwant] = servo_fields[i];
            want[nwant++].offset += offsetof(config_layout_t, servos) + s * sizeof(config_servo_t);
        }

    walk();

    int failures = 0;
    if (nfound != nwant) {
        printf("FAIL: CDI has %d fields, config_layout.h has %d\n", nfound, nwant);
        failures++;
    }
    for (int i = 0; i < nfound && i < nwant; i++) {
        if (found[i].offset != want[i].offset || found[i].size != want[i].size ||
            found[i].kind != want[i].kind) {
            printf("FAIL: field %d: CDI offset %u size %u, struct offset %u size %u\n", i,
                   found[i].offset, found[i].size, want[i].offset, want[i].size);
            failures++;
        }
    }
    unsigned end = found[nfound - 1].offset + found[nfound - 1].size;
    if (end != sizeof(config_layout_t)) {
        printf("FAIL: CDI ends at %u, struct is %u bytes\n", end, (unsigned)sizeof(config_layout_t));
        failures++;
    }
    if (cdi_xml[cdi_xml_len - 1] != '\0') {
        printf("FAIL: CDI space must end with a NUL\n");
        failures++;
    }

    printf("cdi layout: %d fields checked, %d failed\n", nfound, failures);
    return failures != 0;
}
