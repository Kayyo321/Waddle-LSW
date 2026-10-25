/** @file keyboard_scan.c @brief Golden bidirectional evdev translation checks. */
#include "av_input.h"
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MaxEvdevKey = 767, CsvFieldCount = 8 };
typedef struct expected_key_t {
    int present;
    av_key_translation_t translation;
} expected_key_t;
static expected_key_t expected_keys[MaxEvdevKey + 1];

static void split_csv(char *line, char *fields[CsvFieldCount]) {
    fields[0] = line;
    for (unsigned index = 1; index < CsvFieldCount; ++index) {
        char *separator = strchr(fields[index - 1], ',');
        assert(separator != NULL);
        *separator = '\0';
        fields[index] = separator + 1;
    }
    char *newline = strpbrk(fields[CsvFieldCount - 1], "\r\n");
    if (newline) *newline = '\0';
    assert(strchr(fields[CsvFieldCount - 1], ',') == NULL);
}

static unsigned parse_decimal(const char *text) {
    char *end = NULL;
    errno = 0;
    const unsigned long value = strtoul(text, &end, 10);
    assert(errno == 0 && end != text && *end == '\0' && value <= UINT32_MAX);
    return (unsigned)value;
}

static unsigned parse_hex(const char *text) {
    char *end = NULL;
    errno = 0;
    const unsigned long value = strtoul(text, &end, 16);
    assert(errno == 0 && end != text && *end == '\0' && value <= UINT16_MAX);
    return (unsigned)value;
}

static av_key_translation_t parse_translation(char *fields[CsvFieldCount]) {
    av_key_translation_t result = {0};
    if (strcmp(fields[2], "scan") == 0) {
        result.kind = AvKeyTranslationScanCode;
        result.scan_code = (uint8_t)parse_hex(fields[3]);
        result.extended = (uint8_t)parse_decimal(fields[4]);
        assert(result.scan_code != 0 && result.extended <= 1);
        assert(fields[5][0] == '\0' && fields[6][0] == '\0' && fields[7][0] == '\0');
    } else if (strcmp(fields[2], "virtual_key") == 0) {
        result.kind = AvKeyTranslationVirtualKey;
        result.virtual_key = (uint16_t)parse_hex(fields[5]);
        assert(result.virtual_key != 0 && fields[3][0] == '\0' && fields[4][0] == '\0' &&
               fields[6][0] == '\0' && fields[7][0] == '\0');
    } else if (strcmp(fields[2], "pause") == 0) {
        result.kind = AvKeyTranslationPause;
        assert(strcmp(fields[6], "E1 1D 45") == 0 && strcmp(fields[7], "E1 9D C5") == 0);
        result.pause_make[0] = 0xe1; result.pause_make[1] = 0x1d; result.pause_make[2] = 0x45;
        result.pause_break[0] = 0xe1; result.pause_break[1] = 0x9d; result.pause_break[2] = 0xc5;
        assert(fields[3][0] == '\0' && fields[4][0] == '\0' && fields[5][0] == '\0');
    } else if (strcmp(fields[2], "suppressed") == 0) {
        result.kind = AvKeyTranslationSuppressed;
        for (unsigned index = 3; index < CsvFieldCount; ++index) assert(fields[index][0] == '\0');
    } else {
        assert(!"unknown golden translation kind");
    }
    return result;
}

static int translation_equal(av_key_translation_t left, av_key_translation_t right) {
    return left.kind == right.kind && left.scan_code == right.scan_code &&
        left.extended == right.extended && left.virtual_key == right.virtual_key &&
        memcmp(left.pause_make, right.pause_make, sizeof(left.pause_make)) == 0 &&
        memcmp(left.pause_break, right.pause_break, sizeof(left.pause_break)) == 0;
}

static void load_golden(const char *path) {
    FILE *file = fopen(path, "r");
    assert(file != NULL);
    char line[512];
    assert(fgets(line, sizeof(line), file) != NULL);
    char *header[CsvFieldCount];
    split_csv(line, header);
    static const char *const ExpectedHeader[CsvFieldCount] = {
        "evdev", "name", "kind", "scan_code", "extended", "virtual_key", "pause_make", "pause_break",
    };
    for (unsigned index = 0; index < CsvFieldCount; ++index)
        assert(strcmp(header[index], ExpectedHeader[index]) == 0);

    unsigned rows = 0;
    while (fgets(line, sizeof(line), file) != NULL) {
        char *fields[CsvFieldCount];
        split_csv(line, fields);
        const unsigned key = parse_decimal(fields[0]);
        assert(key >= 1 && key <= MaxEvdevKey);
        assert(fields[1][0] != '\0' && !expected_keys[key].present);
        expected_keys[key].present = 1;
        expected_keys[key].translation = parse_translation(fields);
        ++rows;
    }
    assert(fclose(file) == 0 && rows >= 140);
}

