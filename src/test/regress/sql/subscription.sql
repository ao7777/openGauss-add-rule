--
-- SUBSCRIPTION
--
--- prepare
CREATE ROLE regress_subscription_user LOGIN SYSADMIN PASSWORD 'Abcdef@123';
SET SESSION AUTHORIZATION 'regress_subscription_user' PASSWORD 'Abcdef@123';
DROP SUBSCRIPTION IF EXISTS testsub;
--- create subscription
-- fail - syntax error, no publications
CREATE SUBSCRIPTION testsub CONNECTION 'foo';
-- fail - syntax error, no connection
CREATE SUBSCRIPTION testsub PUBLICATION foo;
-- fail - could not connect to the publisher
create subscription testsub2 connection 'host=abc' publication pub;
set client_min_messages to error;
-- fail - syntax error, invalid connection string syntax: missing "="
CREATE SUBSCRIPTION testsub CONNECTION 'testconn' PUBLICATION testpub;
-- fail - unrecognized subscription parameter: create_slot
CREATE SUBSCRIPTION testsub CONNECTION 'dbname=doesnotexist' PUBLICATION testpub WITH (create_slot=false);
CREATE SUBSCRIPTION testsub CONNECTION 'dbname=doesnotexist' PUBLICATION testpub WITH (ENABLED=false, slot_name='testsub', synchronous_commit=off);
reset client_min_messages;
select subname, pg_get_userbyid(subowner) as Owner, subenabled, subconninfo, subpublications from pg_subscription where subname='testsub';
--- alter subscription
------ set publication
ALTER SUBSCRIPTION testsub SET PUBLICATION testpub2, testpub3;
select subname, subenabled, subpublications from pg_subscription  where subname='testsub';
------ modify conninfo
ALTER SUBSCRIPTION testsub CONNECTION 'dbname=doesnotexist2';
select subname, subenabled, subconninfo from pg_subscription  where subname='testsub';
ALTER SUBSCRIPTION testsub SET (conninfo='dbname=doesnotexist3');
select subname, subenabled, subconninfo from pg_subscription  where subname='testsub';
------ modify synchronous_commit
ALTER SUBSCRIPTION testsub SET (synchronous_commit=on);
select subname, subenabled, subsynccommit from pg_subscription  where subname='testsub';
------ modify slot_name to non-null value
------ fail - Currently enabled=false, cannot change slot_name to a non-null value.
ALTER SUBSCRIPTION testsub SET (slot_name='testsub');
--- inside a transaction block
------ CREATE SUBSCRIPTION ... WITH (enabled = true)
------ fail - ERROR:  CREATE SUBSCRIPTION ... WITH (enabled = true) cannot run inside a transaction block
BEGIN;
CREATE SUBSCRIPTION testsub CONNECTION 'dbname=doesnotexist' PUBLICATION testpub WITH (ENABLED=true);
COMMIT;
-- -- active SUBSCRIPTION
BEGIN;
ALTER SUBSCRIPTION testsub ENABLE;
select subname, subenabled from pg_subscription  where subname='testsub';
ALTER SUBSCRIPTION testsub SET (ENABLED=false);
select subname, subenabled from pg_subscription  where subname='testsub';
COMMIT;
--- drop subscription
DROP SUBSCRIPTION IF EXISTS testsub;
--- cleanup
RESET SESSION AUTHORIZATION;
DROP ROLE regress_subscription_user;

-- built-in function test
select pg_replication_origin_create('origin_test');
select pg_replication_origin_oid('origin_test');
select * from pg_replication_origin_status;
select pg_replication_origin_session_is_setup();
select pg_replication_origin_session_setup('origin_test');
select pg_replication_origin_session_is_setup();

create table t_origin_test(a int);
begin;
insert into t_origin_test values(1);
select pg_replication_origin_xact_setup('1/12345678', now());
commit;
select local_id,external_id,remote_lsn from pg_replication_origin_status;

select * from pg_replication_origin_progress('origin_test', false);
select * from pg_replication_origin_session_progress(false);
select local_id,external_id,remote_lsn from pg_show_replication_origin_status();

select pg_replication_origin_session_reset();
select pg_replication_origin_advance('origin_test', '1/87654321');

select pg_replication_origin_session_setup('origin_test');
select * from pg_replication_origin_session_progress(false);

select pg_replication_origin_xact_reset();
select pg_replication_origin_session_reset();

select pg_replication_origin_drop('origin_test');
-- error
select pg_replication_origin_session_setup('origin_test');
drop table t_origin_test;
