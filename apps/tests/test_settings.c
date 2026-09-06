#define APP_HOST_TEST
#include "../settings.c"
#include <assert.h>
#include <stdio.h>
static void test_cycling(void) {
    struct outrun_settings s = {1, 0x22e4ff, 50, 5};
    assert(settings_change(&s, 0) && s.scale == 2);
    assert(settings_change(&s, 0) && s.scale == 1);
    assert(settings_change(&s, 1) && s.accent == 0x3df5c4);
    assert(settings_change(&s, 2) && s.repeat_delay == 75);
    assert(settings_change(&s, 3) && s.repeat_period == 10);
    for (int i = 0; i < 20; ++i) {
        settings_change(&s, 2); settings_change(&s, 3);
        assert(s.repeat_delay >= 25 && s.repeat_delay <= 100);
        assert(s.repeat_period >= 2 && s.repeat_period <= 50);
    }
    assert(!settings_change(&s, 99));
}
/* Instant apply: a row click pushes the new value to the kernel in the same
 * step. If the kernel refuses, the local copy must roll back to what the
 * kernel still holds, so the screen never shows a setting that is not live. */
static int pushes, refuse;
static struct outrun_settings kernel_copy;
static long long fake_push(const struct outrun_settings *s) {
    ++pushes;
    if (refuse) return -1;
    kernel_copy = *s; return 0;
}
static void test_instant_apply_pushes_on_change(void) {
    struct outrun_settings s = {1, 0x22e4ff, 50, 5};
    kernel_copy = s; pushes = 0; refuse = 0;
    assert(settings_apply_item(&s, 0, fake_push) == 1);
    assert(pushes == 1 && s.scale == 2 && kernel_copy.scale == 2);
    assert(settings_apply_item(&s, 1, fake_push) == 1);
    assert(pushes == 2 && kernel_copy.accent == 0x3df5c4);
}
static void test_refused_change_rolls_back(void) {
    struct outrun_settings s = {1, 0x22e4ff, 50, 5};
    kernel_copy = s; pushes = 0; refuse = 1;
    assert(settings_apply_item(&s, 0, fake_push) == -1);
    assert(pushes == 1);
    assert(s.scale == 1);                 /* rolled back, not left at 2 */
    assert(kernel_copy.scale == 1);       /* and the kernel was never changed */
    assert(settings_apply_item(&s, 2, fake_push) == -1);
    assert(s.repeat_delay == 50);
}
static void test_invalid_item_pushes_nothing(void) {
    struct outrun_settings s = {1, 0x22e4ff, 50, 5};
    pushes = 0; refuse = 0;
    assert(settings_apply_item(&s, 99, fake_push) == 0);
    assert(pushes == 0);
}
int main(void) {
    test_cycling();
    test_instant_apply_pushes_on_change();
    test_refused_change_rolls_back();
    test_invalid_item_pushes_nothing();
    puts("settings: scale/accent cycling, bounded repeat controls and instant apply with rollback PASS");
}
