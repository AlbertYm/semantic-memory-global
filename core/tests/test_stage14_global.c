#include "memory/global_memory.h"
#include "memory/evolution_engine.h"
#include "memory/edge_lifecycle.h"
#include "memory/concept_growth.h"
#include "memory/project_resolver.h"
#include "store/store.h"

#include <sqlite3.h>
#include <yyjson/yyjson.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#include <process.h>
#include <windows.h>
#include <wchar.h>
#endif

#define CHECK(value) do { if (!(value)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#value); return 1; } } while(0)

int cbm_evolution_event_lookup_for_test(sqlite3 *db, const char *key,
                                        const char *payload_hash, int *out_exact);
int cbm_evolution_open_atomic_db_for_test(sqlite3 *memory_db, sqlite3 *graph_db,
                                           sqlite3 **out_db);
int cbm_evolution_attach_authorizer_failure_for_test(sqlite3 *memory_db,
                                                      sqlite3 *graph_db);

#ifdef _WIN32
typedef struct {
    ULONGLONG size;
    FILETIME last_write;
    DWORD volume_serial;
    DWORD file_index_high;
    DWORD file_index_low;
    DWORD link_count;
    LONGLONG change_time;
    char sha256[65];
} stage14_file_fact_t;

static int stage14_utf8_to_wide(const char *path,wchar_t *wide,size_t wide_count) {
    if(!path||!wide||wide_count==0||wide_count>(size_t)INT_MAX)return 0;
    return MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path,-1,wide,(int)wide_count)>0;
}

static int stage14_wide_to_utf8(const wchar_t *wide,char *path,size_t path_size) {
    if(!wide||!path||path_size==0||path_size>(size_t)INT_MAX)return 0;
    return WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide,-1,path,(int)path_size,NULL,NULL)>0;
}

static int stage14_path_exists(const char *path) {
    wchar_t wide[4096];return stage14_utf8_to_wide(path,wide,4096)&&GetFileAttributesW(wide)!=INVALID_FILE_ATTRIBUTES;
}

static int stage14_copy_file(const char *source,const char *target) {
    wchar_t source_wide[4096],target_wide[4096];
    return stage14_utf8_to_wide(source,source_wide,4096)&&stage14_utf8_to_wide(target,target_wide,4096)&&
           CopyFileW(source_wide,target_wide,FALSE)!=0;
}

static int stage14_create_hardlink(const char *link_path,const char *target_path) {
    wchar_t link_wide[4096],target_wide[4096];
    return stage14_utf8_to_wide(link_path,link_wide,4096)&&stage14_utf8_to_wide(target_path,target_wide,4096)&&
           CreateHardLinkW(link_wide,target_wide,NULL)!=0;
}

static int stage14_create_symlink(const char *link_path,const char *target_path) {
    wchar_t link_wide[4096],target_wide[4096];
    if(!stage14_utf8_to_wide(link_path,link_wide,4096)||!stage14_utf8_to_wide(target_path,target_wide,4096))return 0;
#ifdef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
    if(CreateSymbolicLinkW(link_wide,target_wide,SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE))return 1;
#endif
    return CreateSymbolicLinkW(link_wide,target_wide,0)!=0;
}

static int stage14_write_file(const char *path,const void *bytes,DWORD size) {
    wchar_t wide[4096];if(!stage14_utf8_to_wide(path,wide,4096))return 0;
    HANDLE file=CreateFileW(wide,GENERIC_WRITE,FILE_SHARE_READ,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
    if(file==INVALID_HANDLE_VALUE)return 0;
    DWORD written=0;int ok=WriteFile(file,bytes,size,&written,NULL)&&written==size&&FlushFileBuffers(file);
    if(!CloseHandle(file))ok=0;return ok;
}

static int stage14_delete_file(const char *path) {
    wchar_t wide[4096];return stage14_utf8_to_wide(path,wide,4096)&&DeleteFileW(wide)!=0;
}

static int stage14_file_fact(const char *path,stage14_file_fact_t *fact) {
    wchar_t wide[4096];if(!fact||!stage14_utf8_to_wide(path,wide,4096))return 0;
    HANDLE file=CreateFileW(wide,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                            NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_SEQUENTIAL_SCAN,NULL);
    if(file==INVALID_HANDLE_VALUE)return 0;
    BY_HANDLE_FILE_INFORMATION info;FILE_BASIC_INFO basic;
    int ok=GetFileInformationByHandle(file,&info)!=0&&GetFileInformationByHandleEx(file,FileBasicInfo,&basic,sizeof(basic))!=0;
    ULARGE_INTEGER size;size.HighPart=info.nFileSizeHigh;size.LowPart=info.nFileSizeLow;
    if(ok&&(size.QuadPart>(ULONGLONG)SIZE_MAX||size.QuadPart>(ULONGLONG)UINT32_MAX))ok=0;
    unsigned char *bytes=ok?malloc((size_t)(size.QuadPart?size.QuadPart:1)):NULL;
    if(ok&&!bytes)ok=0;
    DWORD read=0;if(ok&&size.QuadPart)ok=ReadFile(file,bytes,(DWORD)size.QuadPart,&read,NULL)&&read==(DWORD)size.QuadPart;
    if(ok&&cbm_stage7_sha256_hex(bytes,(size_t)size.QuadPart,fact->sha256)!=CBM_STORE_OK)ok=0;
    if(ok){fact->size=size.QuadPart;fact->last_write=info.ftLastWriteTime;
        fact->volume_serial=info.dwVolumeSerialNumber;fact->file_index_high=info.nFileIndexHigh;fact->file_index_low=info.nFileIndexLow;}
    if(ok){fact->link_count=info.nNumberOfLinks;fact->change_time=basic.ChangeTime.QuadPart;}
    free(bytes);if(!CloseHandle(file))ok=0;return ok;
}

static int stage14_file_fact_equal(const stage14_file_fact_t *left,const stage14_file_fact_t *right) {
    return left&&right&&left->size==right->size&&
           left->last_write.dwLowDateTime==right->last_write.dwLowDateTime&&
           left->last_write.dwHighDateTime==right->last_write.dwHighDateTime&&
           left->volume_serial==right->volume_serial&&left->file_index_high==right->file_index_high&&
           left->file_index_low==right->file_index_low&&
           left->link_count==right->link_count&&left->change_time==right->change_time&&
           strcmp(left->sha256,right->sha256)==0;
}

static int stage14_no_sidecars(const char *const *paths,size_t path_count) {
    const char *suffixes[]={"-wal","-shm","-journal"};char sidecar[8192];
    for(size_t i=0;i<path_count;i++)for(size_t j=0;j<3;j++){
        if(snprintf(sidecar,sizeof(sidecar),"%s%s",paths[i],suffixes[j])<0||stage14_path_exists(sidecar))return 0;
    }
    return 1;
}
#endif

extern int cbm_global_migration_immutable_uri_for_test(const char *path,int windows_semantics,
                                                       char *out,size_t out_size);
typedef void (*stage14_migration_test_hook_fn)(const char *phase,
                                               const char *const *target_paths,
                                               size_t target_path_count,
                                               void *context);
extern void cbm_global_migration_set_test_hook_for_test(stage14_migration_test_hook_fn hook,
                                                        void *context);
extern int cbm_global_migration_backup_limits_for_test(int *max_attempts,
                                                       uint64_t *max_elapsed_ms);

#ifdef _WIN32
enum {
    STAGE14_MIGRATION_HOOK_CREATE_COMPETITOR=1,
    STAGE14_MIGRATION_HOOK_LOCK_SOURCE=2
};

typedef struct {
    int mode;
    const char *source_memory_path;
    int called;
    int hook_rc;
    sqlite3 *locker;
    char sentinel_path[4096];
    stage14_file_fact_t sentinel_before;
} stage14_migration_hook_context_t;

static void stage14_migration_apply_hook(const char *phase,
                                         const char *const *target_paths,
                                         size_t target_path_count,
                                         void *opaque) {
    stage14_migration_hook_context_t *context=opaque;
    if(!context||strcmp(phase,"after-all-absent-precheck")||context->called)return;
    context->called=1;context->hook_rc=SQLITE_ERROR;
    if(context->mode==STAGE14_MIGRATION_HOOK_CREATE_COMPETITOR){
        if(!target_paths||target_path_count<2||
           snprintf(context->sentinel_path,sizeof(context->sentinel_path),"%s",
                    target_paths[1])<0)return;
        sqlite3 *db=NULL;
        int rc=sqlite3_open_v2(context->sentinel_path,&db,
                               SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE,NULL);
        if(rc==SQLITE_OK)rc=sqlite3_exec(db,
            "PRAGMA journal_mode=DELETE;"
            "CREATE TABLE competitor_sentinel(id INTEGER PRIMARY KEY,payload TEXT NOT NULL);"
            "INSERT INTO competitor_sentinel(payload) VALUES('must-remain-byte-identical');",
            NULL,NULL,NULL);
        if(db&&sqlite3_close(db)!=SQLITE_OK)rc=SQLITE_ERROR;
        if(rc==SQLITE_OK&&!stage14_file_fact(context->sentinel_path,
                                             &context->sentinel_before))
            rc=SQLITE_ERROR;
        context->hook_rc=rc;
        return;
    }
    if(context->mode==STAGE14_MIGRATION_HOOK_LOCK_SOURCE&&
       context->source_memory_path){
        int rc=sqlite3_open_v2(context->source_memory_path,&context->locker,
                               SQLITE_OPEN_READWRITE,NULL);
        if(rc==SQLITE_OK){
            sqlite3_busy_timeout(context->locker,0);
            rc=sqlite3_exec(context->locker,"BEGIN EXCLUSIVE;",NULL,NULL,NULL);
        }
        if(rc!=SQLITE_OK&&context->locker){
            sqlite3_close(context->locker);context->locker=NULL;
        }
        context->hook_rc=rc;
    }
}
#endif

static int scalar(sqlite3 *db,const char *sql) {
    sqlite3_stmt *stmt=NULL;int value=-1;
    if(sqlite3_prepare_v2(db,sql,-1,&stmt,NULL)==SQLITE_OK&&sqlite3_step(stmt)==SQLITE_ROW)value=sqlite3_column_int(stmt,0);
    sqlite3_finalize(stmt);return value;
}

typedef struct {
    int update_count;
} stage14_update_trace_t;

typedef struct {
    int total_changes;
    int maintenance_runs;
    int evolution_events;
    int maintenance_leases;
    int memory_items;
    int memory_items_not_active;
    int memory_edges;
    int edge_states;
    int edge_runs;
    int edge_decisions;
    int concept_runs;
    int concept_candidates;
    int concept_versions;
    int concept_sources;
} stage14_maintenance_state_t;

static void stage14_update_trace(void *opaque,int operation,const char *database,
                                 const char *table,sqlite3_int64 row_id) {
    stage14_update_trace_t *trace=opaque;
    (void)operation;(void)database;(void)table;(void)row_id;
    if(trace)trace->update_count++;
}

static stage14_maintenance_state_t stage14_maintenance_state(sqlite3 *db) {
    stage14_maintenance_state_t state={0};
    state.total_changes=sqlite3_total_changes(db);
    state.maintenance_runs=scalar(db,"SELECT COUNT(*) FROM global_maintenance_run;");
    state.evolution_events=scalar(db,"SELECT COUNT(*) FROM global_evolution_event;");
    state.maintenance_leases=scalar(db,"SELECT COUNT(*) FROM global_maintenance_lease;");
    state.memory_items=scalar(db,"SELECT COUNT(*) FROM memory_item;");
    state.memory_items_not_active=scalar(db,
        "SELECT COUNT(*) FROM memory_item WHERE status<>'active' OR deleted_at IS NOT NULL;");
    state.memory_edges=scalar(db,"SELECT COUNT(*) FROM memory_edge;");
    state.edge_states=scalar(db,"SELECT COUNT(*) FROM edge_lifecycle_state;");
    state.edge_runs=scalar(db,"SELECT COUNT(*) FROM edge_maintenance_run;");
    state.edge_decisions=scalar(db,"SELECT COUNT(*) FROM edge_maintenance_decision;");
    state.concept_runs=scalar(db,"SELECT COUNT(*) FROM concept_growth_run;");
    state.concept_candidates=scalar(db,"SELECT COUNT(*) FROM concept_candidate;");
    state.concept_versions=scalar(db,"SELECT COUNT(*) FROM concept_candidate_version;");
    state.concept_sources=scalar(db,"SELECT COUNT(*) FROM concept_candidate_source;");
    return state;
}

static int stage14_maintenance_state_equal(const stage14_maintenance_state_t *left,
                                           const stage14_maintenance_state_t *right) {
    return left&&right&&left->total_changes==right->total_changes&&
        left->maintenance_runs==right->maintenance_runs&&
        left->evolution_events==right->evolution_events&&
        left->maintenance_leases==right->maintenance_leases&&
        left->memory_items==right->memory_items&&
        left->memory_items_not_active==right->memory_items_not_active&&
        left->memory_edges==right->memory_edges&&left->edge_states==right->edge_states&&
        left->edge_runs==right->edge_runs&&left->edge_decisions==right->edge_decisions&&
        left->concept_runs==right->concept_runs&&
        left->concept_candidates==right->concept_candidates&&
        left->concept_versions==right->concept_versions&&
        left->concept_sources==right->concept_sources;
}

static int scalar_text(sqlite3 *db,const char *sql,char *out,size_t out_size) {
    if(!db||!sql||!out||out_size<2)return 0;
    out[0]=0;sqlite3_stmt *stmt=NULL;int ok=0;
    if(sqlite3_prepare_v2(db,sql,-1,&stmt,NULL)==SQLITE_OK&&sqlite3_step(stmt)==SQLITE_ROW){
        const char *value=(const char *)sqlite3_column_text(stmt,0);
        if(value&&strlen(value)<out_size){
            snprintf(out,out_size,"%s",value);
            ok=sqlite3_step(stmt)==SQLITE_DONE;
        }
    }
    sqlite3_finalize(stmt);return ok;
}

static int report_id(const char *json,const char *name,char *out,size_t size) {
    const char *key=strstr(json,name);if(!key)return 0;
    const char *colon=strchr(key,':');if(!colon)return 0;
    const char *start=strchr(colon,'"');if(!start)return 0;start++;
    const char *end=strchr(start,'"');if(!end||(size_t)(end-start)>=size)return 0;
    memcpy(out,start,(size_t)(end-start));out[end-start]=0;return 1;
}

static cbm_memory_item_t item(const char *id,const char *project,const char *content,double importance);

static double scalar_double(sqlite3 *db,const char *sql) {
    sqlite3_stmt *stmt=NULL;double value=-1.0;
    if(sqlite3_prepare_v2(db,sql,-1,&stmt,NULL)==SQLITE_OK&&sqlite3_step(stmt)==SQLITE_ROW)value=sqlite3_column_double(stmt,0);
    sqlite3_finalize(stmt);return value;
}

static void stage14_set_env(const char *name,const char *value) {
#ifdef _WIN32
    _putenv_s(name,value?value:"");
#else
    if(value)setenv(name,value,1);else unsetenv(name);
#endif
}

static int stage14_write_edge_manifest(const char *report_json,const char *run_id,
                                        const char *path,char out_sha256[65]) {
    yyjson_doc *report_doc=yyjson_read(report_json,strlen(report_json),0);
    yyjson_val *report=report_doc?yyjson_doc_get_root(report_doc):NULL;
    yyjson_val *decisions=report?yyjson_obj_get(report,"decisions"):NULL;
    if(!report||!yyjson_is_obj(report)||!decisions||!yyjson_is_arr(decisions)){yyjson_doc_free(report_doc);return 1;}
    yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL);yyjson_mut_val *root=doc?yyjson_mut_obj(doc):NULL;
    yyjson_mut_val *edges=doc?yyjson_mut_arr(doc):NULL;
    if(!doc||!root||!edges){yyjson_mut_doc_free(doc);yyjson_doc_free(report_doc);return 1;}
    yyjson_mut_doc_set_root(doc,root);size_t index,max;yyjson_val *decision;
    yyjson_arr_foreach(decisions,index,max,decision){yyjson_val *id=yyjson_obj_get(decision,"edge_id");if(!id||!yyjson_is_str(id)){yyjson_mut_doc_free(doc);yyjson_doc_free(report_doc);return 1;}yyjson_mut_arr_add_strcpy(doc,edges,yyjson_get_str(id));}
    yyjson_mut_obj_add_str(doc,root,"schema","stage9-production-canary-manifest/v1");
    yyjson_mut_obj_add_strcpy(doc,root,"project",yyjson_get_str(yyjson_obj_get(report,"project")));
    yyjson_mut_obj_add_str(doc,root,"run_id",run_id);
    yyjson_mut_obj_add_sint(doc,root,"as_of_ms",yyjson_get_sint(yyjson_obj_get(report,"as_of_ms")));
    yyjson_mut_obj_add_strcpy(doc,root,"algorithm_version",yyjson_get_str(yyjson_obj_get(report,"algorithm_version")));
    yyjson_mut_obj_add_int(doc,root,"policy_version",CBM_STAGE9_POLICY_VERSION);
    yyjson_mut_obj_add_int(doc,root,"config_version",CBM_STAGE9_CONFIG_VERSION);
    yyjson_mut_obj_add_str(doc,root,"policy_sha256",CBM_STAGE9_POLICY_SHA256);
    yyjson_mut_obj_add_val(doc,root,"edge_ids",edges);
    yyjson_mut_obj_add_strcpy(doc,root,"decision_set_sha256",yyjson_get_str(yyjson_obj_get(report,"decision_set_sha256")));
    yyjson_mut_obj_add_str(doc,root,"operation","maintenance");
    size_t size=0;char *json=yyjson_mut_write(doc,0,&size);yyjson_mut_doc_free(doc);yyjson_doc_free(report_doc);
    if(!json)return 1;FILE *file=fopen(path,"wb");int rc=!file||fwrite(json,1,size,file)!=size;if(file)fclose(file);
    if(!rc&&cbm_stage7_sha256_hex(json,size,out_sha256)!=CBM_STORE_OK)rc=1;free(json);return rc;
}

static int stage14_write_concept_manifest(const char *report_json,const char *path,
                                           char out_sha256[65]) {
    yyjson_doc *report_doc=yyjson_read(report_json,strlen(report_json),0);
    yyjson_val *report=report_doc?yyjson_doc_get_root(report_doc):NULL;
    yyjson_val *candidates=report?yyjson_obj_get(report,"candidates"):NULL;
    if(!report||!yyjson_is_obj(report)||!candidates||!yyjson_is_arr(candidates)){yyjson_doc_free(report_doc);return 1;}
    yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL);yyjson_mut_val *root=doc?yyjson_mut_obj(doc):NULL;
    if(!doc||!root){yyjson_mut_doc_free(doc);yyjson_doc_free(report_doc);return 1;}
    yyjson_mut_doc_set_root(doc,root);yyjson_mut_obj_add_str(doc,root,"schema","stage10-production-canary-manifest/v1");
    const char *fields[]={"project","store","run_id","request_sha256","algorithm_version","generator_version","policy_sha256","schema_sha256","decision_set_sha256"};
    for(size_t i=0;i<sizeof(fields)/sizeof(fields[0]);i++){yyjson_val *v=yyjson_obj_get(report,fields[i]);if(!v||!yyjson_is_str(v)){yyjson_mut_doc_free(doc);yyjson_doc_free(report_doc);return 1;}yyjson_mut_obj_add_strcpy(doc,root,fields[i],yyjson_get_str(v));}
    yyjson_mut_obj_add_int(doc,root,"policy_version",CBM_STAGE10_POLICY_VERSION);yyjson_mut_obj_add_int(doc,root,"config_version",CBM_STAGE10_CONFIG_VERSION);
    yyjson_mut_obj_add_int(doc,root,"eligible_count",(int)yyjson_arr_size(candidates));
    yyjson_mut_obj_add_val(doc,root,"candidates",yyjson_val_mut_copy(doc,candidates));
    size_t size=0;char *json=yyjson_mut_write(doc,0,&size);yyjson_mut_doc_free(doc);yyjson_doc_free(report_doc);
    if(!json)return 1;FILE *file=fopen(path,"wb");int rc=!file||fwrite(json,1,size,file)!=size;if(file)fclose(file);
    if(!rc&&cbm_stage7_sha256_hex(json,size,out_sha256)!=CBM_STORE_OK)rc=1;free(json);return rc;
}

static int stage14_write_task_evolution_manifest(
        const char *plan_json,const char *path,const char *mode_override,
        const char *task_override,const char *key_override,const char *request_override,
        int max_events_override,int max_edges_override,const char *memory_override,
        const char *feedback_override,int add_extra_field,char out_sha256[65]) {
    yyjson_doc *plan_doc=yyjson_read(plan_json,strlen(plan_json),0);
    yyjson_val *plan=plan_doc?yyjson_doc_get_root(plan_doc):NULL;
    yyjson_val *mode=plan?yyjson_obj_get(plan,"mode"):NULL;
    yyjson_val *task=plan?yyjson_obj_get(plan,"task_id"):NULL;
    yyjson_val *key=plan?yyjson_obj_get(plan,"idempotency_key"):NULL;
    yyjson_val *request=plan?yyjson_obj_get(plan,"request_sha256"):NULL;
    yyjson_val *memory_ids=plan?yyjson_obj_get(plan,"memory_item_ids"):NULL;
    yyjson_val *feedback_ids=plan?yyjson_obj_get(plan,"feedback_event_ids"):NULL;
    yyjson_val *max_events=plan?yyjson_obj_get(plan,"max_evolution_events"):NULL;
    yyjson_val *max_edges=plan?yyjson_obj_get(plan,"max_cross_project_edges"):NULL;
    if(!plan||!yyjson_is_obj(plan)||!mode||!yyjson_is_str(mode)||!task||!yyjson_is_str(task)||
       !key||!yyjson_is_str(key)||!request||!yyjson_is_str(request)||
       !memory_ids||!yyjson_is_arr(memory_ids)||!feedback_ids||!yyjson_is_arr(feedback_ids)||
       !max_events||!yyjson_is_int(max_events)||!max_edges||!yyjson_is_int(max_edges)){
        yyjson_doc_free(plan_doc);return 1;
    }
    yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root=doc?yyjson_mut_obj(doc):NULL;
    yyjson_mut_val *memories=doc?yyjson_mut_arr(doc):NULL;
    yyjson_mut_val *feedback=doc?yyjson_mut_arr(doc):NULL;
    if(!doc||!root||!memories||!feedback){
        yyjson_mut_doc_free(doc);yyjson_doc_free(plan_doc);return 1;
    }
    yyjson_mut_doc_set_root(doc,root);
    size_t index,max;yyjson_val *value;
    yyjson_arr_foreach(memory_ids,index,max,value){
        if(!yyjson_is_str(value)){yyjson_mut_doc_free(doc);yyjson_doc_free(plan_doc);return 1;}
        yyjson_mut_arr_add_strcpy(doc,memories,
            memory_override&&index==0?memory_override:yyjson_get_str(value));
    }
    yyjson_arr_foreach(feedback_ids,index,max,value){
        if(!yyjson_is_str(value)){yyjson_mut_doc_free(doc);yyjson_doc_free(plan_doc);return 1;}
        yyjson_mut_arr_add_strcpy(doc,feedback,
            feedback_override&&index==0?feedback_override:yyjson_get_str(value));
    }
    yyjson_mut_obj_add_str(doc,root,"schema","stage14-task-evolution-canary-manifest/v1");
    yyjson_mut_obj_add_strcpy(doc,root,"mode",mode_override?mode_override:yyjson_get_str(mode));
    yyjson_mut_obj_add_strcpy(doc,root,"task_id",task_override?task_override:yyjson_get_str(task));
    yyjson_mut_obj_add_strcpy(doc,root,"idempotency_key",
                               key_override?key_override:yyjson_get_str(key));
    yyjson_mut_obj_add_strcpy(doc,root,"request_sha256",
                               request_override?request_override:yyjson_get_str(request));
    yyjson_mut_obj_add_val(doc,root,"memory_item_ids",memories);
    yyjson_mut_obj_add_val(doc,root,"feedback_event_ids",feedback);
    yyjson_mut_obj_add_int(doc,root,"max_evolution_events",
                           max_events_override>=0?max_events_override:
                           (int)yyjson_get_sint(max_events));
    yyjson_mut_obj_add_int(doc,root,"max_cross_project_edges",
                           max_edges_override>=0?max_edges_override:
                           (int)yyjson_get_sint(max_edges));
    if(add_extra_field)yyjson_mut_obj_add_str(doc,root,"unexpected","fail-closed");
    size_t size=0;char *json=yyjson_mut_write(doc,0,&size);
    yyjson_mut_doc_free(doc);yyjson_doc_free(plan_doc);
    if(!json)return 1;
    FILE *file=fopen(path,"wb");
    int rc=!file||fwrite(json,1,size,file)!=size;
    if(file&&fclose(file)!=0)rc=1;
    if(!rc&&cbm_stage7_sha256_hex(json,size,out_sha256)!=CBM_STORE_OK)rc=1;
    free(json);return rc;
}

static int stage14_pad_manifest_file(const char *path,size_t target_size,
                                     char out_sha256[65]) {
    FILE *file=fopen(path,"rb+");if(!file)return 1;
    if(fseek(file,0,SEEK_END)!=0){fclose(file);return 1;}
    long current=ftell(file);
    if(current<0||(size_t)current>target_size){fclose(file);return 1;}
    char padding[4096];memset(padding,' ',sizeof(padding));
    size_t remaining=target_size-(size_t)current;
    while(remaining){
        size_t chunk=remaining<sizeof(padding)?remaining:sizeof(padding);
        if(fwrite(padding,1,chunk,file)!=chunk){fclose(file);return 1;}
        remaining-=chunk;
    }
    if(fclose(file)!=0)return 1;
    file=fopen(path,"rb");if(!file)return 1;
    char *data=malloc(target_size?target_size:1);
    int rc=!data||fread(data,1,target_size,file)!=target_size||fgetc(file)!=EOF;
    if(fclose(file)!=0)rc=1;
    if(!rc&&cbm_stage7_sha256_hex(data,target_size,out_sha256)!=CBM_STORE_OK)rc=1;
    free(data);return rc;
}

