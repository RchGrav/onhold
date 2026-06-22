/* Profile-hash compatibility test.
 *
 * The profile hash is a stable capability key: existing aliases, profiles and
 * sudoers grants are keyed to exactly the binary-path + argv framing in
 * sigmund_profile_hash_for_argv. These golden vectors fail loudly if that
 * framing ever changes (added context, reordered fields, different separators).
 * If you intend to change the hash, that is a breaking change to on-disk state;
 * update these vectors deliberately. */
#include "sigmund/config.h"
#include "sigmund/store.h"
#include <stdio.h>
#include <string.h>

struct vec {
    const char *binary_path;
    int argc;
    char **argv;
    const char *want;
};

int main(void) {
    char *a1[] = {"/bin/sleep", "60"};
    char *a2[] = {"/usr/local/bin/server", "--port", "9000", "--mode", "full"};
    char *a3[] = {"x"};
    struct vec vs[] = {
        {"/bin/sleep", 2, a1,
         "b6565705c9e7f5e645ad55c3c8cd748d859cc1b06b9438506014258629a38451"},
        {"/usr/local/bin/server", 5, a2,
         "3a2d7e028056ff419a35145021defabc3141a388874ca055c5fbaa360db9f939"},
        {"", 0, a3,
         "fdb741da4390ba2ed58240c21fcbd8943a50b271805d1169fa27727241fb24fc"},
    };
    size_t n = sizeof(vs) / sizeof(vs[0]);
    int fails = 0;
    for (size_t i = 0; i < n; i++) {
        char out[PROFILE_HASH_STR_LEN];
        sigmund_profile_hash_for_argv(vs[i].binary_path, vs[i].argc, vs[i].argv, out);
        if (strcmp(out, vs[i].want) != 0) {
            fprintf(stderr, "FAIL: binary_path='%s' argc=%d\n  got  %s\n  want %s\n",
                    vs[i].binary_path, vs[i].argc, out, vs[i].want);
            fails++;
        }
    }
    if (fails) {
        fprintf(stderr, "profile-hash vector: %d/%zu FAILED (capability key changed!)\n", fails, n);
        return 1;
    }
    printf("profile-hash vector: all %zu vectors match (capability key stable)\n", n);
    return 0;
}
