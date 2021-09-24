/************************************************************************/
/*                        anonymous block                               */
/* Anonymous block: Only top-level anonymous block autonomous           */
/* transaction declaration is supported, and anonymous block nesting    */
/* is not supported.                                                    */
/************************************************************************/

/* Normal anonymous block printing */
DECLARE 
	PRAGMA AUTONOMOUS_TRANSACTION;
	res int;
	res2 text;
BEGIN
	raise notice 'just use call.';
	res2 := 'aa55';
	res := 55;
END;
/

create table t1(a int ,b text);

DECLARE 
	PRAGMA AUTONOMOUS_TRANSACTION;
BEGIN
	raise notice 'just use call.';
	insert into t1 values(1,'you are so cute!');
END;
/

select * from t1;

/* 
 * Start a transaction, which is an autonomous transaction anonymous block,
 * Transactions are rolled back, and anonymous blocks are not rolled back.
 */
truncate table t1;

START TRANSACTION;
DECLARE 
	PRAGMA AUTONOMOUS_TRANSACTION;
BEGIN
	raise notice 'just use call.';
	insert into t1 values(1,'you are so cute,will commit!');
END;
/
insert into t1 values(1,'you will rollback!');
rollback;

select * from t1;


/* 
 * The external anonymous block is a common anonymous block, 
 * and the internal anonymous block is an autonomous transactional anonymous block,
 * Transaction rollback and anonymous block rollback
 */
truncate table t1;
DECLARE 
	--PRAGMA AUTONOMOUS_TRANSACTION;
BEGIN
	DECLARE 
		PRAGMA AUTONOMOUS_TRANSACTION;
	BEGIN
		raise notice 'just use call.';
		insert into t1 values(1,'can you rollback!');
	END;
	insert into t1 values(2,'I will rollback!');
	rollback;
END;
/

select * from t1;

drop table if exists t1;

/*
 * Anonymous directly executes autonomy and throws an exception.
 */
DECLARE 
	PRAGMA AUTONOMOUS_TRANSACTION;
	res int := 0;
	res2 int := 1;
BEGIN
	raise notice 'just use call.';
	res2 = res2/res;
END;
/

/*
 * An anonymous block is caught after an exception is thrown during execution.
 */
DECLARE 
	PRAGMA AUTONOMOUS_TRANSACTION;
	res int := 0;
	res2 int := 1;
BEGIN
	raise notice 'just use call.';
	res2 = res2/res;
EXCEPTION
	WHEN division_by_zero THEN
	    raise notice 'autonomous throw exception.';
END;
/


/************************************************************************/
/*                              PROCEDURE                               */
/************************************************************************/
/* int return value */
CREATE OR REPLACE PROCEDURE autonomous_1(out res int)  AS 
DECLARE 
	PRAGMA AUTONOMOUS_TRANSACTION;
BEGIN
	res := 55;
END;
/
select autonomous_1();
 
/* text return value */
CREATE OR REPLACE PROCEDURE autonomous_2(proc_name VARCHAR2(10), out res2 text)  AS 
DECLARE 
	PRAGMA AUTONOMOUS_TRANSACTION;
BEGIN
	res2 := 'aa55';
END;
/
select autonomous_2('');

/* record return value */
CREATE OR REPLACE PROCEDURE autonomous_3(out res int, out res2 text)  AS 
DECLARE 
	PRAGMA AUTONOMOUS_TRANSACTION;
BEGIN
	res2 := 'aa55';
	res := 55;
END;
/
select autonomous_3();

/*
 * The main transaction invokes the autonomous transaction,
 * The main transaction is rolled back, and the autonomous transaction is not rolled back.
 */
create table t2(a int, b int);
insert into t2 values(1,2);
select * from t2;

CREATE OR REPLACE PROCEDURE autonomous_4(a int, b int)  AS 
DECLARE 
	num3 int := a;
	num4 int := b;
	PRAGMA AUTONOMOUS_TRANSACTION;
BEGIN
	insert into t2 values(num3, num4); 
	raise notice 'just use call.';
END;
/
CREATE OR REPLACE PROCEDURE autonomous_5(a int, b int)  AS 
DECLARE 
BEGIN
	raise notice 'just no use call.';
	insert into t2 values(666, 666);
	autonomous_4(a,b);
	rollback;
END;
/
select autonomous_5(11,22);
select * from t2;

/*
 * The main transaction invokes the autonomous transaction,
 * The autonomous transaction is rolled back. The main transaction is not rolled back.
 */
truncate table t2;
select * from t2;

CREATE OR REPLACE PROCEDURE autonomous_6(a int, b int)  AS 
DECLARE 
	num3 int := a;
	num4 int := b;
	PRAGMA AUTONOMOUS_TRANSACTION;
BEGIN
	insert into t2 values(num3, num4); 
	raise notice 'just use call.';
	rollback;