static int stage14_prepare_maintenance_manifests(cbm_store_t *store,const char *project,
        const char *run_id,const char *key,int64_t as_of_ms,const char *edge_path,
        char edge_hash[65],const char *concept_path,char concept_hash[65]) {
    char child_run[256],child_key[256];snprintf(child_run,sizeof(child_run),"%s:edge",run_id);
    cbm_edge_lifecycle_input_t edge={.project=project,.mode="dry_run",.run_id=child_run,
        .as_of_ms=as_of_ms,.algorithm_version=CBM_STAGE9_ALGORITHM_VERSION,
        .policy_sha256=CBM_STAGE9_POLICY_SHA256,.policy_version=CBM_STAGE9_POLICY_VERSION,
        .config_version=CBM_STAGE9_CONFIG_VERSION};cbm_edge_lifecycle_result_t er={0};
    int rc=cbm_store_memory_edge_maintenance(store,&edge,&er);
    if(rc==CBM_STORE_OK)rc=stage14_write_edge_manifest(er.report_json,child_run,edge_path,edge_hash)?CBM_STORE_ERR:CBM_STORE_OK;
    cbm_store_memory_edge_lifecycle_result_free(&er);if(rc!=CBM_STORE_OK)return 1;
    snprintf(child_run,sizeof(child_run),"%s:concept",run_id);snprintf(child_key,sizeof(child_key),"%s:concept",key);
    cbm_concept_generate_input_t concept={.project=project,.store="project-memory",.operation="generate",
        .mode="dry_run",.run_id=child_run,.idempotency_key=child_key,
        .algorithm_version=CBM_STAGE10_ALGORITHM_VERSION,.policy_sha256=CBM_STAGE10_POLICY_SHA256,
        .policy_version=CBM_STAGE10_POLICY_VERSION,.config_version=CBM_STAGE10_CONFIG_VERSION,
        .generator_version=CBM_STAGE10_GENERATOR_VERSION};cbm_concept_result_t cr={0};
    rc=cbm_store_memory_concept_generate(store,&concept,&cr);
    if(rc==CBM_STORE_OK)rc=stage14_write_concept_manifest(cr.report_json,concept_path,concept_hash)?CBM_STORE_ERR:CBM_STORE_OK;
    cbm_store_memory_concept_result_free(&cr);return rc==CBM_STORE_OK?0:1;
}

static int prepare_feedback_task(cbm_global_memory_t *g,const cbm_project_resolution_t *project,
                                 const char *memory_item_id,const char *query_text,
                                 const char *suffix,const char *trust,const char *source,
                                 const char *state,char out_task_id[128]) {
    char request_id[128],session_id[128],turn_id[128],begin_key[128],result_id[128];
    char evidence_id[128],evidence_key[128],complete_key[128];
    snprintf(request_id,sizeof(request_id),"evo-retrieve-%s",suffix);
    cbm_memory_query_t query={.query=query_text,.limit=10};
    cbm_global_retrieval_result_t retrieved={0};
    CHECK(cbm_global_memory_retrieve(g,request_id,project->project_uuid,100000,&query,&retrieved)==CBM_STORE_OK);
    int found=0;for(int i=0;i<retrieved.count;i++)if(!strcmp(retrieved.items[i].item.id,memory_item_id))found=1;
    CHECK(found&&retrieved.session_id);
    snprintf(session_id,sizeof(session_id),"evo-session-%s",suffix);
    snprintf(turn_id,sizeof(turn_id),"evo-turn-%s",suffix);
    snprintf(begin_key,sizeof(begin_key),"evo-begin-%s",suffix);
    cbm_task_begin_input_t begin={.session_id=session_id,.turn_id=turn_id,
        .prompt_sha256="abababababababababababababababababababababababababababababababab",
        .prompt_length=24,.retrieval_session_id=retrieved.session_id,.idempotency_key=begin_key};
    char *report=NULL;
    CHECK(cbm_global_task_begin(g,project,&begin,&report)==CBM_STORE_OK);
    CHECK(report_id(report,"\"task_id\"",out_task_id,128));free(report);report=NULL;
    snprintf(result_id,sizeof(result_id),"evo-result-%s",suffix);
    snprintf(evidence_id,sizeof(evidence_id),"evo-evidence-%s",suffix);
    snprintf(evidence_key,sizeof(evidence_key),"evo-evidence-key-%s",suffix);
    cbm_task_evidence_input_t evidence={.task_id=out_task_id,.result_id=result_id,
        .result_hash="cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd",
        .evidence_id=evidence_id,
        .evidence_hash="efefefefefefefefefefefefefefefefefefefefefefefefefefefefefefefef",
        .evidence_trust=trust,.evidence_source=source,.idempotency_key=evidence_key};
    CHECK(cbm_global_task_record_evidence(g,&evidence,&report)==CBM_STORE_OK);free(report);report=NULL;
    cbm_task_attribution_input_t attribution={.memory_item_id=memory_item_id,.state=state,
        .evidence_id=evidence_id};
    snprintf(complete_key,sizeof(complete_key),"evo-complete-%s",suffix);
    cbm_task_complete_input_t complete={.project=project->project_uuid,.task_id=out_task_id,
        .outcome="completed",.idempotency_key=complete_key,.attributions=&attribution,
        .attribution_count=1};
    CHECK(cbm_global_task_complete(g,&complete,&report)==CBM_STORE_OK);free(report);
    cbm_global_retrieval_result_free(&retrieved);
    return 0;
}

static int prepare_terminal_task(cbm_global_memory_t *g,const cbm_project_resolution_t *project,
                                 const char *suffix,const char *outcome,char out_task_id[128]) {
    char session_id[128],turn_id[128],begin_key[128],complete_key[128];
    snprintf(session_id,sizeof(session_id),"terminal-session-%s",suffix);
    snprintf(turn_id,sizeof(turn_id),"terminal-turn-%s",suffix);
    snprintf(begin_key,sizeof(begin_key),"terminal-begin-%s",suffix);
    cbm_task_begin_input_t begin={.session_id=session_id,.turn_id=turn_id,
        .prompt_sha256="8989898989898989898989898989898989898989898989898989898989898989",
        .prompt_length=12,.idempotency_key=begin_key};
    char *report=NULL;CHECK(cbm_global_task_begin(g,project,&begin,&report)==CBM_STORE_OK);
    CHECK(report_id(report,"\"task_id\"",out_task_id,128));free(report);report=NULL;
    snprintf(complete_key,sizeof(complete_key),"terminal-complete-%s",suffix);
    cbm_task_complete_input_t complete={.project=project->project_uuid,.task_id=out_task_id,
        .outcome=outcome,.idempotency_key=complete_key};
    CHECK(cbm_global_task_complete(g,&complete,&report)==CBM_STORE_OK);free(report);return 0;
}

static int stage14_append_lifecycle_state(sqlite3 *db,const char *task_id,
                                          const char *suffix,const char *state,
                                          const char *outcome) {
    sqlite3_stmt *stmt=NULL;char lifecycle_id[128],key[128];
    snprintf(lifecycle_id,sizeof(lifecycle_id),"stage14-latest-life-%s",suffix);
    snprintf(key,sizeof(key),"stage14-latest-life-key-%s",suffix);
    const char *sql=
        "INSERT INTO codex_task_lifecycle(lifecycle_id,task_id,session_id,turn_id,"
        "prompt_sha256,prompt_length,retrieval_session_id,state,outcome,idempotency_key,"
        "payload_sha256,policy_version,created_at) "
        "SELECT ?2,task_id,session_id,turn_id,prompt_sha256,prompt_length,"
        "retrieval_session_id,?3,?4,?5,"
        "'7171717171717171717171717171717171717171717171717171717171717171',"
        "policy_version,'2000-01-01T00:00:00Z' FROM codex_task_lifecycle "
        "WHERE task_id=?1 ORDER BY rowid DESC LIMIT 1;";
    if(sqlite3_prepare_v2(db,sql,-1,&stmt,NULL)!=SQLITE_OK)return 1;
    sqlite3_bind_text(stmt,1,task_id,-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt,2,lifecycle_id,-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt,3,state,-1,SQLITE_TRANSIENT);
    if(outcome)sqlite3_bind_text(stmt,4,outcome,-1,SQLITE_TRANSIENT);
    else sqlite3_bind_null(stmt,4);
    sqlite3_bind_text(stmt,5,key,-1,SQLITE_TRANSIENT);
    int step=sqlite3_step(stmt);sqlite3_finalize(stmt);
    return step==SQLITE_DONE&&sqlite3_changes(db)==1?0:1;
}

static int observe_chain_feedback(cbm_global_memory_t *g,const cbm_project_resolution_t *project,
                                  const char *task_id,const char *session_id,
                                  const char *candidate_id,const char *usage_id,
                                  const char *suffix,const char *action,
                                  const char *supersedes_event_id,char out_event_id[128],
                                  char out_evidence_id[128]) {
    char result_id[128],result_payload[128],result_hash[65];
    char evidence_payload[128],evidence_hash[65];
    snprintf(out_event_id,128,"chain-feedback-%s",suffix);
    snprintf(result_id,sizeof(result_id),"chain-result-%s",suffix);
    snprintf(out_evidence_id,128,"chain-evidence-%s",suffix);
    snprintf(result_payload,sizeof(result_payload),"stage14 result %s",suffix);
    snprintf(evidence_payload,sizeof(evidence_payload),"stage14 evidence %s",suffix);
    CHECK(cbm_stage7_sha256_hex(result_payload,strlen(result_payload),result_hash)==CBM_STORE_OK);
    CHECK(cbm_stage7_sha256_hex(evidence_payload,strlen(evidence_payload),evidence_hash)==CBM_STORE_OK);
    cbm_feedback_observe_input_t input={
        .project=project->project_uuid,.processing_mode="observe_only",.event_id=out_event_id,
        .task_id=task_id,.task_type="user_task",.session_id=session_id,
        .candidate_id=candidate_id,.usage_id=usage_id,.result_id=result_id,
        .result_type="runtime",.result_status="succeeded",.result_ref="stage14:test",
        .result_hash=result_hash,.result_payload=result_payload,.evidence_id=out_evidence_id,
        .evidence_trust="external_verified",
        .evidence_state=!strcmp(action,"withdraw")?"withdrawn":"valid",
        .evidence_source="test",.evidence_ref="stage14:test",.evidence_hash=evidence_hash,
        .evidence_payload=evidence_payload,.action=action,
        .supersedes_event_id=supersedes_event_id,
        .algorithm_version="stage14-evolution-chain-v1",.config_version=1};
    cbm_feedback_observe_result_t observed={0};
    int rc=cbm_store_memory_feedback_observe(cbm_global_memory_store(g),&input,&observed);
    cbm_store_memory_feedback_observe_result_free(&observed);
    CHECK(rc==CBM_STORE_OK);return 0;
}

static int prepare_existing_feedback_chain(cbm_global_memory_t *g,
                                           const cbm_project_resolution_t *project,
                                           const char *memory_item_id,const char *query_text,
                                           const char *suffix,const char *terminal_action,
                                           char out_task_id[128],char out_terminal_event[128],
                                           char out_terminal_evidence[128]) {
    char request_id[128],session_id[128],turn_id[128],begin_key[128],usage_id[128];
    snprintf(request_id,sizeof(request_id),"chain-retrieve-%s",suffix);
    cbm_memory_query_t query={.query=query_text,.limit=10};
    cbm_global_retrieval_result_t retrieved={0};
    CHECK(cbm_global_memory_retrieve(g,request_id,project->project_uuid,100000,&query,&retrieved)==CBM_STORE_OK);
    const char *candidate_id=NULL;
    for(int i=0;i<retrieved.count;i++)if(!strcmp(retrieved.items[i].item.id,memory_item_id))candidate_id=retrieved.items[i].candidate_id;
    CHECK(candidate_id&&retrieved.session_id);
    snprintf(session_id,sizeof(session_id),"chain-session-%s",suffix);
    snprintf(turn_id,sizeof(turn_id),"chain-turn-%s",suffix);
    snprintf(begin_key,sizeof(begin_key),"chain-begin-%s",suffix);
    cbm_task_begin_input_t begin={.session_id=session_id,.turn_id=turn_id,
        .prompt_sha256="7878787878787878787878787878787878787878787878787878787878787878",
        .prompt_length=24,.retrieval_session_id=retrieved.session_id,.idempotency_key=begin_key};
    char *report=NULL;CHECK(cbm_global_task_begin(g,project,&begin,&report)==CBM_STORE_OK);
    CHECK(report_id(report,"\"task_id\"",out_task_id,128));free(report);report=NULL;
    snprintf(usage_id,sizeof(usage_id),"chain-usage-%s",suffix);
    cbm_observe_usage_input_t usage={.event_id=usage_id,.session_id=retrieved.session_id,
        .candidate_id=candidate_id,.outcome="used",.evidence_type="test",
        .evidence_ref="stage14:test",
        .evidence_hash="6767676767676767676767676767676767676767676767676767676767676767"};
    CHECK(cbm_store_memory_observe_usage(cbm_global_memory_store(g),&usage)==CBM_STORE_OK);
    char root_event[128],root_evidence[128],middle_event[128],middle_evidence[128];
    char root_suffix[128],middle_suffix[128],terminal_suffix[128];
    snprintf(root_suffix,sizeof(root_suffix),"%s-root",suffix);
    snprintf(middle_suffix,sizeof(middle_suffix),"%s-middle",suffix);
    snprintf(terminal_suffix,sizeof(terminal_suffix),"%s-terminal",suffix);
    CHECK(observe_chain_feedback(g,project,out_task_id,retrieved.session_id,candidate_id,
                                 usage_id,root_suffix,"confirm",NULL,root_event,
                                 root_evidence)==0);
    CHECK(observe_chain_feedback(g,project,out_task_id,retrieved.session_id,candidate_id,
                                 usage_id,middle_suffix,"correct",root_event,middle_event,
                                 middle_evidence)==0);
    CHECK(observe_chain_feedback(g,project,out_task_id,retrieved.session_id,candidate_id,
                                 usage_id,terminal_suffix,terminal_action,middle_event,
                                 out_terminal_event,out_terminal_evidence)==0);
    cbm_task_attribution_input_t attribution={.memory_item_id=memory_item_id,.state="used",
        .evidence_id=root_evidence,.feedback_event_id=root_event};
    char complete_key[128];snprintf(complete_key,sizeof(complete_key),"chain-complete-%s",suffix);
    cbm_task_complete_input_t complete={.project=project->project_uuid,.task_id=out_task_id,
        .outcome="completed",.idempotency_key=complete_key,.attributions=&attribution,
        .attribution_count=1};
    sqlite3 *db=cbm_global_memory_db(g);
    int feedback_before=scalar(db,"SELECT COUNT(*) FROM feedback_event;");
    int feedback_attr_before=scalar(db,"SELECT COUNT(*) FROM feedback_attribution;");
    int usage_before=scalar(db,"SELECT COUNT(*) FROM memory_usage_attribution;");
    int audit_before=scalar(db,"SELECT COUNT(*) FROM plasticity_audit_event;");
    if(!strcmp(suffix,"withdraw-chain")){
        int codex_before=scalar(db,"SELECT COUNT(*) FROM codex_task_attribution;");
        int lifecycle_before=scalar(db,"SELECT COUNT(*) FROM codex_task_lifecycle;");
        attribution.feedback_event_id="chain-feedback-correct-chain-root";
        complete.idempotency_key="chain-invalid-withdraw-feedback";
        CHECK(cbm_global_task_complete(g,&complete,&report)==CBM_STORE_REJECTED);free(report);report=NULL;
        CHECK(scalar(db,"SELECT COUNT(*) FROM codex_task_attribution;")==codex_before);
        CHECK(scalar(db,"SELECT COUNT(*) FROM codex_task_lifecycle;")==lifecycle_before);
        CHECK(scalar(db,"SELECT COUNT(*) FROM feedback_event;")==feedback_before);
        attribution.feedback_event_id=root_event;complete.idempotency_key=complete_key;
    }
    CHECK(cbm_global_task_complete(g,&complete,&report)==CBM_STORE_OK);free(report);
    CHECK(scalar(db,"SELECT COUNT(*) FROM feedback_event;")==feedback_before);
    CHECK(scalar(db,"SELECT COUNT(*) FROM feedback_attribution;")==feedback_attr_before);
    CHECK(scalar(db,"SELECT COUNT(*) FROM memory_usage_attribution;")==usage_before);
    CHECK(scalar(db,"SELECT COUNT(*) FROM plasticity_audit_event;")==audit_before);
    int changes_after_complete=sqlite3_total_changes(db);report=NULL;
    CHECK(cbm_global_task_complete(g,&complete,&report)==CBM_STORE_REPLAYED);free(report);report=NULL;
    CHECK(sqlite3_total_changes(db)==changes_after_complete);
    attribution.state="rejected";
    CHECK(cbm_global_task_complete(g,&complete,&report)==CBM_STORE_IDEMPOTENCY_CONFLICT);free(report);
    CHECK(sqlite3_total_changes(db)==changes_after_complete);
    cbm_global_retrieval_result_free(&retrieved);return 0;
}

static int insert_evolution_event(cbm_global_memory_t *g,const char *event_id) {
    sqlite3_stmt *stmt=NULL;sqlite3 *db=cbm_global_memory_db(g);
    const char *sql="INSERT INTO global_evolution_event(event_id,task_id,project_uuid,object_kind,object_id,operation,evidence_grade,evidence_id,before_sha256,after_sha256,algorithm_version,config_version,idempotency_key,payload_sha256,prev_hash,event_hash,created_at) VALUES(?1,NULL,'project-a','memory_item','memory-a','positive','A',NULL,?2,?2,'stage14-test',1,?3,?2,?2,?4,'2026-07-27T00:00:00Z');";
    if(sqlite3_prepare_v2(db,sql,-1,&stmt,NULL)!=SQLITE_OK)return 0;
    char key[256],hash[65];snprintf(key,sizeof(key),"fixture-key-%s",event_id);
    memset(hash,'a',64);hash[64]=0;char event_hash[65];memset(event_hash,'b',64);event_hash[64]=0;
    sqlite3_bind_text(stmt,1,event_id,-1,SQLITE_TRANSIENT);sqlite3_bind_text(stmt,2,hash,-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt,3,key,-1,SQLITE_TRANSIENT);sqlite3_bind_text(stmt,4,event_hash,-1,SQLITE_TRANSIENT);
    int ok=sqlite3_step(stmt)==SQLITE_DONE;sqlite3_finalize(stmt);return ok;
}

static int append_legacy_v2_evolution_event(cbm_global_memory_t *g) {
    sqlite3 *db=cbm_global_memory_db(g);sqlite3_stmt *stmt=NULL;
    char previous[65];memset(previous,'0',64);previous[64]=0;
    if(sqlite3_prepare_v2(db,
            "SELECT event_hash FROM global_evolution_event ORDER BY sequence_no DESC LIMIT 1;",
            -1,&stmt,NULL)!=SQLITE_OK)return 0;
    int step=sqlite3_step(stmt);
    if(step==SQLITE_ROW){
        const char *stored=(const char *)sqlite3_column_text(stmt,0);
        if(!stored||strlen(stored)!=64){sqlite3_finalize(stmt);return 0;}
        snprintf(previous,sizeof(previous),"%s",stored);
    }else if(step!=SQLITE_DONE){sqlite3_finalize(stmt);return 0;}
    sqlite3_finalize(stmt);stmt=NULL;
    char payload[65],before[65],after[65],seed[1024],event_hash[65];
    if(cbm_stage7_sha256_hex("legacy-v2-payload",17,payload)!=CBM_STORE_OK||
       cbm_stage7_sha256_hex("legacy-v2-before",16,before)!=CBM_STORE_OK||
       cbm_stage7_sha256_hex("legacy-v2-after",15,after)!=CBM_STORE_OK)return 0;
    snprintf(seed,sizeof(seed),"%s\n%s\n%s\n%s\n%s\n%s",previous,payload,before,after,
             "memory_item","legacy-v2-memory");
    if(cbm_stage7_sha256_hex(seed,strlen(seed),event_hash)!=CBM_STORE_OK)return 0;
    const char *sql=
        "INSERT INTO global_evolution_event(event_id,task_id,project_uuid,object_kind,"
        "object_id,operation,evidence_grade,evidence_id,before_sha256,after_sha256,"
        "algorithm_version,config_version,idempotency_key,payload_sha256,prev_hash,"
        "event_hash,created_at) VALUES('legacy-v2-compatible','legacy-v2-task',"
        "'legacy-v2-project','memory_item','legacy-v2-memory','positive','A',NULL,"
        "?1,?2,'stage14-evolution-v2',2,'legacy-v2-key',?3,?4,?5,"
        "'2026-07-28T00:00:00Z');";
    if(sqlite3_prepare_v2(db,sql,-1,&stmt,NULL)!=SQLITE_OK)return 0;
    sqlite3_bind_text(stmt,1,before,-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt,2,after,-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt,3,payload,-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt,4,previous,-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt,5,event_hash,-1,SQLITE_TRANSIENT);
    int ok=sqlite3_step(stmt)==SQLITE_DONE;sqlite3_finalize(stmt);return ok;
}

#ifdef _WIN32
static int stage14_evolution_child_main(int argc,char **argv) {
    if(argc!=6)return 90;
    cbm_global_memory_t *g=cbm_global_memory_open(argv[2],argv[3]);
    if(!g)return 91;
    cbm_evolution_task_input_t input={
        .mode="bounded_canary",.task_id=argv[4],.idempotency_key=argv[5],
        .isolated_write_allowed=1,
        .crash_during_commit=!strcmp(argv[1],"--stage14-crash-child")
    };
    cbm_evolution_result_t result={0};
    int rc=cbm_evolution_apply_completed_task(g,&input,&result);
    cbm_evolution_result_free(&result);cbm_global_memory_close(g);
    if(input.crash_during_commit)return 92;
    if(rc==CBM_STORE_OK)return 40;
    if(rc==CBM_STORE_REPLAYED)return 41;
    if(rc==CBM_STORE_IDEMPOTENCY_CONFLICT)return 42;
    if(rc==CBM_STORE_REJECTED)return 43;
    return 49;
}

static int stage14_launch_evolution_child(const char *mode,const char *memory_path,
                                          const char *graph_path,const char *task_id,
                                          const char *key,PROCESS_INFORMATION *process) {
    if(!mode||!memory_path||!graph_path||!task_id||!key||!process)return 0;
    wchar_t executable[4096],mode_w[128],memory_w[4096],graph_w[4096],
            task_w[1024],key_w[1024],command[16384];
    DWORD length=GetModuleFileNameW(NULL,executable,4096);
    if(length==0||length>=4096||
       !stage14_utf8_to_wide(mode,mode_w,128)||
       !stage14_utf8_to_wide(memory_path,memory_w,4096)||
       !stage14_utf8_to_wide(graph_path,graph_w,4096)||
       !stage14_utf8_to_wide(task_id,task_w,1024)||
       !stage14_utf8_to_wide(key,key_w,1024))return 0;
    int used=swprintf(command,16384,L"\"%ls\" \"%ls\" \"%ls\" \"%ls\" \"%ls\" \"%ls\"",
                      executable,mode_w,memory_w,graph_w,task_w,key_w);
    if(used<0||used>=16384)return 0;
    STARTUPINFOW startup;memset(&startup,0,sizeof(startup));startup.cb=sizeof(startup);
    memset(process,0,sizeof(*process));
    if(!CreateProcessW(NULL,command,NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,NULL,
                       &startup,process))return 0;
    CloseHandle(process->hThread);process->hThread=NULL;return 1;
}

static int stage14_wait_evolution_child(PROCESS_INFORMATION *process,DWORD *exit_code) {
    if(!process||!process->hProcess||!exit_code)return 0;
    DWORD waited=WaitForSingleObject(process->hProcess,60000);
    if(waited!=WAIT_OBJECT_0){
        TerminateProcess(process->hProcess,93);
        WaitForSingleObject(process->hProcess,5000);
        CloseHandle(process->hProcess);process->hProcess=NULL;return 0;
    }
    int ok=GetExitCodeProcess(process->hProcess,exit_code)!=0;
    CloseHandle(process->hProcess);process->hProcess=NULL;return ok;
}
#endif

static int test_resolver_frozen_identity(void) {
    cbm_project_resolution_t r={0};
    CHECK(cbm_project_resolve("H:\\Codex_H",NULL,NULL,&r)==0);
    CHECK(strcmp(r.canonical_path,"h:\\codex_h")==0);
    CHECK(strcmp(r.project_uuid,"2fb874ff-b9b3-5d31-997e-793aed30ce00")==0);
    cbm_project_resolution_t uri={0};
    CHECK(cbm_project_resolve("file:///H:/Codex_H/runtime-data/stage14/fixtures/CJK%20%E7%A9%BA%E6%A0%BC%E5%B7%A5%E7%A8%8B",NULL,NULL,&uri)==0);
    CHECK(strstr(uri.canonical_path,"cjk ")!=NULL);
#ifdef _WIN32
    wchar_t temp_w[MAX_PATH],cjk_w[MAX_PATH];char cjk_utf8[MAX_PATH*3];CHECK(GetTempPathW(MAX_PATH,temp_w)>0);swprintf(cjk_w,MAX_PATH,L"%lsstage14-cjk-\x8bb0\x5fc6",temp_w);CreateDirectoryW(cjk_w,NULL);CHECK(WideCharToMultiByte(CP_UTF8,0,cjk_w,-1,cjk_utf8,sizeof(cjk_utf8),NULL,NULL)>0);cbm_project_resolution_t cjk={0};CHECK(cbm_project_resolve(cjk_utf8,NULL,NULL,&cjk)==0);CHECK(cjk.path_exists==1);RemoveDirectoryW(cjk_w);
#endif
    return 0;
}

static int test_real_rename_reuses_identity(void) {
#ifdef _WIN32
    wchar_t temp_w[MAX_PATH],parent_w[MAX_PATH],before_w[MAX_PATH],after_w[MAX_PATH];
    char before_utf8[MAX_PATH*3],after_utf8[MAX_PATH*3];
    CHECK(GetTempPathW(MAX_PATH,temp_w)>0);
    swprintf(parent_w,MAX_PATH,L"%lsstage14-move-%lu",temp_w,(unsigned long)GetCurrentProcessId());
    swprintf(before_w,MAX_PATH,L"%ls\\before",parent_w);swprintf(after_w,MAX_PATH,L"%ls\\after",parent_w);
    CHECK(CreateDirectoryW(parent_w,NULL)!=0);CHECK(CreateDirectoryW(before_w,NULL)!=0);
    CHECK(WideCharToMultiByte(CP_UTF8,0,before_w,-1,before_utf8,sizeof(before_utf8),NULL,NULL)>0);
    CHECK(WideCharToMultiByte(CP_UTF8,0,after_w,-1,after_utf8,sizeof(after_utf8),NULL,NULL)>0);
    cbm_project_resolution_t before={0},after={0};CHECK(cbm_project_resolve(before_utf8,NULL,NULL,&before)==0);
    CHECK(before.source_fingerprint[0]!=0);CHECK(MoveFileW(before_w,after_w)!=0);
    CHECK(cbm_project_resolve(after_utf8,NULL,NULL,&after)==0);
    CHECK(strcmp(before.source_fingerprint,after.source_fingerprint)==0);
    cbm_global_memory_t *g=cbm_global_memory_open(":memory:",":memory:");CHECK(g!=NULL);char *report=NULL;
    CHECK(cbm_global_ensure_project(g,&before,"real-move-before",&report)==CBM_STORE_OK);free(report);report=NULL;
    CHECK(cbm_global_ensure_project(g,&after,"real-move-after",&report)==CBM_STORE_OK);
    CHECK(report&&strstr(report,"\"alias_created\":true"));free(report);
    sqlite3 *db=cbm_global_memory_db(g);CHECK(scalar(db,"SELECT COUNT(*) FROM global_project_catalog;")==1);
    CHECK(scalar(db,"SELECT COUNT(*) FROM global_project_alias;")==1);cbm_global_memory_close(g);
    CHECK(RemoveDirectoryW(after_w)!=0);CHECK(RemoveDirectoryW(parent_w)!=0);
#endif
    return 0;
}