static void test_forward_table(void) {
    for (unsigned key = 1; key <= MaxEvdevKey; ++key) {
        const av_key_translation_t actual = av_input_translate_key(key);
        if (!expected_keys[key].present) {
            const av_key_translation_t unsupported = {.kind = AvKeyTranslationUnsupported};
            assert(translation_equal(actual, unsupported));
            assert(av_input_scan_code(key) == 0);
            continue;
        }
        const av_key_translation_t expected = expected_keys[key].translation;
        assert(translation_equal(actual, expected));
        if (key >= AvInputKeys) {
            assert(av_input_scan_code(key) == 0);
        } else if (expected.kind == AvKeyTranslationScanCode) {
            const uint16_t packed = (uint16_t)expected.scan_code | (expected.extended ? 0x100 : 0);
            assert(av_input_scan_code(key) == packed);
        } else {
            assert(av_input_scan_code(key) == 0);
        }
    }
    const av_key_translation_t unsupported = {.kind = AvKeyTranslationUnsupported};
    assert(translation_equal(av_input_translate_key(0), unsupported));
    assert(translation_equal(av_input_translate_key(768), unsupported));
    assert(translation_equal(av_input_translate_key(UINT32_MAX), unsupported));
    assert(av_input_scan_code(0) == 0 && av_input_scan_code(768) == 0 && av_input_scan_code(UINT32_MAX) == 0);
    const av_key_translation_t hangul = av_input_translate_key(122);
    const av_key_translation_t hanja = av_input_translate_key(123);
    assert(hangul.kind == AvKeyTranslationVirtualKey && hangul.virtual_key == 0x15); /* VK_HANGUL */
    assert(hanja.kind == AvKeyTranslationVirtualKey && hanja.virtual_key == 0x19); /* VK_HANJA */
}

static void test_inverse_equivalence_classes(void) {
    for (unsigned left = 1; left <= MaxEvdevKey; ++left) {
        if (!expected_keys[left].present) continue;
        for (unsigned right = 1; right <= MaxEvdevKey; ++right) {
            const int expected_same = expected_keys[right].present &&
                translation_equal(expected_keys[left].translation, expected_keys[right].translation);
            const int actual_same = translation_equal(av_input_translate_key(left), av_input_translate_key(right));
            assert(actual_same == expected_same);
        }
    }
}

static unsigned emitted;
static int test_target(void *context, uint64_t window_id, uint64_t incarnation, int activate) {
    (void)context;
    return window_id == 7 && incarnation == 99 && activate == 0 ? 0 : -1;
}
static int test_emit(void *context, const av_message_t *event) {
    (void)context; (void)event;
    ++emitted;
    return 0;
}

static void test_legacy_admitted_bounds(void) {
    const av_input_ops_t ops = {.target = test_target, .emit = test_emit};
    av_input_state_t state = {.window_id = 7, .incarnation = 99};
    av_message_t event = {.type = MsgInputKey, .window_id = 7, .sequence = 99, .width = 127, .flags = 1};
    assert(av_input_apply_admitted(&state, &event, &ops, NULL) == 0);
    assert(state.keys[127] && emitted == 1);
    event.width = 128;
    assert(av_input_apply_admitted(&state, &event, &ops, NULL) == -1 && emitted == 1);
    event.width = 0;
    assert(av_input_apply_admitted(&state, &event, &ops, NULL) == -1 && emitted == 1);
    event.type = MsgInputButton; event.width = 6;
    assert(av_input_apply_admitted(&state, &event, &ops, NULL) == -1 && emitted == 1);
    event.width = 4; /* XBUTTON1 is a valid typed pointer button (IMPL_DESC 3.6.4). */
    assert(av_input_apply_admitted(&state, &event, &ops, NULL) == 0 && emitted == 2 && state.buttons[4] == 1);
    event.width = 0;
    assert(av_input_apply_admitted(&state, &event, &ops, NULL) == -1 && emitted == 2);
    assert(av_input_apply_admitted(NULL, &event, &ops, NULL) == -1);
    assert(av_input_apply_admitted(&state, NULL, &ops, NULL) == -1);
    assert(av_input_apply_admitted(&state, &event, NULL, NULL) == -1);
}

int av_input_legacy_test_main(void);
int main(void) {
    load_golden("tests/interop/scancodes.csv");
    test_forward_table();
    test_inverse_equivalence_classes();
    test_legacy_admitted_bounds();
    assert(av_input_legacy_test_main() == 0);
    puts("AV keyboard golden table, inverse classes, legacy bounds, and state tests passed");
    return 0;
}