END;
/
CREATE OR REPLACE PROCEDURE autonomous_7(a int, b int)  AS 
DECLARE 
BEGIN
	raise notice 'just no use call.';
	insert into t2 values(666, 666);
	autonomous_6(a,b);
END;
/

select autonomous_7(11,22);
select * from t2;

drop table if exists t2;

/*
 * An autonomous transaction exception occurs. After an exception is thrown,
 * The main transaction can receive an exception.
 */
CREATE OR REPLACE PROCEDURE autonomous_8()  AS 
DECLARE 
	a int := 0;
	b int := 1;
	PRAGMA AUTONOMOUS_TRANSACTION;
BEGIN
	b := b/a;
END;
/
select autonomous_8();

CREATE OR REPLACE PROCEDURE autonomous_9()  AS 
DECLARE 
	PRAGMA AUTONOMOUS_TRANSACTION;
BEGIN
	autonomous_8();
EXCEPTION
	WHEN division_by_zero THEN
		raise notice 'autonomous throw exception.';
END;
/

select autonomous_9();


/*
 * Autonomous transaction exception. After the exception is captured,
 * The main transaction no longer catches exceptions.
 */

CREATE OR REPLACE PROCEDURE autonomous_10()  AS 
DECLARE 
	a int := 0;
	b int := 1;
	PRAGMA AUTONOMOUS_TRANSACTION;
BEGIN
	b := b/a;
EXCEPTION
	WHEN division_by_zero THEN
		raise notice 'inner autonomous exception.';
END;
/
select autonomous_10();

CREATE OR REPLACE PROCEDURE autonomous_11()  AS 
DECLARE 
BEGIN
	autonomous_10();
EXCEPTION
	WHEN division_by_zero THEN
	    raise notice 'autonomous throw exception.';
END;
/

select autonomous_11();

/*
 * The main transaction is abnormal. After the exception is captured,
 * Invoke the autonomous transaction and roll back the main transaction,
 * Autonomous transactions are not rolled back.
 */
 
create table t3(a int , b int ,c text);
select * from t3;

CREATE OR REPLACE PROCEDURE autonomous_12(a int ,b int ,c text)  AS 
DECLARE 
	PRAGMA AUTONOMOUS_TRANSACTION;
BEGIN
	insert into t3 values(a, b, c);
END;
/

CREATE OR REPLACE PROCEDURE autonomous_13()  AS 
DECLARE 
	a int := 0;
	b int := 1;
BEGIN
	b := b/a;
EXCEPTION
	WHEN division_by_zero THEN
		autonomous_12(a, b, sqlerrm);
		rollback;
END;
/

select autonomous_13();
select * from t3;

CREATE OR REPLACE PROCEDURE autonomous_14()  AS 
DECLARE 
	a int := 0;
	b int := 1;
BEGIN
	autonomous_12(a, b, 'Pre-computing storage');
	insert into t3 values(a, b, 'i will roll back');
	b := b/a;
	autonomous_12(999, 999, 'you can not reach here,hehehe!');
EXCEPTION
	WHEN division_by_zero THEN
		autonomous_12(a, b, sqlerrm);
		rollback;
END;
/

select autonomous_14();
select * from t3;



/*
 * recursion
 */

CREATE OR REPLACE PROCEDURE autonomous_15(a int ,b int ,c text)  AS 
DECLARE 
	PRAGMA AUTONOMOUS_TRANSACTION;
BEGIN
	insert into t3 values(a, b, c);
	raise notice 'inner autonomous exception.';
	autonomous_15(a, b, c);
END;
/

select autonomous_15(1,2,'1111');
select * from t3;




/*truncate table t3;*/

/************************** commit/rollback statement*****************************************/
truncate table t3;

CREATE OR REPLACE PROCEDURE autonomous_16(a int ,b int ,c text)  AS 
DECLARE 
	PRAGMA AUTONOMOUS_TRANSACTION;
BEGIN
	insert into t3 values(a, b, c);
END;
/

CREATE OR REPLACE PROCEDURE autonomous_17(a int ,b int ,c text)  AS 
DECLARE 
	PRAGMA AUTONOMOUS_TRANSACTION;
BEGIN
	insert into t3 values(a, b, c);
	rollback;
END;
/

/* The main transaction is rolled back, but the autonomous transaction is not rolled back. */
CREATE OR REPLACE PROCEDURE autonomous_18()  AS 
DECLARE 
	a int := 0;
	b int := 1;
BEGIN
	autonomous_16(a, b, 'Pre-computing storage');
	insert into t3 values(a, b, 'i will roll back,you can not catch me');
	rollback;
END;
/

select autonomous_18();
select * from t3;

/* The commit and rollback of the main transaction do not affect the autonomous transaction. */
CREATE OR REPLACE PROCEDURE autonomous_19()  AS 
DECLARE 
	a int := 0;
	b int := 1;
