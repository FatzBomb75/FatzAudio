/* Preloaded into linkerconfig by run.sh: a chroot has no property service, so the
 * system properties it reads come from the image's build.prop files instead. */
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

struct prop_info { char name[128]; char value[92]; };

static struct prop_info g_props[4096];
static int g_count = -1;

/* No stdio or malloc: libc asks for properties while it sets up malloc. */
static void load_file(const char* path)
{
    static char buf[256 * 1024];
    ssize_t got, n = 0;
    char* line;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    while (n < (ssize_t)sizeof(buf) - 1 && (got = read(fd, buf + n, sizeof(buf) - 1 - (size_t)n)) > 0) n += got;
    close(fd);
    buf[n] = 0;
    for (line = buf; *line && g_count < (int)(sizeof(g_props) / sizeof(g_props[0]));) {
        size_t len = strcspn(line, "\n");
        char* eq = memchr(line, '=', len);
        if (line[0] != '#' && eq && (size_t)(eq - line) < sizeof(g_props[0].name) &&
            len - (size_t)(eq - line) - 1 < sizeof(g_props[0].value)) {
            size_t vlen = len - (size_t)(eq - line) - 1;
            memcpy(g_props[g_count].name, line, (size_t)(eq - line));
            g_props[g_count].name[eq - line] = 0;
            memcpy(g_props[g_count].value, eq + 1, vlen);
            g_props[g_count].value[vlen] = 0;
            if (vlen && g_props[g_count].value[vlen - 1] == '\r') g_props[g_count].value[vlen - 1] = 0;
            g_count++;
        }
        line += len;
        if (*line) line++;
    }
}

static void load(void)
{
    if (g_count >= 0) return;
    g_count = 0;
    load_file("/system/build.prop");
    load_file("/system/system_ext/etc/build.prop");
    load_file("/product/etc/build.prop");
    load_file("/vendor/build.prop");
    load_file("/vendor/default.prop");
}

const struct prop_info* __system_property_find(const char* name)
{
    int i;
    load();
    for (i = g_count - 1; i >= 0; i--)   /* the last definition wins */
        if (strcmp(g_props[i].name, name) == 0) return &g_props[i];
    /* The image's product partition names no VNDK version; a booted device
     * derives it from the vendor's. */
    if (strcmp(name, "ro.product.vndk.version") == 0) return __system_property_find("ro.vndk.version");
    return NULL;
}

int __system_property_read(const struct prop_info* pi, char* name, char* value)
{
    if (name) strcpy(name, pi->name);
    strcpy(value, pi->value);
    return (int)strlen(pi->value);
}

void __system_property_read_callback(const struct prop_info* pi,
                                     void (*callback)(void* cookie, const char* name, const char* value, uint32_t serial),
                                     void* cookie)
{
    callback(cookie, pi->name, pi->value, 0);
}

int __system_property_get(const char* name, char* value)
{
    const struct prop_info* pi = __system_property_find(name);
    if (!pi) { value[0] = 0; return 0; }
    return __system_property_read(pi, NULL, value);
}
