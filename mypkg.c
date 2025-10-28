#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <archive.h>
#include <archive_entry.h>
#include <errno.h>
#include <time.h>

#define CONFIG_FILE "/etc/mpkg.conf"
#define LOG_FILE "/var/log/mpkg.log"
#define HISTORY_DIR "/var/db/mpkg/history"

char PKG_DB_PATH[256] = "/var/db/mpkg";
char PKG_CACHE_PATH[256] = "/var/cache/mpkg";
char PKG_REPO_URL[512] = "https://loxsete.github.io/mpkg-server/";

typedef struct {
    char name[256];
    char version[64];
    char arch[16];
    char depends[1024];
    char description[512];
    size_t size;
    time_t install_time;
} Package;

int is_installed(const char *package_name);
int read_config(void);
int db_init(void);
Package* read_package_info(const char *archive_path);
int check_dependencies(const char *depends);
int download_package(const char *package_name);
static int copy_data(struct archive *ar, struct archive *aw);
int check_conflicts(const char *package_name);
int extract_package(const char *package_name);
void log_action(const char *action, const char *package_name, int status);
int mark_installed(const char *package_name, Package *pkg);
Package* read_installed_package(const char *package_name);
int sync_repository(void);
int update_package(const char *package_name);
int install_multiple_packages(int count, char *packages[]);
int install_package(const char *package_name);
int remove_package(const char *package_name);
void list_installed(void);
void search_packages(const char *query);
void show_package_info(const char *package_name);
int ghost_install(const char *package_name);
int self_update(void);
void show_stats(void);
int clean_aggressive(void);
void run_doctor(void);


int read_config(void) {
    FILE *f = fopen(CONFIG_FILE, "r");
    if (!f) return 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = 0;
        if (line[0] == '#' || line[0] == '\0') continue;
        char *key = strtok(line, "=");
        char *value = strtok(NULL, "=");
        if (!key || !value) continue;
        while (*key == ' ') key++;
        while (*value == ' ') value++;
        char *end = value + strlen(value) - 1;
        while (end > value && *end == ' ') *end-- = '\0';
        if (strcmp(key, "PKG_DB_PATH") == 0) strncpy(PKG_DB_PATH, value, sizeof(PKG_DB_PATH)-1);
        else if (strcmp(key, "PKG_CACHE_PATH") == 0) strncpy(PKG_CACHE_PATH, value, sizeof(PKG_CACHE_PATH)-1);
        else if (strcmp(key, "PKG_REPO_URL") == 0) strncpy(PKG_REPO_URL, value, sizeof(PKG_REPO_URL)-1);
    }
    fclose(f);
    return 0;
}

int db_init(void) {
    if (read_config() != 0) return -1;
    mkdir(PKG_DB_PATH, 0755);
    mkdir(PKG_CACHE_PATH, 0755);
    mkdir(HISTORY_DIR, 0755);
    return 0;
}

int is_installed(const char *package_name) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.installed", PKG_DB_PATH, package_name);
    return access(path, F_OK) == 0;
}


Package* read_package_info(const char *archive_path) {
    struct archive *a = archive_read_new();
    archive_read_support_format_all(a);
    archive_read_support_filter_all(a);
    if (archive_read_open_filename(a, archive_path, 10240)) {
        archive_read_free(a);
        return NULL;
    }
    struct archive_entry *entry;
    Package *pkg = NULL;
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        const char *name = archive_entry_pathname(entry);
        if (strcmp(name, "PKGINFO") && strcmp(name, "./PKGINFO")) {
            archive_read_data_skip(a);
            continue;
        }
        pkg = malloc(sizeof(Package));
        memset(pkg, 0, sizeof(Package));
        size_t sz = archive_entry_size(entry);
        char *buf = malloc(sz + 1);
        archive_read_data(a, buf, sz);
        buf[sz] = '\0';
        char *line = strtok(buf, "\n");
        while (line) {
            if (!strncmp(line, "name=", 5)) strncpy(pkg->name, line+5, sizeof(pkg->name)-1);
            else if (!strncmp(line, "version=", 8)) strncpy(pkg->version, line+8, sizeof(pkg->version)-1);
            else if (!strncmp(line, "arch=", 5)) strncpy(pkg->arch, line+5, sizeof(pkg->arch)-1);
            else if (!strncmp(line, "description=", 12)) strncpy(pkg->description, line+12, sizeof(pkg->description)-1);
            else if (!strncmp(line, "depends=", 8)) strncpy(pkg->depends, line+8, sizeof(pkg->depends)-1);
            else if (!strncmp(line, "size=", 5)) pkg->size = atol(line+5);
            line = strtok(NULL, "\n");
        }
        free(buf);
        break;
    }
    archive_read_close(a);
    archive_read_free(a);
    return pkg;
}

