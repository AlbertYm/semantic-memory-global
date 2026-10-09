#include "memory/verified_learning.h"
#include "store/store.h"
#include "foundation/platform.h"
#include "yyjson/yyjson.h"
#include <sqlite3.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LEARNING_BATCH 64
#define LEARNING_DAY_MS INT64_C(86400000)

static int learning_enabled(void) {
    char value[8] = {0};
    cbm_safe_getenv("CBM_VERIFIED_LEARNING", value, sizeof(value), NULL);
    return strcmp(value, "1") == 0;
}

static int exec_sql(sqlite3 *db, const char *sql) {
    return sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK ? CBM_STORE_OK : CBM_STORE_ERR;
}

static int learning_schema(sqlite3 *db) {
    return exec_sql(db,
        "CREATE TABLE IF NOT EXISTS verified_learning_control("
        "id INTEGER PRIMARY KEY CHECK(id=1),enabled INTEGER NOT NULL DEFAULT 1,"
        "cursor INTEGER NOT NULL DEFAULT 0,pair_cursor INTEGER NOT NULL DEFAULT 0,"
        "generation INTEGER NOT NULL DEFAULT 0,last_code INTEGER NOT NULL DEFAULT 0,"
        "last_maintenance_ms INTEGER NOT NULL DEFAULT 0);"
        "INSERT OR IGNORE INTO verified_learning_control(id) VALUES(1);"
        "CREATE TABLE IF NOT EXISTS verified_learning_request("
        "request_id TEXT PRIMARY KEY,payload_hash TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS verified_learning_receipt("
        "evidence_id TEXT PRIMARY KEY,result_hash TEXT NOT NULL,outcome INTEGER NOT NULL "
        "CHECK(outcome IN (0,1)));"
        "CREATE TABLE IF NOT EXISTS verified_learning_user_receipt("
        "evidence_id TEXT PRIMARY KEY,result_hash TEXT NOT NULL);"
        "CREATE INDEX IF NOT EXISTS verified_learning_feedback_task ON feedback_event(task_id,candidate_id);"
        "CREATE TABLE IF NOT EXISTS verified_learning_state("
        "item_id TEXT PRIMARY KEY,positive INTEGER NOT NULL,negative INTEGER NOT NULL,"
        "last_verified_ms INTEGER NOT NULL,utility REAL NOT NULL,decay REAL NOT NULL,"
        "archived_version INTEGER,restore_status TEXT,restore_until_ms INTEGER NOT NULL DEFAULT 0);"
        "CREATE TABLE IF NOT EXISTS verified_learning_association("
        "src_id TEXT NOT NULL,dst_id TEXT NOT NULL,success_count INTEGER NOT NULL,"
        "last_verified_ms INTEGER NOT NULL,PRIMARY KEY(src_id,dst_id));"
        "CREATE TABLE IF NOT EXISTS verified_learning_audit("
        "sequence INTEGER PRIMARY KEY AUTOINCREMENT,operation TEXT NOT NULL,item_id TEXT NOT NULL,"
        "before_json TEXT NOT NULL,after_json TEXT NOT NULL,prev_hash TEXT NOT NULL,"
        "event_hash TEXT NOT NULL,created_at_ms INTEGER NOT NULL);"
        "CREATE TRIGGER IF NOT EXISTS verified_learning_audit_no_update BEFORE UPDATE ON "
        "verified_learning_audit BEGIN SELECT RAISE(ABORT,'append-only'); END;"
        "CREATE TRIGGER IF NOT EXISTS verified_learning_audit_no_delete BEFORE DELETE ON "
        "verified_learning_audit BEGIN SELECT RAISE(ABORT,'append-only'); END;"
        /* At most one vote per task/item. Later corrections/withdrawals replace the vote,
         * rather than rewarding each event or counting repeated retrievals as success. */
        "CREATE VIEW IF NOT EXISTS verified_learning_credit AS "
        "SELECT c.memory_item_id AS item_id,f.task_id,t.project,f.event_id,"
        "CASE WHEN f.action='confirm' AND u.outcome='used' AND r.status='succeeded' THEN 1 "
        "WHEN f.action IN ('reject','correct') AND u.outcome IN ('rejected','contradicted') "
        "AND (r.status='failed' OR e.trust_class='explicit_user') THEN -1 ELSE 0 END AS vote,"
        "CAST((julianday(f.received_at)-2440587.5)*86400000 AS INTEGER) AS verified_ms "
        "FROM feedback_event f JOIN retrieval_candidate c ON c.id=f.candidate_id "
        "AND c.session_id=f.session_id JOIN memory_task t ON t.task_id=f.task_id "
        "JOIN memory_task_session ts ON ts.task_id=t.task_id AND ts.session_id=f.session_id "
        "JOIN memory_item m ON m.id=c.memory_item_id "
        "JOIN memory_usage_attribution u ON u.id=f.usage_id AND u.candidate_id=c.id "
        "AND u.session_id=c.session_id "
        "JOIN memory_evidence e ON e.evidence_id=f.evidence_id AND e.task_id=t.task_id "
        "AND e.result_id=f.result_id JOIN memory_task_result r ON r.result_id=f.result_id "
        "AND r.task_id=t.task_id "
        "WHERE m.deleted_at IS NULL AND (m.scope_project IS NULL OR m.scope_project=t.project) "
        "AND e.evidence_state='valid' AND "
        "((e.trust_class='external_verified' AND EXISTS(SELECT 1 FROM verified_learning_receipt "
        "p WHERE p.evidence_id=e.evidence_id AND p.result_hash=r.result_hash "
        "AND ((p.outcome=0 AND r.status='succeeded') OR (p.outcome=1 AND r.status='failed')))) "
        "OR (e.trust_class='explicit_user' AND e.source_type='user' "
        "AND r.result_type='user_confirmation' AND EXISTS(SELECT 1 FROM verified_learning_user_receipt "
        "p WHERE p.evidence_id=e.evidence_id AND p.result_hash=r.result_hash))) "
        "AND EXISTS(SELECT 1 FROM codex_task_lifecycle l WHERE l.task_id=t.task_id "
        "AND l.state='completed' AND l.outcome='completed') "
        "AND f.rowid=(SELECT MAX(x.rowid) FROM feedback_event x JOIN retrieval_candidate xc "
        "ON xc.id=x.candidate_id JOIN memory_evidence xe ON xe.evidence_id=x.evidence_id "
        "JOIN memory_task_result xr ON xr.result_id=x.result_id "
        "WHERE x.task_id=f.task_id AND xc.memory_item_id=c.memory_item_id AND "
        "((xe.trust_class='external_verified' AND EXISTS(SELECT 1 FROM verified_learning_receipt xp "
        "WHERE xp.evidence_id=xe.evidence_id AND xp.result_hash=xr.result_hash)) OR "
        "(xe.trust_class='explicit_user' AND xe.source_type='user' AND xr.result_type='user_confirmation' "
        "AND EXISTS(SELECT 1 FROM verified_learning_user_receipt xp WHERE xp.evidence_id=xe.evidence_id "
        "AND xp.result_hash=xr.result_hash))));"
    );
}

