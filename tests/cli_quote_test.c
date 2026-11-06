/* Windows command-line quoting used by cli_run: each case must survive
 * CommandLineToArgvW / the MSVC CRT parser unchanged. Expected strings are
 * written by hand from the documented rules, not produced by the function. */
#include "cli_os.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void expect(const char *arg, const char *quoted) {
    char out[256];
    size_t n = cli_quote_windows_arg(arg, out, sizeof out);
    if (n == (size_t)-1 || strcmp(out, quoted)) {
        fprintf(stderr, "quote(%s) = %s, expected %s\n", arg, n == (size_t)-1 ? "<overflow>" : out, quoted);
        assert(0);
    }
    assert(n == strlen(quoted));
}

int main(void) {
    expect("plain", "plain");
    expect("", "\"\"");
    expect("D:\\New folder (5)\\x.copper", "\"D:\\New folder (5)\\x.copper\"");
    expect("a\"b", "\"a\\\"b\"");
    expect("dir with space\\", "\"dir with space\\\\\"");   /* trailing backslash doubled before closing quote */
    expect("a\\\\\"b", "\"a\\\\\\\\\\\"b\"");               /* two backslashes then quote: 5 backslashes */
    expect("a\\b c", "\"a\\b c\"");                         /* backslashes not before a quote stay single */
    expect("%PATH%&echo;$(x)", "%PATH%&echo;$(x)");          /* no shell is involved, so no quoting needed */
    expect("tab\there", "\"tab\there\"");

    char tiny[4];
    assert(cli_quote_windows_arg("abcdef", tiny, sizeof tiny) == (size_t)-1);
    assert(cli_quote_windows_arg("a b", tiny, sizeof tiny) == (size_t)-1);
    puts("cli_quote: PASS");
    return 0;
}