int check_dependencies(const char *depends) {
    if (!*depends) return 0;
    char copy[1024];
    strncpy(copy, depends, sizeof(copy)-1);
    char *dep = strtok(copy, ",");
    int miss = 0;
    while (dep) {
        while (*dep == ' ') dep++;
        char *e = dep + strlen(dep) - 1;
        while (e > dep && *e == ' ') *e-- = '\0';
        if (!is_installed(dep)) { printf("Error: dependency '%s' is missing!\n", dep); miss++; }
        else printf("Dependency '%s' is installed.\n", dep);
        dep = strtok(NULL, ",");
    }
    if (miss) { fprintf(stderr, "%d dependencies are missing.\n", miss); return -1; }
    return 0;
}

int download_package(const char *package_name) {
    char url[512], out[512], cmd[1024];
    snprintf(url, sizeof(url), "%s/%s.tar.xz", PKG_REPO_URL, package_name);
    snprintf(out, sizeof(out), "%s/%s.tar.xz", PKG_CACHE_PATH, package_name);
    snprintf(cmd, sizeof(cmd), "curl -L -f --progress-bar -o %s %s", out, url);
    printf("Grabbing %s\n", package_name);
    return system(cmd) ? -1 : 0;
}

static int copy_data(struct archive *ar, struct archive *aw) {
    const void *buf; size_t sz; la_int64_t off;
    for (;;) {
        int r = archive_read_data_block(ar, &buf, &sz, &off);
        if (r == ARCHIVE_EOF) return ARCHIVE_OK;
        if (r < ARCHIVE_OK) return r;
        r = archive_write_data_block(aw, buf, sz, off);
        if (r < ARCHIVE_OK) return r;
    }
}

int check_conflicts(const char *package_name) {
    char files[512];
    snprintf(files, sizeof(files), "%s/%s.files", PKG_DB_PATH, package_name);
    FILE *nf = fopen(files, "r");
    if (!nf) return 0;
    DIR *d = opendir(PKG_DB_PATH);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strstr(e->d_name, ".files") || strstr(e->d_name, package_name)) continue;
        char other[512];
        snprintf(other, sizeof(other), "%s/%s", PKG_DB_PATH, e->d_name);
        FILE *of = fopen(other, "r");
        if (!of) continue;
        char npath[1024], opath[1024];
        while (fgets(npath, sizeof(npath), nf)) {
            npath[strcspn(npath, "\n")] = 0;
            rewind(of);
            while (fgets(opath, sizeof(opath), of)) {
                opath[strcspn(opath, "\n")] = 0;
                if (!strcmp(npath, opath)) {
                    printf("Conflict: %s already owned by another package\n", npath);
                    fclose(of); fclose(nf); closedir(d);
                    return -1;
                }
            }
        }
        fclose(of);
    }
    fclose(nf); closedir(d);
    return 0;
}

