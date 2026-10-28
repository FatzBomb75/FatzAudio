/* Runs a fuzz target over saved inputs (files, or directories of files) once each, so
 * the inputs that once crashed a reader stay in the test suite. Needs no fuzzer. */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#  include <dirent.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

static int g_files;

static void run_file(const char* path)
{
    FILE* f = fopen(path, "rb");
    long n;
    uint8_t* p;
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    p = (uint8_t*)malloc((size_t)n + 1);
    if (!p || fread(p, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "cannot read %s\n", path); exit(1); }
    fclose(f);
    LLVMFuzzerTestOneInput(p, (size_t)n);
    free(p);
    g_files++;
}

static void run_path(const char* path)
{
    struct stat st;
    if (stat(path, &st) != 0) return;
#if !defined(_WIN32)
    if (S_ISDIR(st.st_mode)) {
        DIR* d = opendir(path);
        struct dirent* e;
        while (d && (e = readdir(d)) != NULL) {
            char sub[1024];
            if (e->d_name[0] == '.') continue;
            snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name);
            run_path(sub);
        }
        if (d) closedir(d);
        return;
    }
#endif
#if defined(_WIN32)
    if (st.st_mode & _S_IFREG) run_file(path);
#else
    if (S_ISREG(st.st_mode)) run_file(path);
#endif
}

int main(int argc, char** argv)
{
    int i;
    for (i = 1; i < argc; i++) run_path(argv[i]);
    printf("%d saved inputs ran\n", g_files);
    return 0;
}