static int scalar(sqlite3 *db, const char *sql, int fallback) {
    sqlite3_stmt *stmt = NULL;
    int value = fallback;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_ROW) value = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return value;
}

static int audit(sqlite3 *db, const char *op, const char *id, const char *before,
                 const char *after, int64_t now) {
    sqlite3_stmt *stmt = NULL;
    char prev[65]; memset(prev, '0', 64); prev[64] = '\0';
    if (sqlite3_prepare_v2(db, "SELECT event_hash FROM verified_learning_audit ORDER BY sequence "
                             "DESC LIMIT 1", -1, &stmt, NULL) != SQLITE_OK) return CBM_STORE_ERR;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        snprintf(prev, sizeof(prev), "%s", sqlite3_column_text(stmt, 0));
    sqlite3_finalize(stmt);
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return CBM_STORE_ERR;
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "operation", op);
    yyjson_mut_obj_add_str(doc, root, "item_id", id);
    yyjson_mut_obj_add_str(doc, root, "before", before);
    yyjson_mut_obj_add_str(doc, root, "after", after);
    yyjson_mut_obj_add_str(doc, root, "prev_hash", prev);
    yyjson_mut_obj_add_sint(doc, root, "created_at_ms", now);
    char *json = yyjson_mut_write(doc, 0, NULL); yyjson_mut_doc_free(doc);
    char hash[65];
    int rc = json ? cbm_stage7_sha256_hex(json, strlen(json), hash) : CBM_STORE_ERR;
    free(json);
    if (rc != CBM_STORE_OK || sqlite3_prepare_v2(db,
        "INSERT INTO verified_learning_audit(operation,item_id,before_json,after_json,"
        "prev_hash,event_hash,created_at_ms) VALUES(?1,?2,?3,?4,?5,?6,?7)",
        -1, &stmt, NULL) != SQLITE_OK) return CBM_STORE_ERR;
    const char *values[] = {op,id,before,after,prev,hash};
    for (int i=0;i<6;i++) sqlite3_bind_text(stmt,i+1,values[i],-1,SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt,7,now);
    rc=sqlite3_step(stmt)==SQLITE_DONE ? CBM_STORE_OK : CBM_STORE_ERR;
    sqlite3_finalize(stmt); return rc;
}

int cbm_learning_record_receipt(cbm_store_t *store, const char *id,
                                const char *hash, int outcome) {
    sqlite3 *db=store ? cbm_store_get_db(store) : NULL;
    if (!db || !id || !hash || (outcome!=0 && outcome!=1) || !learning_enabled())
        return CBM_STORE_REJECTED;
    if (learning_schema(db)!=CBM_STORE_OK) return CBM_STORE_ERR;
    sqlite3_stmt *stmt=NULL;
    if (sqlite3_prepare_v2(db,"INSERT OR IGNORE INTO verified_learning_receipt "
        "SELECT ?1,?2,?3 WHERE EXISTS(SELECT 1 FROM memory_evidence e JOIN memory_task_result r "
        "ON r.result_id=e.result_id WHERE e.evidence_id=?1 AND r.result_hash=?2 "
        "AND e.trust_class='external_verified')",-1,&stmt,NULL)!=SQLITE_OK) return CBM_STORE_ERR;
    sqlite3_bind_text(stmt,1,id,-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt,2,hash,-1,SQLITE_TRANSIENT); sqlite3_bind_int(stmt,3,outcome);
    int rc=sqlite3_step(stmt)==SQLITE_DONE ? CBM_STORE_OK : CBM_STORE_ERR;
    sqlite3_finalize(stmt); return rc;
}

/* No free-form PASS parsing. Unknown, incomplete and asynchronous outputs stay unverified. */
static int outcome_value(yyjson_val *root, int depth) {
    if (!root || depth>5) return -1;
    if (yyjson_is_obj(root)) {
        yyjson_val *exit=yyjson_obj_get(root,"exit_code");
        yyjson_val *output=yyjson_obj_get(root,"output");
        if (yyjson_is_int(exit) && yyjson_is_str(output) &&
            !yyjson_obj_get(root,"session_id")) return yyjson_get_sint(exit)==0 ? 0 : 1;
        /* Only documented tool-envelope fields, never arbitrary keys inside output. */
        const char *keys[]={"structuredContent","content","text"};
        for(int i=0;i<3;i++) {int value=outcome_value(yyjson_obj_get(root,keys[i]),depth+1);
            if(value>=0) return value;}
    } else if (yyjson_is_arr(root)) {
        /* Multiple commands must all finish successfully; any failure vetoes success. */
        int found=-1; bool unknown=false; size_t i,max; yyjson_val *child;
        yyjson_arr_foreach(root,i,max,child) {int value=outcome_value(child,depth+1);
            if(value==1)return 1;
            if(value==0)found=0;
            if(value<0)unknown=true;}
        return unknown ? -1 : found;
    } else if (yyjson_is_str(root)) {
        const char *text=yyjson_get_str(root);
        if(strlen(text)>65536)return -1;
        yyjson_doc *doc=yyjson_read(text,strlen(text),0);
        int value=doc ? outcome_value(yyjson_doc_get_root(doc),depth+1) : -1;
        yyjson_doc_free(doc);return value;
    }
    return -1;
}

int cbm_learning_tool_outcome(const char *name,const char *json) {
    if(!name || !json || !(strstr(name,"exec_command") || strstr(name,"write_stdin") ||
        strcmp(name,"functions.exec")==0))return -1;
    yyjson_doc *doc=yyjson_read(json,strlen(json),0);
    int result=doc ? outcome_value(yyjson_doc_get_root(doc),0) : -1;
    yyjson_doc_free(doc);return result;
}