int extract_package(const char *package_name) {
    char arch[512];
    snprintf(arch, sizeof(arch), "%s/%s.tar.xz", PKG_CACHE_PATH, package_name);
    struct archive *a = archive_read_new();
    archive_read_support_format_all(a);
    archive_read_support_filter_all(a);
    struct archive *ext = archive_write_disk_new();
    archive_write_disk_set_options(ext, ARCHIVE_EXTRACT_TIME|ARCHIVE_EXTRACT_PERM|ARCHIVE_EXTRACT_OWNER);
    if (archive_read_open_filename(a, arch, 10240)) return -1;
    printf("Unpacking %s\n", package_name);
    char log[512];
    snprintf(log, sizeof(log), "%s/%s.files", PKG_DB_PATH, package_name);
    FILE *fl = fopen(log, "w");
    if (!fl) return -1;
    char *cwd = getcwd(NULL, 0);
    chdir("/");
    struct archive_entry *e;
    while (archive_read_next_header(a, &e) == ARCHIVE_OK) {
        const char *name = archive_entry_pathname(e);
        if (!strncmp(name, "PKGINFO", 7) || !strncmp(name, "FILES", 5) ||
            !strncmp(name, "./PKGINFO", 9) || !strncmp(name, "./FILES", 7)) {
            archive_read_data_skip(a); continue;
        }
        printf(" %s\n", name);
        if (archive_entry_filetype(e) == AE_IFREG) {
            const char *p = name;
            if (!strncmp(name, "./", 2)) p = name + 1;
            fprintf(fl, p[0] == '/' ? "%s\n" : "/%s\n", p);
        }
        archive_write_header(ext, e);
        copy_data(a, ext);
    }
    if (cwd) { chdir(cwd); free(cwd); }
    fclose(fl);
    archive_read_close(a); archive_read_free(a);
    archive_write_close(ext); archive_write_free(ext);
    return 0;
}


void log_action(const char *action, const char *package_name, int status) {
    FILE *f = fopen(LOG_FILE, "a");
    if (!f) return;
    time_t t = time(NULL);
    fprintf(f, "[%s] %s %s: %s\n", ctime(&t), action, package_name, status ? "failed" : "success");
    fclose(f);
}

int mark_installed(const char *package_name, Package *pkg) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.installed", PKG_DB_PATH, package_name);
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    time_t now = time(NULL);
    fprintf(f, "name=%s\n", pkg ? pkg->name : package_name);
    if (pkg) {
        fprintf(f, "version=%s\narch=%s\ndescription=%s\ndepends=%s\nsize=%zu\n",
                pkg->version, pkg->arch, pkg->description, pkg->depends, pkg->size);
    }
    fprintf(f, "install_time=%ld\ninstalled=1\n", now);
    fclose(f);
    return 0;
}

Package* read_installed_package(const char *package_name) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.installed", PKG_DB_PATH, package_name);
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    Package *pkg = malloc(sizeof(Package));
    memset(pkg, 0, sizeof(Package));
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = 0;
        if (!strncmp(line, "name=", 5)) strncpy(pkg->name, line+5, sizeof(pkg->name)-1);
        else if (!strncmp(line, "version=", 8)) strncpy(pkg->version, line+8, sizeof(pkg->version)-1);
        else if (!strncmp(line, "arch=", 5)) strncpy(pkg->arch, line+5, sizeof(pkg->arch)-1);
        else if (!strncmp(line, "description=", 12)) strncpy(pkg->description, line+12, sizeof(pkg->description)-1);
        else if (!strncmp(line, "depends=", 8)) strncpy(pkg->depends, line+8, sizeof(pkg->depends)-1);
        else if (!strncmp(line, "size=", 5)) pkg->size = atol(line+5);
        else if (!strncmp(line, "install_time=", 13)) pkg->install_time = atol(line+13);
    }
    fclose(f);
    return pkg;
}

int sync_repository(void) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "curl -L -o %s/repo.db %s/repo.db", PKG_DB_PATH, PKG_REPO_URL);
    if (system(cmd)) { fprintf(stderr, "Failed to sync repo\n"); return -1; }
    log_action("sync", "repository", 0);
    printf("Repository synced\n");
    return 0;
}