static int test_schema_catalog_alias_replay(void) {
    cbm_global_memory_t *g=cbm_global_memory_open(":memory:",":memory:");CHECK(g!=NULL);
    sqlite3 *db=cbm_global_memory_db(g);
    CHECK(scalar(db,"SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name LIKE 'global_%';")>=10);
    cbm_project_resolution_t a={0},moved={0};
    const char *fp="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    CHECK(cbm_project_resolve("H:\\Codex_H\\runtime-data\\stage14\\fixtures\\empty",NULL,fp,&a)==0);
    char *report=NULL;
    CHECK(cbm_global_ensure_project(g,&a,"ensure-a",&report)==CBM_STORE_OK);
    CHECK(report&&strstr(report,"\"status\":\"recorded\""));free(report);report=NULL;
    int catalog=scalar(db,"SELECT COUNT(*) FROM global_project_catalog;");
    CHECK(cbm_global_ensure_project(g,&a,"ensure-a",&report)==CBM_STORE_REPLAYED);
    CHECK(scalar(db,"SELECT COUNT(*) FROM global_project_catalog;")==catalog);free(report);report=NULL;
    CHECK(cbm_project_resolve("H:\\Codex_H\\runtime-data\\stage14\\fixtures\\empty-moved",NULL,fp,&moved)==0);
    CHECK(cbm_global_ensure_project(g,&moved,"ensure-moved",&report)==CBM_STORE_OK);
    CHECK(report&&strstr(report,"\"alias_created\":true"));
    CHECK(scalar(db,"SELECT COUNT(*) FROM global_project_catalog;")==1);
    CHECK(scalar(db,"SELECT COUNT(*) FROM global_project_alias;")==1);
    free(report);report=NULL;CHECK(cbm_global_ensure_project(g,&moved,"ensure-moved",&report)==CBM_STORE_REPLAYED);CHECK(scalar(db,"SELECT COUNT(*) FROM global_project_alias;")==1);free(report);report=NULL;
    cbm_project_resolution_t altered=moved;snprintf(altered.volume_id,sizeof(altered.volume_id),"altered-volume");CHECK(cbm_global_ensure_project(g,&altered,"ensure-moved",&report)==CBM_STORE_IDEMPOTENCY_CONFLICT);CHECK(scalar(db,"SELECT COUNT(*) FROM global_project_alias;")==1);free(report);cbm_global_memory_close(g);return 0;
}

static int test_catalog_observation_changes_do_not_change_identity(void) {
    cbm_global_memory_t *g=cbm_global_memory_open(":memory:",":memory:");CHECK(g!=NULL);
    cbm_project_resolution_t hook={0},mcp={0};
    CHECK(cbm_project_resolve("H:\\Codex_H",NULL,NULL,&hook)==0);
    CHECK(cbm_project_resolve("H:\\Codex_H","H-Codex_H-neuroplastic-main",NULL,&mcp)==0);
    char *report=NULL;CHECK(cbm_global_ensure_project(g,&hook,"cross-entry-hook",&report)==CBM_STORE_OK);free(report);report=NULL;
    CHECK(cbm_global_ensure_project(g,&mcp,"cross-entry-mcp",&report)==CBM_STORE_REPLAYED);free(report);report=NULL;
    cbm_project_resolution_t observed=mcp;observed.path_writable=!mcp.path_writable;observed.path_exists=1;
    CHECK(cbm_global_ensure_project(g,&observed,"cross-entry-observed",&report)==CBM_STORE_REPLAYED);free(report);report=NULL;
    cbm_task_begin_input_t begin={.session_id="cross-entry-session",.turn_id="cross-entry-turn",
        .prompt_sha256="4545454545454545454545454545454545454545454545454545454545454545",
        .prompt_length=18,.idempotency_key="cross-entry-begin"};
    CHECK(cbm_global_task_begin(g,&mcp,&begin,&report)==CBM_STORE_OK);free(report);
    sqlite3 *db=cbm_global_memory_db(g);CHECK(scalar(db,"SELECT COUNT(*) FROM global_project_catalog;")==1);
    CHECK(scalar(db,"SELECT COUNT(*) FROM global_task_workspace;")==1);cbm_global_memory_close(g);return 0;
}

static int test_global_lifecycle_replay_conflict(void) {
    cbm_global_memory_t *g=cbm_global_memory_open(":memory:",":memory:");CHECK(g!=NULL);
    cbm_project_resolution_t r={0};CHECK(cbm_project_resolve("H:\\Codex_H\\runtime-data\\stage14\\fixtures\\empty",NULL,NULL,&r)==0);
    cbm_task_begin_input_t begin={.project="ignored",.session_id="global-session-1",.turn_id="turn-1",
        .prompt_sha256="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",.prompt_length=10,.idempotency_key="global-begin-1"};
    char *report=NULL,task_id[128];
    int begin_rc=cbm_global_task_begin(g,&r,&begin,&report);
    if(begin_rc!=CBM_STORE_OK) fprintf(stderr,"global begin rc=%d report=%s store=%s\n",begin_rc,report?report:"(null)",cbm_store_error(cbm_global_memory_store(g)));
    CHECK(begin_rc==CBM_STORE_OK);
    CHECK(report_id(report,"\"task_id\"",task_id,sizeof(task_id)));free(report);report=NULL;
    sqlite3 *db=cbm_global_memory_db(g);int lifecycle=scalar(db,"SELECT COUNT(*) FROM codex_task_lifecycle;");
    CHECK(cbm_global_task_begin(g,&r,&begin,&report)==CBM_STORE_REPLAYED);
    CHECK(scalar(db,"SELECT COUNT(*) FROM codex_task_lifecycle;")==lifecycle);free(report);report=NULL;
    begin.prompt_sha256="bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    CHECK(cbm_global_task_begin(g,&r,&begin,&report)==CBM_STORE_IDEMPOTENCY_CONFLICT);
    CHECK(scalar(db,"SELECT COUNT(*) FROM global_task_workspace;")==1);free(report);report=NULL;
    cbm_task_evidence_input_t evidence={.task_id=task_id,.result_id="global-result-1",
        .result_hash="cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
        .evidence_id="global-evidence-1",.evidence_hash="dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd",
        .evidence_trust="external_verified",.evidence_source="test",.idempotency_key="global-evidence-key-1"};
    CHECK(cbm_global_task_record_evidence(g,&evidence,&report)==CBM_STORE_OK);free(report);report=NULL;
    cbm_task_complete_input_t complete={.project=r.project_uuid,.task_id=task_id,.outcome="completed",.idempotency_key="global-complete-key-1"};
    CHECK(cbm_global_task_complete(g,&complete,&report)==CBM_STORE_OK);free(report);report=NULL;
    CHECK(cbm_global_task_complete(g,&complete,&report)==CBM_STORE_REPLAYED);free(report);
    CHECK(scalar(db,"SELECT COUNT(*) FROM feedback_event;")==0);
    cbm_global_memory_close(g);return 0;
}

static int test_global_unknown_workspace_chain_and_atomicity(void) {
    cbm_global_memory_t *g=cbm_global_memory_open(":memory:",":memory:");CHECK(g!=NULL);
    sqlite3 *db=cbm_global_memory_db(g);
    cbm_project_resolution_t failed={0};
    CHECK(cbm_project_resolve("H:\\Stage14 Unknown 空格\\failed",NULL,NULL,&failed)==0);
    CHECK(sqlite3_exec(db,"CREATE TRIGGER fail_global_workspace BEFORE INSERT ON global_task_workspace BEGIN SELECT RAISE(ABORT,'fixture'); END;",NULL,NULL,NULL)==SQLITE_OK);
    cbm_task_begin_input_t failed_begin={.session_id="atomic-session",.turn_id="atomic-turn",
        .prompt_sha256="1212121212121212121212121212121212121212121212121212121212121212",
        .prompt_length=12,.idempotency_key="atomic-begin-key"};
    char *report=NULL;
    CHECK(cbm_global_task_begin(g,&failed,&failed_begin,&report)==CBM_STORE_ERR);free(report);report=NULL;
    CHECK(scalar(db,"SELECT COUNT(*) FROM global_project_catalog;")==0);
    CHECK(scalar(db,"SELECT COUNT(*) FROM memory_task;")==0);
    CHECK(scalar(db,"SELECT COUNT(*) FROM codex_task_lifecycle;")==0);
    CHECK(sqlite3_exec(db,"DROP TRIGGER fail_global_workspace;",NULL,NULL,NULL)==SQLITE_OK);

    cbm_project_resolution_t project={0};
    CHECK(cbm_project_resolve("H:\\Stage14 Unknown 空格\\工作区",NULL,NULL,&project)==0);
    cbm_memory_item_t memory=item("unknown-chain-memory","source-project","unknown chain marker",0.9);
    char *new_id=NULL;CHECK(cbm_store_memory_append_candidate(cbm_global_memory_store(g),&memory,&new_id)==CBM_STORE_OK);free(new_id);
    CHECK(cbm_store_memory_index_candidate(cbm_global_memory_store(g),&memory,memory.id,NULL)==CBM_STORE_OK);
    cbm_memory_query_t query={.query="unknown chain marker",.limit=5};
    cbm_global_retrieval_result_t retrieved={0};
    CHECK(cbm_global_memory_retrieve(g,"unknown-retrieval",project.project_uuid,100000,&query,&retrieved)==CBM_STORE_OK);
    CHECK(retrieved.count==1&&retrieved.items[0].candidate_id&&retrieved.items[0].provenance_id);
    char *candidate_id=strdup(retrieved.items[0].candidate_id);
    char *provenance_id=strdup(retrieved.items[0].provenance_id);
    char *retrieval_evidence_id=strdup(retrieved.items[0].evidence_id);
    CHECK(candidate_id&&provenance_id&&retrieval_evidence_id);
    int retrieval_sessions=scalar(db,"SELECT COUNT(*) FROM retrieval_session;");
    int retrieval_candidates=scalar(db,"SELECT COUNT(*) FROM retrieval_candidate;");
    int retrieval_contexts=scalar(db,"SELECT COUNT(*) FROM global_retrieval_project_context;");
    cbm_global_retrieval_result_t replayed={0};
    CHECK(cbm_global_memory_retrieve(g,"unknown-retrieval",project.project_uuid,100000,&query,&replayed)==CBM_STORE_REPLAYED);
    CHECK(replayed.count==1);
    CHECK(strcmp(replayed.items[0].candidate_id,candidate_id)==0);
    CHECK(strcmp(replayed.items[0].provenance_id,provenance_id)==0);
    CHECK(strcmp(replayed.items[0].evidence_id,retrieval_evidence_id)==0);
    CHECK(scalar(db,"SELECT COUNT(*) FROM retrieval_session;")==retrieval_sessions);
    CHECK(scalar(db,"SELECT COUNT(*) FROM retrieval_candidate;")==retrieval_candidates);
    CHECK(scalar(db,"SELECT COUNT(*) FROM global_retrieval_project_context;")==retrieval_contexts);
    cbm_global_retrieval_result_free(&replayed);
    free(candidate_id);free(provenance_id);free(retrieval_evidence_id);

    CHECK(sqlite3_exec(db,"CREATE TRIGGER fail_global_retrieval_context BEFORE INSERT ON global_retrieval_project_context BEGIN SELECT RAISE(ABORT,'fixture'); END;",NULL,NULL,NULL)==SQLITE_OK);
    cbm_global_retrieval_result_t failed_retrieval={0};
    CHECK(cbm_global_memory_retrieve(g,"atomic-retrieval",project.project_uuid,100000,&query,&failed_retrieval)==CBM_STORE_ERR);
    CHECK(scalar(db,"SELECT COUNT(*) FROM retrieval_session WHERE id='atomic-retrieval';")==0);
    CHECK(scalar(db,"SELECT COUNT(*) FROM retrieval_candidate WHERE session_id='atomic-retrieval';")==0);
    CHECK(scalar(db,"SELECT COUNT(*) FROM global_retrieval_project_context WHERE session_id='atomic-retrieval';")==0);
    CHECK(sqlite3_exec(db,"DROP TRIGGER fail_global_retrieval_context;",NULL,NULL,NULL)==SQLITE_OK);
    cbm_task_begin_input_t begin={.session_id="unknown-session",.turn_id="unknown-turn",
        .prompt_sha256="3434343434343434343434343434343434343434343434343434343434343434",
        .prompt_length=20,.retrieval_session_id="unknown-retrieval",.idempotency_key="unknown-begin"};
    char task_id[128];CHECK(cbm_global_task_begin(g,&project,&begin,&report)==CBM_STORE_OK);
    CHECK(report_id(report,"\"task_id\"",task_id,sizeof(task_id)));free(report);report=NULL;
    CHECK(scalar(db,"SELECT COUNT(*) FROM global_project_catalog;")==1);
    CHECK(scalar(db,"SELECT COUNT(*) FROM global_task_workspace;")==1);
    CHECK(scalar(db,"SELECT COUNT(*) FROM pragma_table_info('global_task_workspace') WHERE name='idempotency_key';")==0);
    CHECK(scalar(db,"SELECT COUNT(*) FROM global_task_workspace w JOIN codex_task_lifecycle l ON l.task_id=w.task_id WHERE l.idempotency_key='unknown-begin';")==1);
    CHECK(scalar(db,"SELECT COUNT(*) FROM retrieval_session WHERE id='unknown-retrieval' AND status='completed';")==1);
    cbm_task_evidence_input_t evidence={.task_id=task_id,.result_id="unknown-result",
        .result_hash="5656565656565656565656565656565656565656565656565656565656565656",
        .evidence_id="unknown-evidence",.evidence_hash="7878787878787878787878787878787878787878787878787878787878787878",
        .evidence_trust="external_verified",.evidence_source="test",.idempotency_key="unknown-evidence-key"};
    CHECK(cbm_global_task_record_evidence(g,&evidence,&report)==CBM_STORE_OK);free(report);report=NULL;
    char *found_project=NULL;
    CHECK(cbm_global_task_status(g,NULL,"unknown-session","unknown-turn",&found_project,&report)==CBM_STORE_OK);
    CHECK(found_project&&strcmp(found_project,project.project_uuid)==0);free(found_project);free(report);report=NULL;
    cbm_task_complete_input_t complete={.project="wrong-project-is-not-a-store-boundary",.task_id=task_id,
        .outcome="completed",.idempotency_key="unknown-complete"};
    CHECK(cbm_global_task_complete(g,&complete,&report)==CBM_STORE_OK);free(report);report=NULL;
    int terminal_rows=scalar(db,"SELECT COUNT(*) FROM codex_task_lifecycle WHERE state='completed';");
    CHECK(cbm_global_task_complete(g,&complete,&report)==CBM_STORE_REPLAYED);free(report);report=NULL;
    CHECK(scalar(db,"SELECT COUNT(*) FROM codex_task_lifecycle WHERE state='completed';")==terminal_rows);
    CHECK(cbm_global_task_abandon(g,"unknown-session","unknown-turn","closed-stop",&report)==CBM_STORE_REPLAYED);free(report);report=NULL;
    CHECK(scalar(db,"SELECT COUNT(*) FROM codex_task_lifecycle WHERE state='abandoned';")==0);

    cbm_project_resolution_t altered=project;
    snprintf(altered.display_name,sizeof(altered.display_name),"altered workspace payload");
    CHECK(cbm_global_task_begin(g,&altered,&begin,&report)==CBM_STORE_IDEMPOTENCY_CONFLICT);free(report);
    CHECK(scalar(db,"SELECT COUNT(*) FROM global_project_catalog;")==1);
    cbm_global_retrieval_result_free(&retrieved);
    cbm_global_memory_close(g);return 0;
}

static int test_global_pool_expands_before_soft_boost_limit(void) {
    cbm_global_memory_t *g=cbm_global_memory_open(":memory:",":memory:");CHECK(g!=NULL);
    cbm_store_t *store=cbm_global_memory_store(g);char *new_id=NULL;
    cbm_memory_item_t other=item("boost-other","project-other","boost contract",0.86);
    cbm_memory_item_t current=item("boost-current","project-current","boost contract",0.80);
    CHECK(cbm_store_memory_append_candidate(store,&other,&new_id)==CBM_STORE_OK);free(new_id);new_id=NULL;
    CHECK(cbm_store_memory_append_candidate(store,&current,&new_id)==CBM_STORE_OK);free(new_id);
    CHECK(cbm_store_memory_index_candidate(store,&other,other.id,NULL)==CBM_STORE_OK);
    CHECK(cbm_store_memory_index_candidate(store,&current,current.id,NULL)==CBM_STORE_OK);
    cbm_memory_query_t query={.kind="lesson",.limit=1};cbm_global_retrieval_result_t out={0};
    CHECK(cbm_global_memory_retrieve(g,"boost-before-limit","project-current",250000,&query,&out)==CBM_STORE_OK);
    CHECK(out.count==1);CHECK(strcmp(out.items[0].item.id,"boost-current")==0);
    CHECK(out.items[0].project_soft_boost_ppm==250000);
    cbm_global_retrieval_result_free(&out);cbm_global_memory_close(g);return 0;
}

static cbm_memory_item_t item(const char *id,const char *project,const char *content,double importance) {
    cbm_memory_item_t value={0};value.id=id;value.kind="lesson";value.layer="episodic";value.title=content;
    value.summary=content;value.content=content;value.scope_project=project;value.importance=importance;
    value.confidence=importance;value.reusability=importance;value.specificity=importance;value.status="active";value.version=1;
    return value;
}

typedef struct {
    int rank;
    int hit_at_1;
    int hit_at_k;
    double mrr;
} stage14_rank_metrics_t;

static int stage14_measure_rank(cbm_global_memory_t *g,const char *request_id,
                                const char *query_text,const char *target_id,int k,
                                stage14_rank_metrics_t *metrics) {
    cbm_memory_query_t query={.query=query_text,.kind="lesson",.limit=10};
    cbm_global_retrieval_result_t out={0};
    if(cbm_global_memory_retrieve(g,request_id,"stage14-e-metric-project",0,&query,&out)!=CBM_STORE_OK)return 0;
    memset(metrics,0,sizeof(*metrics));
    for(int i=0;i<out.count;i++)if(!strcmp(out.items[i].item.id,target_id)){metrics->rank=i+1;break;}
    if(metrics->rank>0){metrics->hit_at_1=metrics->rank==1;metrics->hit_at_k=metrics->rank<=k;metrics->mrr=1.0/(double)metrics->rank;}
    cbm_global_retrieval_result_free(&out);return metrics->rank>0;
}

static int stage14_add_rank_item(cbm_global_memory_t *g,cbm_memory_item_t *memory) {
    char *new_id=NULL;cbm_store_t *store=cbm_global_memory_store(g);
    int rc=cbm_store_memory_append_candidate(store,memory,&new_id);free(new_id);
    if(rc!=CBM_STORE_OK)return 0;
    return cbm_store_memory_index_candidate(store,memory,memory->id,NULL)==CBM_STORE_OK;
}

static int test_stage14_e_isolated_ab_rank_metrics(void) {
#ifdef _WIN32
    char temp[MAX_PATH],memory_path[MAX_PATH],graph_path[MAX_PATH];CHECK(GetTempPathA(MAX_PATH,temp)>0);
    snprintf(memory_path,sizeof(memory_path),"%sstage14-e-ab-%lu-%lld-memory.db",temp,
             (unsigned long)_getpid(),(long long)time(NULL));
    snprintf(graph_path,sizeof(graph_path),"%sstage14-e-ab-%lu-%lld-graph.db",temp,
             (unsigned long)_getpid(),(long long)time(NULL));
    cbm_global_memory_t *g=cbm_global_memory_open(memory_path,graph_path);CHECK(g!=NULL);
    cbm_project_resolution_t target_project={0};
    CHECK(cbm_project_resolve("H:\\Stage14 E Isolated A B Target",NULL,NULL,&target_project)==0);
    sqlite3 *db=cbm_global_memory_db(g),*graph=cbm_global_graph_db(g);
    stage14_rank_metrics_t before={0},after={0};cbm_evolution_result_t result={0};char task_id[128];

    const char *a_query="Restore build cache after dependency lockfile corruption";
    cbm_memory_item_t a_target=item("stage14-e-a-target","stage14-e-source-a",a_query,0.4900);
    cbm_memory_item_t a_d1=item("stage14-e-a-distractor-1","stage14-e-source-d1",a_query,0.5000);
    cbm_memory_item_t a_d2=item("stage14-e-a-distractor-2","stage14-e-source-d2",a_query,0.4975);
    CHECK(stage14_add_rank_item(g,&a_target));CHECK(stage14_add_rank_item(g,&a_d1));CHECK(stage14_add_rank_item(g,&a_d2));
    CHECK(stage14_measure_rank(g,"stage14-e-a-before",a_query,a_target.id,2,&before));
    CHECK(before.rank==3&&before.hit_at_1==0&&before.hit_at_k==0&&before.mrr>0.33&&before.mrr<0.34);
    CHECK(prepare_feedback_task(g,&target_project,a_target.id,a_target.content,"stage14-e-a",
                                "external_verified","test","used",task_id)==0);
    cbm_evolution_task_input_t input={.mode="bounded_canary",.task_id=task_id,
        .idempotency_key="stage14-e-a-controller",.isolated_write_allowed=1};
    CHECK(cbm_evolution_apply_completed_task(g,&input,&result)==CBM_STORE_OK);
    CHECK(result.eligible==1&&result.positive==1&&result.negative==0);cbm_evolution_result_free(&result);
    CHECK(stage14_measure_rank(g,"stage14-e-a-after",a_query,a_target.id,2,&after));
    CHECK(after.rank==1&&after.hit_at_1==1&&after.hit_at_k==1&&after.mrr==1.0);
    CHECK(after.hit_at_1>before.hit_at_1&&after.hit_at_k>before.hit_at_k&&after.mrr>before.mrr);
    int events=scalar(db,"SELECT COUNT(*) FROM global_evolution_event;");
    int versions=scalar(graph,"SELECT COUNT(*) FROM global_cross_project_edge_version;");
    double confidence=scalar_double(db,"SELECT confidence FROM memory_item WHERE id='stage14-e-a-target';");
    CHECK(cbm_evolution_apply_completed_task(g,&input,&result)==CBM_STORE_REPLAYED);CHECK(result.wrote==0);cbm_evolution_result_free(&result);
    CHECK(scalar(db,"SELECT COUNT(*) FROM global_evolution_event;")==events&&scalar(graph,"SELECT COUNT(*) FROM global_cross_project_edge_version;")==versions);
    CHECK(scalar_double(db,"SELECT confidence FROM memory_item WHERE id='stage14-e-a-target';")==confidence);
    input.mode="active";CHECK(cbm_evolution_apply_completed_task(g,&input,&result)==CBM_STORE_IDEMPOTENCY_CONFLICT);cbm_evolution_result_free(&result);input.mode="bounded_canary";
    CHECK(scalar(db,"SELECT COUNT(*) FROM global_evolution_event;")==events&&scalar(graph,"SELECT COUNT(*) FROM global_cross_project_edge_version;")==versions);
    CHECK(scalar_double(db,"SELECT confidence FROM memory_item WHERE id='stage14-e-a-target';")==confidence);
    fprintf(stderr,"stage14-e-ab grade=A before_hit1=%d before_hit2=%d before_mrr=%.6f after_hit1=%d after_hit2=%d after_mrr=%.6f\n",before.hit_at_1,before.hit_at_k,before.mrr,after.hit_at_1,after.hit_at_k,after.mrr);

    const char *b_query="Verify database backup integrity before schema migration";
    cbm_memory_item_t b_target=item("stage14-e-b-target","stage14-e-source-b",b_query,0.4900);
    cbm_memory_item_t b_d1=item("stage14-e-b-distractor-1","stage14-e-source-bd1",b_query,0.4950);
    cbm_memory_item_t b_d2=item("stage14-e-b-distractor-2","stage14-e-source-bd2",b_query,0.4925);
    CHECK(stage14_add_rank_item(g,&b_target));CHECK(stage14_add_rank_item(g,&b_d1));CHECK(stage14_add_rank_item(g,&b_d2));
    CHECK(stage14_measure_rank(g,"stage14-e-b-before",b_query,b_target.id,2,&before));CHECK(before.rank==3&&before.hit_at_1==0&&before.hit_at_k==0);
    CHECK(prepare_feedback_task(g,&target_project,b_target.id,b_target.content,"stage14-e-b",
                                "explicit_user","user","used",task_id)==0);
    input.task_id=task_id;input.idempotency_key="stage14-e-b-controller";
    CHECK(cbm_evolution_apply_completed_task(g,&input,&result)==CBM_STORE_OK);CHECK(result.eligible==1&&result.positive==1);cbm_evolution_result_free(&result);
    CHECK(stage14_measure_rank(g,"stage14-e-b-after",b_query,b_target.id,2,&after));
    CHECK(after.rank==1&&after.hit_at_1>before.hit_at_1&&after.hit_at_k>before.hit_at_k&&after.mrr>before.mrr);
    fprintf(stderr,"stage14-e-ab grade=B before_hit1=%d before_hit2=%d before_mrr=%.6f after_hit1=%d after_hit2=%d after_mrr=%.6f\n",before.hit_at_1,before.hit_at_k,before.mrr,after.hit_at_1,after.hit_at_k,after.mrr);

    const char *c_query="Record model suggestion without changing verified ranking";
    cbm_memory_item_t c_target=item("stage14-e-c-target","stage14-e-source-c",c_query,0.4900);
    cbm_memory_item_t c_d1=item("stage14-e-c-distractor","stage14-e-source-cd",c_query,0.5000);
    CHECK(stage14_add_rank_item(g,&c_target));CHECK(stage14_add_rank_item(g,&c_d1));
    CHECK(stage14_measure_rank(g,"stage14-e-c-before",c_query,c_target.id,1,&before));
    CHECK(prepare_feedback_task(g,&target_project,c_target.id,c_target.content,"stage14-e-c",
                                "model_self_report","model","used",task_id)==0);
    input.task_id=task_id;input.idempotency_key="stage14-e-c-controller";
    CHECK(cbm_evolution_apply_completed_task(g,&input,&result)==CBM_STORE_OK);CHECK(result.positive==1);cbm_evolution_result_free(&result);
    CHECK(stage14_measure_rank(g,"stage14-e-c-after",c_query,c_target.id,1,&after));
    CHECK(after.rank==before.rank&&after.hit_at_1==before.hit_at_1&&after.hit_at_k==before.hit_at_k&&after.mrr==before.mrr);
    fprintf(stderr,"stage14-e-ab contamination=C before_rank=%d after_rank=%d before_mrr=%.6f after_mrr=%.6f\n",before.rank,after.rank,before.mrr,after.mrr);

    const char *n_query="Reject stale deployment advice after rollback failure";
    cbm_memory_item_t n_target=item("stage14-e-negative-target","stage14-e-source-n",n_query,0.5000);
    cbm_memory_item_t n_d1=item("stage14-e-negative-distractor","stage14-e-source-nd",n_query,0.4900);
    CHECK(stage14_add_rank_item(g,&n_target));CHECK(stage14_add_rank_item(g,&n_d1));
    CHECK(stage14_measure_rank(g,"stage14-e-negative-before",n_query,n_target.id,1,&before));CHECK(before.rank==1);
    CHECK(prepare_feedback_task(g,&target_project,n_target.id,n_target.content,"stage14-e-negative",
                                "external_verified","test","rejected",task_id)==0);
    input.task_id=task_id;input.idempotency_key="stage14-e-negative-controller";
    CHECK(cbm_evolution_apply_completed_task(g,&input,&result)==CBM_STORE_OK);CHECK(result.negative==1);cbm_evolution_result_free(&result);
    CHECK(stage14_measure_rank(g,"stage14-e-negative-after",n_query,n_target.id,1,&after));
    CHECK(after.rank>=before.rank&&after.hit_at_1<=before.hit_at_1&&after.hit_at_k<=before.hit_at_k&&after.mrr<=before.mrr);
    fprintf(stderr,"stage14-e-ab negative before_rank=%d after_rank=%d before_mrr=%.6f after_mrr=%.6f\n",before.rank,after.rank,before.mrr,after.mrr);
    cbm_global_memory_close(g);
#endif
    return 0;
}

