/* Minimal test harness: CHECK aborts the current test function. */
#ifndef CHECK_H
#define CHECK_H

#include <stdio.h>

extern int check_failures;
extern const char *check_current;

#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s\n  %s:%d: CHECK(%s)\n", check_current, __FILE__, __LINE__, #cond); \
    check_failures++; return; } } while (0)

#define CHECK_EQ(a, b) do { long long a_ = (long long)(a), b_ = (long long)(b); if (a_ != b_) { \
    printf("FAIL %s\n  %s:%d: CHECK_EQ(%s, %s): %lld != %lld\n", check_current, \
           __FILE__, __LINE__, #a, #b, a_, b_); \
    check_failures++; return; } } while (0)

#define RUN(fn) do { int before_ = check_failures; check_current = #fn; fn(); \
    if (check_failures == before_) check_passed++; } while (0)

extern int check_passed;

#endif
