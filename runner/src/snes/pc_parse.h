/*
 * pc_parse.h — strict 24-bit PC parser for SNESRECOMP_COUNT_PC.
 *
 * WHY THIS EXISTS
 *
 * `interp816.c:325` parsed the SNESRECOMP_COUNT_PC knob with
 * `strtoul(e, NULL, 0)`. Base 0 auto-detects: a leading `0` means octal, and
 * parsing stops at the first digit that is not octal. Measured on the Deck:
 *
 *     strtoul("009311",NULL,0)  = 0        = 0x0        <-- what every ticket got
 *     strtoul("0x009311",NULL,0)= 37649    = 0x9311
 *     strtoul("00804D",NULL,0)  = 0        = 0x0
 *     strtoul("0x00804D",NULL,0)= 32845    = 0x804D
 *     strtoul("038026",NULL,0)  = 3        = 0x3
 *     strtoul("0x038026",NULL,0)= 229414   = 0x38026
 *
 * So every COUNT_PC measurement taken without the `0x` prefix counted
 * executions of `$000000` (or `$000003`, etc.) — a confident count of the
 * wrong address, printed as `[count] pc watched: 0 executions over N frames`.
 * A silent wrong answer is worse than no answer, so this parser fails loudly
 * on anything it cannot parse unambiguously.
 *
 * ACCEPTED FORMS
 *
 *   `0x` + exactly six hex digits, case-insensitive, e.g. `0x009311`.
 *   `0X` is also accepted (strtoul accepts it; we re-validate the length so
 *   the prefix is consumed and the digit count is exact).
 *
 * REJECTED, AND WHY
 *
 *   bare `9311`    — ambiguous: decimal 9311 = 0x245F, not the intended 0x9311.
 *   bare `009311`  — strtoul reads this as octal → 0. The leading zero is the
 *                     octal prefix, not a zero-padded hex digit.
 *   `0x9311`       — too few digits; could be 0x009311 or 0x931100.
 *   `0x00931100`   — too many digits; a 24-bit PC has three bytes.
 *   `0x00931G`     — non-hex character.
 *   `0x00931`      — trailing whitespace, or anything after the six digits.
 *   empty string    — nothing to parse.
 *   `0x`           — prefix with no digits.
 *
 * The instrument must never again report a confident count of the wrong
 * address. Failing loudly is strictly better than parsing: a run that aborts
 * on a bad knob is a run whose numbers are all void, which is discoverable;
 * a run that silently watches the wrong address is a run whose numbers look
 * clean and mean nothing.
 *
 * Vendored from the snesrecomp runner tree. See THIRD_PARTY_ATTRIBUTION.md.
 */
#ifndef PC_PARSE_H
#define PC_PARSE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Parse a 24-bit PC from a SNESRECOMP_COUNT_PC value.
 *
 * Returns true and writes the 24-bit value to *out only if the entire string
 * is a valid `0x`-prefixed six-digit hex literal. On any malformed input
 * returns false and leaves *out untouched.
 *
 * The caller is expected to print the rejected value and abort, so a bad knob
 * can never silently become `$000000`.
 */
static inline bool pc_parse_count_pc(const char *s, uint32_t *out)
{
    static const char hexdigits[] = "0123456789abcdefABCDEF";
    size_t i, n;

    if (s == NULL) return false;
    n = 0;
    while (s[n] != '\0') n++;
    if (n == 0) return false;

    /* Exact length: "0x" (2) + 6 hex digits = 8. No leading/trailing space,
     * no sign, no suffix. */
    if (n != 8) return false;
    if (s[0] != '0' || (s[1] != 'x' && s[1] != 'X')) return false;

    {
        uint32_t v = 0;
        for (i = 2; i < 8; i++) {
            const char *h = hexdigits;
            int digit = -1;
            while (*h) {
                if (*h == s[i]) { digit = (int)(h - hexdigits); break; }
                h++;
            }
            if (digit < 0) return false;
            if (digit >= 16) digit -= 6;    /* fold A-F (idx 16..21) to 10..15 */
            v = (v << 4) | (uint32_t)(digit & 0xF);
        }
        *out = v;
        return true;
    }
}

#endif /* PC_PARSE_H */