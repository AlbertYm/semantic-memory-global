#ifndef CBM_PROJECT_RESOLVER_H
#define CBM_PROJECT_RESOLVER_H

#include <stddef.h>

#define CBM_STAGE14_NAMESPACE_UUID "4bf6ec6a-c90e-4eb6-8749-7e831665b27c"
#define CBM_PROJECT_UUID_SIZE 37
#define CBM_PROJECT_PATH_HASH_SIZE 65

typedef struct {
    char canonical_path[4096];
    char path_hash[CBM_PROJECT_PATH_HASH_SIZE];
    char project_uuid[CBM_PROJECT_UUID_SIZE];
    char display_name[512];
    char volume_id[128];
    char source_fingerprint[CBM_PROJECT_PATH_HASH_SIZE];
    int path_exists;
    int path_writable;
} cbm_project_resolution_t;

/* Resolve cwd/rootUri input into one deterministic, case-folded workspace identity.
 * No filesystem writes are performed. An explicit source fingerprint is accepted;
 * otherwise an existing workspace receives a read-only filesystem identity used
 * by the catalog to reconnect same-volume moves and renames. */
int cbm_project_resolve(const char *path_or_file_uri, const char *display_name,
                        const char *source_fingerprint, cbm_project_resolution_t *out);

/* Exposed for frozen fixture verification and catalog replay checks. */
int cbm_project_canonicalize_path(const char *path_or_file_uri, char *out, size_t out_size);
int cbm_project_uuid_v5(const char *canonical_path, char out[CBM_PROJECT_UUID_SIZE]);

#endif
