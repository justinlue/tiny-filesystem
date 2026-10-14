#include "check.h"

int check_failures;
int check_passed;
const char *check_current;

void sim_tests(void);
void api_tests(void);

int main(void)
{
    sim_tests();
    api_tests();
    printf("%d passed, %d failed\n", check_passed, check_failures);
    return check_failures != 0;
}