int update_package(const char *package_name) {
    Package *local = read_installed_package(package_name);
    if (!local) { printf("%s not installed\n", package_name); return -1; }
    char db[512];
    snprintf(db, sizeof(db), "%s/repo.db", PKG_DB_PATH);
    FILE *f = fopen(db, "r");
    if (!f) { free(local); return -1; }
    char line[1024], ver[64] = "";
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = 0;
        if (!strncmp(line, "name=", 5) && !strcmp(line+5, package_name)) {
            while (fgets(line, sizeof(line), f)) {
                line[strcspn(line, "\n")] = 0;
                if (!strncmp(line, "version=", 8)) { strncpy(ver, line+8, sizeof(ver)-1); break; }
            }
            break;
        }
    }
    fclose(f);
    if (!*ver) { free(local); return -1; }
    if (!strcmp(local->version, ver)) { printf("%s is up to date\n", package_name); free(local); return 0; }
    printf("Updating %s %s to %s\n", package_name, local->version, ver);
    free(local);
    if (download_package(package_name)) return -1;
    char cache[512];
    snprintf(cache, sizeof(cache), "%s/%s.tar.xz", PKG_CACHE_PATH, package_name);
    Package *pkg = read_package_info(cache);
    if (!pkg) return -1;
    if (check_conflicts(package_name)) { free(pkg); return -1; }
    if (extract_package(package_name)) { free(pkg); return -1; }
    mark_installed(package_name, pkg);
    log_action("update", package_name, 0);
    printf("%s updated\n", package_name);
    free(pkg);
    return 0;
}

int install_multiple_packages(int count, char *packages[]) {
    int err = 0;
    for (int i = 0; i < count; i++) {
        if (install_package(packages[i])) err++;
    }
    return err;
}

int install_package(const char *package_name) {
    if (is_installed(package_name)) { printf("%s is already installed\n", package_name); return 0; }
    printf("Installing %s\n", package_name);
    if (download_package(package_name)) return -1;
    char cache[512];
    snprintf(cache, sizeof(cache), "%s/%s.tar.xz", PKG_CACHE_PATH, package_name);
    Package *pkg = read_package_info(cache);
    if (pkg) {
        printf(" name: %s\n version: %s\n arch: %s\n description: %s\n", pkg->name, pkg->version, pkg->arch, pkg->description);
        if (*pkg->depends) { printf(" depends: %s\n", pkg->depends); if (check_dependencies(pkg->depends)) { free(pkg); return -1; } }
        if (pkg->size) printf(" size: %zu bytes\n", pkg->size);
    }
    if (check_conflicts(package_name)) { if (pkg) free(pkg); return -1; }
    if (extract_package(package_name)) { if (pkg) free(pkg); return -1; }
    mark_installed(package_name, pkg);
    log_action("install", package_name, 0);
    printf("%s installed\n", package_name);
    if (pkg) free(pkg);
    return 0;
}

int remove_package(const char *package_name) {
    if (!is_installed(package_name)) { printf("%s ain't installed?\n", package_name); return 0; }
    printf("Nuking %s\n", package_name);
    char files[512];
    snprintf(files, sizeof(files), "%s/%s.files", PKG_DB_PATH, package_name);
    FILE *f = fopen(files, "r");
    int ok = 0, fail = 0;
    if (f) {
        char path[1024];
        while (fgets(path, sizeof(path), f)) {
            path[strcspn(path, "\n")] = 0;
            if (!*path) continue;
            printf(" Deleting: %s\n", path);
            unlink(path) ? fail++ : ok++;
        }
        fclose(f);
        printf("Cleanup: %d files trashed, %d failed\n", ok, fail);
        unlink(files);
    }
    char db[512];
    snprintf(db, sizeof(db), "%s/%s.installed", PKG_DB_PATH, package_name);
    unlink(db);
    log_action("remove", package_name, 0);
    printf("%s is gone, baby, gone\n", package_name);
    return 0;
}

void list_installed(void) {
    DIR *d = opendir(PKG_DB_PATH);
    if (!d) return;
    printf("Installed packages:\n");
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strstr(e->d_name, ".installed")) continue;
        char name[256];
        strncpy(name, e->d_name, sizeof(name)-1);
        *strstr(name, ".installed") = '\0';
        Package *p = read_installed_package(name);
        if (p) printf(" %s-%s (%s)\n", p->name, p->version, p->description);
        else printf(" %s\n", name);
        if (p) free(p);
    }
    closedir(d);
}