static int test_global_pool_soft_boost_and_provenance(void) {
    cbm_global_memory_t *g=cbm_global_memory_open(":memory:",":memory:");CHECK(g!=NULL);
    cbm_store_t *store=cbm_global_memory_store(g);char *id=NULL;
    cbm_memory_item_t a=item("global-memory-a","project-a","rare-stage14-recovery",0.95);
    cbm_memory_item_t b=item("global-memory-b","project-b","rare-stage14-recovery",0.30);
    CHECK(cbm_store_memory_append_candidate(store,&a,&id)==CBM_STORE_OK);free(id);id=NULL;
    CHECK(cbm_store_memory_append_candidate(store,&b,&id)==CBM_STORE_OK);free(id);
    CHECK(cbm_store_memory_index_candidate(store,&a,a.id,NULL)==CBM_STORE_OK);
    CHECK(cbm_store_memory_index_candidate(store,&b,b.id,NULL)==CBM_STORE_OK);
    sqlite3 *db=cbm_global_memory_db(g);CHECK(sqlite3_exec(db,"INSERT INTO global_memory_provenance(memory_item_id,project_uuid,legacy_project_id,source_kind,payload_sha256,created_at) VALUES('global-memory-b','project-b','legacy-project-b','legacy_migration','aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa','2026-07-27T00:00:00Z');",NULL,NULL,NULL)==SQLITE_OK);
    cbm_memory_query_t q={.kind="lesson",.limit=10};cbm_global_retrieval_result_t out={0};
    CHECK(cbm_global_memory_retrieve(g,"global-retrieve-1","project-b",250000,&q,&out)==CBM_STORE_OK);
    CHECK(out.count==2);CHECK(strcmp(out.items[0].item.id,"global-memory-a")==0);
    CHECK(out.items[0].project_uuid&&strcmp(out.items[0].project_uuid,"project-a")==0);
    CHECK(out.items[1].project_uuid&&strcmp(out.items[1].project_uuid,"project-b")==0);CHECK(out.items[1].legacy_project_id&&strcmp(out.items[1].legacy_project_id,"legacy-project-b")==0);
    CHECK(strcmp(out.candidate_pool,"global")==0);
    cbm_global_retrieval_result_free(&out);CHECK(cbm_global_memory_retrieve(g,"global-retrieve-1","project-b",250000,&q,&out)==CBM_STORE_REPLAYED);cbm_global_retrieval_result_free(&out);q.query="altered-query";CHECK(cbm_global_memory_retrieve(g,"global-retrieve-1","project-b",250000,&q,&out)==CBM_STORE_IDEMPOTENCY_CONFLICT);cbm_global_memory_close(g);return 0;
}

static int test_cross_project_edge_replay_conflict(void) {
    cbm_global_memory_t *g=cbm_global_memory_open(":memory:",":memory:");CHECK(g!=NULL);
    char *report=NULL;
    CHECK(cbm_global_cross_project_edge(g,"edge-missing","project-a","project-b","reuses",900000,800000,"active",1,"missing-evolution","edge-missing-key",&report)==CBM_STORE_REJECTED);free(report);report=NULL;
    CHECK(scalar(cbm_global_graph_db(g),"SELECT COUNT(*) FROM global_cross_project_edge_version;")==0);
    CHECK(insert_evolution_event(g,"evidence-1"));
    CHECK(cbm_global_cross_project_edge(g,"edge-1","project-a","project-b","reuses",900000,800000,"active",1,"evidence-1","edge-key-1",&report)==CBM_STORE_OK);free(report);report=NULL;
    sqlite3 *db=cbm_global_graph_db(g);CHECK(scalar(db,"SELECT COUNT(*) FROM global_cross_project_edge_version;")==1);
    CHECK(cbm_global_cross_project_edge(g,"edge-1","project-a","project-b","reuses",900000,800000,"active",1,"evidence-1","edge-key-1",&report)==CBM_STORE_REPLAYED);free(report);report=NULL;
    CHECK(cbm_global_cross_project_edge(g,"edge-1","project-a","project-b","contradicts",900000,800000,"active",1,"evidence-1","edge-key-1",&report)==CBM_STORE_IDEMPOTENCY_CONFLICT);free(report);
    CHECK(scalar(db,"SELECT COUNT(*) FROM global_cross_project_edge_version;")==1);
    cbm_global_memory_close(g);return 0;
}

#ifdef _WIN32
typedef struct {
    HANDLE blocked;
    HANDLE proceed;
    volatile LONG begin_count;
} stage14_maintenance_authorizer_gate_t;

typedef struct {
    cbm_global_memory_t *global;
    const cbm_evolution_maintenance_input_t *input;
    cbm_evolution_result_t result;
    int rc;
} stage14_maintenance_thread_t;

static int stage14_maintenance_authorizer_gate(void *opaque,int action,
        const char *first,const char *second,const char *database,const char *trigger) {
    (void)second;(void)database;(void)trigger;
    stage14_maintenance_authorizer_gate_t *gate=opaque;
    if(action==SQLITE_TRANSACTION&&gate&&first&&!_stricmp(first,"BEGIN")&&
       InterlockedIncrement(&gate->begin_count)==2){
        SetEvent(gate->blocked);
        WaitForSingleObject(gate->proceed,30000);
    }
    return SQLITE_OK;
}

static unsigned __stdcall stage14_maintenance_thread_main(void *opaque) {
    stage14_maintenance_thread_t *thread=opaque;
    thread->rc=cbm_evolution_maintenance(thread->global,thread->input,&thread->result);
    return 0;
}

static void stage14_remove_sqlite_fixture(const char *path) {
    char sidecar[MAX_PATH+16];
    if(!path||!path[0])return;
    remove(path);
    snprintf(sidecar,sizeof(sidecar),"%s-wal",path);remove(sidecar);
    snprintf(sidecar,sizeof(sidecar),"%s-shm",path);remove(sidecar);
    snprintf(sidecar,sizeof(sidecar),"%s-journal",path);remove(sidecar);
}

static int stage14_force_maintenance_temp_failure=0;

static int stage14_maintenance_fencing_probe(void) {
    char temp[MAX_PATH]={0},memory_path[MAX_PATH]={0},graph_path[MAX_PATH]={0};
    char edge_path[MAX_PATH]={0},concept_path[MAX_PATH]={0};
    char edge_hash[65],concept_hash[65];
    cbm_global_memory_t *first=NULL,*second=NULL;
    HANDLE first_handle=NULL;
    stage14_maintenance_authorizer_gate_t first_gate={0};
    stage14_maintenance_thread_t first_thread={0},second_thread={0};
    int setup_ok=0,first_blocked=0,paths_ready=0;
    int generation_before=-1,run_after_first=-1,lease_after_first=-1;
    int phase_events_before=-1,phase_events_after_first=-1;
    int run_final=-1,lease_final=-1;
    char operations_after_first[512]={0};
    unsigned thread_id=0;
    ULONGLONG nonce=GetTickCount64();
    DWORD temp_length=stage14_force_maintenance_temp_failure
        ?0:GetTempPathA((DWORD)sizeof(temp),temp);
    if(temp_length==0||temp_length>=sizeof(temp))goto cleanup;
    int memory_used=snprintf(memory_path,sizeof(memory_path),
        "%sstage14-maint-fence-%lu-%llu-memory.db",temp,
        (unsigned long)_getpid(),(unsigned long long)nonce);
    int graph_used=snprintf(graph_path,sizeof(graph_path),
        "%sstage14-maint-fence-%lu-%llu-graph.db",temp,
        (unsigned long)_getpid(),(unsigned long long)nonce);
    int edge_used=snprintf(edge_path,sizeof(edge_path),
        "%sstage14-maint-fence-%lu-%llu-edge.json",temp,
        (unsigned long)_getpid(),(unsigned long long)nonce);
    int concept_used=snprintf(concept_path,sizeof(concept_path),
        "%sstage14-maint-fence-%lu-%llu-concept.json",temp,
        (unsigned long)_getpid(),(unsigned long long)nonce);
    if(memory_used<0||(size_t)memory_used>=sizeof(memory_path)||
       graph_used<0||(size_t)graph_used>=sizeof(graph_path)||
       edge_used<0||(size_t)edge_used>=sizeof(edge_path)||
       concept_used<0||(size_t)concept_used>=sizeof(concept_path))goto cleanup;
    paths_ready=1;
    first=cbm_global_memory_open(memory_path,graph_path);
    second=cbm_global_memory_open(memory_path,graph_path);
    if(!first||!second)goto cleanup;
    const char *project="stage14-maintenance-fencing";
    const char *run_id="stage14-maintenance-fencing-run";
    const char *key="stage14-maintenance-fencing-key";
    if(stage14_prepare_maintenance_manifests(
           cbm_global_memory_store(first),project,run_id,key,INT64_C(1893456000000),
           edge_path,edge_hash,concept_path,concept_hash)!=0)goto cleanup;
    cbm_evolution_maintenance_input_t input={
        .mode="active",.project_uuid=project,.owner_id="shared-maintenance-owner",
        .idempotency_key=key,.run_id=run_id,.edge_manifest_path=edge_path,
        .edge_manifest_sha256=edge_hash,.concept_manifest_path=concept_path,
        .concept_manifest_sha256=concept_hash,.frozen_as_of_ms=INT64_C(1893456000000),
        .limit=1,.budget_seconds=30,.isolated_write_allowed=1,
        .test_budget_after_steps=1
    };
    first_gate.blocked=CreateEventW(NULL,TRUE,FALSE,NULL);
    first_gate.proceed=CreateEventW(NULL,TRUE,FALSE,NULL);
    if(!first_gate.blocked||!first_gate.proceed)goto cleanup;
    sqlite3 *first_db=cbm_global_memory_db(first),*second_db=cbm_global_memory_db(second);
    if(sqlite3_set_authorizer(first_db,stage14_maintenance_authorizer_gate,
                              &first_gate)!=SQLITE_OK)goto cleanup;
    stage14_set_env("CBM_STAGE14_ACTIVE_FIXTURE","1");
    first_thread.global=first;first_thread.input=&input;
    second_thread.global=second;second_thread.input=&input;
    first_handle=(HANDLE)_beginthreadex(NULL,0,stage14_maintenance_thread_main,
                                        &first_thread,0,&thread_id);
    if(!first_handle)goto cleanup;
    first_blocked=WaitForSingleObject(first_gate.blocked,30000)==WAIT_OBJECT_0;
    if(!first_blocked)goto cleanup;
    if(sqlite3_exec(second_db,
        "BEGIN IMMEDIATE;"
        "UPDATE global_maintenance_run SET "
        "checkpoint_json=json_set(checkpoint_json,'$.attempt',2) "
        "WHERE run_id='stage14-maintenance-fencing-run' AND "
        "CAST(json_extract(checkpoint_json,'$.attempt') AS INTEGER)=1;"
        "UPDATE global_maintenance_lease SET "
        "acquired_at='2030-01-01T00:00:00Z',expires_at='2099-01-01T00:00:00Z',"
        "checkpoint_json=json_set(checkpoint_json,'$.attempt',2) "
        "WHERE lease_name='stage14-global-maintenance' AND "
        "owner_id='shared-maintenance-owner' AND "
        "CAST(json_extract(checkpoint_json,'$.attempt') AS INTEGER)=1;"
        "COMMIT;",NULL,NULL,NULL)!=SQLITE_OK||
       sqlite3_changes(second_db)!=1)goto cleanup;
    generation_before=scalar(second_db,
        "SELECT CAST(json_extract(checkpoint_json,'$.attempt') AS INTEGER) "
        "FROM global_maintenance_lease WHERE lease_name='stage14-global-maintenance';");
    phase_events_before=scalar(second_db,
        "SELECT COUNT(*) FROM global_evolution_event WHERE "
        "object_id='stage14-maintenance-fencing-run' AND "
        "operation NOT IN ('begin','resume');");
    SetEvent(first_gate.proceed);
    if(WaitForSingleObject(first_handle,30000)!=WAIT_OBJECT_0)goto cleanup;
    run_after_first=scalar(second_db,
        "SELECT CAST(json_extract(checkpoint_json,'$.attempt') AS INTEGER) "
        "FROM global_maintenance_run WHERE run_id='stage14-maintenance-fencing-run';");
    lease_after_first=scalar(second_db,
        "SELECT CAST(json_extract(checkpoint_json,'$.attempt') AS INTEGER) "
        "FROM global_maintenance_lease WHERE lease_name='stage14-global-maintenance';");
    phase_events_after_first=scalar(second_db,
        "SELECT COUNT(*) FROM global_evolution_event WHERE "
        "object_id='stage14-maintenance-fencing-run' AND "
        "operation NOT IN ('begin','resume');");
    scalar_text(second_db,
        "SELECT COALESCE(group_concat(operation||':'||idempotency_key,','),'') "
        "FROM global_evolution_event "
        "WHERE object_id='stage14-maintenance-fencing-run' ORDER BY sequence_no;",
        operations_after_first,sizeof(operations_after_first));
    if(sqlite3_exec(second_db,
        "UPDATE global_maintenance_lease SET expires_at='2000-01-01T00:00:00Z' "
        "WHERE lease_name='stage14-global-maintenance' AND "
        "CAST(json_extract(checkpoint_json,'$.attempt') AS INTEGER)=2;",
        NULL,NULL,NULL)!=SQLITE_OK||sqlite3_changes(second_db)!=1)goto cleanup;
    second_thread.rc=cbm_evolution_maintenance(second,&input,&second_thread.result);
    run_final=scalar(second_db,
        "SELECT CAST(json_extract(checkpoint_json,'$.attempt') AS INTEGER) "
        "FROM global_maintenance_run WHERE run_id='stage14-maintenance-fencing-run';");
    lease_final=scalar(second_db,
        "SELECT CAST(json_extract(checkpoint_json,'$.attempt') AS INTEGER) "
        "FROM global_maintenance_lease WHERE lease_name='stage14-global-maintenance';");
    setup_ok=1;

cleanup:
    if(first_gate.proceed)SetEvent(first_gate.proceed);
    if(first_handle){WaitForSingleObject(first_handle,30000);CloseHandle(first_handle);}
    if(first)sqlite3_set_authorizer(cbm_global_memory_db(first),NULL,NULL);
    cbm_evolution_result_free(&first_thread.result);
    cbm_evolution_result_free(&second_thread.result);
    if(first_gate.blocked)CloseHandle(first_gate.blocked);
    if(first_gate.proceed)CloseHandle(first_gate.proceed);
    if(second)cbm_global_memory_close(second);
    if(first)cbm_global_memory_close(first);
    stage14_set_env("CBM_STAGE14_ACTIVE_FIXTURE",NULL);
    if(paths_ready){
        remove(concept_path);remove(edge_path);
        stage14_remove_sqlite_fixture(graph_path);
        stage14_remove_sqlite_fixture(memory_path);
    }
    fprintf(stderr,
        "stage14-f-fencing setup=%d blocked=%d generation=%d "
        "stale_rc=%d run_after=%d lease_after=%d phase_events=%d->%d "
        "operations=%s current_rc=%d final=%d/%d\n",setup_ok,first_blocked,
        generation_before,first_thread.rc,run_after_first,lease_after_first,
        phase_events_before,phase_events_after_first,operations_after_first,
        second_thread.rc,run_final,lease_final);
    return !(setup_ok&&generation_before==2&&
             first_thread.rc!=CBM_STORE_OK&&
             first_thread.rc!=CBM_STORE_REPLAYED&&
             first_thread.rc!=CBM_STORE_CHECKPOINTED&&
             run_after_first==2&&lease_after_first==2&&
             phase_events_after_first==phase_events_before&&
             second_thread.rc==CBM_STORE_CHECKPOINTED&&run_final==3&&lease_final==3);
}
#endif

static int test_evidence_gated_evolution_and_maintenance(void) {
    const char *project="stage14-fixture-core-p1";const int64_t as_of=INT64_C(1893456000000);
    cbm_global_memory_t *g=cbm_global_memory_open(":memory:",":memory:");CHECK(g!=NULL);
    cbm_store_t *store=cbm_global_memory_store(g);sqlite3 *db=cbm_global_memory_db(g);
    cbm_project_resolution_t target={0};CHECK(cbm_project_resolve("H:\\Stage14 Maintenance Target",NULL,NULL,&target)==0);
    cbm_memory_item_t survivor=item("stage14-maint-survivor",project,"Preserve a logical backup before migration",0.60);survivor.entity_key="maintenance-dedup";survivor.predicate="requires";
    cbm_memory_item_t duplicate=item("stage14-maint-duplicate",project,"Preserve a logical backup before migration",0.60);duplicate.entity_key="maintenance-dedup";duplicate.predicate="requires";
    CHECK(stage14_add_rank_item(g,&survivor));CHECK(stage14_add_rank_item(g,&duplicate));
    CHECK(sqlite3_exec(db,"UPDATE memory_item SET status='candidate' WHERE id='stage14-maint-duplicate';",NULL,NULL,NULL)==SQLITE_OK);
    cbm_memory_item_t s1=item("stage14-concept-1",project,"Verify capability visibility before a state changing operation",0.95);
    cbm_memory_item_t s2=item("stage14-concept-2",project,"Check required interface availability prior to a local mutation",0.95);
    cbm_memory_item_t f1=item("stage14-false-1",project,"Always preserve exact backup before migration",0.95);
    cbm_memory_item_t f2=item("stage14-false-2",project,"Always preserve exact backup before migration",0.95);
    cbm_memory_item_t e1=item("stage14-edge-low-1",project,"Low value lifecycle source",0.30);
    cbm_memory_item_t e2=item("stage14-edge-low-2",project,"Low value lifecycle target",0.30);
    CHECK(stage14_add_rank_item(g,&s1));CHECK(stage14_add_rank_item(g,&s2));CHECK(stage14_add_rank_item(g,&f1));CHECK(stage14_add_rank_item(g,&f2));CHECK(stage14_add_rank_item(g,&e1));CHECK(stage14_add_rank_item(g,&e2));
    char task[128];CHECK(prepare_feedback_task(g,&target,s1.id,s1.content,"maint-source-1","external_verified","test","used",task)==0);
    CHECK(prepare_feedback_task(g,&target,s2.id,s2.content,"maint-source-2","external_verified","test","used",task)==0);
    CHECK(prepare_feedback_task(g,&target,f1.id,f1.content,"maint-false-1","external_verified","test","used",task)==0);
    CHECK(prepare_feedback_task(g,&target,f2.id,f2.content,"maint-false-2","external_verified","test","used",task)==0);
    CHECK(sqlite3_exec(db,"UPDATE memory_item SET entity_key='stage14-safe-write',kind=CASE id WHEN 'stage14-concept-1' THEN 'fact' ELSE 'lesson' END,source_event_ids=CASE id WHEN 'stage14-concept-1' THEN '[\"source-one\"]' ELSE '[\"source-two\"]' END WHERE id IN ('stage14-concept-1','stage14-concept-2');UPDATE memory_item SET entity_key='stage14-false-duplicate',source_event_ids=CASE id WHEN 'stage14-false-1' THEN '[\"false-one\"]' ELSE '[\"false-two\"]' END WHERE id IN ('stage14-false-1','stage14-false-2');",NULL,NULL,NULL)==SQLITE_OK);
    CHECK(sqlite3_exec(db,"CREATE TABLE IF NOT EXISTS plastic_edge_state(edge_id TEXT PRIMARY KEY,pheromone_ppm INTEGER NOT NULL,success_count INTEGER NOT NULL,failure_count INTEGER NOT NULL,rebuilt_at TEXT NOT NULL);INSERT INTO memory_edge(id,src_id,dst_id,type,weight,origin,confidence,created_at) VALUES('stage14-maint-edge','stage14-edge-low-1','stage14-edge-low-2','used_in',1.0,'stage14_fixture',0.2,1858896000000);INSERT INTO plastic_edge_state(edge_id,pheromone_ppm,success_count,failure_count,rebuilt_at) VALUES('stage14-maint-edge',200000,0,4,'2028-11-27T00:00:00Z');",NULL,NULL,NULL)==SQLITE_OK);
    cbm_evolution_result_t result={0};cbm_evolution_maintenance_input_t maintenance={.mode="shadow",.project_uuid=project,.owner_id="stage14-core-owner",.idempotency_key="stage14-maint-controller",.run_id="stage14-maint-run",.frozen_as_of_ms=as_of,.limit=16,.budget_seconds=30};
    int before_changes=sqlite3_total_changes(db);CHECK(cbm_evolution_maintenance(g,&maintenance,&result)==CBM_STORE_OK);char *shadow=strdup(result.report_json);CHECK(shadow!=NULL);cbm_evolution_result_free(&result);
    CHECK(cbm_evolution_maintenance(g,&maintenance,&result)==CBM_STORE_OK);CHECK(strcmp(shadow,result.report_json)==0&&sqlite3_total_changes(db)==before_changes);free(shadow);cbm_evolution_result_free(&result);
    maintenance.mode="dry_run";CHECK(cbm_evolution_maintenance(g,&maintenance,&result)==CBM_STORE_OK);CHECK(result.wrote==0&&sqlite3_total_changes(db)==before_changes);cbm_evolution_result_free(&result);
#ifdef _WIN32
    stage14_force_maintenance_temp_failure=1;
    CHECK(stage14_maintenance_fencing_probe()!=0);
    stage14_force_maintenance_temp_failure=0;
    CHECK(stage14_maintenance_fencing_probe()==0);
    char temp[MAX_PATH],edge_manifest[MAX_PATH],concept_manifest[MAX_PATH],edge_hash[65],concept_hash[65];CHECK(GetTempPathA(MAX_PATH,temp)>0);
    snprintf(edge_manifest,sizeof(edge_manifest),"%sstage14-maint-edge-%lu.json",temp,(unsigned long)_getpid());snprintf(concept_manifest,sizeof(concept_manifest),"%sstage14-maint-concept-%lu.json",temp,(unsigned long)_getpid());
    CHECK(sqlite3_exec(db,"BEGIN IMMEDIATE;INSERT INTO global_maintenance_run(run_id,project_uuid,mode,status,owner_id,limit_count,budget_seconds,checkpoint_json,consolidated_count,decayed_count,archived_count,idempotency_key,payload_sha256,started_at,completed_at) VALUES('stage14-manifest-simulation','stage14-fixture-core-p1','active','running','manifest-simulator',16,30,'{}',0,0,0,'stage14-manifest-simulation-key','0000000000000000000000000000000000000000000000000000000000000000','2026-07-27T00:00:00Z',NULL);",NULL,NULL,NULL)==SQLITE_OK);
    int simulated=0;CHECK(cbm_store_memory_consolidate_in_transaction(store,project,16,"stage14-manifest-simulation",&simulated)==CBM_STORE_OK&&simulated==1);
    CHECK(stage14_prepare_maintenance_manifests(store,project,maintenance.run_id,maintenance.idempotency_key,as_of,edge_manifest,edge_hash,concept_manifest,concept_hash)==0);
    CHECK(sqlite3_exec(db,"ROLLBACK;",NULL,NULL,NULL)==SQLITE_OK);
    cbm_evolution_maintenance_input_t production_guard=maintenance;
    production_guard.mode="active";production_guard.production_gate_allowed=1;
    production_guard.edge_manifest_path=edge_manifest;
    production_guard.edge_manifest_sha256=edge_hash;
    production_guard.concept_manifest_path=concept_manifest;
    production_guard.concept_manifest_sha256=concept_hash;
    int guard_changes=sqlite3_total_changes(db);
    int guard_runs=scalar(db,"SELECT COUNT(*) FROM global_maintenance_run;");
    int guard_events=scalar(db,"SELECT COUNT(*) FROM global_evolution_event;");
    CHECK(cbm_evolution_maintenance(g,&production_guard,&result)==CBM_STORE_REJECTED);
    cbm_evolution_result_free(&result);
    production_guard.mode="bounded_canary";production_guard.failure_after_write=1;
    CHECK(cbm_evolution_maintenance(g,&production_guard,&result)==CBM_STORE_REJECTED);
    cbm_evolution_result_free(&result);
    production_guard.failure_after_write=0;production_guard.failure_before_report=1;
    CHECK(cbm_evolution_maintenance(g,&production_guard,&result)==CBM_STORE_REJECTED);
    cbm_evolution_result_free(&result);
    production_guard.failure_before_report=0;production_guard.test_budget_after_steps=1;
    CHECK(cbm_evolution_maintenance(g,&production_guard,&result)==CBM_STORE_REJECTED);
    cbm_evolution_result_free(&result);
    production_guard.test_budget_after_steps=0;production_guard.isolated_write_allowed=2;
    CHECK(cbm_evolution_maintenance(g,&production_guard,&result)==CBM_STORE_ERR);
    cbm_evolution_result_free(&result);
    production_guard.isolated_write_allowed=0;production_guard.production_gate_allowed=2;
    CHECK(cbm_evolution_maintenance(g,&production_guard,&result)==CBM_STORE_ERR);
    cbm_evolution_result_free(&result);
    CHECK(sqlite3_total_changes(db)==guard_changes);
    CHECK(scalar(db,"SELECT COUNT(*) FROM global_maintenance_run;")==guard_runs);
    CHECK(scalar(db,"SELECT COUNT(*) FROM global_evolution_event;")==guard_events);
    stage14_set_env("CBM_STAGE14_ACTIVE_FIXTURE","1");maintenance.mode="active";maintenance.isolated_write_allowed=1;maintenance.edge_manifest_path=edge_manifest;maintenance.edge_manifest_sha256=edge_hash;maintenance.concept_manifest_path=concept_manifest;maintenance.concept_manifest_sha256=concept_hash;maintenance.test_budget_after_steps=1;
    CHECK(sqlite3_exec(db,
        "INSERT INTO global_maintenance_lease(lease_name,owner_id,acquired_at,expires_at,"
        "checkpoint_json,payload_sha256) VALUES('stage14-global-maintenance',"
        "'stage14-core-owner','2026-07-28T00:00:00Z','2099-01-01T00:00:00Z','{}',"
        "'0000000000000000000000000000000000000000000000000000000000000000');",
        NULL,NULL,NULL)==SQLITE_OK);
    int lease_runs=scalar(db,"SELECT COUNT(*) FROM global_maintenance_run;");
    int lease_events=scalar(db,"SELECT COUNT(*) FROM global_evolution_event;");
    CHECK(cbm_evolution_maintenance(g,&maintenance,&result)==CBM_STORE_REJECTED);
    cbm_evolution_result_free(&result);
    CHECK(scalar(db,"SELECT COUNT(*) FROM global_maintenance_run;")==lease_runs);
    CHECK(scalar(db,"SELECT COUNT(*) FROM global_evolution_event;")==lease_events);
    CHECK(scalar(db,
        "SELECT COUNT(*) FROM global_maintenance_lease WHERE "
        "owner_id='stage14-core-owner' AND expires_at='2099-01-01T00:00:00Z' AND "
        "payload_sha256='0000000000000000000000000000000000000000000000000000000000000000';")
        ==1);
    CHECK(scalar(db,
        "SELECT COUNT(*) FROM memory_item WHERE id='stage14-maint-duplicate' "
        "AND status='candidate';")==1);
    CHECK(sqlite3_exec(db,
        "DELETE FROM global_maintenance_lease WHERE lease_name='stage14-global-maintenance';",
        NULL,NULL,NULL)==SQLITE_OK);
    CHECK(cbm_evolution_maintenance(g,&maintenance,&result)==CBM_STORE_CHECKPOINTED);CHECK(result.checkpointed==1&&result.consolidated==1);cbm_evolution_result_free(&result);
    CHECK(scalar(db,"SELECT COUNT(*) FROM global_maintenance_run WHERE status='checkpointed' AND checkpoint_json LIKE '%\"phase\":\"decay\"%';")==1);
    maintenance.test_budget_after_steps=0;maintenance.failure_after_write=2;
    int fault_rc=cbm_evolution_maintenance(g,&maintenance,&result);
    CHECK(fault_rc!=CBM_STORE_OK&&fault_rc!=CBM_STORE_REPLAYED&&result.checkpointed==1&&result.report_json&&strstr(result.report_json,"injected_edge_fault"));cbm_evolution_result_free(&result);
    CHECK(scalar(db,"SELECT COUNT(*) FROM edge_maintenance_run;")==0);CHECK(scalar(db,"SELECT COUNT(*) FROM concept_growth_run;")==0);
    maintenance.failure_after_write=0;maintenance.failure_before_report=1;
    int completed_events=scalar(db,
        "SELECT COUNT(*) FROM global_evolution_event WHERE operation='completed' "
        "AND object_id='stage14-maint-run';");
    CHECK(cbm_evolution_maintenance(g,&maintenance,&result)==CBM_STORE_ERR);
    cbm_evolution_result_free(&result);
    CHECK(scalar(db,
        "SELECT COUNT(*) FROM global_maintenance_run WHERE run_id='stage14-maint-run' "
        "AND status='completed';")==0);
    CHECK(scalar(db,
        "SELECT COUNT(*) FROM global_maintenance_run WHERE run_id='stage14-maint-run' "
        "AND checkpoint_json LIKE '%\"phase\":\"finalize\"%';")==1);
    CHECK(scalar(db,
        "SELECT COUNT(*) FROM global_evolution_event WHERE operation='completed' "
        "AND object_id='stage14-maint-run';")==completed_events);
    maintenance.failure_before_report=0;
    CHECK(cbm_evolution_maintenance(g,&maintenance,&result)==CBM_STORE_REJECTED);
    cbm_evolution_result_free(&result);
    CHECK(scalar(db,
        "SELECT COUNT(*) FROM global_maintenance_run WHERE run_id='stage14-maint-run' "
        "AND status='completed';")==0);
    CHECK(sqlite3_exec(db,
        "UPDATE global_maintenance_lease SET expires_at='2000-01-01T00:00:00Z' "
        "WHERE lease_name='stage14-global-maintenance' AND "
        "owner_id='stage14-core-owner';",NULL,NULL,NULL)==SQLITE_OK&&
        sqlite3_changes(db)==1);
    CHECK(cbm_evolution_maintenance(g,&maintenance,&result)==CBM_STORE_OK);
    CHECK(result.wrote==1&&result.consolidated==1&&result.edge_decisions>=1&&result.edge_transitions>=1&&result.concept_eligible==1&&result.concept_proposed==1);cbm_evolution_result_free(&result);
    CHECK(scalar(db,"SELECT COUNT(*) FROM memory_item WHERE id='stage14-maint-duplicate' AND status='archived';")==1);
    CHECK(scalar(db,"SELECT COUNT(*) FROM edge_lifecycle_state WHERE edge_id='stage14-maint-edge' AND lifecycle_state='cold';")==1);
    CHECK(scalar(db,"SELECT COUNT(*) FROM concept_candidate;")==1);CHECK(scalar(db,"SELECT COUNT(*) FROM memory_item WHERE deleted_at IS NOT NULL;")==0);
    int runs=scalar(db,"SELECT COUNT(*) FROM global_maintenance_run;"),events=scalar(db,"SELECT COUNT(*) FROM global_evolution_event;");
    CHECK(cbm_evolution_maintenance(g,&maintenance,&result)==CBM_STORE_REPLAYED);CHECK(result.wrote==0);cbm_evolution_result_free(&result);CHECK(scalar(db,"SELECT COUNT(*) FROM global_maintenance_run;")==runs&&scalar(db,"SELECT COUNT(*) FROM global_evolution_event;")==events);
    maintenance.limit=15;CHECK(cbm_evolution_maintenance(g,&maintenance,&result)==CBM_STORE_IDEMPOTENCY_CONFLICT);cbm_evolution_result_free(&result);
    fprintf(stderr,"stage14-f-maintenance consolidated=1 edge_decisions=%d edge_transitions=%d concept_eligible=1 concept_proposed=1 checkpoint_resume=PASS same_owner_live_lease_rejected=PASS precommit_report_rollback=PASS production_active_rejected=PASS production_fault_injection_rejected=PASS strict_booleans=PASS hard_delete=0\n",scalar(db,"SELECT COUNT(*) FROM edge_maintenance_decision;"),scalar(db,"SELECT COUNT(*) FROM edge_lifecycle_state WHERE lifecycle_state<>'active';"));
    stage14_set_env("CBM_STAGE14_ACTIVE_FIXTURE",NULL);remove(concept_manifest);remove(edge_manifest);
#endif
    cbm_global_memory_close(g);return 0;
}