static int refresh_item(sqlite3 *db,const char *id,int64_t now) {
    sqlite3_stmt *stmt=NULL;
    const char *sql="SELECT m.status,m.version,m.importance,m.kind,m.created_at,m.last_hit_at,"
        "COALESCE(s.positive,0),COALESCE(s.negative,0),COALESCE(s.utility,0),"
        "COALESCE(s.decay,0),COALESCE(s.archived_version,0),COALESCE(s.restore_until_ms,0),"
        "COALESCE((SELECT SUM(vote=1) FROM verified_learning_credit WHERE item_id=m.id),0),"
        "COALESCE((SELECT SUM(vote=-1) FROM verified_learning_credit WHERE item_id=m.id),0),"
        "COALESCE((SELECT MAX(verified_ms) FROM verified_learning_credit WHERE item_id=m.id "
        "AND vote!=0),0),COALESCE(s.last_verified_ms,0) FROM memory_item m LEFT JOIN verified_learning_state s ON s.item_id=m.id "
        "WHERE m.id=?1 AND m.deleted_at IS NULL";
    if(sqlite3_prepare_v2(db,sql,-1,&stmt,NULL)!=SQLITE_OK)return CBM_STORE_ERR;
    sqlite3_bind_text(stmt,1,id,-1,SQLITE_TRANSIENT);
    if(sqlite3_step(stmt)!=SQLITE_ROW){sqlite3_finalize(stmt);return CBM_STORE_OK;}
    char status[32],kind[32];
    snprintf(status,sizeof(status),"%s",sqlite3_column_text(stmt,0));
    snprintf(kind,sizeof(kind),"%s",sqlite3_column_text(stmt,3));
    int version=sqlite3_column_int(stmt,1),old_pos=sqlite3_column_int(stmt,6),
        old_neg=sqlite3_column_int(stmt,7),archived_version=sqlite3_column_int(stmt,10),
        pos=sqlite3_column_int(stmt,12),neg=sqlite3_column_int(stmt,13);
    double importance=sqlite3_column_double(stmt,2),old_utility=sqlite3_column_double(stmt,8),
           old_decay=sqlite3_column_double(stmt,9);
    int64_t created=sqlite3_column_int64(stmt,4),last_hit=sqlite3_column_int64(stmt,5),
            restore_until=sqlite3_column_int64(stmt,11),verified=sqlite3_column_int64(stmt,14),old_verified=sqlite3_column_int64(stmt,15);
    sqlite3_finalize(stmt);
    /* A verified result timestamp in the future never permits maintenance. */
    if(verified>now || created>now || last_hit>now)return CBM_STORE_REJECTED;
    int64_t touched=verified;
    if(touched<created)touched=created;
    double days=(double)(now-touched)/(double)LEARNING_DAY_MS;
    double decay=fmin(0.12,fmax(0.0,days-30.0)/150.0*0.12);
    if(importance>=0.8 || !strcmp(kind,"preference") || !strcmp(kind,"constraint") ||
       !strcmp(kind,"decision"))decay=0;
    double utility=0.15*tanh((double)(pos-neg)/3.0)*exp(-fmax(0.0,days-30.0)/180.0);
    utility=round(utility*1000000.0)/1000000.0;decay=round(decay*1000000.0)/1000000.0;
    char before[256],after[256];
    snprintf(before,sizeof(before),"{\"positive\":%d,\"negative\":%d,\"utility\":%.6f,\"decay\":%.6f,\"last_verified_ms\":%lld}",
             old_pos,old_neg,old_utility,old_decay,(long long)old_verified);
    snprintf(after,sizeof(after),"{\"positive\":%d,\"negative\":%d,\"utility\":%.6f,\"decay\":%.6f,\"last_verified_ms\":%lld}",
             pos,neg,utility,decay,(long long)verified);
    if(strcmp(before,after)!=0 && audit(db,"recompute_credit",id,before,after,now)!=CBM_STORE_OK)
        return CBM_STORE_ERR;
    if(sqlite3_prepare_v2(db,"INSERT INTO verified_learning_state(item_id,positive,negative,"
        "last_verified_ms,utility,decay) VALUES(?1,?2,?3,?4,?5,?6) ON CONFLICT(item_id) DO UPDATE "
        "SET positive=excluded.positive,negative=excluded.negative,last_verified_ms=excluded.last_verified_ms,"
        "utility=excluded.utility,decay=excluded.decay",-1,&stmt,NULL)!=SQLITE_OK)return CBM_STORE_ERR;
    sqlite3_bind_text(stmt,1,id,-1,SQLITE_TRANSIENT);sqlite3_bind_int(stmt,2,pos);
    sqlite3_bind_int(stmt,3,neg);sqlite3_bind_int64(stmt,4,verified);
    sqlite3_bind_double(stmt,5,utility);sqlite3_bind_double(stmt,6,decay);
    int rc=sqlite3_step(stmt)==SQLITE_DONE ? CBM_STORE_OK:CBM_STORE_ERR;sqlite3_finalize(stmt);
    if(rc!=CBM_STORE_OK)return rc;
    /* Positive verified use is required for promotion: never blindly promote all candidates. */
    int promote=!strcmp(status,"candidate") && pos>=2 && pos>neg && importance>=0.7;
    int archive=!strcmp(status,"active") && !archived_version && pos+neg>0 && days>=180 &&
        importance<0.8 && now>restore_until &&
        (!strcmp(kind,"lesson") || !strcmp(kind,"fact") || !strcmp(kind,"reference"));
    bool duplicate=false;
    if (!promote && !strcmp(status,"candidate") && !archived_version && now>restore_until &&
        (!strcmp(kind,"lesson") || !strcmp(kind,"fact") || !strcmp(kind,"reference"))) {
        const char *duplicate_sql="SELECT 1 FROM memory_item canonical JOIN verified_learning_state v "
            "ON v.item_id=canonical.id JOIN memory_item original ON original.id=?1 "
            "WHERE canonical.id!=original.id AND canonical.status='active' AND canonical.deleted_at IS NULL "
            "AND v.positive>v.negative AND canonical.kind=original.kind "
            "AND canonical.scope_project IS original.scope_project AND canonical.scope_user IS original.scope_user "
            "AND canonical.scope_task IS original.scope_task AND canonical.entity_key IS original.entity_key "
            "AND canonical.predicate IS original.predicate AND canonical.content=original.content "
            "AND canonical.summary=original.summary LIMIT 1";
        if(sqlite3_prepare_v2(db,duplicate_sql,-1,&stmt,NULL)!=SQLITE_OK)return CBM_STORE_ERR;
        sqlite3_bind_text(stmt,1,id,-1,SQLITE_TRANSIENT);
        duplicate=sqlite3_step(stmt)==SQLITE_ROW;
        sqlite3_finalize(stmt);
    }
    archive=archive || duplicate;
    if(!promote && !archive)return CBM_STORE_OK;
    const char *next=promote ? "active":"archived";
    if(sqlite3_prepare_v2(db,"UPDATE memory_item SET status=?1,version=version+1,updated_at=?2 "
        "WHERE id=?3 AND version=?4 AND status=?5",-1,&stmt,NULL)!=SQLITE_OK)return CBM_STORE_ERR;
    sqlite3_bind_text(stmt,1,next,-1,SQLITE_STATIC);sqlite3_bind_int64(stmt,2,now);
    sqlite3_bind_text(stmt,3,id,-1,SQLITE_TRANSIENT);sqlite3_bind_int(stmt,4,version);
    sqlite3_bind_text(stmt,5,status,-1,SQLITE_TRANSIENT);
    rc=sqlite3_step(stmt)==SQLITE_DONE && sqlite3_changes(db)==1 ? CBM_STORE_OK:CBM_STORE_ERR;
    sqlite3_finalize(stmt);if(rc!=CBM_STORE_OK)return rc;
    if(archive){
        if(sqlite3_prepare_v2(db,"UPDATE verified_learning_state SET archived_version=?1,"
            "restore_status=?2 WHERE item_id=?3",-1,&stmt,NULL)!=SQLITE_OK)return CBM_STORE_ERR;
        sqlite3_bind_int(stmt,1,version+1);sqlite3_bind_text(stmt,2,status,-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt,3,id,-1,SQLITE_TRANSIENT);
        rc=sqlite3_step(stmt)==SQLITE_DONE ? CBM_STORE_OK:CBM_STORE_ERR;sqlite3_finalize(stmt);
    }
    if(rc==CBM_STORE_OK)rc=audit(db,promote ? "promote":(duplicate ? "archive_duplicate":"archive"),id,status,next,now);
    return rc;
}

