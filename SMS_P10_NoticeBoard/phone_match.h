#ifndef PHONE_MATCH_H
#define PHONE_MATCH_H
#include <stddef.h>
#include <string.h>

/* Preserve the documented last-ten-digit policy, but reject short,
 * malformed, placeholder, and overlong telephone fields. Caller ID is
 * still not cryptographic authentication. */
static inline bool phoneMatches(const char *number, const char *admin)
{
    if (!number || !admin) return false;
    if (*number == '+') ++number;
    if (*admin == '+') ++admin;
    size_t n = strlen(number), a = strlen(admin);
    if (n < 10 || n > 15 || a < 10 || a > 15) return false;
    for (size_t i = 0; i < n; ++i)
        if (number[i] < '0' || number[i] > '9') return false;
    for (size_t i = 0; i < a; ++i)
        if (admin[i] < '0' || admin[i] > '9') return false;
    return memcmp(number + n - 10, admin + a - 10, 10) == 0;
}
#endif
