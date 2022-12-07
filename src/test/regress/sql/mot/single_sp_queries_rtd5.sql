create foreign table t_auto_call(
	SM_USER_ID int not null primary key,
	ROSTER_ID varchar (48),
	DD_APPDATE date not null
	);

create foreign table t_prmi_transaction(
	SM_USER_ID int not null,
	SD_KEY int not null,
	SEQ_NO varchar(32),
	SD_PT_SRV_ENTRY_NDE_L2 varchar(2),
	SD_PT_SRV_COND_CDE varchar(4),
	SD_TERM_CNTRY varchar(4),
	SD_ACO_INST_ID_NUM_R4 int,
	DD_APPDATE date not null
	);
	
CREATE OR REPLACE PROCEDURE select_count(ev_ddApdate date, en_smUserId int, ev_sdKey int, ev_sdTermCntry character varying, ev_sdPtSrvCondCde varchar, 
ev_sdAcqInstIdNumR4 int, result_type varchar, flag char, send_user varchar, OUT rv_autocall_wk_24h_abroadcntry INTEGER, OUT msg character varying) 
    AS
BEGIN
	select count (1) into rv_autocall_wk_24h_abroadcntry 
	from T_AUTO_CALL t_auto_call, T_PRMI_TRANSACTION t_prmi_transaction
	where t_auto_call.SM_USER_ID = en_smUserId
	and t_prmi_transaction.SM_USER_ID = en_smUserId
	and t_prmi_transaction.SD_KEY = ev_sdKey
	and result_type = 'OK'
	and flag = 'I' 
	and (send_user = 'PRMI' or right( roster_id, 9) = 'MANU_CALL')
	and left(t_auto_call.ROSTER_ID, 32) = t_prmi_transaction.SEQ_NO
	and t_prmi_transaction.SD_PT_SRV_ENTRY_NDE_L2 not in ('01','02','81','10','04','94')
	and t_prmi_transaction.SD_PT_SRV_COND_CDE in ('V001','M001','A001','J001','N012','A003')
	and ( 
		t_prmi_transaction.SD_TERM_CNTRY = ev_sdTermCntry 
		and ev_sdPtSrvCondCde != 'NO12'
		and t_prmi_transaction.SD_PT_SRV_COND_CDE = 'N012' 
	or
		t_prmi_transaction.SD_ACO_INST_ID_NUM_R4 = ev_sdAcqInstIdNumR4
		and t_prmi_transaction.SD_PT_SRV_COND_CDE = 'N012'
		and ev_sdPtSrvCondCde = 'N012'
	)
	and extract(second from ev_ddApdate) <= extract(second from t_prmi_transaction.DD_APPDATE) + 3600*24
	--and since_epoch(second, cast(ev_ddApdate as timestamp)) <= since_epoch(second, cast(t_prmi_transaction.DD_APPDATE as timestamp)) + 3600*24
	and extract(second from ev_ddApdate) <= extract(second from t_auto_call.DD_APPDATE) + 3600*24;
	--and since_epoch(second, cast(ev_ddApdate as timestamp)) <= since_epoch(second, cast(t_auto_call.DD_APPDATE as timestamp)) + 3600*24;
	
	msg := 'OK';
EXCEPTION
    WHEN OTHERS THEN
        msg := SQLERRM;
END;
/

create index on t_auto_call(DD_APPDATE);
create index on t_prmi_transaction(SM_USER_ID);
create index on t_prmi_transaction(SD_KEY);
create index on t_prmi_transaction(DD_APPDATE);

insert into t_auto_call values (1, 'qwertyuiopasdfghjklzxcvbnmqwertyuiopas', current_date - '1 day'::INTERVAL);
insert into t_auto_call values (2, 'qwertyuiopasdfghjklzxcvbnmqwertyuiopas', current_date - '1 day'::INTERVAL);
insert into t_auto_call values (3, 'qwertyuiopasdfghjklzxcvbnmqwertyuiopas', current_date - '1 day'::INTERVAL);

insert into t_prmi_transaction values (1, 1, 'qwertyuiopasdfghjklzxcvbnmqwerty', '02', 'N012', 'XYZ', 1, current_date - '1 day'::INTERVAL);
insert into t_prmi_transaction values (1, 2, 'qwertyuiopasdfghjklzxcvbnmqwerty', '81', 'V001', 'XXZ', 1, current_date - '1 day'::INTERVAL);
insert into t_prmi_transaction values (2, 1, 'qwertyuiopasdfghjklzxcvbnmqwery', '04', 'J001', 'XYY', 1, current_date - '1 day'::INTERVAL);
insert into t_prmi_transaction values (1, 3, 'qwertyuiopasdfghjklzxcvbnmqwerty', '94', 'A003', 'XZZ', 1, current_date - '1 day'::INTERVAL);
insert into t_prmi_transaction values (1, 1, 'qwertyuiopasdfghjklzxcvbnmqwerty', '01', 'N012', 'XYZ', 1, current_date - '1 day'::INTERVAL);

insert into t_prmi_transaction values (1, 1, 'qwertyuiopasdfghjklzxcvbnmqwerty', '07', 'N012', 'XYZ', 1, current_date - '1 day'::INTERVAL);
insert into t_prmi_transaction values (1, 1, 'qwertyuiopasdfghjklzxcvbnmqwerty', '00', 'N011', 'XYZ', 1, current_date - '1 day'::INTERVAL);

select select_count(current_date,1,1,'XYZ','N012',1,'OK', 'I', 'PRMI');
select select_count(current_date,1,2,'XYY','N012',1,'OK', 'I', 'PRMI');	
select select_count(current_date,1,1,'XYZ','N012',1,'OK', 'I', 'PRMI');	
select select_count(current_date,2,1,'XZZ','N012',1,'OK', 'I', 'PRMI');	
select select_count(current_date,1,1,'XYZ','N012',1,'OK', 'I', 'PRMI');	
select select_count(current_date,3,1,'XXZ','N012',1,'OK', 'I', 'PRMI');	

select select_count(current_date,1,1,'XYZ','N012',1,'OK', 'I', 'PRMI');
select select_count(current_date,1,1,'XZ','N012',1,'OK', 'I', 'PRMI');
select select_count(current_date,1,1,'XZ','N017',1,'OK', 'I', 'PRMI');
select select_count(current_date,1,1,'XYZ','N017',1,'OK', 'I', 'PRMI');
select select_count(current_date,1,1,'XYZ','N017',1,'OK', 'i', 'PRMI');
select select_count(current_date,1,1,'XYZ','N017',1,'OK', 'I', 'PRMI');
select select_count(current_date,1,1,'XYZ','N012',2,'OK', 'I', 'PRMI');
select select_count(current_date,1,1,'XYZ','N012',10,'OK', 'I', 'PRMI');
select select_count(current_date,1,1,'XYZ','N0123',10,'OK', 'I', 'PRMI');
select select_count(current_date,1,1,'XYzZ','N0123',10,'OK', 'I', 'PRMI');
select select_count(current_date,1,1,'XYzZ','N0123',10,'OK', 'I', 'PMI');

drop foreign table t_auto_call;
drop foreign table t_prmi_transaction;