static int vm_budget(void *pointer) {
    int *remaining=pointer;
    return --(*remaining)<=0;
}

int cbm_learning_refresh(cbm_store_t *store,int64_t now) {
    sqlite3 *db=store ? cbm_store_get_db(store):NULL;
    if(!db || !learning_enabled())return CBM_STORE_REJECTED;
    if(now<=0)now=(int64_t)time(NULL)*1000;
    if(!sqlite3_get_autocommit(db) || learning_schema(db)!=CBM_STORE_OK)return CBM_STORE_ERR;
    if(!scalar(db,"SELECT enabled FROM verified_learning_control WHERE id=1",0))return CBM_STORE_REJECTED;
    sqlite3_busy_timeout(db,100);
    int begin_rc=exec_sql(db,"BEGIN IMMEDIATE");
    sqlite3_busy_timeout(db,3000);
    if(begin_rc!=CBM_STORE_OK)return CBM_STORE_ERR;
    int64_t last=0; sqlite3_stmt *stmt=NULL;
    if(sqlite3_prepare_v2(db,"SELECT last_maintenance_ms FROM verified_learning_control WHERE id=1",
        -1,&stmt,NULL)==SQLITE_OK && sqlite3_step(stmt)==SQLITE_ROW)last=sqlite3_column_int64(stmt,0);
    sqlite3_finalize(stmt);
    if(now<last){exec_sql(db,"ROLLBACK");return CBM_STORE_REJECTED;}
    int budget=2000;
    sqlite3_progress_handler(db,1000,vm_budget,&budget);
    int cursor=scalar(db,"SELECT cursor FROM verified_learning_control WHERE id=1",0);
    char ids[LEARNING_BATCH][256];int count=0;
    if(sqlite3_prepare_v2(db,"SELECT id FROM memory_item WHERE deleted_at IS NULL "
        "AND status IN ('candidate','active','archived') ORDER BY id LIMIT 64 OFFSET ?1",
        -1,&stmt,NULL)!=SQLITE_OK){sqlite3_progress_handler(db,0,NULL,NULL);exec_sql(db,"ROLLBACK");return CBM_STORE_ERR;}
    sqlite3_bind_int(stmt,1,cursor);
    while(count<LEARNING_BATCH && sqlite3_step(stmt)==SQLITE_ROW){
        snprintf(ids[count++],256,"%s",sqlite3_column_text(stmt,0));}
    sqlite3_finalize(stmt);
    int rc=CBM_STORE_OK;
    for(int i=0;i<count && rc==CBM_STORE_OK;i++)rc=refresh_item(db,ids[i],now);
    /* At most 64 pairs, including existing pairs needing compensation after withdrawal.
     * A SQL VM budget also bounds scans; interruption rolls back this entire batch. */
    int pair_cursor=scalar(db,"SELECT pair_cursor FROM verified_learning_control WHERE id=1",0);
    char sources[LEARNING_BATCH][256],destinations[LEARNING_BATCH][256];int pairs=0;
    if(rc==CBM_STORE_OK && sqlite3_prepare_v2(db,
        "SELECT src,dst FROM (SELECT src_id src,dst_id dst FROM verified_learning_association "
        "UNION SELECT a.item_id,b.item_id FROM verified_learning_credit a JOIN verified_learning_credit b "
        "ON a.task_id=b.task_id AND a.item_id<b.item_id WHERE a.vote=1 AND b.vote=1) "
        "ORDER BY src,dst LIMIT 64 OFFSET ?1",-1,&stmt,NULL)==SQLITE_OK){
        sqlite3_bind_int(stmt,1,pair_cursor);
        int step;
        while((step=sqlite3_step(stmt))==SQLITE_ROW && pairs<LEARNING_BATCH){
            snprintf(sources[pairs],256,"%s",sqlite3_column_text(stmt,0));
            snprintf(destinations[pairs],256,"%s",sqlite3_column_text(stmt,1));pairs++;}
        if(step!=SQLITE_DONE)rc=CBM_STORE_ERR;
        sqlite3_finalize(stmt);
    }else if(rc==CBM_STORE_OK)rc=CBM_STORE_ERR;
    for(int i=0;i<pairs && rc==CBM_STORE_OK;i++){
        int previous=0,current=0;int64_t timestamp=0;
        if(sqlite3_prepare_v2(db,"SELECT success_count FROM verified_learning_association WHERE "
            "src_id=?1 AND dst_id=?2",-1,&stmt,NULL)!=SQLITE_OK){rc=CBM_STORE_ERR;break;}
        sqlite3_bind_text(stmt,1,sources[i],-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt,2,destinations[i],-1,SQLITE_TRANSIENT);
        if(sqlite3_step(stmt)==SQLITE_ROW)previous=sqlite3_column_int(stmt,0);
        sqlite3_finalize(stmt);
        if(sqlite3_prepare_v2(db,"SELECT COUNT(*),COALESCE(MAX(MAX(a.verified_ms,b.verified_ms)),0) "
            "FROM verified_learning_credit a JOIN verified_learning_credit b ON a.task_id=b.task_id "
            "WHERE a.item_id=?1 AND b.item_id=?2 AND a.vote=1 AND b.vote=1",
            -1,&stmt,NULL)!=SQLITE_OK){rc=CBM_STORE_ERR;break;}
        sqlite3_bind_text(stmt,1,sources[i],-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt,2,destinations[i],-1,SQLITE_TRANSIENT);
        if(sqlite3_step(stmt)==SQLITE_ROW){current=sqlite3_column_int(stmt,0);timestamp=sqlite3_column_int64(stmt,1);}
        else rc=CBM_STORE_ERR;
        sqlite3_finalize(stmt);
        if(rc!=CBM_STORE_OK)break;
        if(previous!=current){
            char before[48],after[48],pair[520];
            snprintf(before,sizeof(before),"%d",previous);snprintf(after,sizeof(after),"%d",current);
            snprintf(pair,sizeof(pair),"%s:%s",sources[i],destinations[i]);
            rc=audit(db,"association",pair,before,after,now);
        }
        if(rc==CBM_STORE_OK && sqlite3_prepare_v2(db,"INSERT INTO verified_learning_association "
            "VALUES(?1,?2,?3,?4) ON CONFLICT(src_id,dst_id) DO UPDATE SET success_count=excluded.success_count,"
            "last_verified_ms=excluded.last_verified_ms",-1,&stmt,NULL)==SQLITE_OK){
            sqlite3_bind_text(stmt,1,sources[i],-1,SQLITE_TRANSIENT);sqlite3_bind_text(stmt,2,destinations[i],-1,SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt,3,current);sqlite3_bind_int64(stmt,4,timestamp);
            rc=sqlite3_step(stmt)==SQLITE_DONE ? CBM_STORE_OK:CBM_STORE_ERR;sqlite3_finalize(stmt);
        }else if(rc==CBM_STORE_OK)rc=CBM_STORE_ERR;
    }
    if(rc==CBM_STORE_OK && sqlite3_prepare_v2(db,"UPDATE verified_learning_control SET cursor=?1,"
        "last_maintenance_ms=?2,pair_cursor=?3,last_code=0 WHERE id=1",-1,&stmt,NULL)==SQLITE_OK){
        sqlite3_bind_int(stmt,1,count<LEARNING_BATCH ? 0:cursor+count);sqlite3_bind_int64(stmt,2,now);
        sqlite3_bind_int(stmt,3,pairs<LEARNING_BATCH ? 0:pair_cursor+pairs);
        rc=sqlite3_step(stmt)==SQLITE_DONE ? CBM_STORE_OK:CBM_STORE_ERR;sqlite3_finalize(stmt);
    }else if(rc==CBM_STORE_OK)rc=CBM_STORE_ERR;
    sqlite3_progress_handler(db,0,NULL,NULL);
    if(rc==CBM_STORE_OK)rc=exec_sql(db,"COMMIT");else {
        exec_sql(db,"ROLLBACK");
        exec_sql(db,"UPDATE verified_learning_control SET last_code=-1 WHERE id=1");
    }
    return rc;
}