static int test_maintenance_production_bounded_canary(void) {
#ifdef _WIN32
    const char *project="H-Codex_H-neuroplastic-main";
    const int64_t as_of=INT64_C(1893456000000);
    CHECK(getenv("CBM_STAGE14_ACTIVE_FIXTURE")==NULL);
    CHECK(getenv("CBM_STAGE9_PRODUCTION_CANARY")==NULL);
    CHECK(getenv("CBM_STAGE9_PRODUCTION_CANARY_MANIFEST")==NULL);
    CHECK(getenv("CBM_STAGE9_PRODUCTION_CANARY_MANIFEST_SHA256")==NULL);
    CHECK(getenv("CBM_STAGE10_PRODUCTION_CANARY")==NULL);
    CHECK(getenv("CBM_STAGE10_PRODUCTION_CANARY_MANIFEST")==NULL);
    CHECK(getenv("CBM_STAGE10_PRODUCTION_CANARY_SHA256")==NULL);
    char temp[MAX_PATH],edge_path[MAX_PATH],concept_path[MAX_PATH];
    char relative_edge_path[MAX_PATH],relative_concept_path[MAX_PATH];
    char edge_hash[65],concept_hash[65],relative_edge_hash[65],relative_concept_hash[65];
    CHECK(GetTempPathA(MAX_PATH,temp)>0);
    snprintf(edge_path,sizeof(edge_path),"%sstage14-maint-production-edge-%lu.json",
             temp,(unsigned long)_getpid());
    snprintf(concept_path,sizeof(concept_path),"%sstage14-maint-production-concept-%lu.json",
             temp,(unsigned long)_getpid());
    snprintf(relative_edge_path,sizeof(relative_edge_path),
             "%sstage14-maint-production-relative-edge-%lu.json",
             temp,(unsigned long)_getpid());
    snprintf(relative_concept_path,sizeof(relative_concept_path),
             "%sstage14-maint-production-relative-concept-%lu.json",
             temp,(unsigned long)_getpid());

    cbm_global_memory_t *g=cbm_global_memory_open(":memory:",":memory:");CHECK(g!=NULL);
    cbm_store_t *store=cbm_global_memory_store(g);sqlite3 *db=cbm_global_memory_db(g);
    cbm_evolution_result_t result={0};
    cbm_evolution_maintenance_input_t boundary={
        .mode="shadow",.project_uuid=project,.owner_id="stage14-production-owner",
        .idempotency_key="stage14-production-boundary",.run_id="stage14-production-boundary",
        .frozen_as_of_ms=as_of,.limit=1000,.budget_seconds=30
    };
    CHECK(cbm_evolution_maintenance(g,&boundary,&result)==CBM_STORE_OK);
    cbm_evolution_result_free(&result);
    boundary.limit=0;CHECK(cbm_evolution_maintenance(g,&boundary,&result)==CBM_STORE_ERR);
    boundary.limit=1001;CHECK(cbm_evolution_maintenance(g,&boundary,&result)==CBM_STORE_ERR);
    boundary.limit=1;boundary.budget_seconds=0;
    CHECK(cbm_evolution_maintenance(g,&boundary,&result)==CBM_STORE_ERR);
    boundary.budget_seconds=31;
    CHECK(cbm_evolution_maintenance(g,&boundary,&result)==CBM_STORE_ERR);

    CHECK(stage14_prepare_maintenance_manifests(
        store,project,"stage14-production-run","stage14-production-key",as_of,
        edge_path,edge_hash,concept_path,concept_hash)==0);

    cbm_edge_lifecycle_input_t direct_edge={
        .project=project,.mode="active",.run_id="stage14-production-run:edge",
        .as_of_ms=as_of,.algorithm_version=CBM_STAGE9_ALGORITHM_VERSION,
        .policy_sha256=CBM_STAGE9_POLICY_SHA256,.policy_version=CBM_STAGE9_POLICY_VERSION,
        .config_version=CBM_STAGE9_CONFIG_VERSION,.manifest_path=edge_path,
        .manifest_sha256=edge_hash
    };
    cbm_edge_lifecycle_result_t direct_edge_result={0};
    CHECK(cbm_store_memory_edge_maintenance(
        store,&direct_edge,&direct_edge_result)==CBM_STORE_REJECTED);
    cbm_store_memory_edge_lifecycle_result_free(&direct_edge_result);
    cbm_concept_generate_input_t direct_concept={
        .project=project,.store="project-memory",.operation="generate",.mode="active",
        .run_id="stage14-production-run:concept",
        .idempotency_key="stage14-production-key:concept",
        .algorithm_version=CBM_STAGE10_ALGORITHM_VERSION,
        .policy_sha256=CBM_STAGE10_POLICY_SHA256,.policy_version=CBM_STAGE10_POLICY_VERSION,
        .config_version=CBM_STAGE10_CONFIG_VERSION,
        .generator_version=CBM_STAGE10_GENERATOR_VERSION,
        .manifest_path=concept_path,.manifest_sha256=concept_hash
    };
    cbm_concept_result_t direct_concept_result={0};
    CHECK(cbm_store_memory_concept_generate(
        store,&direct_concept,&direct_concept_result)==CBM_STORE_REJECTED);
    cbm_store_memory_concept_result_free(&direct_concept_result);

    const char *manifest_case_names[]={
        "missing-edge","missing-concept","tampered-edge","tampered-concept"
    };
    int manifest_write_violations=0;
    for(int case_index=0;case_index<4;case_index++){
        char case_run[128],case_key[128],case_edge[MAX_PATH],case_concept[MAX_PATH];
        char case_edge_hash[65],case_concept_hash[65];
        snprintf(case_run,sizeof(case_run),"stage14-production-%s-run",
                 manifest_case_names[case_index]);
        snprintf(case_key,sizeof(case_key),"stage14-production-%s-key",
                 manifest_case_names[case_index]);
        snprintf(case_edge,sizeof(case_edge),"%sstage14-production-%s-edge-%lu.json",
                 temp,manifest_case_names[case_index],(unsigned long)_getpid());
        snprintf(case_concept,sizeof(case_concept),
                 "%sstage14-production-%s-concept-%lu.json",temp,
                 manifest_case_names[case_index],(unsigned long)_getpid());
        CHECK(stage14_prepare_maintenance_manifests(
            store,project,case_run,case_key,as_of,case_edge,case_edge_hash,
            case_concept,case_concept_hash)==0);
        if(case_index==0)CHECK(remove(case_edge)==0);
        else if(case_index==1)CHECK(remove(case_concept)==0);
        else{
            const char *tampered_path=case_index==2?case_edge:case_concept;
            FILE *tampered=fopen(tampered_path,"ab");
            CHECK(tampered!=NULL&&fwrite(" ",1,1,tampered)==1&&fclose(tampered)==0);
        }
        cbm_evolution_maintenance_input_t rejected={
            .mode="bounded_canary",.project_uuid=project,
            .owner_id="stage14-production-owner",.idempotency_key=case_key,
            .run_id=case_run,.edge_manifest_path=case_edge,
            .edge_manifest_sha256=case_edge_hash,.concept_manifest_path=case_concept,
            .concept_manifest_sha256=case_concept_hash,.frozen_as_of_ms=as_of,
            .limit=1,.budget_seconds=1,.production_gate_allowed=1
        };
        stage14_maintenance_state_t before=stage14_maintenance_state(db);
        stage14_update_trace_t trace={0};
        sqlite3_update_hook(db,stage14_update_trace,&trace);
        int rejected_rc=cbm_evolution_maintenance(g,&rejected,&result);
        sqlite3_update_hook(db,NULL,NULL);
        cbm_evolution_result_free(&result);
        stage14_maintenance_state_t after=stage14_maintenance_state(db);
        int state_equal=stage14_maintenance_state_equal(&before,&after);
        fprintf(stderr,"stage14-f-manifest-prewrite case=%s rc=%d update_count=%d "
            "state_equal=%d\n",manifest_case_names[case_index],rejected_rc,
            trace.update_count,state_equal);
        if(rejected_rc!=CBM_STORE_REJECTED||trace.update_count!=0||!state_equal)
            manifest_write_violations++;
        remove(case_concept);remove(case_edge);
    }
    CHECK(manifest_write_violations==0);

    cbm_evolution_maintenance_input_t input={
        .mode="bounded_canary",.project_uuid=project,.owner_id="stage14-production-owner",
        .idempotency_key="stage14-production-key",.run_id="stage14-production-run",
        .edge_manifest_path=edge_path,.edge_manifest_sha256=edge_hash,
        .concept_manifest_path=concept_path,.concept_manifest_sha256=concept_hash,
        .frozen_as_of_ms=as_of,.limit=1,.budget_seconds=1,.production_gate_allowed=1
    };
    CHECK(cbm_evolution_maintenance(g,&input,&result)==CBM_STORE_OK);
    CHECK(result.wrote==1&&result.replayed==0);cbm_evolution_result_free(&result);
    int runs=scalar(db,"SELECT COUNT(*) FROM global_maintenance_run;");
    int events=scalar(db,"SELECT COUNT(*) FROM global_evolution_event;");
    int changes=sqlite3_total_changes(db);
    CHECK(cbm_evolution_maintenance(g,&input,&result)==CBM_STORE_REPLAYED);
    CHECK(result.wrote==0&&result.replayed==1);cbm_evolution_result_free(&result);
    CHECK(scalar(db,"SELECT COUNT(*) FROM global_maintenance_run;")==runs&&
          scalar(db,"SELECT COUNT(*) FROM global_evolution_event;")==events&&
          sqlite3_total_changes(db)==changes);
    input.limit=2;
    CHECK(cbm_evolution_maintenance(g,&input,&result)==CBM_STORE_IDEMPOTENCY_CONFLICT);
    cbm_evolution_result_free(&result);input.limit=1;
    CHECK(scalar(db,"SELECT COUNT(*) FROM memory_item WHERE deleted_at IS NOT NULL;")==0);

    CHECK(stage14_prepare_maintenance_manifests(
        store,project,"stage14-production-relative-run","stage14-production-relative-key",as_of,
        relative_edge_path,relative_edge_hash,relative_concept_path,relative_concept_hash)==0);
    char uppercase_edge_hash[65];
    for(int i=0;i<64;i++)uppercase_edge_hash[i]=
        relative_edge_hash[i]>='a'&&relative_edge_hash[i]<='f'
            ?(char)(relative_edge_hash[i]-('a'-'A')):relative_edge_hash[i];
    uppercase_edge_hash[64]=0;
    cbm_evolution_maintenance_input_t invalid_hash=input;
    invalid_hash.run_id="stage14-production-relative-run";
    invalid_hash.idempotency_key="stage14-production-relative-key";
    invalid_hash.edge_manifest_path=relative_edge_path;
    invalid_hash.edge_manifest_sha256=uppercase_edge_hash;
    invalid_hash.concept_manifest_path=relative_concept_path;
    invalid_hash.concept_manifest_sha256=relative_concept_hash;
    CHECK(cbm_evolution_maintenance(g,&invalid_hash,&result)==CBM_STORE_ERR);
    cbm_evolution_result_free(&result);
    CHECK(scalar(db,
        "SELECT COUNT(*) FROM global_maintenance_run WHERE "
        "idempotency_key='stage14-production-relative-key';")==0);
    char original_directory[MAX_PATH];
    CHECK(GetCurrentDirectoryA(MAX_PATH,original_directory)>0);
    CHECK(SetCurrentDirectoryA(temp));
    cbm_evolution_maintenance_input_t relative=input;
    relative.run_id="stage14-production-relative-run";
    relative.idempotency_key="stage14-production-relative-key";
    relative.edge_manifest_path=strrchr(relative_edge_path,'\\')+1;
    relative.edge_manifest_sha256=relative_edge_hash;
    relative.concept_manifest_path=strrchr(relative_concept_path,'\\')+1;
    relative.concept_manifest_sha256=relative_concept_hash;
    int relative_rc=cbm_evolution_maintenance(g,&relative,&result);
    cbm_evolution_result_free(&result);
    CHECK(SetCurrentDirectoryA(original_directory));
    int relative_runs=scalar(db,
        "SELECT COUNT(*) FROM global_maintenance_run WHERE "
        "idempotency_key='stage14-production-relative-key';");
    remove(relative_concept_path);remove(relative_edge_path);
    remove(concept_path);remove(edge_path);cbm_global_memory_close(g);
    CHECK(relative_rc==CBM_STORE_ERR&&relative_runs==0);
    fprintf(stderr,"stage14-f-production-maintenance bounded_canary=PASS "
        "replay_zero_write=PASS altered_payload_conflict=PASS manifest_path_sha=PASS "
        "manifest_missing_tampered_prewrite_zero=PASS limit_budget_boundaries=PASS "
        "legacy_direct_fail_closed=PASS old_canary_env_absent=PASS hard_delete=0\n");
#endif
    return 0;
}

