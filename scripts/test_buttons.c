#include "../source/ff7_buttons_core.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void colored_and_uncolored(void) {
    uint8_t s[] = {
        0x11,
        FF7_COLOR, 0xD8, FF7_LPAREN, 'B' - 0x20, FF7_RPAREN,
        0x12,
        FF7_LPAREN, 'A' - 0x20, FF7_RPAREN,
        0x13,
        FF7_LPAREN, 'X' - 0x20, FF7_RPAREN,
        0x14,
        FF7_LPAREN, 'Y' - 0x20, FF7_RPAREN,
        0x16,
        FF7_COLOR, 0xD8, 'B' - 0x20, FF7_COLOR, 0xD9,
        0x17,
        FF7_BUTTON_CIRCLE, FF7_BUTTON_TRIANGLE, FF7_BUTTON_SQUARE, FF7_BUTTON_CROSS,
        0x15, FF7_END
    };
    const uint8_t want[] = {
        0x11, FF7_COLOR, 0xD3, PSB_FIRST_CHAR + PSB_CROSS,
        0x12, FF7_COLOR, 0xD4, PSB_FIRST_CHAR + PSB_CIRCLE,
        0x13, FF7_COLOR, 0xD5, PSB_FIRST_CHAR + PSB_SQUARE,
        0x14, FF7_COLOR, 0xD6, PSB_FIRST_CHAR + PSB_TRIANGLE,
        0x16, FF7_COLOR, 0xD3, PSB_FIRST_CHAR + PSB_CROSS, FF7_COLOR, 0xD9,
        0x17,
        PSB_FIRST_CHAR + PSB_CROSS, PSB_FIRST_CHAR + PSB_TRIANGLE,
        PSB_FIRST_CHAR + PSB_SQUARE, PSB_FIRST_CHAR + PSB_CIRCLE,
        0x15, FF7_END, FF7_END, FF7_END
    };
    assert(psb_replace(s, sizeof(s)) == 9);
    assert(memcmp(s, want, sizeof(s)) == 0);
}

static void leaves_other_text_alone(void) {
    uint8_t s[] = {
        FF7_LPAREN, 'C' - 0x20, FF7_RPAREN,
        FF7_COLOR, 0xD6, FF7_LPAREN, 'Q' - 0x20, FF7_RPAREN,
        'A' - 0x20, FF7_END
    };
    uint8_t before[sizeof(s)];
    memcpy(before, s, sizeof(s));
    assert(psb_replace(s, sizeof(s)) == 0);
    assert(memcmp(s, before, sizeof(s)) == 0);
}

static void requires_a_terminator(void) {
    uint8_t s[] = { FF7_LPAREN, 'A' - 0x20, FF7_RPAREN };
    assert(psb_replace(s, sizeof(s)) == -1);
}

static void stock_android_labels(void) {
    uint8_t s[] = {
        FF7_COLOR, 0xD5, FF7_LBRACKET, 'A' - 0x20, FF7_RBRACKET,
        FF7_COLOR, 0xD9,
        FF7_LBRACKET, 'B' - 0x20, FF7_RBRACKET,
        FF7_LBRACKET, 'X' - 0x20, FF7_RBRACKET,
        FF7_LBRACKET, 'Y' - 0x20, FF7_RBRACKET, FF7_END
    };
    const uint8_t want[] = {
        FF7_COLOR, 0xD5, FF7_COLOR, 0xD3, PSB_FIRST_CHAR + PSB_CROSS,
        FF7_COLOR, 0xD9,
        FF7_COLOR, 0xD4, PSB_FIRST_CHAR + PSB_CIRCLE,
        FF7_COLOR, 0xD5, PSB_FIRST_CHAR + PSB_SQUARE,
        FF7_COLOR, 0xD6, PSB_FIRST_CHAR + PSB_TRIANGLE, FF7_END
    };
    assert(psb_replace(s, sizeof(s)) == 4);
    assert(memcmp(s, want, sizeof(s)) == 0);
    assert(psb_replace(s, sizeof(s)) == 0);
    uint8_t other[] = { FF7_LBRACKET, 'C' - 0x20, FF7_RBRACKET,
                        FF7_LBRACKET, 'A' - 0x20, FF7_END };
    uint8_t before[sizeof(other)];
    memcpy(before, other, sizeof(other));
    assert(psb_replace(other, sizeof(other)) == 0);
    assert(memcmp(before, other, sizeof(other)) == 0);
}

int main(void) {
    stock_android_labels();
    colored_and_uncolored();
    leaves_other_text_alone();
    requires_a_terminator();
    puts("ff7_buttons_core: colored and uncolored labels passed");
    return 0;
}