BEGIN
	insert into t3 values(a, b, 'commit after me');
	commit;
	autonomous_16(a, b, 'Pre-computing storage');
	insert into t3 values(a, b, 'i will roll back,you can not catch me');
	rollback;
END;
/

select autonomous_19();
select * from t3;

CREATE OR REPLACE PROCEDURE autonomous_20()  AS 
DECLARE 
	a int := 0;
	b int := 1;
BEGIN
	insert into t3 values(a, b, 'autonomous rollback after me,but i will commit');
	autonomous_17(a, b, 'Pre-computing storage');
	commit;
	insert into t3 values(a, b, 'i will roll back,you can not catch me');
	rollback;
END;
/

select autonomous_20();
select * from t3;

/* Main and Autonomous Transactions with Exceptions */
truncate table t3;

CREATE OR REPLACE PROCEDURE autonomous_21()  AS 
DECLARE 
	a int := 0;
	b int := 1;
BEGIN
	insert into t3 values(a, b, 'i will roll back');
	b := b/a;
EXCEPTION
	WHEN division_by_zero THEN
		autonomous_16(a, b, sqlerrm);
		insert into t3 values(a, b, 'i will roll back,you can not catch me');
		rollback;
END;
/

select autonomous_21();
select * from t3;



drop table if exists t3;

/************************************************************************/ 
/*                              function                                */
/************************************************************************/
create table t4(a int, b int, c text);


/*
 * Common autonomous transaction function
 */
CREATE OR REPLACE function autonomous_31(num1 text) RETURN int AS 
DECLARE 
	num3 int := 220;
	PRAGMA AUTONOMOUS_TRANSACTION;
BEGIN
	return num3;
END;
/

select autonomous_31('a222');


/*
 * The main transaction is rolled back, but the autonomous transaction is not rolled back.
 */

select * from t4;

CREATE OR REPLACE function autonomous_32(a int ,b int ,c text) RETURN int AS 
DECLARE 
	PRAGMA AUTONOMOUS_TRANSACTION;
BEGIN
	insert into t4 values(a, b, c);
	return 1;
END;
/
CREATE OR REPLACE function autonomous_33(num1 int) RETURN int AS 
DECLARE 
	num3 int := 220;
	tmp int;
	PRAGMA AUTONOMOUS_TRANSACTION;
BEGIN
	num3 := num3/num1;
	return num3;
EXCEPTION
	WHEN division_by_zero THEN
		select autonomous_32(num3, num1, sqlerrm) into tmp;
		return 0;
END;
/

select autonomous_33(0);

select * from t4;

drop table if exists t4;

create table t5(a int,b text);
CREATE USER jim PASSWORD 'gauss_123';
SET SESSION AUTHORIZATION jim PASSWORD 'gauss_123';
DECLARE
PRAGMA AUTONOMOUS_TRANSACTION;
BEGIN
raise notice 'just use call.';
insert into t5 values(1,'aaa');
END;
/

RESET SESSION AUTHORIZATION;

select * from t5;

drop table if exists t5;
DROP USER IF EXISTS jim CASCADE;

drop table if exists test1;
create table test1(c1 date);
INSERT INTO test1 VALUES (date '12-10-2010');

create or replace package datatype_test as
data1 date;
function datatype_test_func() return date;
procedure datatype_test_proc();
end datatype_test;
create or replace package body datatype_test as
function datatype_test_func() return date is
declare
data2 date;
PRAGMA AUTONOMOUS_TRANSACTION;
begin
select c1 into data2 from test1;
data1 = data2;
return(data1);
end;
procedure datatype_test_proc() is
declare
data2 date;
begin
select c1 into data2 from test1;
data1 = data2;
end;
end datatype_test;
/

call datatype_test.datatype_test_func();

create table test_in (id int,a date);

CREATE OR REPLACE FUNCTION autonomous_test_in_f_1(num1  int) RETURNS integer
LANGUAGE plpgsql AS $$
DECLARE PRAGMA AUTONOMOUS_TRANSACTION;
BEGIN
EXECUTE 'INSERT INTO test_in VALUES (' || num1::text || ',sysdate)';
EXECUTE 'select pg_sleep(' || num1::text || ')';
RETURN num1;
END;
$$;
declare begin
select autonomous_test_in_f_1(0);
insert into test_in values (2,1);
end;
/

select count(*) from pg_stat_activity where application_name = 'autonomoustransaction';

drop table if exists test_in;

CREATE OR REPLACE FUNCTION autonomous_f_064(num1 int) RETURNS integer     
LANGUAGE plpgsql AS $$
DECLARE
BEGIN
DECLARE PRAGMA AUTONOMOUS_TRANSACTION;
BEGIN
insert into test_in values(num1,sysdate);
END;
RETURN num1;
END;
$$;

