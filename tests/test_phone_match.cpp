#include "../SMS_P10_NoticeBoard/phone_match.h"
#include <stdio.h>
static int checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line %d\n", __LINE__); } } while (0)
int main()
{
    const char *admin = "+919876543210";
    CHECK(phoneMatches("9876543210", admin));
    CHECK(phoneMatches("919876543210", admin));
    CHECK(phoneMatches(admin, admin));
    CHECK(!phoneMatches("0", admin));
    CHECK(!phoneMatches("876543210", admin));
    CHECK(!phoneMatches("", admin));
    CHECK(!phoneMatches("+", admin));
    CHECK(!phoneMatches("+91XXXXXXXXXX", "+91XXXXXXXXXX"));
    CHECK(!phoneMatches("987654321x", admin));
    CHECK(!phoneMatches("1234567890123456", admin));
    CHECK(!phoneMatches("9876543211", admin));
    CHECK(!phoneMatches(admin, "3210"));
    CHECK(!phoneMatches(NULL, admin));
    CHECK(!phoneMatches(admin, NULL));
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
