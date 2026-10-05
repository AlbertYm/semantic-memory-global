#ifndef CBM_UI_ADMIN_OPERATIONS_H
#define CBM_UI_ADMIN_OPERATIONS_H

char *cbm_admin_health_json(const char *project, const char *version);
char *cbm_admin_tasks_json(const char *project, int limit);
char *cbm_admin_create_backup_json(const char *project, const char *destination_root);
char *cbm_admin_verify_restore_json(const char *project, const char *source_directory,
                                    const char *target_directory);

#endif