void cbm_learning_rank(cbm_store_t *store,cbm_memory_result_t *result) {
    sqlite3 *db=store ? cbm_store_get_db(store):NULL;
    if(!db || !result || result->count<=0 || result->count>2048 || !learning_enabled() ||
        !scalar(db,"SELECT enabled FROM verified_learning_control WHERE id=1",0))return;
    /* Two set queries, not an N*N matrix of SQL preparations. Associations never add
     * new candidates, bypass scope filtering, or turn co-use into factual support. */
    double *adjustments=calloc((size_t)result->count,sizeof(double));
    double *boosts=calloc((size_t)result->count,sizeof(double));
    yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL);
    if(!adjustments || !boosts || !doc){free(adjustments);free(boosts);yyjson_mut_doc_free(doc);return;}
    yyjson_mut_val *ids=yyjson_mut_arr(doc);yyjson_mut_doc_set_root(doc,ids);
    for(int i=0;i<result->count;i++)yyjson_mut_arr_add_str(doc,ids,result->items[i].id);
    char *json=yyjson_mut_write(doc,0,NULL);yyjson_mut_doc_free(doc);
    if(!json){free(adjustments);free(boosts);return;}
    int budget=1000,step=SQLITE_DONE;bool complete=false;sqlite3_stmt *stmt=NULL;
    sqlite3_progress_handler(db,1000,vm_budget,&budget);
    if(sqlite3_prepare_v2(db,"SELECT item_id,utility-decay FROM verified_learning_state WHERE "
        "item_id IN (SELECT value FROM json_each(?1))",-1,&stmt,NULL)!=SQLITE_OK)goto cleanup;
    sqlite3_bind_text(stmt,1,json,-1,SQLITE_TRANSIENT);
    while((step=sqlite3_step(stmt))==SQLITE_ROW){
        const char *id=(const char*)sqlite3_column_text(stmt,0);
        for(int i=0;i<result->count;i++)if(!strcmp(id,result->items[i].id)){
            adjustments[i]=sqlite3_column_double(stmt,1);break;}
    }
    sqlite3_finalize(stmt);stmt=NULL;if(step!=SQLITE_DONE)goto cleanup;
    if(sqlite3_prepare_v2(db,"SELECT src_id,dst_id,success_count FROM verified_learning_association "
        "WHERE success_count>0 AND src_id IN (SELECT value FROM json_each(?1)) "
        "AND dst_id IN (SELECT value FROM json_each(?1)) ORDER BY src_id,dst_id LIMIT 512",
        -1,&stmt,NULL)!=SQLITE_OK)goto cleanup;
    sqlite3_bind_text(stmt,1,json,-1,SQLITE_TRANSIENT);
    while((step=sqlite3_step(stmt))==SQLITE_ROW){
        const char *src=(const char*)sqlite3_column_text(stmt,0),*dst=(const char*)sqlite3_column_text(stmt,1);
        double boost=fmin(0.03,0.01*sqlite3_column_int(stmt,2));
        for(int i=0;i<result->count;i++)if(!strcmp(src,result->items[i].id) || !strcmp(dst,result->items[i].id))
            boosts[i]=fmax(boosts[i],boost);
    }
    complete=step==SQLITE_DONE;