static int test_completed_task_evolution_controller(void) {
#ifdef _WIN32
    char temp[MAX_PATH],memory_path[MAX_PATH],graph_path[MAX_PATH];
    CHECK(GetTempPathA(MAX_PATH,temp)>0);
    snprintf(memory_path,sizeof(memory_path),"%sstage14-evolution-%lu-%lld-memory.db",temp,
             (unsigned long)_getpid(),(long long)time(NULL));
    snprintf(graph_path,sizeof(graph_path),"%sstage14-evolution-%lu-%lld-graph.db",temp,
             (unsigned long)_getpid(),(long long)time(NULL));
    cbm_global_memory_t *g=cbm_global_memory_open(memory_path,graph_path);CHECK(g!=NULL);
    cbm_project_resolution_t target={0};
    CHECK(cbm_project_resolve("H:\\Stage14 Evolution Target",NULL,NULL,&target)==0);
    cbm_memory_item_t memory=item("controller-memory","source-project-controller",
                                  "controller eligibility marker",0.50);
    char *new_id=NULL;cbm_store_t *store=cbm_global_memory_store(g);
    CHECK(cbm_store_memory_append_candidate(store,&memory,&new_id)==CBM_STORE_OK);free(new_id);
    CHECK(cbm_store_memory_index_candidate(store,&memory,memory.id,NULL)==CBM_STORE_OK);
    sqlite3 *db=cbm_global_memory_db(g),*graph=cbm_global_graph_db(g);
    char task_a[128];CHECK(prepare_feedback_task(g,&target,memory.id,memory.content,"grade-a",
        "external_verified","test","used",task_a)==0);
    cbm_evolution_task_input_t input={.mode="bounded_canary",.task_id=task_a,
        .idempotency_key="controller-grade-a",.isolated_write_allowed=1};
    cbm_evolution_result_t result={0};double before=scalar_double(db,"SELECT confidence FROM memory_item WHERE id='controller-memory';");
    CHECK(cbm_evolution_apply_completed_task(g,&input,&result)==CBM_STORE_OK);
    CHECK(result.wrote==1&&result.eligible==1&&result.positive==1&&result.negative==0);
    CHECK(result.evolution_events==2&&result.cross_project_edges==1);cbm_evolution_result_free(&result);
    double after_a=scalar_double(db,"SELECT confidence FROM memory_item WHERE id='controller-memory';");
    CHECK(after_a>before+0.039&&after_a<before+0.041);
    CHECK(scalar(db,"SELECT COUNT(*) FROM global_evolution_event;")==2);
    CHECK(scalar(graph,"SELECT COUNT(*) FROM global_cross_project_edge_version;")==1);
    CHECK(cbm_evolution_apply_completed_task(g,&input,&result)==CBM_STORE_REPLAYED);
    CHECK(result.wrote==0);cbm_evolution_result_free(&result);
    CHECK(scalar(db,"SELECT COUNT(*) FROM global_evolution_event;")==2);
    input.mode="active";CHECK(cbm_evolution_apply_completed_task(g,&input,&result)==CBM_STORE_IDEMPOTENCY_CONFLICT);cbm_evolution_result_free(&result);input.mode="bounded_canary";
    CHECK(scalar(db,"SELECT COUNT(*) FROM global_evolution_event;")==2);

    char task_c[128];CHECK(prepare_feedback_task(g,&target,memory.id,memory.content,"grade-c",
        "model_self_report","model","used",task_c)==0);
    input.task_id=task_c;input.idempotency_key="controller-grade-c";before=scalar_double(db,"SELECT confidence FROM memory_item WHERE id='controller-memory';");
    CHECK(cbm_evolution_apply_completed_task(g,&input,&result)==CBM_STORE_OK);
    CHECK(result.positive==1&&result.cross_project_edges==0);cbm_evolution_result_free(&result);
    CHECK(scalar_double(db,"SELECT confidence FROM memory_item WHERE id='controller-memory';")==before);
    CHECK(scalar(graph,"SELECT COUNT(*) FROM global_cross_project_edge_version;")==1);

    char task_b[128];CHECK(prepare_feedback_task(g,&target,memory.id,memory.content,"grade-b",
        "explicit_user","user","used",task_b)==0);
    input.task_id=task_b;input.idempotency_key="controller-grade-b";before=scalar_double(db,"SELECT confidence FROM memory_item WHERE id='controller-memory';");
    CHECK(cbm_evolution_apply_completed_task(g,&input,&result)==CBM_STORE_OK);cbm_evolution_result_free(&result);
    CHECK(scalar_double(db,"SELECT confidence FROM memory_item WHERE id='controller-memory';")>before+0.019);
    CHECK(scalar(graph,"SELECT COUNT(*) FROM global_cross_project_edge_version;")==2);

    char task_correct[128],correct_event[128],correct_evidence[128];
    CHECK(prepare_existing_feedback_chain(g,&target,memory.id,memory.content,"correct-chain",
        "correct",task_correct,correct_event,correct_evidence)==0);
    input.task_id=task_correct;input.idempotency_key="controller-correct-chain";
    CHECK(cbm_evolution_apply_completed_task(g,&input,&result)==CBM_STORE_OK);
    CHECK(result.eligible==1&&result.negative==1);cbm_evolution_result_free(&result);
    char chain_sql[1024];
    snprintf(chain_sql,sizeof(chain_sql),"SELECT COUNT(*) FROM global_evolution_event WHERE task_id='%s' AND operation='correct' AND evidence_id='%s';",task_correct,correct_evidence);
    CHECK(scalar(db,chain_sql)==1);

    char task_withdraw[128],withdraw_event[128],withdraw_evidence[128];
    CHECK(prepare_existing_feedback_chain(g,&target,memory.id,memory.content,"withdraw-chain",
        "withdraw",task_withdraw,withdraw_event,withdraw_evidence)==0);
    input.task_id=task_withdraw;input.idempotency_key="controller-withdraw-chain";
    CHECK(cbm_evolution_apply_completed_task(g,&input,&result)==CBM_STORE_OK);
    CHECK(result.eligible==1&&result.negative==1);cbm_evolution_result_free(&result);
    snprintf(chain_sql,sizeof(chain_sql),"SELECT COUNT(*) FROM global_evolution_event WHERE task_id='%s' AND operation='withdraw' AND evidence_id='%s';",task_withdraw,withdraw_evidence);
    CHECK(scalar(db,chain_sql)==1);

    char task_reject[128];CHECK(prepare_feedback_task(g,&target,memory.id,memory.content,"reject",
        "external_verified","runtime","rejected",task_reject)==0);
    input.task_id=task_reject;input.idempotency_key="controller-reject";
    CHECK(cbm_evolution_apply_completed_task(g,&input,&result)==CBM_STORE_OK);CHECK(result.negative==1);cbm_evolution_result_free(&result);
    CHECK(scalar(db,"SELECT COUNT(*) FROM global_evolution_event WHERE operation='reject';")==1);
    char task_contradict[128];CHECK(prepare_feedback_task(g,&target,memory.id,memory.content,"contradict",
        "external_verified","runtime","contradicted",task_contradict)==0);
    input.task_id=task_contradict;input.idempotency_key="controller-contradict";
    CHECK(cbm_evolution_apply_completed_task(g,&input,&result)==CBM_STORE_OK);CHECK(result.negative==1);cbm_evolution_result_free(&result);
    CHECK(scalar(db,"SELECT COUNT(*) FROM global_evolution_event WHERE operation='contradict';")==1);

    char task_fault[128];CHECK(prepare_feedback_task(g,&target,memory.id,memory.content,"fault",
        "external_verified","test","used",task_fault)==0);
    int events_before=scalar(db,"SELECT COUNT(*) FROM global_evolution_event;");
    int versions_before=scalar(graph,"SELECT COUNT(*) FROM global_cross_project_edge_version;");
    before=scalar_double(db,"SELECT confidence FROM memory_item WHERE id='controller-memory';");
    input.task_id=task_fault;input.idempotency_key="controller-fault";input.failure_after_write=2;
    CHECK(cbm_evolution_apply_completed_task(g,&input,&result)==CBM_STORE_ERR);cbm_evolution_result_free(&result);
    CHECK(scalar(db,"SELECT COUNT(*) FROM global_evolution_event;")==events_before);
    CHECK(scalar(graph,"SELECT COUNT(*) FROM global_cross_project_edge_version;")==versions_before);
    CHECK(scalar_double(db,"SELECT confidence FROM memory_item WHERE id='controller-memory';")==before);
    input.failure_after_write=0;

    char task_degraded[128];
    CHECK(prepare_feedback_task(g,&target,memory.id,memory.content,"latest-degraded",
        "external_verified","test","used",task_degraded)==0);
    CHECK(stage14_append_lifecycle_state(db,task_degraded,"degraded","degraded",NULL)==0);
    events_before=scalar(db,"SELECT COUNT(*) FROM global_evolution_event;");
    int business_before=scalar(db,
        "SELECT COUNT(*) FROM global_evolution_event WHERE operation<>'noop';");
    int noop_before=scalar(db,
        "SELECT COUNT(*) FROM global_evolution_event WHERE operation='noop';");
    versions_before=scalar(graph,"SELECT COUNT(*) FROM global_cross_project_edge_version;");
    before=scalar_double(db,"SELECT confidence FROM memory_item WHERE id='controller-memory';");
    cbm_evolution_memory_input_t degraded_state={
        .mode="bounded_canary",.task_id=task_degraded,
        .project_uuid=target.project_uuid,.memory_item_id=memory.id,
        .operation="archive",.evidence_grade="A",
        .evidence_id="evo-evidence-latest-degraded",
        .idempotency_key="controller-latest-degraded-state",
        .isolated_write_allowed=1
    };
    CHECK(cbm_evolution_memory_state(g,&degraded_state,&result)==CBM_STORE_REJECTED);
    cbm_evolution_result_free(&result);
    CHECK(scalar(db,"SELECT COUNT(*) FROM global_evolution_event;")==events_before);
    input.task_id=task_degraded;input.idempotency_key="controller-latest-degraded";
    CHECK(cbm_evolution_plan_completed_task(g,&input,&result)==CBM_STORE_OK);
    CHECK(result.eligible==0&&result.positive==0&&result.planned_evolution_events==1);
    cbm_evolution_result_free(&result);
    CHECK(cbm_evolution_apply_completed_task(g,&input,&result)==CBM_STORE_OK);
    CHECK(result.wrote==0&&result.eligible==0&&result.positive==0&&
          result.evolution_events==1);
    cbm_evolution_result_free(&result);
    CHECK(scalar(db,"SELECT COUNT(*) FROM global_evolution_event;")==events_before+1);
    CHECK(scalar(db,
        "SELECT COUNT(*) FROM global_evolution_event WHERE operation<>'noop';")
        ==business_before);
    CHECK(scalar(db,
        "SELECT COUNT(*) FROM global_evolution_event WHERE operation='noop';")
        ==noop_before+1);
    CHECK(scalar(graph,"SELECT COUNT(*) FROM global_cross_project_edge_version;")
        ==versions_before);
    CHECK(scalar_double(db,"SELECT confidence FROM memory_item WHERE id='controller-memory';")
        ==before);

    const char *outcomes[]={"failed","cancelled","abandoned","completed"};
    for(int i=0;i<4;i++){
        char suffix[32],task[128],key[64];snprintf(suffix,sizeof(suffix),"zero-%d",i);
        CHECK(prepare_terminal_task(g,&target,suffix,outcomes[i],task)==0);
        snprintf(key,sizeof(key),"controller-zero-%d",i);input.task_id=task;input.idempotency_key=key;
        CHECK(cbm_evolution_apply_completed_task(g,&input,&result)==CBM_STORE_OK);
        CHECK(result.wrote==0&&result.eligible==0&&result.positive==0);cbm_evolution_result_free(&result);
    }
    CHECK(scalar(db,"SELECT COUNT(*) FROM memory_item WHERE deleted_at IS NOT NULL;")==0);

    int bad_sequence=0;
    CHECK(scalar(db,
        "SELECT COUNT(*) FROM global_evolution_event "
        "WHERE algorithm_version='stage14-evolution-v3';")>0);
    CHECK(cbm_evolution_event_chain_verify_for_test(db,&bad_sequence)==CBM_STORE_OK&&
          bad_sequence==0);
    const char *v3_tamper_sql[]={
        "UPDATE global_evolution_event SET operation=operation||'-tampered' "
        "WHERE sequence_no=(SELECT MIN(sequence_no) FROM global_evolution_event "
        "WHERE algorithm_version='stage14-evolution-v3');",
        "UPDATE global_evolution_event SET event_id=event_id||'-tampered' "
        "WHERE sequence_no=(SELECT MIN(sequence_no) FROM global_evolution_event "
        "WHERE algorithm_version='stage14-evolution-v3');",
        "UPDATE global_evolution_event SET sequence_no=sequence_no+100000 "
        "WHERE sequence_no=(SELECT MIN(sequence_no) FROM global_evolution_event "
        "WHERE algorithm_version='stage14-evolution-v3');",
        "UPDATE global_evolution_event SET prev_hash="
        "'ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff' "
        "WHERE sequence_no=(SELECT MIN(sequence_no) FROM global_evolution_event "
        "WHERE algorithm_version='stage14-evolution-v3');",
        "UPDATE global_evolution_event SET event_hash="
        "'ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff' "
        "WHERE sequence_no=(SELECT MIN(sequence_no) FROM global_evolution_event "
        "WHERE algorithm_version='stage14-evolution-v3');"
    };
    for(size_t i=0;i<sizeof(v3_tamper_sql)/sizeof(v3_tamper_sql[0]);i++){
        CHECK(sqlite3_exec(db,"BEGIN IMMEDIATE;",NULL,NULL,NULL)==SQLITE_OK);
        CHECK(sqlite3_exec(db,v3_tamper_sql[i],NULL,NULL,NULL)==SQLITE_OK&&
              sqlite3_changes(db)==1);
        CHECK(cbm_evolution_event_chain_verify_for_test(db,&bad_sequence)==CBM_STORE_ERR&&
              bad_sequence>0);
        CHECK(sqlite3_exec(db,"ROLLBACK;",NULL,NULL,NULL)==SQLITE_OK);
        CHECK(cbm_evolution_event_chain_verify_for_test(db,&bad_sequence)==CBM_STORE_OK&&
              bad_sequence==0);
    }
    CHECK(append_legacy_v2_evolution_event(g));
    CHECK(cbm_evolution_event_chain_verify_for_test(db,&bad_sequence)==CBM_STORE_OK&&
          bad_sequence==0);
    CHECK(scalar(db,
        "SELECT COUNT(*) FROM global_evolution_event "
        "WHERE algorithm_version='stage14-evolution-v2';")==1);

    char race_task[128];
    CHECK(prepare_feedback_task(g,&target,memory.id,memory.content,"cross-process-race",
        "external_verified","test","used",race_task)==0);
    int race_events_before=scalar(db,"SELECT COUNT(*) FROM global_evolution_event;");
    int race_versions_before=scalar(graph,
        "SELECT COUNT(*) FROM global_cross_project_edge_version;");
    double race_confidence_before=scalar_double(db,
        "SELECT confidence FROM memory_item WHERE id='controller-memory';");
    cbm_global_memory_close(g);g=NULL;db=NULL;graph=NULL;
    PROCESS_INFORMATION race_first,race_second;DWORD race_first_exit=0,race_second_exit=0;
    CHECK(stage14_launch_evolution_child("--stage14-apply-child",memory_path,graph_path,
                                         race_task,"controller-cross-process-race",
                                         &race_first));
    CHECK(stage14_launch_evolution_child("--stage14-apply-child",memory_path,graph_path,
                                         race_task,"controller-cross-process-race",
                                         &race_second));
    CHECK(stage14_wait_evolution_child(&race_first,&race_first_exit));
    CHECK(stage14_wait_evolution_child(&race_second,&race_second_exit));
    fprintf(stderr,"stage14-e-cross-process child_exit_first=%lu child_exit_second=%lu\n",
            (unsigned long)race_first_exit,(unsigned long)race_second_exit);
    CHECK((race_first_exit==40&&(race_second_exit==41||race_second_exit==43))||
          (race_second_exit==40&&(race_first_exit==41||race_first_exit==43)));
    g=cbm_global_memory_open(memory_path,graph_path);CHECK(g!=NULL);
    db=cbm_global_memory_db(g);graph=cbm_global_graph_db(g);
    CHECK(scalar(db,"SELECT COUNT(*) FROM global_evolution_event;")==race_events_before+2);
    CHECK(scalar(graph,
        "SELECT COUNT(*) FROM global_cross_project_edge_version;")==race_versions_before+1);
    CHECK(scalar_double(db,
        "SELECT confidence FROM memory_item WHERE id='controller-memory';")
        >race_confidence_before+0.039);
    CHECK(cbm_evolution_event_chain_verify_for_test(db,&bad_sequence)==CBM_STORE_OK&&
          bad_sequence==0);
    char memory_mode_before[16],graph_mode_before[16],memory_mode_after[16],
         graph_mode_after[16];
    CHECK(scalar_text(db,"PRAGMA journal_mode;",memory_mode_before,
                      sizeof(memory_mode_before)));
    CHECK(scalar_text(graph,"PRAGMA journal_mode;",graph_mode_before,
                      sizeof(graph_mode_before)));
    input=(cbm_evolution_task_input_t){
        .mode="bounded_canary",.task_id=race_task,
        .idempotency_key="controller-cross-process-race",.isolated_write_allowed=1
    };
    CHECK(cbm_evolution_apply_completed_task(g,&input,&result)==CBM_STORE_REPLAYED);
    CHECK(result.wrote==0&&result.replayed==1);cbm_evolution_result_free(&result);
    CHECK(scalar(db,"SELECT COUNT(*) FROM global_evolution_event;")==race_events_before+2);
    CHECK(scalar(graph,
        "SELECT COUNT(*) FROM global_cross_project_edge_version;")==race_versions_before+1);
    CHECK(scalar_text(db,"PRAGMA journal_mode;",memory_mode_after,
                      sizeof(memory_mode_after)));
    CHECK(scalar_text(graph,"PRAGMA journal_mode;",graph_mode_after,
                      sizeof(graph_mode_after)));
    CHECK(!strcmp(memory_mode_before,memory_mode_after)&&
          !strcmp(graph_mode_before,graph_mode_after));

    char crash_task[128],quick[32];
    CHECK(prepare_feedback_task(g,&target,memory.id,memory.content,"commit-crash",
        "external_verified","test","used",crash_task)==0);
    int crash_events_before=scalar(db,"SELECT COUNT(*) FROM global_evolution_event;");
    int crash_versions_before=scalar(graph,
        "SELECT COUNT(*) FROM global_cross_project_edge_version;");
    double crash_confidence_before=scalar_double(db,
        "SELECT confidence FROM memory_item WHERE id='controller-memory';");
    cbm_global_memory_close(g);g=NULL;db=NULL;graph=NULL;
    PROCESS_INFORMATION crash_process;DWORD crash_exit=0;
    CHECK(stage14_launch_evolution_child("--stage14-crash-child",memory_path,graph_path,
                                         crash_task,"controller-commit-crash",
                                         &crash_process));
    CHECK(stage14_wait_evolution_child(&crash_process,&crash_exit)&&crash_exit==86);
    g=cbm_global_memory_open(memory_path,graph_path);CHECK(g!=NULL);
    db=cbm_global_memory_db(g);graph=cbm_global_graph_db(g);
    CHECK(scalar_text(db,"PRAGMA quick_check;",quick,sizeof(quick))&&
          !strcmp(quick,"ok"));
    CHECK(scalar_text(graph,"PRAGMA quick_check;",quick,sizeof(quick))&&
          !strcmp(quick,"ok"));
    CHECK(scalar(db,"SELECT COUNT(*) FROM pragma_foreign_key_check;")==0);
    CHECK(scalar(graph,"SELECT COUNT(*) FROM pragma_foreign_key_check;")==0);
    CHECK(scalar(db,"SELECT COUNT(*) FROM global_evolution_event;")==crash_events_before);
    CHECK(scalar(graph,
        "SELECT COUNT(*) FROM global_cross_project_edge_version;")==crash_versions_before);
    CHECK(scalar_double(db,
        "SELECT confidence FROM memory_item WHERE id='controller-memory';")
        ==crash_confidence_before);
    CHECK(cbm_evolution_event_chain_verify_for_test(db,&bad_sequence)==CBM_STORE_OK&&
          bad_sequence==0);
    input=(cbm_evolution_task_input_t){
        .mode="bounded_canary",.task_id=crash_task,
        .idempotency_key="controller-commit-crash",.isolated_write_allowed=1
    };
    CHECK(cbm_evolution_apply_completed_task(g,&input,&result)==CBM_STORE_OK);
    CHECK(result.wrote==1&&result.evolution_events==2&&result.cross_project_edges==1);
    cbm_evolution_result_free(&result);
    CHECK(scalar(db,"SELECT COUNT(*) FROM global_evolution_event;")==crash_events_before+2);
    CHECK(scalar(graph,
        "SELECT COUNT(*) FROM global_cross_project_edge_version;")==crash_versions_before+1);
    CHECK(scalar_double(db,
        "SELECT confidence FROM memory_item WHERE id='controller-memory';")
        >crash_confidence_before+0.039);
    CHECK(cbm_evolution_apply_completed_task(g,&input,&result)==CBM_STORE_REPLAYED);
    CHECK(result.wrote==0&&result.replayed==1);cbm_evolution_result_free(&result);
    CHECK(scalar(db,"SELECT COUNT(*) FROM global_evolution_event;")==crash_events_before+2);
    CHECK(scalar(graph,
        "SELECT COUNT(*) FROM global_cross_project_edge_version;")==crash_versions_before+1);
    CHECK(cbm_evolution_event_chain_verify_for_test(db,&bad_sequence)==CBM_STORE_OK&&
          bad_sequence==0);
    fprintf(stderr,
        "stage14-e-event-chain v3_bound_field_tamper=PASS v2_compatible=PASS "
        "cross_process_mutex=PASS commit_hook_pre_pager_exit=86 "
        "crash_rollback_both=PASS reopen_quick_fk=PASS replay_zero_write=PASS\n");
    cbm_global_memory_close(g);
#endif
    return 0;
}

typedef struct {
    const char *memory_path;
    int begin_rc;
} stage14_toctou_context_t;

static void stage14_toctou_revoke_hook(void *opaque) {
    stage14_toctou_context_t *context=(stage14_toctou_context_t *)opaque;
    sqlite3 *db=NULL;context->begin_rc=SQLITE_ERROR;
    if(sqlite3_open_v2(context->memory_path,&db,SQLITE_OPEN_READWRITE,NULL)!=SQLITE_OK){sqlite3_close(db);return;}
    sqlite3_busy_timeout(db,0);context->begin_rc=sqlite3_exec(db,"BEGIN IMMEDIATE;",NULL,NULL,NULL);
    if(context->begin_rc==SQLITE_OK)sqlite3_exec(db,"ROLLBACK;",NULL,NULL,NULL);
    sqlite3_close(db);
}

static int test_evolution_toctou_snapshot_is_transactional(void) {
#ifdef _WIN32
    char temp[MAX_PATH],memory_path[MAX_PATH],graph_path[MAX_PATH];CHECK(GetTempPathA(MAX_PATH,temp)>0);
    snprintf(memory_path,sizeof(memory_path),"%sstage14-toctou-%lu-%lld-memory.db",temp,(unsigned long)_getpid(),(long long)time(NULL));
    snprintf(graph_path,sizeof(graph_path),"%sstage14-toctou-%lu-%lld-graph.db",temp,(unsigned long)_getpid(),(long long)time(NULL));
    cbm_global_memory_t *g=cbm_global_memory_open(memory_path,graph_path);CHECK(g!=NULL);
    cbm_project_resolution_t target={0};CHECK(cbm_project_resolve("H:\\Stage14 TOCTOU Target",NULL,NULL,&target)==0);
    cbm_memory_item_t memory=item("stage14-toctou-memory","stage14-toctou-source","Lock evidence snapshot before applying feedback",0.50);
    CHECK(stage14_add_rank_item(g,&memory));char task_id[128],terminal_event[128],terminal_evidence[128];
    CHECK(prepare_existing_feedback_chain(g,&target,memory.id,memory.content,"toctou-chain","correct",task_id,terminal_event,terminal_evidence)==0);
    stage14_toctou_context_t hook={.memory_path=memory_path};
    cbm_evolution_task_input_t input={.mode="bounded_canary",.task_id=task_id,
        .idempotency_key="stage14-toctou-controller",.isolated_write_allowed=1,
        .snapshot_hook=stage14_toctou_revoke_hook,.snapshot_hook_context=&hook};
    cbm_evolution_result_t result={0};sqlite3 *db=cbm_global_memory_db(g);
    CHECK(cbm_evolution_apply_completed_task(g,&input,&result)==CBM_STORE_OK);CHECK(result.wrote==1&&result.eligible==1);cbm_evolution_result_free(&result);
    CHECK(hook.begin_rc==SQLITE_BUSY||hook.begin_rc==SQLITE_LOCKED);
    int events=scalar(db,"SELECT COUNT(*) FROM global_evolution_event;");double confidence=scalar_double(db,"SELECT confidence FROM memory_item WHERE id='stage14-toctou-memory';");
    sqlite3_stmt *stmt=NULL;CHECK(sqlite3_prepare_v2(db,"SELECT session_id,candidate_id,usage_id FROM feedback_event WHERE event_id=?1;",-1,&stmt,NULL)==SQLITE_OK);
    sqlite3_bind_text(stmt,1,terminal_event,-1,SQLITE_TRANSIENT);CHECK(sqlite3_step(stmt)==SQLITE_ROW);
    char session_id[128],candidate_id[128],usage_id[128];snprintf(session_id,sizeof(session_id),"%s",sqlite3_column_text(stmt,0));snprintf(candidate_id,sizeof(candidate_id),"%s",sqlite3_column_text(stmt,1));snprintf(usage_id,sizeof(usage_id),"%s",sqlite3_column_text(stmt,2));sqlite3_finalize(stmt);
    char withdrawn_event[128],withdrawn_evidence[128];CHECK(observe_chain_feedback(g,&target,task_id,session_id,candidate_id,usage_id,"toctou-post-withdraw","withdraw",terminal_event,withdrawn_event,withdrawn_evidence)==0);
    input.snapshot_hook=NULL;input.snapshot_hook_context=NULL;
    CHECK(cbm_evolution_apply_completed_task(g,&input,&result)==CBM_STORE_IDEMPOTENCY_CONFLICT);cbm_evolution_result_free(&result);
    CHECK(scalar(db,"SELECT COUNT(*) FROM global_evolution_event;")==events);
    CHECK(scalar_double(db,"SELECT confidence FROM memory_item WHERE id='stage14-toctou-memory';")==confidence);
    fprintf(stderr,"stage14-e-toctou blocked_rc=%d committed_events=%d post_revoke_replay=IDEMPOTENCY_CONFLICT\n",hook.begin_rc,events);
    cbm_global_memory_close(g);remove(graph_path);remove(memory_path);
#endif
    return 0;
}

