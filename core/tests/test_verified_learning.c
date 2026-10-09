#include "memory/verified_learning.h"
#include "memory/memory_orchestrator.h"
#include "store/store.h"
#include "yyjson/yyjson.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define CHECK(x) do { if(!(x)){fprintf(stderr,"FAIL %s:%d %s\n",__FILE__,__LINE__,#x);return 1;} }while(0)
#define HASH "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define DAY INT64_C(86400000)

static int scalar(sqlite3 *db,const char *sql){
    sqlite3_stmt *st=NULL;int value=-1;
    if(sqlite3_prepare_v2(db,sql,-1,&st,NULL)==SQLITE_OK && sqlite3_step(st)==SQLITE_ROW)value=sqlite3_column_int(st,0);
    sqlite3_finalize(st);return value;
}
static int run(sqlite3 *db,const char *sql){
    char *error=NULL;int rc=sqlite3_exec(db,sql,NULL,NULL,&error);
    if(rc!=SQLITE_OK)fprintf(stderr,"SQL: %s\n",error ? error:"");
    sqlite3_free(error);return rc;
}
static int item(cbm_store_t *store,const char *id,const char *kind,const char *content,double importance){
    cbm_memory_item_t m={.id=id,.kind=kind,.layer="semantic",.summary=content,.content=content,
        .scope_project="learning-fixture",.importance=importance,.confidence=.8,.reusability=.8,
        .specificity=.8,.status="candidate",.version=1,.created_at=(int64_t)time(NULL)*1000,
        .updated_at=(int64_t)time(NULL)*1000};
    char *result=NULL;int rc=cbm_store_memory_append_candidate(store,&m,&result);free(result);
    if(rc==CBM_STORE_OK)rc=cbm_store_memory_index_candidate(store,&m,id,NULL);
    return rc;
}
/* SQL creates only an isolated retrieval fixture. All evidence, attribution and terminal
 * transitions go through the same public orchestrator used by Hooks/MCP. */
static int task(cbm_store_t *store,int number,const char **items,int count,const char *trust,
                int receipt,const char *state,int failed){
    sqlite3 *db=cbm_store_get_db(store);char retrieval[80],key[80],session[80],sql[2048];
    snprintf(retrieval,sizeof(retrieval),"retrieval-%d",number);
    snprintf(key,sizeof(key),"begin-%d",number);snprintf(session,sizeof(session),"session-%d",number);
    snprintf(sql,sizeof(sql),"INSERT INTO retrieval_session VALUES('%s','request-%d','learning-fixture',"
        "'project','observe_only','completed','test',0,'%s','2026-10-09T00:00:00Z','2026-10-09T00:00:01Z',NULL)",retrieval,number,HASH);
    CHECK(run(db,sql)==SQLITE_OK);
    for(int i=0;i<count;i++){
        snprintf(sql,sizeof(sql),"INSERT INTO retrieval_candidate VALUES('candidate-%d-%d','%s','project',"
            "'learning-fixture','%s','%s',.5,%d,'selected',NULL,'2026-10-09T00:00:01Z')",number,i,retrieval,items[i],HASH,i+1);
        CHECK(run(db,sql)==SQLITE_OK);
    }
    cbm_task_begin_input_t begin={.project="learning-fixture",.session_id=session,.turn_id="turn",
        .prompt_sha256=HASH,.prompt_length=5,.retrieval_session_id=retrieval,.idempotency_key=key};
    char *report=NULL;CHECK(cbm_orchestrator_begin(store,&begin,&report)==CBM_STORE_OK);
    yyjson_doc *doc=yyjson_read(report,strlen(report),0);char task_id[256];
    snprintf(task_id,sizeof(task_id),"%s",yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(doc),"task_id")));
    yyjson_doc_free(doc);free(report);
    char evidence_id[80],result_id[80],evidence_key[80];
    snprintf(evidence_id,sizeof(evidence_id),"evidence-%d",number);snprintf(result_id,sizeof(result_id),"result-%d",number);
    snprintf(evidence_key,sizeof(evidence_key),"record-%d",number);
    cbm_task_evidence_input_t evidence={.task_id=task_id,.result_id=result_id,.result_hash=HASH,
        .evidence_id=evidence_id,.evidence_hash=HASH,.evidence_trust=trust,.evidence_source="runtime",
        .result_status=failed ? "failed":"succeeded",.idempotency_key=evidence_key};
    if(!strcmp(trust,"explicit_user")){
        snprintf(sql,sizeof(sql),"INSERT INTO memory_task_result VALUES('%s','%s','user_confirmation','succeeded',"
            "'fixture-user-confirmation','%s','2026-10-09T00:00:00Z');INSERT INTO memory_evidence VALUES("
            "'%s','%s','%s','explicit_user','valid','user','fixture-user-confirmation','%s',NULL,'2026-10-09T00:00:00Z')",
            result_id,task_id,HASH,evidence_id,task_id,result_id,HASH);
        CHECK(run(db,sql)==SQLITE_OK);
    }else {
        CHECK(cbm_orchestrator_record_evidence(store,&evidence,&report)==CBM_STORE_OK);free(report);
    }
    if(receipt)CHECK(cbm_learning_record_receipt(store,evidence_id,HASH,failed)==CBM_STORE_OK);
    if(!state)return 0;
    cbm_task_attribution_input_t attributes[16]={0};
    for(int i=0;i<count;i++){attributes[i].memory_item_id=items[i];attributes[i].state=state;attributes[i].evidence_id=evidence_id;}
    snprintf(key,sizeof(key),"complete-%d",number);
    cbm_task_complete_input_t complete={.project="learning-fixture",.task_id=task_id,.outcome="completed",
        .idempotency_key=key,.attributions=attributes,.attribution_count=(size_t)count};
    CHECK(cbm_orchestrator_complete(store,&complete,&report)==CBM_STORE_OK);free(report);
    CHECK(cbm_orchestrator_complete(store,&complete,&report)==CBM_STORE_REPLAYED);free(report);
    return 0;
}