cleanup:
    sqlite3_finalize(stmt);sqlite3_progress_handler(db,0,NULL,NULL);free(json);
    if(complete){
        for(int i=0;i<result->count;i++){
            result->items[i].retrieval_score-=result->items[i].learning_adjustment;
            result->items[i].learning_adjustment=adjustments[i]+boosts[i];
            result->items[i].retrieval_score+=adjustments[i]+boosts[i];
        }
        for(int i=1;i<result->count;i++){
            cbm_memory_item_t item=result->items[i];int j=i-1;
            while(j>=0 && result->items[j].retrieval_score<item.retrieval_score){
                result->items[j+1]=result->items[j];j--;}
            result->items[j+1]=item;
        }
    }
    free(adjustments);free(boosts);
}

char *cbm_learning_status(cbm_store_t *store,const char *project) {
    sqlite3 *db=store ? cbm_store_get_db(store):NULL;
    if(!db)return NULL;
    yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL);yyjson_mut_val *root=yyjson_mut_obj(doc);
    if(!doc || !root){yyjson_mut_doc_free(doc);return NULL;}
    yyjson_mut_doc_set_root(doc,root);
    yyjson_mut_obj_add_str(doc,root,"schema","verified-learning/v1");
    yyjson_mut_obj_add_bool(doc,root,"enabled",learning_enabled() &&
        scalar(db,"SELECT enabled FROM verified_learning_control WHERE id=1",1));
    bool ready=scalar(db,"SELECT COUNT(*) FROM sqlite_master WHERE name='verified_learning_control'",0)==1;
    yyjson_mut_obj_add_bool(doc,root,"ready",ready);
    yyjson_mut_obj_add_int(doc,root,"generation",scalar(db,"SELECT generation FROM verified_learning_control WHERE id=1",0));
    yyjson_mut_obj_add_int(doc,root,"last_code",scalar(db,"SELECT last_code FROM verified_learning_control WHERE id=1",0));
    yyjson_mut_obj_add_int(doc,root,"pair_batch_limit",LEARNING_BATCH);
    yyjson_mut_obj_add_int(doc,root,"sql_vm_budget",2000000);
    yyjson_mut_obj_add_int(doc,root,"batch_limit",LEARNING_BATCH);
    yyjson_mut_obj_add_bool(doc,root,"physical_delete",false);
    yyjson_mut_obj_add_str(doc,root,"evidence_gate","hook_receipt_or_native_manager_user_confirmation");
    yyjson_mut_val *items=yyjson_mut_arr(doc),*edges=yyjson_mut_arr(doc);sqlite3_stmt *stmt=NULL;
    if(sqlite3_prepare_v2(db,"SELECT s.item_id,s.positive,s.negative,s.utility,s.decay,"
        "CASE WHEN m.status='archived' AND m.version=s.archived_version THEN s.archived_version END,m.status FROM verified_learning_state s JOIN memory_item m ON m.id=s.item_id "
        "WHERE m.scope_project IS NULL OR m.scope_project=?1 ORDER BY s.item_id LIMIT 200",
        -1,&stmt,NULL)==SQLITE_OK){
        sqlite3_bind_text(stmt,1,project,-1,SQLITE_TRANSIENT);
        while(sqlite3_step(stmt)==SQLITE_ROW){yyjson_mut_val *item=yyjson_mut_obj(doc);
            yyjson_mut_obj_add_strcpy(doc,item,"item_id",(const char*)sqlite3_column_text(stmt,0));
            yyjson_mut_obj_add_int(doc,item,"positive",sqlite3_column_int(stmt,1));
            yyjson_mut_obj_add_int(doc,item,"negative",sqlite3_column_int(stmt,2));
            yyjson_mut_obj_add_real(doc,item,"utility",sqlite3_column_double(stmt,3));
            yyjson_mut_obj_add_real(doc,item,"decay",sqlite3_column_double(stmt,4));
            yyjson_mut_obj_add_bool(doc,item,"restorable",sqlite3_column_type(stmt,5)!=SQLITE_NULL);
            yyjson_mut_obj_add_strcpy(doc,item,"status",(const char*)sqlite3_column_text(stmt,6));
            yyjson_mut_arr_append(items,item);}
    }sqlite3_finalize(stmt);stmt=NULL;
    if(sqlite3_prepare_v2(db,"SELECT a.src_id,a.dst_id,a.success_count FROM verified_learning_association a "
        "JOIN memory_item s ON s.id=a.src_id JOIN memory_item d ON d.id=a.dst_id WHERE a.success_count>0 "
        "AND (s.scope_project IS NULL OR s.scope_project=?1) AND "
        "(d.scope_project IS NULL OR d.scope_project=?1) LIMIT 200",-1,&stmt,NULL)==SQLITE_OK){
        sqlite3_bind_text(stmt,1,project,-1,SQLITE_TRANSIENT);
        while(sqlite3_step(stmt)==SQLITE_ROW){yyjson_mut_val *edge=yyjson_mut_obj(doc);
            yyjson_mut_obj_add_strcpy(doc,edge,"src_id",(const char*)sqlite3_column_text(stmt,0));
            yyjson_mut_obj_add_strcpy(doc,edge,"dst_id",(const char*)sqlite3_column_text(stmt,1));
            yyjson_mut_obj_add_int(doc,edge,"success_count",sqlite3_column_int(stmt,2));
            yyjson_mut_arr_append(edges,edge);}
    }sqlite3_finalize(stmt);
    yyjson_mut_obj_add_val(doc,root,"items",items);yyjson_mut_obj_add_val(doc,root,"associations",edges);
    yyjson_mut_val *pending=yyjson_mut_arr(doc);stmt=NULL;
    if(sqlite3_prepare_v2(db,"SELECT e.evidence_id,r.result_hash,e.evidence_ref,r.result_ref,"
        "SUBSTR(COALESCE(GROUP_CONCAT(DISTINCT m.summary),''),1,1024) FROM memory_evidence e "
        "JOIN memory_task t ON t.task_id=e.task_id JOIN memory_task_result r ON r.result_id=e.result_id "
        "JOIN feedback_event f ON f.evidence_id=e.evidence_id JOIN retrieval_candidate c ON c.id=f.candidate_id "
        "JOIN memory_item m ON m.id=c.memory_item_id "
        "WHERE t.project=?1 AND e.trust_class='explicit_user' AND e.source_type='user' "
        "AND e.evidence_state='valid' AND r.result_type='user_confirmation' AND NOT EXISTS("
        "SELECT 1 FROM verified_learning_user_receipt p WHERE p.evidence_id=e.evidence_id) GROUP BY e.evidence_id LIMIT 50",
        -1,&stmt,NULL)==SQLITE_OK){
        sqlite3_bind_text(stmt,1,project,-1,SQLITE_TRANSIENT);
        while(sqlite3_step(stmt)==SQLITE_ROW){yyjson_mut_val *entry=yyjson_mut_obj(doc);
            yyjson_mut_obj_add_strcpy(doc,entry,"evidence_id",(const char*)sqlite3_column_text(stmt,0));
            yyjson_mut_obj_add_strcpy(doc,entry,"result_hash",(const char*)sqlite3_column_text(stmt,1));
            yyjson_mut_obj_add_strcpy(doc,entry,"evidence_ref",(const char*)sqlite3_column_text(stmt,2));
            yyjson_mut_obj_add_strcpy(doc,entry,"result_ref",(const char*)sqlite3_column_text(stmt,3));
            yyjson_mut_obj_add_strcpy(doc,entry,"memory_summary",(const char*)sqlite3_column_text(stmt,4));
            yyjson_mut_arr_append(pending,entry);}
    }
    sqlite3_finalize(stmt);yyjson_mut_obj_add_val(doc,root,"pending_user_confirmations",pending);
    char *json=yyjson_mut_write(doc,0,NULL);yyjson_mut_doc_free(doc);return json;
}