void search_packages(const char *q) {
    DIR *d = opendir(PKG_DB_PATH);
    if (d) {
        printf("Searching for '%s':\n", q);
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strstr(e->d_name, ".installed") || !strstr(e->d_name, q)) continue;
            char name[256];
            strncpy(name, e->d_name, sizeof(name)-1);
            *strstr(name, ".installed") = '\0';
            Package *p = read_installed_package(name);
            if (p) printf(" %s-%s (%s)\n", p->name, p->version, p->description);
            else printf(" %s\n", name);
            if (p) free(p);
        }
        closedir(d);
    }
    char db[512];
    snprintf(db, sizeof(db), "%s/repo.db", PKG_DB_PATH);
    FILE *f = fopen(db, "r");
    if (!f) return;
    char line[1024], name[256], ver[64], desc[512];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = 0;
        if (!strncmp(line, "name=", 5) && strstr(line+5, q)) {
            strncpy(name, line+5, sizeof(name)-1);
            ver[0] = desc[0] = '\0';
            while (fgets(line, sizeof(line), f)) {
                line[strcspn(line, "\n")] = 0;
                if (!strncmp(line, "version=", 8)) strncpy(ver, line+8, sizeof(ver)-1);
                else if (!strncmp(line, "description=", 12)) strncpy(desc, line+12, sizeof(desc)-1);
                if (ver[0] && desc[0]) break;
            }
            printf(" %s-%s (%s) [repo]\n", name, ver, desc);
        }
    }
    fclose(f);
}

void show_package_info(const char *package_name) {
    if (!is_installed(package_name)) { printf("%s ain't installed\n", package_name); return; }
    Package *p = read_installed_package(package_name);
    if (!p) { printf("Can't read info for %s\n", package_name); return; }
    printf("Package info:\n name: %s\n version: %s\n arch: %s\n description: %s\n",
           p->name, p->version, p->arch, p->description);
    if (*p->depends) printf(" dependencies: %s\n", p->depends);
    if (p->size) printf(" installed size: %zu bytes\n", p->size);
    if (p->install_time) printf(" install date: %s", ctime(&p->install_time));
    char files[512];
    snprintf(files, sizeof(files), "%s/%s.files", PKG_DB_PATH, package_name);
    FILE *f = fopen(files, "r");
    if (f) {
        printf(" files (first 10):\n");
        char path[1024]; int c = 0;
        while (fgets(path, sizeof(path), f) && c < 10) {
            path[strcspn(path, "\n")] = 0;
            if (*path) { printf(" %s\n", path); c++; }
        }
        fclose(f);
    }
    free(p);
}

/* 6. ghost */
int ghost_install(const char *package_name) {
    if (download_package(package_name)) return -1;
    char cache[512];
    snprintf(cache, sizeof(cache), "%s/%s.tar.xz", PKG_CACHE_PATH, package_name);
    Package *pkg = read_package_info(cache);
    if (pkg && *pkg->depends && check_dependencies(pkg->depends)) { free(pkg); return -1; }
    if (check_conflicts(package_name)) { if (pkg) free(pkg); return -1; }
    if (extract_package(package_name)) { if (pkg) free(pkg); return -1; }
    printf("%s ghost-installed (no DB entry)\n", package_name);
    if (pkg) free(pkg);
    return 0;
}

/* 7. self-update */
int self_update(void) {
    if (download_package("mpkg")) return -1;
    char cache[512];
    snprintf(cache, sizeof(cache), "%s/mpkg.tar.xz", PKG_CACHE_PATH);
    Package *p = read_package_info(cache);
    if (!p) return -1;
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "cp %s /bin/mpkg && chmod 755 /bin/mpkg", cache);
    if (system(cmd)) { free(p); return -1; }
    printf("mpkg updated to %s\n", p->version);
    free(p);
    return 0;
}