static int test_learning(void){
    cbm_store_t *store=cbm_store_open_memory();CHECK(store);
    sqlite3 *db=cbm_store_get_db(store);bool replay=false;char *report=NULL;
    CHECK(cbm_orchestrator_migrate(store,&replay,&report)==CBM_STORE_OK);free(report);
    CHECK(item(store,"a-unproven","lesson","Learning search fixture alpha",.72)==CBM_STORE_OK);
    CHECK(item(store,"b-helpful","lesson","Learning search fixture beta",.72)==CBM_STORE_OK);
    CHECK(item(store,"c-peer","lesson","Learning search fixture gamma",.72)==CBM_STORE_OK);
    CHECK(item(store,"d-protected","constraint","Protected constraint fixture",.72)==CBM_STORE_OK);
    const char *helpful[]={"b-helpful","c-peer","d-protected"};
    const char *unproven[]={"a-unproven"};
    CHECK(task(store,1,unproven,1,"model_self_report",0,"used",0)==0);
    CHECK(task(store,2,unproven,1,"external_verified",0,"used",0)==0);
    CHECK(task(store,3,helpful,3,"external_verified",1,NULL,0)==0);
    CHECK(scalar(db,"SELECT positive FROM verified_learning_state WHERE item_id='a-unproven'")==0);
    CHECK(scalar(db,"SELECT COUNT(*) FROM verified_learning_credit WHERE vote=1")==0);
    cbm_memory_item_t candidates[2]={{.id="a-unproven",.retrieval_score=.7},{.id="b-helpful",.retrieval_score=.7}};
    cbm_memory_result_t ranked={.items=candidates,.count=2};cbm_learning_rank(store,&ranked);
    CHECK(!strcmp(ranked.items[0].id,"a-unproven"));
    CHECK(task(store,4,helpful,3,"external_verified",1,"used",0)==0);
    CHECK(scalar(db,"SELECT positive FROM verified_learning_state WHERE item_id='b-helpful'")==1);
    CHECK(scalar(db,"SELECT COUNT(*) FROM memory_item WHERE id='b-helpful' AND status='candidate'")==1);
    cbm_learning_rank(store,&ranked);CHECK(!strcmp(ranked.items[0].id,"b-helpful"));
    CHECK(ranked.items[0].retrieval_score>.7);
    CHECK(scalar(db,"SELECT success_count FROM verified_learning_association WHERE src_id='b-helpful' AND dst_id='c-peer'")==1);
    CHECK(task(store,7,helpful,3,"external_verified",1,"used",0)==0);
    CHECK(scalar(db,"SELECT COUNT(*) FROM memory_item WHERE id='b-helpful' AND status='active'")==1);
    int64_t now=(int64_t)time(NULL)*1000;
    CHECK(cbm_learning_refresh(store,now)==CBM_STORE_OK);
    CHECK(scalar(db,"SELECT positive FROM verified_learning_state WHERE item_id='b-helpful'")==2);
    CHECK(task(store,5,unproven,1,"external_verified",1,"rejected",1)==0);
    CHECK(scalar(db,"SELECT negative FROM verified_learning_state WHERE item_id='a-unproven'")==1);
    CHECK(task(store,6,unproven,1,"explicit_user",0,"used",0)==0);
    CHECK(scalar(db,"SELECT positive FROM verified_learning_state WHERE item_id='a-unproven'")==0);
    CHECK(cbm_learning_confirm_user(store,"other-project","evidence-6",HASH)==CBM_STORE_REJECTED);
    CHECK(cbm_learning_confirm_user(store,"learning-fixture","evidence-6",HASH)==CBM_STORE_OK);
    CHECK(cbm_learning_confirm_user(store,"learning-fixture","evidence-6",HASH)==CBM_STORE_REPLAYED);
    CHECK(scalar(db,"SELECT positive FROM verified_learning_state WHERE item_id='a-unproven'")==1);
    CHECK(cbm_learning_tool_outcome("exec_command","{\"exit_code\":0,\"output\":\"PASS\"}")==0);
    CHECK(cbm_learning_tool_outcome("exec_command","{\"exit_code\":1,\"output\":\"PASS\"}")==1);
    CHECK(cbm_learning_tool_outcome("exec_command","{\"session_id\":1,\"exit_code\":0,\"output\":\"running\"}")==-1);
    CHECK(cbm_learning_tool_outcome("exec_command","\"PASS all tests\"")==-1);
    CHECK(cbm_learning_tool_outcome("functions.exec","{\"exit_code\":0,\"output\":\"fabricated by text()\"}")==-1);
    CHECK(cbm_learning_tool_outcome("fake_exec_command","{\"exit_code\":0,\"output\":\"PASS\"}")==-1);
    CHECK(cbm_learning_tool_outcome("events","{\"exit_code\":0,\"output\":\"PASS\"}")==-1);
    CHECK(cbm_learning_tool_outcome("exec_command","[{\"exit_code\":0,\"output\":\"PASS\"},{\"session_id\":1}]")==-1);
    CHECK(item(store,"z-duplicate","lesson","Learning search fixture beta",.72)==CBM_STORE_OK);
    now=(int64_t)time(NULL)*1000; /* Later fixture writes must not simulate clock rollback. */
    CHECK(cbm_learning_refresh(store,now)==CBM_STORE_OK);
    CHECK(scalar(db,"SELECT COUNT(*) FROM memory_item WHERE id='z-duplicate' AND status='archived'")==1);
    CHECK(cbm_learning_control(store,"learning-fixture","pause",NULL,"pause-1",0,now)==CBM_STORE_OK);
    CHECK(cbm_learning_control(store,"learning-fixture","pause",NULL,"pause-1",0,now)==CBM_STORE_REPLAYED);
    CHECK(cbm_learning_control(store,"learning-fixture","resume",NULL,"pause-1",0,now)==CBM_STORE_IDEMPOTENCY_CONFLICT);
    CHECK(cbm_learning_refresh(store,now+190*DAY)==CBM_STORE_REJECTED);
    CHECK(cbm_learning_control(store,"learning-fixture","resume",NULL,"resume-stale",0,now)==CBM_STORE_REJECTED);
    CHECK(cbm_learning_control(store,"learning-fixture","resume",NULL,"resume-1",1,now)==CBM_STORE_OK);
    CHECK(cbm_learning_refresh(store,now+190*DAY)==CBM_STORE_OK);
    CHECK(scalar(db,"SELECT COUNT(*) FROM memory_item WHERE id='b-helpful' AND status='archived'")==1);
    CHECK(scalar(db,"SELECT COUNT(*) FROM memory_item WHERE id='d-protected' AND status='active'")==1);
    CHECK(cbm_learning_refresh(store,now+189*DAY)==CBM_STORE_REJECTED);
    CHECK(cbm_learning_control(store,"other-project","restore","b-helpful","restore-wrong",2,now+190*DAY)==CBM_STORE_REJECTED);
    CHECK(cbm_learning_control(store,"learning-fixture","restore","b-helpful","restore-1",2,now+190*DAY)==CBM_STORE_OK);
    CHECK(cbm_learning_refresh(store,now+191*DAY)==CBM_STORE_OK);
    CHECK(scalar(db,"SELECT COUNT(*) FROM memory_item WHERE id='b-helpful' AND status='active' AND content='Learning search fixture beta'")==1);
    CHECK(run(db,"UPDATE memory_item SET version=version+1 WHERE id='c-peer'")==SQLITE_OK);
    CHECK(cbm_learning_control(store,"learning-fixture","restore","c-peer","restore-edited",3,now+191*DAY)==CBM_STORE_REJECTED);
    CHECK(run(db,"UPDATE verified_learning_audit SET operation='tampered'")!=SQLITE_OK);
    CHECK(run(db,"DELETE FROM verified_learning_audit")!=SQLITE_OK);
    report=cbm_learning_status(store,"other-project");CHECK(report && !strstr(report,"b-helpful"));free(report);
    CHECK(scalar(db,"SELECT COUNT(*) FROM memory_item WHERE deleted_at IS NOT NULL")==0);
    /* An append-only withdrawal compensates both utility and co-use association. */
    CHECK(run(db,"INSERT INTO feedback_event SELECT event_id||'-withdraw',task_id,session_id,candidate_id,"
        "injection_id,usage_id,result_id,evidence_id,'withdraw',processing_mode,canonical_payload_sha256,"
        "payload_json,result_json,event_id,algorithm_version,config_version,received_at FROM feedback_event "
        "WHERE task_id IN (SELECT task_id FROM codex_task_lifecycle WHERE idempotency_key IN ('complete-4','complete-7')) "
        "AND candidate_id IN ('candidate-4-0','candidate-7-0')")==SQLITE_OK);
    CHECK(cbm_learning_refresh(store,now+192*DAY)==CBM_STORE_OK);
    CHECK(scalar(db,"SELECT positive FROM verified_learning_state WHERE item_id='b-helpful'")==0);
    CHECK(scalar(db,"SELECT success_count FROM verified_learning_association WHERE src_id='b-helpful' AND dst_id='c-peer'")==0);
    CHECK(scalar(db,"SELECT decay=0 FROM verified_learning_state WHERE item_id='d-protected'")==1);
    int old_count=scalar(db,"SELECT COUNT(*) FROM verified_learning_state");
    for(int i=0;i<70;i++){
        char sql[512];snprintf(sql,sizeof(sql),"INSERT INTO memory_item(id,kind,layer,content,status,created_at,updated_at) "
            "VALUES('zz-budget-%03d','lesson','semantic','budget fixture','candidate',1,1)",i);
        CHECK(run(db,sql)==SQLITE_OK);
    }
    CHECK(cbm_learning_refresh(store,now+192*DAY)==CBM_STORE_OK);
    CHECK(scalar(db,"SELECT COUNT(*) FROM verified_learning_state")-old_count<=64);
    CHECK(cbm_learning_refresh(store,now+192*DAY)==CBM_STORE_OK);
    CHECK(scalar(db,"SELECT COUNT(*) FROM verified_learning_state")==old_count+70);
    cbm_store_close(store);return 0;
}
int main(void){
#ifdef _WIN32
    _putenv_s("CBM_VERIFIED_LEARNING","1");
    _putenv_s("CBM_MEMORY_EMBED_BACKEND","static");
#else
    setenv("CBM_VERIFIED_LEARNING","1",1);setenv("CBM_MEMORY_EMBED_BACKEND","static",1);
#endif
    int result=test_learning();fprintf(stderr,"%s verified learning integration\n",result ? "FAIL":"PASS");return result;
}