int cbm_learning_confirm_user(cbm_store_t *store,const char *project,const char *id,const char *hash){
    sqlite3 *db=store ? cbm_store_get_db(store):NULL;
    if(!db || !project || !id || !hash || strlen(hash)!=64 || learning_schema(db)!=CBM_STORE_OK)return CBM_STORE_REJECTED;
    if(exec_sql(db,"BEGIN IMMEDIATE")!=CBM_STORE_OK)return CBM_STORE_ERR;
    sqlite3_stmt *stmt=NULL;int rc=CBM_STORE_ERR;
    if(sqlite3_prepare_v2(db,"INSERT OR IGNORE INTO verified_learning_user_receipt "
        "SELECT e.evidence_id,r.result_hash FROM memory_evidence e JOIN memory_task t ON t.task_id=e.task_id "
        "JOIN memory_task_result r ON r.result_id=e.result_id WHERE e.evidence_id=?1 AND t.project=?2 "
        "AND r.result_hash=?3 AND r.result_type='user_confirmation' AND e.trust_class='explicit_user' "
        "AND e.source_type='user' AND e.evidence_state='valid'",-1,&stmt,NULL)==SQLITE_OK){
        sqlite3_bind_text(stmt,1,id,-1,SQLITE_TRANSIENT);sqlite3_bind_text(stmt,2,project,-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt,3,hash,-1,SQLITE_TRANSIENT);
        if(sqlite3_step(stmt)==SQLITE_DONE)rc=sqlite3_changes(db)==1 ? CBM_STORE_OK:CBM_STORE_REPLAYED;
    }
    sqlite3_finalize(stmt);
    if(rc==CBM_STORE_REPLAYED){
        if(sqlite3_prepare_v2(db,"SELECT 1 FROM verified_learning_user_receipt p JOIN memory_evidence e "
            "ON e.evidence_id=p.evidence_id JOIN memory_task t ON t.task_id=e.task_id "
            "WHERE p.evidence_id=?1 AND p.result_hash=?2 AND t.project=?3",-1,&stmt,NULL)!=SQLITE_OK)rc=CBM_STORE_ERR;
        else {
            sqlite3_bind_text(stmt,1,id,-1,SQLITE_TRANSIENT);sqlite3_bind_text(stmt,2,hash,-1,SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt,3,project,-1,SQLITE_TRANSIENT);
            if(sqlite3_step(stmt)!=SQLITE_ROW)rc=CBM_STORE_REJECTED;
        }
        sqlite3_finalize(stmt);
    }
    if(rc==CBM_STORE_OK)rc=audit(db,"user_confirmation",id,"pending","confirmed",(int64_t)time(NULL)*1000);
    if(rc==CBM_STORE_OK || rc==CBM_STORE_REPLAYED){
        if(exec_sql(db,"COMMIT")!=CBM_STORE_OK)return CBM_STORE_ERR;
        (void)cbm_learning_refresh(store,0);
    }else exec_sql(db,"ROLLBACK");
    return rc;
}