static int test_task_evolution_production_manifest_gate(void) {
#ifdef _WIN32
    char temp[MAX_PATH],memory_path[MAX_PATH],graph_path[MAX_PATH];
    CHECK(GetTempPathA(MAX_PATH,temp)>0);
    snprintf(memory_path,sizeof(memory_path),"%sstage14-production-evolution-%lu-%lld-memory.db",
             temp,(unsigned long)_getpid(),(long long)time(NULL));
    snprintf(graph_path,sizeof(graph_path),"%sstage14-production-evolution-%lu-%lld-graph.db",
             temp,(unsigned long)_getpid(),(long long)time(NULL));
    cbm_global_memory_t *g=cbm_global_memory_open(memory_path,graph_path);CHECK(g!=NULL);
    cbm_project_resolution_t target={0};
    CHECK(cbm_project_resolve("H:\\Stage14 Production Evolution Target",NULL,NULL,&target)==0);
    cbm_memory_item_t memory=item("stage14-production-evolution-memory",
                                  "stage14-production-evolution-source",
                                  "Manifest bound production evolution marker",0.50);
    CHECK(stage14_add_rank_item(g,&memory));
    char task_id[128];CHECK(prepare_feedback_task(g,&target,memory.id,memory.content,
        "production-manifest","external_verified","runtime","used",task_id)==0);
    cbm_evolution_task_input_t input={
        .mode="bounded_canary",.task_id=task_id,.project_uuid=target.project_uuid,
        .run_id="stage14-production-evolution-run",
        .idempotency_key="stage14-production-evolution-key",
        .max_evolution_events=2,.max_cross_project_edges=1,
        .production_gate_allowed=1
    };
    cbm_evolution_result_t result={0};
    cbm_evolution_task_input_t cap_boundary=input;
    cap_boundary.max_evolution_events=17;cap_boundary.max_cross_project_edges=16;
    CHECK(cbm_evolution_plan_completed_task(g,&cap_boundary,&result)==CBM_STORE_OK);
    cbm_evolution_result_free(&result);
    cap_boundary.max_evolution_events=18;
    CHECK(cbm_evolution_plan_completed_task(g,&cap_boundary,&result)==CBM_STORE_REJECTED);
    cbm_evolution_result_free(&result);
    cap_boundary.max_evolution_events=17;cap_boundary.max_cross_project_edges=17;
    CHECK(cbm_evolution_plan_completed_task(g,&cap_boundary,&result)==CBM_STORE_REJECTED);
    cbm_evolution_result_free(&result);
    sqlite3 *db=cbm_global_memory_db(g);
    sqlite3 *graph=cbm_global_graph_db(g);
    sqlite3 *atomic_check=NULL;
    CHECK(cbm_evolution_open_atomic_db_for_test(db,graph,&atomic_check)==CBM_STORE_OK);
    CHECK(atomic_check!=NULL&&scalar(atomic_check,"PRAGMA foreign_keys;")==1);
    CHECK(scalar(atomic_check,
        "SELECT COUNT(*) FROM stage14_graph.sqlite_master WHERE type='table';")>=0);
    CHECK(sqlite3_close(atomic_check)==SQLITE_OK);
    CHECK(cbm_evolution_attach_authorizer_failure_for_test(db,graph)==CBM_STORE_ERR);
    CHECK(scalar(db,
        "SELECT COUNT(*) FROM pragma_database_list WHERE name='stage14_graph';")==0);
    CHECK(sqlite3_exec(db,
        "ATTACH DATABASE ':memory:' AS forbidden_stage14;",NULL,NULL,NULL)==SQLITE_AUTH);
    char size_task[128];
    CHECK(prepare_feedback_task(g,&target,memory.id,memory.content,
        "production-manifest-size","external_verified","runtime","used",size_task)==0);
    cbm_evolution_task_input_t size_input=input;
    size_input.task_id=size_task;size_input.run_id="stage14-production-size-run";
    size_input.idempotency_key="stage14-production-size-key";
    size_input.manifest_path=NULL;size_input.manifest_sha256=NULL;
    CHECK(cbm_evolution_plan_completed_task(g,&size_input,&result)==CBM_STORE_OK);
    char *size_plan=strdup(result.report_json);CHECK(size_plan!=NULL);
    cbm_evolution_result_free(&result);
    char max_size_path[MAX_PATH],over_size_path[MAX_PATH];
    char max_size_hash[65],over_size_hash[65];
    snprintf(max_size_path,sizeof(max_size_path),
             "%sstage14-task-evolution-max-size-%lu.json",temp,(unsigned long)_getpid());
    snprintf(over_size_path,sizeof(over_size_path),
             "%sstage14-task-evolution-over-size-%lu.json",temp,(unsigned long)_getpid());
    CHECK(stage14_write_task_evolution_manifest(size_plan,max_size_path,NULL,NULL,NULL,NULL,
          -1,-1,NULL,NULL,0,max_size_hash)==0);
    CHECK(stage14_write_task_evolution_manifest(size_plan,over_size_path,NULL,NULL,NULL,NULL,
          -1,-1,NULL,NULL,0,over_size_hash)==0);free(size_plan);
    CHECK(stage14_pad_manifest_file(max_size_path,1024*1024,max_size_hash)==0);
    CHECK(stage14_pad_manifest_file(over_size_path,1024*1024+1,over_size_hash)==0);
    size_input.manifest_path=max_size_path;size_input.manifest_sha256=max_size_hash;
    CHECK(cbm_evolution_apply_completed_task(g,&size_input,&result)==CBM_STORE_OK);
    cbm_evolution_result_free(&result);
    int size_events=scalar(cbm_global_memory_db(g),
        "SELECT COUNT(*) FROM global_evolution_event;");
    int size_edges=scalar(cbm_global_graph_db(g),
        "SELECT COUNT(*) FROM global_cross_project_edge_version;");
    size_input.manifest_path=over_size_path;size_input.manifest_sha256=over_size_hash;
    CHECK(cbm_evolution_apply_completed_task(g,&size_input,&result)==CBM_STORE_REJECTED);
    cbm_evolution_result_free(&result);
    CHECK(scalar(cbm_global_memory_db(g),"SELECT COUNT(*) FROM global_evolution_event;")
          ==size_events);
    CHECK(scalar(cbm_global_graph_db(g),
          "SELECT COUNT(*) FROM global_cross_project_edge_version;")==size_edges);
    remove(over_size_path);remove(max_size_path);
    int events_before=scalar(db,"SELECT COUNT(*) FROM global_evolution_event;");
    int edges_before=scalar(graph,"SELECT COUNT(*) FROM global_cross_project_edge_version;");
    double confidence_before=scalar_double(
        db,"SELECT confidence FROM memory_item WHERE id='stage14-production-evolution-memory';");

    stage14_set_env("CBM_STAGE14_PRODUCTION_GATE","1");
    stage14_set_env("CBM_STAGE14_EVOLUTION_MODE","bounded_canary");
    CHECK(cbm_evolution_apply_completed_task(g,&input,&result)==CBM_STORE_REJECTED);
    cbm_evolution_result_free(&result);
    input.mode="active";
    CHECK(cbm_evolution_apply_completed_task(g,&input,&result)==CBM_STORE_REJECTED);
    cbm_evolution_result_free(&result);input.mode="bounded_canary";
    CHECK(scalar(db,"SELECT COUNT(*) FROM global_evolution_event;")==events_before);
    CHECK(scalar(graph,"SELECT COUNT(*) FROM global_cross_project_edge_version;")==edges_before);
    CHECK(scalar_double(db,
        "SELECT confidence FROM memory_item WHERE id='stage14-production-evolution-memory';")
        ==confidence_before);

    input.production_gate_allowed=0;
    CHECK(cbm_evolution_plan_completed_task(g,&input,&result)==CBM_STORE_OK);
    CHECK(result.wrote==0&&result.eligible==1&&result.planned_evolution_events==2&&
          result.planned_cross_project_edges==1&&strlen(result.request_sha256)==64);
    CHECK(result.report_json&&strstr(result.report_json,
          "\"schema\":\"stage14-task-evolution-plan/v1\"")&&
          strstr(result.report_json,"\"memory_item_ids\":[\"stage14-production-evolution-memory\"]")&&
          strstr(result.report_json,"\"feedback_event_ids\":["));
    char *plan=strdup(result.report_json);CHECK(plan!=NULL);cbm_evolution_result_free(&result);
    CHECK(scalar(db,"SELECT COUNT(*) FROM global_evolution_event;")==events_before);
    cbm_evolution_task_input_t wrong_project=input;
    wrong_project.project_uuid="wrong-project";
    CHECK(cbm_evolution_plan_completed_task(g,&wrong_project,&result)==CBM_STORE_REJECTED);
    cbm_evolution_task_input_t insufficient_cap=input;
    insufficient_cap.max_evolution_events=1;
    CHECK(cbm_evolution_plan_completed_task(g,&insufficient_cap,&result)==CBM_STORE_REJECTED);
    cbm_evolution_task_input_t active=input;
    active.mode="active";active.run_id="stage14-production-evolution-active-run";
    active.idempotency_key="stage14-production-evolution-active-key";
    CHECK(cbm_evolution_plan_completed_task(g,&active,&result)==CBM_STORE_OK);
    char *active_plan=strdup(result.report_json);CHECK(active_plan!=NULL);
    cbm_evolution_result_free(&result);
    char active_path[MAX_PATH],active_hash[65];
    snprintf(active_path,sizeof(active_path),"%sstage14-task-evolution-active-%lu-%lld.json",
             temp,(unsigned long)_getpid(),(long long)time(NULL));
    CHECK(stage14_write_task_evolution_manifest(active_plan,active_path,NULL,NULL,NULL,NULL,
          -1,-1,NULL,NULL,0,active_hash)==0);free(active_plan);
    active.production_gate_allowed=1;active.manifest_path=active_path;
    active.manifest_sha256=active_hash;
    CHECK(cbm_evolution_apply_completed_task(g,&active,&result)==CBM_STORE_REJECTED);
    cbm_evolution_result_free(&result);
    CHECK(scalar(db,"SELECT COUNT(*) FROM global_evolution_event;")==events_before);
    input.production_gate_allowed=1;

    enum { MANIFEST_COUNT=10 };
    char paths[MANIFEST_COUNT][MAX_PATH],hashes[MANIFEST_COUNT][65];
    const char *labels[MANIFEST_COUNT]={"exact","wrong-mode","wrong-task","wrong-request",
        "wrong-events-cap","wrong-edge-cap","wrong-memory","wrong-feedback","extra-field","drift"};
    for(int i=0;i<MANIFEST_COUNT;i++){
        snprintf(paths[i],sizeof(paths[i]),"%sstage14-task-evolution-%s-%lu-%lld.json",
                 temp,labels[i],(unsigned long)_getpid(),(long long)time(NULL));
    }
    char wrong_request[65];memset(wrong_request,'0',64);wrong_request[64]=0;
    CHECK(stage14_write_task_evolution_manifest(plan,paths[0],NULL,NULL,NULL,NULL,
          -1,-1,NULL,NULL,0,hashes[0])==0);
    CHECK(stage14_write_task_evolution_manifest(plan,paths[1],"active",NULL,NULL,NULL,
          -1,-1,NULL,NULL,0,hashes[1])==0);
    CHECK(stage14_write_task_evolution_manifest(plan,paths[2],NULL,"wrong-task",NULL,NULL,
          -1,-1,NULL,NULL,0,hashes[2])==0);
    CHECK(stage14_write_task_evolution_manifest(plan,paths[3],NULL,NULL,NULL,wrong_request,
          -1,-1,NULL,NULL,0,hashes[3])==0);
    CHECK(stage14_write_task_evolution_manifest(plan,paths[4],NULL,NULL,NULL,NULL,
          1,-1,NULL,NULL,0,hashes[4])==0);
    CHECK(stage14_write_task_evolution_manifest(plan,paths[5],NULL,NULL,NULL,NULL,
          -1,0,NULL,NULL,0,hashes[5])==0);
    CHECK(stage14_write_task_evolution_manifest(plan,paths[6],NULL,NULL,NULL,NULL,
          -1,-1,"wrong-memory",NULL,0,hashes[6])==0);
    CHECK(stage14_write_task_evolution_manifest(plan,paths[7],NULL,NULL,NULL,NULL,
          -1,-1,NULL,"wrong-feedback",0,hashes[7])==0);
    CHECK(stage14_write_task_evolution_manifest(plan,paths[8],NULL,NULL,NULL,NULL,
          -1,-1,NULL,NULL,1,hashes[8])==0);

    const char *short_manifest_paths[]={"C","C:","/"};
    for(size_t i=0;i<sizeof(short_manifest_paths)/sizeof(short_manifest_paths[0]);i++){
        input.manifest_path=short_manifest_paths[i];input.manifest_sha256=hashes[0];
        CHECK(cbm_evolution_apply_completed_task(g,&input,&result)==CBM_STORE_REJECTED);
        cbm_evolution_result_free(&result);
    }
    input.manifest_path="relative-task-evolution-manifest.json";
    input.manifest_sha256=hashes[0];
    CHECK(cbm_evolution_apply_completed_task(g,&input,&result)==CBM_STORE_REJECTED);
    cbm_evolution_result_free(&result);
    char uppercase_hash[65];
    for(int i=0;i<64;i++)uppercase_hash[i]=hashes[0][i]>='a'&&hashes[0][i]<='f'?
        (char)(hashes[0][i]-('a'-'A')):hashes[0][i];
    uppercase_hash[64]=0;
    input.manifest_path=paths[0];input.manifest_sha256=uppercase_hash;
    CHECK(cbm_evolution_apply_completed_task(g,&input,&result)==CBM_STORE_REJECTED);
    cbm_evolution_result_free(&result);
    input.manifest_path=paths[0];input.manifest_sha256=wrong_request;
    CHECK(cbm_evolution_apply_completed_task(g,&input,&result)==CBM_STORE_REJECTED);
    cbm_evolution_result_free(&result);
    for(int i=1;i<=8;i++){
        input.manifest_path=paths[i];input.manifest_sha256=hashes[i];
        CHECK(cbm_evolution_apply_completed_task(g,&input,&result)==CBM_STORE_REJECTED);
        cbm_evolution_result_free(&result);
    }
    CHECK(scalar(db,"SELECT COUNT(*) FROM global_evolution_event;")==events_before);
    CHECK(scalar(graph,"SELECT COUNT(*) FROM global_cross_project_edge_version;")==edges_before);

    char drift_task[128];CHECK(prepare_feedback_task(g,&target,memory.id,memory.content,
        "production-manifest-drift","external_verified","runtime","used",drift_task)==0);
    cbm_evolution_task_input_t drift=input;drift.task_id=drift_task;
    drift.run_id="stage14-production-evolution-drift-run";
    drift.idempotency_key="stage14-production-evolution-drift-key";
    drift.manifest_path=NULL;drift.manifest_sha256=NULL;
    CHECK(cbm_evolution_plan_completed_task(g,&drift,&result)==CBM_STORE_OK);
    char *drift_plan=strdup(result.report_json);CHECK(drift_plan!=NULL);
    cbm_evolution_result_free(&result);
    CHECK(stage14_write_task_evolution_manifest(drift_plan,paths[9],NULL,NULL,NULL,NULL,
          -1,-1,NULL,NULL,0,hashes[9])==0);free(drift_plan);
    sqlite3_stmt *drift_stmt=NULL;
    CHECK(sqlite3_prepare_v2(db,
        "SELECT f.session_id,f.candidate_id,f.usage_id,f.event_id "
        "FROM codex_task_attribution a JOIN feedback_event f "
        "ON f.event_id=a.feedback_event_id WHERE a.task_id=?1;",
        -1,&drift_stmt,NULL)==SQLITE_OK);
    sqlite3_bind_text(drift_stmt,1,drift_task,-1,SQLITE_TRANSIENT);
    CHECK(sqlite3_step(drift_stmt)==SQLITE_ROW);
    char drift_session[128],drift_candidate[128],drift_usage[128],drift_base_event[128];
    snprintf(drift_session,sizeof(drift_session),"%s",sqlite3_column_text(drift_stmt,0));
    snprintf(drift_candidate,sizeof(drift_candidate),"%s",sqlite3_column_text(drift_stmt,1));
    snprintf(drift_usage,sizeof(drift_usage),"%s",sqlite3_column_text(drift_stmt,2));
    snprintf(drift_base_event,sizeof(drift_base_event),"%s",sqlite3_column_text(drift_stmt,3));
    sqlite3_finalize(drift_stmt);
    char drift_terminal_event[128],drift_terminal_evidence[128];
    CHECK(observe_chain_feedback(g,&target,drift_task,drift_session,drift_candidate,
        drift_usage,"production-manifest-drift-terminal","correct",drift_base_event,
        drift_terminal_event,drift_terminal_evidence)==0);
    drift.manifest_path=paths[9];drift.manifest_sha256=hashes[9];
    CHECK(cbm_evolution_apply_completed_task(g,&drift,&result)==CBM_STORE_REJECTED);
    cbm_evolution_result_free(&result);
    CHECK(scalar(db,"SELECT COUNT(*) FROM global_evolution_event;")==events_before);

    input.manifest_path=paths[0];input.manifest_sha256=hashes[0];
    CHECK(cbm_evolution_apply_completed_task(g,&input,&result)==CBM_STORE_OK);
    CHECK(result.wrote==1&&result.evolution_events==2&&result.cross_project_edges==1);
    cbm_evolution_result_free(&result);
    int applied_events=scalar(db,"SELECT COUNT(*) FROM global_evolution_event;");
    int applied_edges=scalar(graph,"SELECT COUNT(*) FROM global_cross_project_edge_version;");
    double applied_confidence=scalar_double(
        db,"SELECT confidence FROM memory_item WHERE id='stage14-production-evolution-memory';");
    CHECK(applied_events==events_before+2&&applied_edges==edges_before+1&&
          applied_confidence>confidence_before);
    CHECK(cbm_evolution_apply_completed_task(g,&input,&result)==CBM_STORE_REPLAYED);
    CHECK(result.wrote==0&&result.replayed==1);cbm_evolution_result_free(&result);
    CHECK(scalar(db,"SELECT COUNT(*) FROM global_evolution_event;")==applied_events);
    CHECK(scalar(graph,"SELECT COUNT(*) FROM global_cross_project_edge_version;")==applied_edges);
    CHECK(scalar_double(db,
        "SELECT confidence FROM memory_item WHERE id='stage14-production-evolution-memory';")
        ==applied_confidence);

    cbm_evolution_task_input_t altered=input;
    altered.run_id="stage14-production-evolution-run-altered";
    altered.manifest_path=NULL;altered.manifest_sha256=NULL;
    CHECK(cbm_evolution_plan_completed_task(g,&altered,&result)==CBM_STORE_OK);
    char *altered_plan=strdup(result.report_json);CHECK(altered_plan!=NULL);
    cbm_evolution_result_free(&result);
    char altered_path[MAX_PATH],altered_hash[65];
    snprintf(altered_path,sizeof(altered_path),
             "%sstage14-task-evolution-altered-%lu-%lld.json",temp,
             (unsigned long)_getpid(),(long long)time(NULL));
    CHECK(stage14_write_task_evolution_manifest(altered_plan,altered_path,NULL,NULL,NULL,NULL,
          -1,-1,NULL,NULL,0,altered_hash)==0);free(altered_plan);
    altered.manifest_path=altered_path;altered.manifest_sha256=altered_hash;
    CHECK(cbm_evolution_apply_completed_task(g,&altered,&result)==
          CBM_STORE_IDEMPOTENCY_CONFLICT);
    cbm_evolution_result_free(&result);
    CHECK(scalar(db,"SELECT COUNT(*) FROM global_evolution_event;")==applied_events);
    CHECK(scalar(graph,"SELECT COUNT(*) FROM global_cross_project_edge_version;")==applied_edges);

    char failed_task[128];CHECK(prepare_feedback_task(g,&target,memory.id,memory.content,
        "production-manifest-zero","external_verified","runtime","used",failed_task)==0);
    CHECK(stage14_append_lifecycle_state(db,failed_task,"failed","failed","failed")==0);
    cbm_evolution_task_input_t zero={
        .mode="bounded_canary",.task_id=failed_task,.project_uuid=target.project_uuid,
        .run_id="stage14-production-evolution-zero-run",
        .idempotency_key="stage14-production-evolution-zero-key",
        .max_evolution_events=1,.max_cross_project_edges=0,
        .production_gate_allowed=1
    };
    CHECK(cbm_evolution_plan_completed_task(g,&zero,&result)==CBM_STORE_OK);
    CHECK(result.eligible==0&&result.positive==0&&result.planned_evolution_events==1&&
          result.planned_cross_project_edges==0);
    char *zero_plan=strdup(result.report_json);CHECK(zero_plan!=NULL);
    cbm_evolution_result_free(&result);
    char zero_path[MAX_PATH],zero_hash[65];
    snprintf(zero_path,sizeof(zero_path),"%sstage14-task-evolution-zero-%lu-%lld.json",
             temp,(unsigned long)_getpid(),(long long)time(NULL));
    CHECK(stage14_write_task_evolution_manifest(zero_plan,zero_path,NULL,NULL,NULL,NULL,
          -1,-1,NULL,NULL,0,zero_hash)==0);free(zero_plan);
    zero.manifest_path=zero_path;zero.manifest_sha256=zero_hash;
    int zero_total_before=scalar(db,"SELECT COUNT(*) FROM global_evolution_event;");
    int zero_business_before=scalar(db,
        "SELECT COUNT(*) FROM global_evolution_event WHERE operation<>'noop';");
    int zero_noop_before=scalar(db,
        "SELECT COUNT(*) FROM global_evolution_event WHERE operation='noop';");
    double zero_confidence_before=scalar_double(
        db,"SELECT confidence FROM memory_item WHERE id='stage14-production-evolution-memory';");
    CHECK(cbm_evolution_apply_completed_task(g,&zero,&result)==CBM_STORE_OK);
    CHECK(result.wrote==0&&result.eligible==0&&result.positive==0&&
          result.evolution_events==1&&result.cross_project_edges==0);
    cbm_evolution_result_free(&result);
    int zero_total_after=scalar(db,"SELECT COUNT(*) FROM global_evolution_event;");
    CHECK(zero_total_after==zero_total_before+1);
    CHECK(scalar(db,
        "SELECT COUNT(*) FROM global_evolution_event WHERE operation<>'noop';")
        ==zero_business_before);
    CHECK(scalar(db,
        "SELECT COUNT(*) FROM global_evolution_event WHERE operation='noop';")
        ==zero_noop_before+1);
    CHECK(scalar(graph,"SELECT COUNT(*) FROM global_cross_project_edge_version;")
        ==applied_edges);
    CHECK(scalar_double(db,
        "SELECT confidence FROM memory_item WHERE id='stage14-production-evolution-memory';")
        ==zero_confidence_before);
    CHECK(cbm_evolution_apply_completed_task(g,&zero,&result)==CBM_STORE_REPLAYED);
    CHECK(result.wrote==0&&result.replayed==1&&result.evolution_events==0);
    cbm_evolution_result_free(&result);
    CHECK(scalar(db,"SELECT COUNT(*) FROM global_evolution_event;")==zero_total_after);

    cbm_evolution_task_input_t zero_altered=zero;
    zero_altered.run_id="stage14-production-evolution-zero-run-altered";
    zero_altered.manifest_path=NULL;zero_altered.manifest_sha256=NULL;
    CHECK(cbm_evolution_plan_completed_task(g,&zero_altered,&result)==CBM_STORE_OK);
    CHECK(result.eligible==0&&result.planned_evolution_events==1);
    char *zero_altered_plan=strdup(result.report_json);CHECK(zero_altered_plan!=NULL);
    cbm_evolution_result_free(&result);
    char zero_altered_path[MAX_PATH],zero_altered_hash[65];
    snprintf(zero_altered_path,sizeof(zero_altered_path),
             "%sstage14-task-evolution-zero-altered-%lu-%lld.json",temp,
             (unsigned long)_getpid(),(long long)time(NULL));
    CHECK(stage14_write_task_evolution_manifest(zero_altered_plan,zero_altered_path,
          NULL,NULL,NULL,NULL,-1,-1,NULL,NULL,0,zero_altered_hash)==0);
    free(zero_altered_plan);
    zero_altered.manifest_path=zero_altered_path;
    zero_altered.manifest_sha256=zero_altered_hash;
    CHECK(cbm_evolution_apply_completed_task(g,&zero_altered,&result)==
          CBM_STORE_IDEMPOTENCY_CONFLICT);
    cbm_evolution_result_free(&result);
    CHECK(scalar(db,"SELECT COUNT(*) FROM global_evolution_event;")==zero_total_after);

    CHECK(stage14_append_lifecycle_state(db,failed_task,"restored","completed",
                                         "completed")==0);
    cbm_evolution_task_input_t zero_restored=zero;
    zero_restored.max_evolution_events=2;zero_restored.max_cross_project_edges=1;
    zero_restored.manifest_path=NULL;zero_restored.manifest_sha256=NULL;
    CHECK(cbm_evolution_plan_completed_task(g,&zero_restored,&result)==CBM_STORE_OK);
    CHECK(result.eligible==1&&result.positive==1&&result.planned_evolution_events==2&&
          result.planned_cross_project_edges==1);
    char *zero_restored_plan=strdup(result.report_json);CHECK(zero_restored_plan!=NULL);
    cbm_evolution_result_free(&result);
    char zero_restored_path[MAX_PATH],zero_restored_hash[65];
    snprintf(zero_restored_path,sizeof(zero_restored_path),
             "%sstage14-task-evolution-zero-restored-%lu-%lld.json",temp,
             (unsigned long)_getpid(),(long long)time(NULL));
    CHECK(stage14_write_task_evolution_manifest(zero_restored_plan,zero_restored_path,
          NULL,NULL,NULL,NULL,-1,-1,NULL,NULL,0,zero_restored_hash)==0);
    free(zero_restored_plan);
    zero_restored.manifest_path=zero_restored_path;
    zero_restored.manifest_sha256=zero_restored_hash;
    CHECK(cbm_evolution_apply_completed_task(g,&zero_restored,&result)==
          CBM_STORE_IDEMPOTENCY_CONFLICT);
    cbm_evolution_result_free(&result);
    CHECK(scalar(db,"SELECT COUNT(*) FROM global_evolution_event;")==zero_total_after);
    CHECK(scalar(graph,"SELECT COUNT(*) FROM global_cross_project_edge_version;")
        ==applied_edges);
    CHECK(scalar_double(db,
        "SELECT confidence FROM memory_item WHERE id='stage14-production-evolution-memory';")
        ==zero_confidence_before);

    fprintf(stderr,"stage14-task-evolution-production manifest_gate=PASS plan_read_only=PASS "
        "raw_sha=PASS exact_fields=PASS allowlists=PASS caps=PASS "
        "manifest_size_boundary=PASS snapshot_drift=PASS "
        "replay_zero_write=PASS altered_payload_conflict=PASS latest_lifecycle=PASS "
        "zero_hit_audit_key=PASS zero_hit_no_reinforcement=PASS "
        "attach_authorizer_fail_cleanup=PASS\n");
    stage14_set_env("CBM_STAGE14_PRODUCTION_GATE",NULL);
    stage14_set_env("CBM_STAGE14_EVOLUTION_MODE",NULL);
    free(plan);remove(zero_restored_path);remove(zero_altered_path);remove(zero_path);
    remove(altered_path);remove(active_path);
    for(int i=0;i<MANIFEST_COUNT;i++)remove(paths[i]);
    cbm_global_memory_close(g);remove(graph_path);remove(memory_path);
#endif
    return 0;
}

static int stage14_interrupt_sqlite_step(void *context) {
    int *calls=(int *)context;(*calls)++;return 1;
}

static int test_evolution_lookup_step_errors_fail_closed(void) {
    sqlite3 *db=NULL;CHECK(sqlite3_open(":memory:",&db)==SQLITE_OK);
    CHECK(sqlite3_exec(db,
        "CREATE TABLE global_evolution_event("
        "idempotency_key TEXT PRIMARY KEY,payload_sha256 TEXT NOT NULL);"
        "INSERT INTO global_evolution_event(idempotency_key,payload_sha256) VALUES("
        "'fault-key','aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa');",
        NULL,NULL,NULL)==SQLITE_OK);
    const char *hash="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    int exact=0,progress_calls=0;
    sqlite3_progress_handler(db,1,stage14_interrupt_sqlite_step,&progress_calls);
    CHECK(cbm_evolution_event_lookup_for_test(db,"fault-key",hash,&exact)==CBM_STORE_ERR);
    CHECK(progress_calls>0);
    sqlite3_progress_handler(db,0,NULL,NULL);
    exact=0;
    CHECK(cbm_evolution_event_lookup_for_test(db,"missing-key",hash,&exact)==
          CBM_STORE_NOT_FOUND);
    CHECK(exact==0);
    CHECK(cbm_evolution_event_lookup_for_test(db,"fault-key",hash,&exact)==CBM_STORE_OK);
    CHECK(exact==1);
    exact=1;
    CHECK(cbm_evolution_event_lookup_for_test(db,"fault-key",
          "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
          &exact)==CBM_STORE_OK);
    CHECK(exact==0);
    sqlite3_close(db);
    fprintf(stderr,"stage14-evolution-step-errors interrupt_fail_closed=PASS "
        "done_not_found=PASS row_exact=PASS\n");
    return 0;
}

static int test_global_migration_immutable_uri_encoding(void) {
    char uri[4096];
    CHECK(cbm_global_migration_immutable_uri_for_test(
        "C:\\Dir A\\100%#?\\\xE4\xB8\xAD\xE6\x96\x87.db",1,uri,sizeof(uri))==CBM_STORE_OK);
    CHECK(strcmp(uri,"file:///C:/Dir%20A/100%25%23%3F/%E4%B8%AD%E6%96%87.db?immutable=1")==0);
    CHECK(cbm_global_migration_immutable_uri_for_test(
        "\\\\server\\share name\\folder\\db#?.sqlite",1,uri,sizeof(uri))==CBM_STORE_OK);
    CHECK(strcmp(uri,"file://server/share%20name/folder/db%23%3F.sqlite?immutable=1")==0);
    CHECK(cbm_global_migration_immutable_uri_for_test(
        "/tmp/a\\b c#?.db",0,uri,sizeof(uri))==CBM_STORE_OK);
    CHECK(strcmp(uri,"file:///tmp/a%5Cb%20c%23%3F.db?immutable=1")==0);
#ifdef _WIN32
    CHECK(cbm_global_migration_immutable_uri_for_test(
        "C:\\Dir A\\100%#?\\\xE4\xB8\xAD\xE6\x96\x87.db",-1,uri,sizeof(uri))==CBM_STORE_OK);
    CHECK(strcmp(uri,"file:///C:/Dir%20A/100%25%23%3F/%E4%B8%AD%E6%96%87.db?immutable=1")==0);
#else
    CHECK(cbm_global_migration_immutable_uri_for_test("/tmp/a\\b c#?.db",-1,uri,sizeof(uri))==CBM_STORE_OK);
    CHECK(strcmp(uri,"file:///tmp/a%5Cb%20c%23%3F.db?immutable=1")==0);
#endif
    fprintf(stderr,"stage14 migration uri drive=PASS unc=PASS posix_backslash=PASS reserved_utf8=PASS\n");
    return 0;
}

static int test_global_migration_bootstrap_target_adoption(void) {
#ifdef _WIN32
    wchar_t temp_wide[MAX_PATH],base_wide[MAX_PATH];
    char base[4096],source_memory[4096],source_graph[4096],source_config[4096],target[4096],
         target_memory[4096],target_graph[4096],target_config[4096],target_global_graph[4096],
         projects_dir[4096],artifacts_dir[4096];
    CHECK(GetTempPathW(MAX_PATH,temp_wide)>0);
    CHECK(swprintf(base_wide,MAX_PATH,L"%lsstage14 bootstrap adoption %% # \x4E2D\x6587-%lu-%llu",
                   temp_wide,(unsigned long)_getpid(),(unsigned long long)GetTickCount64())>0);
    CHECK(CreateDirectoryW(base_wide,NULL)!=0);
    CHECK(stage14_wide_to_utf8(base_wide,base,sizeof(base)));
    snprintf(source_memory,sizeof(source_memory),"%s\\source-memory.db",base);
    snprintf(source_graph,sizeof(source_graph),"%s\\source-graph.db",base);
    snprintf(source_config,sizeof(source_config),"%s\\source-config.db",base);
    snprintf(target,sizeof(target),"%s\\bootstrap-target",base);
    snprintf(target_memory,sizeof(target_memory),"%s\\__global__-memory.db",target);
    snprintf(target_config,sizeof(target_config),"%s\\_config.db",target);
    snprintf(target_global_graph,sizeof(target_global_graph),"%s\\__global__-graph.db",target);
    snprintf(projects_dir,sizeof(projects_dir),"%s\\projects",target);
    snprintf(artifacts_dir,sizeof(artifacts_dir),"%s\\artifacts",target);

    cbm_store_t *source=cbm_store_open_path(source_memory);CHECK(source!=NULL);
    cbm_memory_item_t migrated=item("bootstrap-adoption-memory","H-Codex_H","bootstrap adoption payload",0.9);
    char *id=NULL;CHECK(cbm_store_memory_append_candidate(source,&migrated,&id)==CBM_STORE_OK);free(id);cbm_store_close(source);
    sqlite3 *db=NULL;CHECK(sqlite3_open(source_graph,&db)==SQLITE_OK);
    CHECK(sqlite3_exec(db,"CREATE TABLE nodes(id INTEGER PRIMARY KEY,name TEXT);INSERT INTO nodes(name) VALUES('bootstrap-node');",NULL,NULL,NULL)==SQLITE_OK);CHECK(sqlite3_close(db)==SQLITE_OK);
    CHECK(sqlite3_open(source_config,&db)==SQLITE_OK);
    CHECK(sqlite3_exec(db,"CREATE TABLE config(key TEXT PRIMARY KEY,value TEXT);INSERT INTO config VALUES('fixture','preserved');",NULL,NULL,NULL)==SQLITE_OK);CHECK(sqlite3_close(db)==SQLITE_OK);

    wchar_t target_wide[4096],projects_wide[4096],artifacts_wide[4096];
    CHECK(stage14_utf8_to_wide(target,target_wide,4096));CHECK(CreateDirectoryW(target_wide,NULL)!=0);
    CHECK(stage14_utf8_to_wide(projects_dir,projects_wide,4096));CHECK(CreateDirectoryW(projects_wide,NULL)!=0);
    CHECK(stage14_utf8_to_wide(artifacts_dir,artifacts_wide,4096));CHECK(CreateDirectoryW(artifacts_wide,NULL)!=0);
    cbm_global_memory_t *bootstrap=cbm_global_memory_open(target_memory,target_global_graph);CHECK(bootstrap!=NULL);cbm_global_memory_close(bootstrap);
    CHECK(sqlite3_open(target_config,&db)==SQLITE_OK);
    CHECK(sqlite3_exec(db,"CREATE TABLE config(key TEXT PRIMARY KEY,value TEXT);",NULL,NULL,NULL)==SQLITE_OK);CHECK(sqlite3_close(db)==SQLITE_OK);

    cbm_project_resolution_t project={0};CHECK(cbm_project_resolve("H:\\Codex_H",NULL,NULL,&project)==CBM_STORE_OK);
    snprintf(target_graph,sizeof(target_graph),"%s\\projects\\%s\\graph.db",target,project.project_uuid);
    const char *source_paths[3]={source_memory,source_graph,source_config};
    const char *bootstrap_paths[3]={target_memory,target_config,target_global_graph};
    stage14_file_fact_t source_before[3],bootstrap_before[3],bootstrap_after[3];
    for(size_t i=0;i<3;i++){CHECK(stage14_file_fact(source_paths[i],&source_before[i]));CHECK(stage14_file_fact(bootstrap_paths[i],&bootstrap_before[i]));}
    cbm_global_migration_input_t input={.source_memory_path=source_memory,.source_graph_path=source_graph,.source_config_path=source_config,
        .target_root=target,.project_path="H:\\Codex_H",.idempotency_key="bootstrap-adoption-key",.mode="plan"};
    char *report=NULL;CHECK(cbm_global_migration_execute(&input,&report)==CBM_STORE_OK);
    CHECK(report&&strstr(report,"\"status\":\"planned\""));free(report);report=NULL;
    for(size_t i=0;i<3;i++){CHECK(stage14_file_fact(bootstrap_paths[i],&bootstrap_after[i]));CHECK(stage14_file_fact_equal(&bootstrap_before[i],&bootstrap_after[i]));}
    CHECK(!stage14_path_exists(target_graph));

    input.mode="apply";
    CHECK(cbm_global_migration_execute(&input,&report)==CBM_STORE_OK);
    CHECK(report&&strstr(report,"\"status\":\"applied\"")&&strstr(report,"\"preserved_data_equivalent\":true"));free(report);report=NULL;
    const char *all_paths[7]={source_memory,source_graph,source_config,target_memory,target_graph,target_config,target_global_graph};
    stage14_file_fact_t all_before[7],all_after[7];
    for(size_t i=0;i<7;i++)CHECK(stage14_file_fact(all_paths[i],&all_before[i]));
    CHECK(stage14_no_sidecars(all_paths,7));
    CHECK(cbm_global_migration_execute(&input,&report)==CBM_STORE_REPLAYED);
    CHECK(report&&strstr(report,"\"status\":\"replayed\""));free(report);report=NULL;
    for(size_t i=0;i<7;i++){CHECK(stage14_file_fact(all_paths[i],&all_after[i]));CHECK(stage14_file_fact_equal(&all_before[i],&all_after[i]));}
    CHECK(stage14_no_sidecars(all_paths,7));
    fprintf(stderr,"stage14 migration bootstrap_adoption=PASS plan_zero_write=PASS exact_replay_zero_write=PASS\n");
    return 0;
#else
    return 0;
#endif
}