/* 8. stats */
void show_stats(void) {
    DIR *d = opendir(PKG_DB_PATH);
    if (!d) return;
    int pkgs = 0; size_t total = 0;
    struct { char name[256]; size_t sz; } top[5] = {0};
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strstr(e->d_name, ".installed")) continue;
        char name[256];
        strncpy(name, e->d_name, sizeof(name)-1);
        *strstr(name, ".installed") = '\0';
        Package *p = read_installed_package(name);
        if (p) {
            pkgs++; total += p->size;
            for (int i = 0; i < 5; i++) {
                if (p->size > top[i].sz) {
                    memmove(&top[i+1], &top[i], (4-i)*sizeof(top[0]));
                    strncpy(top[i].name, name, sizeof(top[i].name)-1);
                    top[i].sz = p->size;
                    break;
                }
            }
            free(p);
        }
    }
    closedir(d);
    printf("Packages: %d\nTotal size: %zu bytes\nTop 5 by size:\n", pkgs, total);
    for (int i = 0; i < 5 && top[i].sz; i++) printf(" %s: %zu\n", top[i].name, top[i].sz);
}

/* 9. clean --aggressive */
int clean_aggressive(void) {
    DIR *d = opendir(PKG_DB_PATH);
    if (!d) return -1;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strstr(e->d_name, ".installed")) continue;
        char name[256];
        strncpy(name, e->d_name, sizeof(name)-1);
        *strstr(name, ".installed") = '\0';
        if (strcmp(name, "mpkg") && strcmp(name, "busybox")) remove_package(name);
    }
    closedir(d);
    printf("Aggressive clean complete\n");
    return 0;
}

/* 10. doctor */
void run_doctor(void) {
    printf("Running mpkg doctor...\n");
    DIR *d = opendir(PKG_DB_PATH);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strstr(e->d_name, ".files")) continue;
        char name[256];
        strncpy(name, e->d_name, sizeof(name)-1);
        *strstr(name, ".files") = '\0';
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", PKG_DB_PATH, e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        char line[1024];
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\n")] = 0;
            if (access(line, F_OK)) printf("Missing file: %s (owned by %s)\n", line, name);
        }
        fclose(f);
    }
    closedir(d);
    printf("Doctor finished\n");
}


int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: mpkg <command> [args]\n");
        printf(" install <pkg>      remove <pkg>      list      info <pkg>\n");
        printf(" update [pkg]       search <q>      ghost <pkg>\n");
        printf(" self-update        stats           clean --aggressive\n");
        printf(" doctor\n");
        return 1;
    }
    if (db_init()) return 1;

    if (!strcmp(argv[1], "install")) {
        if (argc < 3) return 1;
        return install_multiple_packages(argc-2, &argv[2]);
    }
    if (!strcmp(argv[1], "remove")) {
        if (argc < 3) return 1;
        return remove_package(argv[2]);
    }
    if (!strcmp(argv[1], "list")) { list_installed(); return 0; }
    if (!strcmp(argv[1], "info")) {
        if (argc < 3) return 1;
        show_package_info(argv[2]); return 0;
    }
    if (!strcmp(argv[1], "update")) {
        if (argc < 3) return sync_repository();
        return update_package(argv[2]);
    }
    if (!strcmp(argv[1], "search")) {
        if (argc < 3) return 1;
        search_packages(argv[2]); return 0;
    }
    if (!strcmp(argv[1], "ghost")) {
        if (argc < 3) return 1;
        return ghost_install(argv[2]);
    }
    if (!strcmp(argv[1], "self-update")) { return self_update(); }
    if (!strcmp(argv[1], "stats")) { show_stats(); return 0; }
    if (!strcmp(argv[1], "clean") && argc > 2 && !strcmp(argv[2], "--aggressive")) {
        return clean_aggressive();
    }
    if (!strcmp(argv[1], "doctor")) { run_doctor(); return 0; }

    fprintf(stderr, "Unknown command: %s\n", argv[1]);
    return 1;
}