int cbm_learning_control(cbm_store_t *store,const char *project,const char *action,
                         const char *id,const char *key,int generation,int64_t now) {
    sqlite3 *db=store ? cbm_store_get_db(store):NULL;
    if(!db || !project || !action || !key || !key[0] || strlen(key)>512 || generation<0 ||
       learning_schema(db)!=CBM_STORE_OK)return CBM_STORE_ERR;
    if(now<=0)now=(int64_t)time(NULL)*1000;
    char material[2048],hash[65];
    snprintf(material,sizeof(material),"%s:%s:%s:%d",project,action,id ? id:"",generation);
    if(cbm_stage7_sha256_hex(material,strlen(material),hash)!=CBM_STORE_OK)return CBM_STORE_ERR;
    if(exec_sql(db,"BEGIN IMMEDIATE")!=CBM_STORE_OK)return CBM_STORE_ERR;
    sqlite3_stmt *stmt=NULL;int rc=CBM_STORE_REJECTED;
    if(sqlite3_prepare_v2(db,"SELECT payload_hash FROM verified_learning_request WHERE request_id=?1",
        -1,&stmt,NULL)!=SQLITE_OK){exec_sql(db,"ROLLBACK");return CBM_STORE_ERR;}
    sqlite3_bind_text(stmt,1,key,-1,SQLITE_TRANSIENT);
    if(sqlite3_step(stmt)==SQLITE_ROW){
        rc=!strcmp(hash,(const char*)sqlite3_column_text(stmt,0)) ? CBM_STORE_REPLAYED:CBM_STORE_IDEMPOTENCY_CONFLICT;
        sqlite3_finalize(stmt);exec_sql(db,"ROLLBACK");return rc;
    }
    sqlite3_finalize(stmt);stmt=NULL;
    int previous_enabled=scalar(db,"SELECT enabled FROM verified_learning_control WHERE id=1",0);
    if(generation!=scalar(db,"SELECT generation FROM verified_learning_control WHERE id=1",-1)){
        exec_sql(db,"ROLLBACK");return CBM_STORE_REJECTED;}
    char before[160],after[160];
    snprintf(before,sizeof(before),"{\"enabled\":%d,\"generation\":%d}",previous_enabled,generation);
    if(!strcmp(action,"pause") || !strcmp(action,"resume")){
        if(sqlite3_prepare_v2(db,"UPDATE verified_learning_control SET enabled=?1 WHERE id=1",
            -1,&stmt,NULL)==SQLITE_OK){sqlite3_bind_int(stmt,1,!strcmp(action,"resume"));
            rc=sqlite3_step(stmt)==SQLITE_DONE ? CBM_STORE_OK:CBM_STORE_ERR;}
    }else if(!strcmp(action,"restore") && id){
        if(sqlite3_prepare_v2(db,"SELECT m.version,s.restore_status FROM memory_item m JOIN "
            "verified_learning_state s ON s.item_id=m.id WHERE m.id=?1 AND m.status='archived' "
            "AND m.version=s.archived_version AND (m.scope_project IS NULL OR m.scope_project=?2)",
            -1,&stmt,NULL)==SQLITE_OK){
            sqlite3_bind_text(stmt,1,id,-1,SQLITE_TRANSIENT);sqlite3_bind_text(stmt,2,project,-1,SQLITE_TRANSIENT);
            if(sqlite3_step(stmt)==SQLITE_ROW){
                snprintf(before,sizeof(before),"{\"status\":\"archived\",\"version\":%d}",sqlite3_column_int(stmt,0));
                snprintf(after,sizeof(after),"{\"status\":\"%s\",\"version\":%d}",sqlite3_column_text(stmt,1),sqlite3_column_int(stmt,0)+1);
                rc=CBM_STORE_OK;
            }
        }
        sqlite3_finalize(stmt);stmt=NULL;
        if(rc==CBM_STORE_OK && sqlite3_prepare_v2(db,"UPDATE memory_item SET status=(SELECT restore_status FROM "
            "verified_learning_state WHERE item_id=?1),version=version+1,updated_at=?2 "
            "WHERE id=?1 AND status='archived' AND version=(SELECT archived_version FROM "
            "verified_learning_state WHERE item_id=?1) AND (scope_project IS NULL OR scope_project=?3)",
            -1,&stmt,NULL)==SQLITE_OK){
            sqlite3_bind_text(stmt,1,id,-1,SQLITE_TRANSIENT);sqlite3_bind_int64(stmt,2,now);
            sqlite3_bind_text(stmt,3,project,-1,SQLITE_TRANSIENT);
            rc=sqlite3_step(stmt)==SQLITE_DONE && sqlite3_changes(db)==1 ? CBM_STORE_OK:CBM_STORE_REJECTED;
        }else if(rc==CBM_STORE_OK)rc=CBM_STORE_ERR;
        sqlite3_finalize(stmt);stmt=NULL;
        if(rc==CBM_STORE_OK && sqlite3_prepare_v2(db,"UPDATE verified_learning_state SET archived_version=NULL,"
            "restore_status=NULL,restore_until_ms=?1 WHERE item_id=?2",-1,&stmt,NULL)==SQLITE_OK){
            sqlite3_bind_int64(stmt,1,now+30*LEARNING_DAY_MS);sqlite3_bind_text(stmt,2,id,-1,SQLITE_TRANSIENT);
            rc=sqlite3_step(stmt)==SQLITE_DONE ? CBM_STORE_OK:CBM_STORE_ERR;
        }else if(rc==CBM_STORE_OK)rc=CBM_STORE_ERR;
    }
    sqlite3_finalize(stmt);stmt=NULL;
    if(strcmp(action,"restore"))snprintf(after,sizeof(after),"{\"enabled\":%d,\"generation\":%d}",!strcmp(action,"resume"),generation+1);
    if(rc==CBM_STORE_OK)rc=exec_sql(db,"UPDATE verified_learning_control SET generation=generation+1 WHERE id=1");
    if(rc==CBM_STORE_OK && sqlite3_prepare_v2(db,"INSERT INTO verified_learning_request VALUES(?1,?2)",
        -1,&stmt,NULL)==SQLITE_OK){
        sqlite3_bind_text(stmt,1,key,-1,SQLITE_TRANSIENT);sqlite3_bind_text(stmt,2,hash,-1,SQLITE_TRANSIENT);
        rc=sqlite3_step(stmt)==SQLITE_DONE ? CBM_STORE_OK:CBM_STORE_ERR;sqlite3_finalize(stmt);
    }else if(rc==CBM_STORE_OK)rc=CBM_STORE_ERR;
    if(rc==CBM_STORE_OK)rc=audit(db,action,id ? id:"controller",before,after,now);
    if(rc==CBM_STORE_OK)rc=exec_sql(db,"COMMIT");else exec_sql(db,"ROLLBACK");
    return rc;
}