static int test_global_migration_plan_apply_replay_conflict(void) {
#ifdef _WIN32
    wchar_t temp_wide[MAX_PATH],base_wide[MAX_PATH];
    char base[4096],source_memory[4096],source_graph[4096],source_config[4096],target[4096],
         alternate_memory[4096],alternate_graph[4096],alternate_config[4096];
    CHECK(GetTempPathW(MAX_PATH,temp_wide)>0);
    CHECK(swprintf(base_wide,MAX_PATH,L"%lsstage14 replay %% # \x4E2D\x6587-%lu-%llu",
                   temp_wide,(unsigned long)_getpid(),(unsigned long long)GetTickCount64())>0);
    CHECK(CreateDirectoryW(base_wide,NULL)!=0);
    CHECK(stage14_wide_to_utf8(base_wide,base,sizeof(base)));
    snprintf(source_memory,sizeof(source_memory),"%s\\source-memory.db",base);
    snprintf(source_graph,sizeof(source_graph),"%s\\source-graph.db",base);
    snprintf(source_config,sizeof(source_config),"%s\\source-config.db",base);
    snprintf(alternate_memory,sizeof(alternate_memory),"%s\\relocated-memory.db",base);
    snprintf(alternate_graph,sizeof(alternate_graph),"%s\\relocated-graph.db",base);
    snprintf(alternate_config,sizeof(alternate_config),"%s\\relocated-config.db",base);
    snprintf(target,sizeof(target),"%s\\isolated-target",base);
    cbm_store_t *source=cbm_store_open_path(source_memory);CHECK(source!=NULL);
    cbm_memory_item_t migrated=item("legacy-memory-1","H-Codex_H-neuroplastic-main","legacy content",0.9);char *id=NULL;
    CHECK(cbm_store_memory_append_candidate(source,&migrated,&id)==CBM_STORE_OK);free(id);cbm_store_close(source);
    sqlite3 *source_checkpoint=NULL;CHECK(sqlite3_open(source_memory,&source_checkpoint)==SQLITE_OK);
    CHECK(sqlite3_exec(source_checkpoint,"PRAGMA wal_checkpoint(TRUNCATE);PRAGMA journal_mode=DELETE;",NULL,NULL,NULL)==SQLITE_OK);
    sqlite3_close(source_checkpoint);
    sqlite3 *graph=NULL;CHECK(sqlite3_open(source_graph,&graph)==SQLITE_OK);
    CHECK(sqlite3_exec(graph,"CREATE TABLE nodes(id INTEGER PRIMARY KEY,name TEXT);INSERT INTO nodes(name) VALUES('legacy-node');",NULL,NULL,NULL)==SQLITE_OK);sqlite3_close(graph);
    sqlite3 *config=NULL;CHECK(sqlite3_open(source_config,&config)==SQLITE_OK);
    CHECK(sqlite3_exec(config,"CREATE TABLE config(key TEXT PRIMARY KEY,value TEXT);INSERT INTO config VALUES('fixture','preserved');",NULL,NULL,NULL)==SQLITE_OK);sqlite3_close(config);
    cbm_global_migration_input_t input={.source_memory_path=source_memory,.source_graph_path=source_graph,.source_config_path=source_config,.target_root=target,
        .project_path="H:\\Codex_H",.idempotency_key="migration-key-1",.mode="plan"};char *report=NULL;
    CHECK(cbm_global_migration_execute(&input,&report)==CBM_STORE_OK);
    CHECK(report&&strstr(report,"\"status\":\"planned\"")&&strstr(report,"\"current_pointer_switched\":false"));free(report);report=NULL;
    CHECK(!stage14_path_exists(target));
    cbm_global_migration_input_t invalid=input;invalid.mode="apply";invalid.target_root="relative-target";
    CHECK(cbm_global_migration_execute(&invalid,&report)==CBM_STORE_REJECTED);free(report);report=NULL;
    char oversized_target[5000];memset(oversized_target,'x',sizeof(oversized_target));
    oversized_target[0]='H';oversized_target[1]=':';oversized_target[2]='\\';oversized_target[sizeof(oversized_target)-1]=0;
    invalid.target_root=oversized_target;
    CHECK(cbm_global_migration_execute(&invalid,&report)==CBM_STORE_REJECTED);free(report);report=NULL;
    char race_target[4096],race_memory[4096],race_graph[4096],race_config[4096],
         race_global_graph[4096];
    snprintf(race_target,sizeof(race_target),"%s\\atomic-race-target",base);
    snprintf(race_memory,sizeof(race_memory),"%s\\__global__-memory.db",race_target);
    snprintf(race_graph,sizeof(race_graph),
             "%s\\projects\\2fb874ff-b9b3-5d31-997e-793aed30ce00\\graph.db",race_target);
    snprintf(race_config,sizeof(race_config),"%s\\_config.db",race_target);
    snprintf(race_global_graph,sizeof(race_global_graph),"%s\\__global__-graph.db",race_target);
    const char *race_paths[4]={race_memory,race_graph,race_config,race_global_graph};
    stage14_migration_hook_context_t race_hook={
        .mode=STAGE14_MIGRATION_HOOK_CREATE_COMPETITOR,
        .source_memory_path=source_memory
    };
    cbm_global_migration_input_t race_input=input;
    race_input.mode="apply";race_input.target_root=race_target;
    race_input.idempotency_key="migration-atomic-create-race";
    cbm_global_migration_set_test_hook_for_test(stage14_migration_apply_hook,&race_hook);
    int race_rc=cbm_global_migration_execute(&race_input,&report);
    cbm_global_migration_set_test_hook_for_test(NULL,NULL);
    CHECK(race_hook.called==1&&race_hook.hook_rc==SQLITE_OK);
    CHECK(race_rc==CBM_STORE_REJECTED);
    CHECK(report&&strstr(report,"\"status\":\"failed\"")&&!strstr(report,"\"status\":\"applied\""));
    free(report);report=NULL;
    stage14_file_fact_t race_after;
    CHECK(stage14_file_fact(race_graph,&race_after));
    CHECK(stage14_file_fact_equal(&race_hook.sentinel_before,&race_after));
    CHECK(!stage14_path_exists(race_memory)&&!stage14_path_exists(race_config)&&
          !stage14_path_exists(race_global_graph));
    CHECK(stage14_no_sidecars(race_paths,4));
    CHECK(stage14_delete_file(race_graph));

    char busy_target[4096],busy_memory[4096],busy_graph[4096],busy_config[4096],
         busy_global_graph[4096];
    snprintf(busy_target,sizeof(busy_target),"%s\\bounded-busy-target",base);
    snprintf(busy_memory,sizeof(busy_memory),"%s\\__global__-memory.db",busy_target);
    snprintf(busy_graph,sizeof(busy_graph),
             "%s\\projects\\2fb874ff-b9b3-5d31-997e-793aed30ce00\\graph.db",busy_target);
    snprintf(busy_config,sizeof(busy_config),"%s\\_config.db",busy_target);
    snprintf(busy_global_graph,sizeof(busy_global_graph),"%s\\__global__-graph.db",busy_target);
    const char *busy_paths[4]={busy_memory,busy_graph,busy_config,busy_global_graph};
    int backup_max_attempts=0;uint64_t backup_max_elapsed_ms=0;
    CHECK(cbm_global_migration_backup_limits_for_test(&backup_max_attempts,
                                                       &backup_max_elapsed_ms)==CBM_STORE_OK);
    CHECK(backup_max_attempts>0&&backup_max_elapsed_ms>0);
    stage14_migration_hook_context_t busy_hook={
        .mode=STAGE14_MIGRATION_HOOK_LOCK_SOURCE,
        .source_memory_path=source_memory
    };
    cbm_global_migration_input_t busy_input=input;
    busy_input.mode="apply";busy_input.target_root=busy_target;
    busy_input.idempotency_key="migration-bounded-source-busy";
    ULONGLONG busy_started=GetTickCount64();
    cbm_global_migration_set_test_hook_for_test(stage14_migration_apply_hook,&busy_hook);
    int busy_rc=cbm_global_migration_execute(&busy_input,&report);
    ULONGLONG busy_elapsed=GetTickCount64()-busy_started;
    cbm_global_migration_set_test_hook_for_test(NULL,NULL);
    if(busy_hook.locker){
        sqlite3_exec(busy_hook.locker,"ROLLBACK;",NULL,NULL,NULL);
        sqlite3_close(busy_hook.locker);busy_hook.locker=NULL;
    }
    CHECK(busy_hook.called==1&&busy_hook.hook_rc==SQLITE_OK);
    CHECK(busy_rc==CBM_STORE_ERR);
    uint64_t busy_acceptance_cap_ms=backup_max_elapsed_ms+3000;
    CHECK(busy_elapsed<=busy_acceptance_cap_ms);
    CHECK(report&&strstr(report,"\"status\":\"failed\"")&&!strstr(report,"\"status\":\"applied\""));
    free(report);report=NULL;
    for(size_t i=0;i<4;i++)CHECK(!stage14_path_exists(busy_paths[i]));
    CHECK(stage14_no_sidecars(busy_paths,4));
    fprintf(stderr,
            "stage14 migration atomic_create_new=PASS race_sentinel_zero_change=PASS "
            "rollback_exact_identity=PASS backup_busy_bounded=PASS elapsed_ms=%llu "
            "retry_deadline_ms=%llu acceptance_wall_clock_cap_ms=%llu\n",
            (unsigned long long)busy_elapsed,(unsigned long long)backup_max_elapsed_ms,
            (unsigned long long)busy_acceptance_cap_ms);
    input.mode="apply";CHECK(cbm_global_migration_execute(&input,&report)==CBM_STORE_OK);
    CHECK(report&&strstr(report,"\"status\":\"applied\"")&&strstr(report,"\"backup\":{\"quick_check\":\"ok\"")&&strstr(report,"\"preserved_data_equivalent\":true")&&strstr(report,"\"graph\":\"projects/2fb874ff-b9b3-5d31-997e-793aed30ce00/graph.db\"")&&strstr(report,"\"config\":\"_config.db\"")&&strstr(report,"\"legacy_alias_counts\""));free(report);report=NULL;
    char target_memory[4096],target_config[4096],target_graph[4096],target_global_graph[4096];
    snprintf(target_memory,sizeof(target_memory),"%s\\__global__-memory.db",target);
    snprintf(target_config,sizeof(target_config),"%s\\_config.db",target);
    snprintf(target_graph,sizeof(target_graph),"%s\\projects\\2fb874ff-b9b3-5d31-997e-793aed30ce00\\graph.db",target);
    snprintf(target_global_graph,sizeof(target_global_graph),"%s\\__global__-graph.db",target);
    const char *target_paths[4]={target_memory,target_graph,target_config,target_global_graph};
    for(size_t i=0;i<4;i++)CHECK(stage14_path_exists(target_paths[i]));
    const char *all_paths[7]={source_memory,source_graph,source_config,target_memory,target_graph,target_config,target_global_graph};
    CHECK(stage14_no_sidecars(all_paths,7));
    stage14_file_fact_t before[7],after[7];
    for(size_t i=0;i<7;i++)CHECK(stage14_file_fact(all_paths[i],&before[i]));
    int first_replay_rc=cbm_global_migration_execute(&input,&report);
    CHECK(first_replay_rc==CBM_STORE_REPLAYED);CHECK(report&&strstr(report,"\"status\":\"replayed\"")&&strstr(report,"\"target\":{\"quick_check\":\"ok\"")&&strstr(report,"\"legacy_alias_counts\":{\"projects\":1,\"memory_items\":1}"));free(report);report=NULL;
    for(size_t i=0;i<7;i++){CHECK(stage14_file_fact(all_paths[i],&after[i]));CHECK(stage14_file_fact_equal(&before[i],&after[i]));}
    CHECK(stage14_no_sidecars(all_paths,7));
    input.mode="verify";
    for(size_t i=0;i<7;i++)CHECK(stage14_file_fact(all_paths[i],&before[i]));
    CHECK(cbm_global_migration_execute(&input,&report)==CBM_STORE_OK);
    CHECK(report&&strstr(report,"\"schema\":\"semantic-memory-global-managed-target-verify/v1\"")&&
          strstr(report,"\"status\":\"verified\"")&&strstr(report,"\"database_write_performed\":false"));
    free(report);report=NULL;
    for(size_t i=0;i<7;i++){CHECK(stage14_file_fact(all_paths[i],&after[i]));CHECK(stage14_file_fact_equal(&before[i],&after[i]));}
    CHECK(stage14_no_sidecars(all_paths,7));
    sqlite3 *verify_drift_db=NULL;CHECK(sqlite3_open(target_config,&verify_drift_db)==SQLITE_OK);
    CHECK(sqlite3_exec(verify_drift_db,"UPDATE config SET value='target-drift';",NULL,NULL,NULL)==SQLITE_OK);
    CHECK(sqlite3_close(verify_drift_db)==SQLITE_OK);verify_drift_db=NULL;CHECK(stage14_no_sidecars(all_paths,7));
    CHECK(cbm_global_migration_execute(&input,&report)==CBM_STORE_REJECTED);
    CHECK(report&&strstr(report,"\"status\":\"target_drift\"")&&strstr(report,"\"target_logical_match\":false"));
    free(report);report=NULL;
    verify_drift_db=NULL;CHECK(sqlite3_open(target_config,&verify_drift_db)==SQLITE_OK);
    CHECK(sqlite3_exec(verify_drift_db,"UPDATE config SET value='preserved';",NULL,NULL,NULL)==SQLITE_OK);
    CHECK(sqlite3_close(verify_drift_db)==SQLITE_OK);verify_drift_db=NULL;CHECK(stage14_no_sidecars(all_paths,7));
    fprintf(stderr,"stage14 managed-target verify=PASS zero_write=PASS target_drift_fail_closed=PASS\n");
    input.mode="apply";
    CHECK(stage14_copy_file(source_memory,alternate_memory));CHECK(stage14_copy_file(source_graph,alternate_graph));CHECK(stage14_copy_file(source_config,alternate_config));
    input.source_memory_path=alternate_memory;input.source_graph_path=alternate_graph;input.source_config_path=alternate_config;
    const char *relocated_paths[7]={alternate_memory,alternate_graph,alternate_config,target_memory,target_graph,target_config,target_global_graph};
    for(size_t i=0;i<7;i++)CHECK(stage14_file_fact(relocated_paths[i],&before[i]));
    CHECK(cbm_global_migration_execute(&input,&report)==CBM_STORE_REPLAYED);CHECK(report&&strstr(report,"\"status\":\"replayed\"")&&strstr(report,"\"target\":{\"quick_check\":\"ok\""));free(report);report=NULL;
    for(size_t i=0;i<7;i++){CHECK(stage14_file_fact(relocated_paths[i],&after[i]));CHECK(stage14_file_fact_equal(&before[i],&after[i]));}
    CHECK(stage14_no_sidecars(relocated_paths,7));
    sqlite3 *writer_db=NULL;CHECK(sqlite3_open_v2(target_memory,&writer_db,SQLITE_OPEN_READWRITE,NULL)==SQLITE_OK);
    for(size_t i=0;i<7;i++)CHECK(stage14_file_fact(relocated_paths[i],&before[i]));
    CHECK(cbm_global_migration_execute(&input,&report)==CBM_STORE_REJECTED);
    CHECK(report&&strstr(report,"\"status\":\"failed\""));free(report);report=NULL;
    for(size_t i=0;i<7;i++){CHECK(stage14_file_fact(relocated_paths[i],&after[i]));CHECK(stage14_file_fact_equal(&before[i],&after[i]));}
    CHECK(sqlite3_close(writer_db)==SQLITE_OK);CHECK(stage14_no_sidecars(relocated_paths,7));
    char target_hardlink[4096];snprintf(target_hardlink,sizeof(target_hardlink),"%s\\target-memory-hardlink.db",base);
    CHECK(stage14_create_hardlink(target_hardlink,target_memory));
    CHECK(sqlite3_open_v2(target_hardlink,&writer_db,SQLITE_OPEN_READWRITE,NULL)==SQLITE_OK);
    for(size_t i=0;i<7;i++)CHECK(stage14_file_fact(relocated_paths[i],&before[i]));
    CHECK(cbm_global_migration_execute(&input,&report)==CBM_STORE_REJECTED);
    CHECK(report&&strstr(report,"\"status\":\"failed\""));free(report);report=NULL;
    for(size_t i=0;i<7;i++){CHECK(stage14_file_fact(relocated_paths[i],&after[i]));CHECK(stage14_file_fact_equal(&before[i],&after[i]));}
    CHECK(sqlite3_close(writer_db)==SQLITE_OK);CHECK(stage14_delete_file(target_hardlink));
    CHECK(stage14_no_sidecars(relocated_paths,7));
    int reparse_exercised=0;char source_symlink[4096];snprintf(source_symlink,sizeof(source_symlink),"%s\\source-config-symlink.db",base);
    if(stage14_create_symlink(source_symlink,alternate_config)){
        reparse_exercised=1;input.source_config_path=source_symlink;
        for(size_t i=0;i<7;i++)CHECK(stage14_file_fact(relocated_paths[i],&before[i]));
        CHECK(cbm_global_migration_execute(&input,&report)==CBM_STORE_REJECTED);
        CHECK(report&&strstr(report,"\"status\":\"failed\""));free(report);report=NULL;
        for(size_t i=0;i<7;i++){CHECK(stage14_file_fact(relocated_paths[i],&after[i]));CHECK(stage14_file_fact_equal(&before[i],&after[i]));}
        input.source_config_path=alternate_config;CHECK(stage14_delete_file(source_symlink));
    }
    const char sidecar_payload[]="stage14-sidecar-sentinel";char sidecar[8192];
    const char *sidecar_suffixes[]={"-wal","-shm","-journal"};
    for(size_t path_index=0;path_index<7;path_index++)for(size_t sidecar_index=0;sidecar_index<3;sidecar_index++){
        snprintf(sidecar,sizeof(sidecar),"%s%s",relocated_paths[path_index],sidecar_suffixes[sidecar_index]);
        CHECK(stage14_write_file(sidecar,sidecar_payload,(DWORD)(sizeof(sidecar_payload)-1)));
        stage14_file_fact_t sidecar_before,sidecar_after;CHECK(stage14_file_fact(sidecar,&sidecar_before));
        for(size_t i=0;i<7;i++)CHECK(stage14_file_fact(relocated_paths[i],&before[i]));
        CHECK(cbm_global_migration_execute(&input,&report)==CBM_STORE_REJECTED);
        CHECK(report&&strstr(report,"\"status\":\"failed\""));free(report);report=NULL;
        for(size_t i=0;i<7;i++){CHECK(stage14_file_fact(relocated_paths[i],&after[i]));CHECK(stage14_file_fact_equal(&before[i],&after[i]));}
        CHECK(stage14_file_fact(sidecar,&sidecar_after));CHECK(stage14_file_fact_equal(&sidecar_before,&sidecar_after));
        CHECK(stage14_delete_file(sidecar));CHECK(stage14_no_sidecars(relocated_paths,7));
    }
    sqlite3 *catalog_db=NULL;CHECK(sqlite3_open(target_memory,&catalog_db)==SQLITE_OK);
    cbm_project_resolution_t migrated_project={0};CHECK(cbm_project_resolve("H:\\Codex_H",NULL,NULL,&migrated_project)==0);
    char catalog_sql[8192];snprintf(catalog_sql,sizeof(catalog_sql),"SELECT COUNT(*) FROM global_project_catalog WHERE project_uuid='%s' AND canonical_path='%s' AND path_hash='%s';",migrated_project.project_uuid,migrated_project.canonical_path,migrated_project.path_hash);
    CHECK(scalar(catalog_db,catalog_sql)==1);
    snprintf(catalog_sql,sizeof(catalog_sql),"SELECT COUNT(*) FROM global_memory_provenance WHERE project_uuid='%s' AND memory_item_id='legacy-memory-1' AND source_kind='legacy_migration';",migrated_project.project_uuid);
    CHECK(scalar(catalog_db,catalog_sql)==1);sqlite3_close(catalog_db);CHECK(stage14_no_sidecars(relocated_paths,7));
    sqlite3 *target_db=NULL;CHECK(sqlite3_open(target_memory,&target_db)==SQLITE_OK);CHECK(sqlite3_exec(target_db,"UPDATE global_migration_ledger SET state='staged' WHERE idempotency_key='migration-key-1';",NULL,NULL,NULL)==SQLITE_OK);sqlite3_close(target_db);
    CHECK(stage14_no_sidecars(relocated_paths,7));
    CHECK(cbm_global_migration_execute(&input,&report)==CBM_STORE_REJECTED);CHECK(report&&strstr(report,"\"status\":\"staged\""));free(report);report=NULL;
    CHECK(sqlite3_open(target_memory,&target_db)==SQLITE_OK);CHECK(sqlite3_exec(target_db,"UPDATE global_migration_ledger SET state='applied' WHERE idempotency_key='migration-key-1';",NULL,NULL,NULL)==SQLITE_OK);sqlite3_close(target_db);
    CHECK(stage14_no_sidecars(relocated_paths,7));
    sqlite3 *changed_source=NULL;CHECK(sqlite3_open(alternate_config,&changed_source)==SQLITE_OK);
    CHECK(sqlite3_exec(changed_source,"UPDATE config SET value='changed';",NULL,NULL,NULL)==SQLITE_OK);sqlite3_close(changed_source);
    CHECK(cbm_global_migration_execute(&input,&report)==CBM_STORE_IDEMPOTENCY_CONFLICT);CHECK(report&&strstr(report,"\"status\":\"IDEMPOTENCY_CONFLICT\""));free(report);
    report=NULL;CHECK(sqlite3_open(alternate_config,&changed_source)==SQLITE_OK);
    CHECK(sqlite3_exec(changed_source,"UPDATE config SET value='preserved';",NULL,NULL,NULL)==SQLITE_OK);sqlite3_close(changed_source);
    CHECK(sqlite3_open(target_memory,&target_db)==SQLITE_OK);
    CHECK(sqlite3_exec(target_db,"DELETE FROM global_migration_ledger WHERE idempotency_key='migration-key-1';",NULL,NULL,NULL)==SQLITE_OK);sqlite3_close(target_db);
    for(size_t i=0;i<7;i++)CHECK(stage14_file_fact(relocated_paths[i],&before[i]));
    CHECK(cbm_global_migration_execute(&input,&report)==CBM_STORE_REJECTED);CHECK(report&&strstr(report,"\"status\":\"failed\""));free(report);report=NULL;
    for(size_t i=0;i<7;i++){CHECK(stage14_file_fact(relocated_paths[i],&after[i]));CHECK(stage14_file_fact_equal(&before[i],&after[i]));}
    CHECK(sqlite3_open(target_memory,&target_db)==SQLITE_OK);
    CHECK(sqlite3_exec(target_db,"DROP TABLE global_migration_ledger;",NULL,NULL,NULL)==SQLITE_OK);sqlite3_close(target_db);
    for(size_t i=0;i<7;i++)CHECK(stage14_file_fact(relocated_paths[i],&before[i]));
    CHECK(cbm_global_migration_execute(&input,&report)==CBM_STORE_REJECTED);CHECK(report&&strstr(report,"\"status\":\"failed\""));free(report);
    for(size_t i=0;i<7;i++){CHECK(stage14_file_fact(relocated_paths[i],&after[i]));CHECK(stage14_file_fact_equal(&before[i],&after[i]));}
    CHECK(stage14_no_sidecars(relocated_paths,7));
    fprintf(stderr,"stage14 migration replay held_deserialize=PASS unicode_path=PASS seven_db_zero_write=PASS writer_fail_closed=PASS hardlink_alias_writer_fail_closed=PASS reparse_rejected=%s sidecars_21=PASS ledger_miss_no_apply=PASS ledger_error_no_apply=PASS\n",reparse_exercised?"PASS":"SKIP");
#endif
    return 0;
}

int main(int argc,char **argv) {
#ifdef _WIN32
    if(argc>1&&(!strcmp(argv[1],"--stage14-apply-child")||
                !strcmp(argv[1],"--stage14-crash-child")))
        return stage14_evolution_child_main(argc,argv);
#else
    (void)argc;(void)argv;
#endif
    int failed=0,tests=0;
#define RUN(test) do { tests++; failed += test(); } while(0)
    RUN(test_resolver_frozen_identity);
    RUN(test_real_rename_reuses_identity);
    RUN(test_schema_catalog_alias_replay);
    RUN(test_catalog_observation_changes_do_not_change_identity);
    RUN(test_global_lifecycle_replay_conflict);
    RUN(test_global_unknown_workspace_chain_and_atomicity);
    RUN(test_global_pool_soft_boost_and_provenance);
    RUN(test_stage14_e_isolated_ab_rank_metrics);
    RUN(test_global_pool_expands_before_soft_boost_limit);
    RUN(test_cross_project_edge_replay_conflict);
    RUN(test_evidence_gated_evolution_and_maintenance);
    RUN(test_maintenance_production_bounded_canary);
    RUN(test_completed_task_evolution_controller);
    RUN(test_evolution_toctou_snapshot_is_transactional);
    RUN(test_task_evolution_production_manifest_gate);
    RUN(test_evolution_lookup_step_errors_fail_closed);
    RUN(test_global_migration_immutable_uri_encoding);
    RUN(test_global_migration_bootstrap_target_adoption);
    RUN(test_global_migration_plan_apply_replay_conflict);
    fprintf(stderr,"%s: %d tests\n",failed?"FAIL":"PASS",tests);
    return failed?1:0;
}
