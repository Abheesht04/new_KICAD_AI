/* A stand-in for kicad-cli, for tests/kicad_cli_test.c. It records the exact argv it received and behaves as
 * FAKE_KICAD_MODE says: (unset) succeed, "fail" exit 3, "violations" DRC exit 5, "nooutput" exit 0 writing nothing,
 * "sleep" hang for 60 s, "flood" write 4 MiB to stdout and to stderr. FAKE_KICAD_VERSION overrides "9.0.6". */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

static const char *mode(void) { const char *m = getenv("FAKE_KICAD_MODE"); return m ? m : ""; }

static int put(const char *path, const char *text, int argc, char **argv)
{
    size_t n = strlen(text) + 1;
    for (int i = 1; argv && i < argc; i++) n += strlen(argv[i]) + 1;
    char *buf = malloc(n), *p = buf;
    if (!buf) return 1;
    p += sprintf(p, "%s", text);
    for (int i = 1; argv && i < argc; i++) p += sprintf(p, "%s\n", argv[i]);
#ifdef _WIN32
    wchar_t wp[4096];
    DWORD wrote = 0;
    HANDLE h = MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, 4096)
        ? CreateFileW(wp, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL) : INVALID_HANDLE_VALUE;
    int bad = h == INVALID_HANDLE_VALUE || !WriteFile(h, buf, (DWORD)(p - buf), &wrote, NULL);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
#else
    FILE *f = fopen(path, "wb");
    int bad = !f || fwrite(buf, 1, (size_t)(p - buf), f) != (size_t)(p - buf);
    if (f) fclose(f);
#endif
    free(buf);
    if (bad) fprintf(stderr, "fake: cannot write %s\n", path);
    return bad;
}

static char *in_dir(const char *dir, const char *name)
{
    static char buf[8192];
    snprintf(buf, sizeof buf, "%s%s", dir, name);   /* dir arrives with its trailing separator, as kicad-cli wants */
    return buf;
}

#ifdef _WIN32
static char **utf8_argv(int *argc)
{
    int n;
    wchar_t **w = CommandLineToArgvW(GetCommandLineW(), &n);
    char **out = calloc((size_t)n + 1, sizeof *out);
    for (int i = 0; i < n; i++) {
        int len = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, NULL, 0, NULL, NULL);
        out[i] = malloc((size_t)len);
        WideCharToMultiByte(CP_UTF8, 0, w[i], -1, out[i], len, NULL, NULL);
    }
    *argc = n;
    return out;
}
#endif

int main(int argc, char **argv)
{
#ifdef _WIN32
    argv = utf8_argv(&argc);
#endif
    if (!strcmp(mode(), "sleep")) {
#ifdef _WIN32
        Sleep(60000);
#else
        struct timespec t = {60, 0};
        nanosleep(&t, NULL);
#endif
    }
    if (argc == 2 && !strcmp(argv[1], "version")) {
        const char *v = getenv("FAKE_KICAD_VERSION");
        printf("%s\n", v ? v : "9.0.6");
        return 0;
    }
    if (!strcmp(mode(), "flood")) {
        for (int i = 0; i < 4 * 1024; i++) { for (int k = 0; k < 1023; k++) { putchar('o'); fputc('e', stderr); } putchar('\n'); fputc('\n', stderr); }
    }
    if (!strcmp(mode(), "fail")) { fputs("boom: the board is broken\n", stderr); return 3; }
    const char *out = NULL;
    for (int i = 1; i + 1 < argc; i++) if (!strcmp(argv[i], "-o")) out = argv[i + 1];
    if (!out) { fputs("fake: no -o\n", stderr); return 2; }
    printf("fake kicad-cli: %s %s\n", argv[1], argc > 2 ? argv[2] : "");
    if (!strcmp(mode(), "nooutput")) return 0;
    if (argc > 2 && !strcmp(argv[1], "pcb") && !strcmp(argv[2], "drc")) {
        if (put(out, "{\"source\": \"fake\", \"violations\": []}\n", argc, argv)) return 1;
        return !strcmp(mode(), "violations") ? 5 : 0;
    }
    if (argc > 3 && !strcmp(argv[3], "gerbers"))
        return put(in_dir(out, "design-F_Cu.gtl"), "G04 fake*\nM02*\n", 0, NULL) || put(in_dir(out, "args.txt"), "", argc, argv);
    if (argc > 3 && !strcmp(argv[3], "drill"))
        return put(in_dir(out, "design.drl"),
                   "M48\n; FORMAT={-:-/ absolute / metric / decimal}\nFMAT,2\nMETRIC\nT1C1.000\n%\nG90\nG05\nT1\nX8.0Y-12.0\nX8.0Y-14.54\nM30\n",
                   0, NULL) || put(in_dir(out, "args.txt"), "", argc, argv);
    if (argc > 3 && !strcmp(argv[3], "pos"))
        return put(out, "Ref,Val,Package,PosX,PosY,Rot,Side\n\"R1\",\"1k\",\"R_0603\",14,-8,0,top\n", argc, argv);
    fputs("fake: unknown command\n", stderr);
    return 2;
}
