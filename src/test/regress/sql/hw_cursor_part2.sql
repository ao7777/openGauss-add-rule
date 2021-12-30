------------------------------------------------------------------------------
-----7 test implicit cursor attributes for DML: select,insert,update,delete-----
------------------------------------------------------------------------------
create database pl_test_cursor_part2 DBCOMPATIBILITY 'pg';
\c pl_test_cursor_part2;
create schema hw_cursor_part2;
set current_schema = hw_cursor_part2;

create table t1(v1 int,v2 varchar2(100));
insert into t1 values (1,'abc1');
insert into t1 values (2,'abc2');
insert into t1 values (3,'abc3');

create or replace procedure sp_testsp
as
    v int:=0;
begin
    --select
    select v1 into v from t1 where v1=1; 
    if not sql%isopen then --sql%isopen always be false    
        raise notice '%','test select: sql%isopen=false';
    end if;
    if sql%found then 
        raise notice '%','test select: sql%found=true';
    end if;
    if sql%notfound then 
        raise notice '%','test select: sql%notfound=true';
    end if;    
    raise notice 'test select: sql%%rowcount=%',sql%rowcount;

    --insert
    insert into t1 values (4,'abc4');
    if not sql%isopen then --sql%isopen always be false    
        raise notice '%','test insert: sql%isopen=false';
    end if;
    if sql%found then 
        raise notice '%','test insert: sql%found=true';
    end if;
    if sql%notfound then 
        raise notice '%','test insert: sql%notfound=true';
    end if;    
    raise notice 'test insert: sql%%rowcount=%',sql%rowcount;
    
    --update
    update t1 set v1=v1+100 where v1>1000;
    if not sql%isopen then --sql%isopen always be false    
        raise notice '%','test update: sql%isopen=false';
    end if;
    if sql%found then 
        raise notice '%','test update: sql%found=true';
    end if;
    if sql%notfound then 
        raise notice '%','test update: sql%notfound=true';
    end if;    
    raise notice 'test update: sql%%rowcount=%',sql%rowcount;
    
    update t1 set v1=v1+100 where v1<1000;
    if not sql%isopen then --sql%isopen always be false    
        raise notice '%','test update: sql%isopen=false';
    end if;
    if sql%found then 
        raise notice '%','test update: sql%notfound=true';
    end if;
    if sql%notfound then 
        raise notice '%','test update: sql%notfound=true';
    end if;    
    raise notice 'test update: sql%%rowcount=%',sql%rowcount;
 
    --delete
    delete from t1 where v1>1000;
    if not sql%isopen then --sql%isopen always be false    
        raise notice '%','test delete: sql%isopen=false';
    end if;
    if sql%found then 
        raise notice '%','test delete: sql%found=true';
    end if;
    if sql%notfound then 
        raise notice '%','test delete: sql%notfound=true';
    end if;    
    raise notice 'test delete: sql%%rowcount=%',sql%rowcount;
    
    delete from t1 where v1<1000;
    if not sql%isopen then --sql%isopen always be false    
        raise notice '%','test delete: sql%isopen=false';
    end if;
    if sql%found then 
        raise notice '%','test delete: sql%found=true';
    end if;
    if sql%notfound then 
        raise notice '%','test delete: sql%notfound=true';
    end if;    
    raise notice 'test delete: sql%%rowcount=%',sql%rowcount;
end;
/
call sp_testsp();
drop procedure sp_testsp;
drop table t1;

------------------------------------------------------------------------------
-----support A db's cursor in or out params---------------------------------
------------------------------------------------------------------------------
CREATE TABLE TBL(VALUE INT);
INSERT INTO TBL VALUES (1);
INSERT INTO TBL VALUES (2);
INSERT INTO TBL VALUES (3);
INSERT INTO TBL VALUES (4);

CREATE OR REPLACE PROCEDURE TEST_SP
IS
    CURSOR C1(NO IN VARCHAR2) IS SELECT * FROM TBL WHERE VALUE < NO ORDER BY 1;
    CURSOR C2(NO OUT VARCHAR2) IS SELECT * FROM TBL WHERE VALUE < 10 ORDER BY 1;
    V INT;
    RESULT INT;
BEGIN
    OPEN C1(10);
    OPEN C2(RESULT);
    LOOP
    FETCH C1 INTO V; 
        IF C1%FOUND THEN 
            raise notice '%',V;
        ELSE 
            EXIT;
        END IF;
    END LOOP;
    CLOSE C1;
    
    LOOP
    FETCH C2 INTO V; 
        IF C2%FOUND THEN 
            raise notice '%',V;
        ELSE 
            EXIT;
        END IF;
    END LOOP;
    CLOSE C2;
END;
/
CALL  TEST_SP();
DROP TABLE TBL;
DROP PROCEDURE TEST_SP;

---------------------------------------------------------------------------------
----- test the mixed use of implicit and explicit cursor attributes -------------
----- test the effect of the implicit cursor use to explicit cursor attributes --
---------------------------------------------------------------------------------
drop table t1;
create table t1(v1 int,v2 varchar2(100));
insert into t1 values (1,'abc1');
insert into t1 values (2,'abc2');
insert into t1 values (3,'abc3');

create or replace procedure sp_testsp_select
as
    v int:=0;
    CURSOR cur IS select v1 from t1; 
begin   
    open cur;
    --select
    select v1 into v from t1 where v1=1;    
    if not cur%isopen then   
        raise notice '%','test select: cur%isopen=false';
    end if;
    if cur%found then 
        raise notice '%','test select: cur%found=true';
    end if;
    if cur%notfound then 
        raise notice '%','test select: cur%notfound=true';
    end if;    
    raise notice 'test select: cur%%rowcount=%',cur%rowcount;
    close cur;
end;
/
call sp_testsp_select();
drop procedure sp_testsp_select;
drop table t1;

create table t1(v1 int,v2 varchar2(100));
insert into t1 values (1,'abc1');
insert into t1 values (2,'abc2');
insert into t1 values (3,'abc3');
create or replace procedure sp_testsp_insert
as
    v int:=0;
    CURSOR cur IS select v1 from t1; 
begin   
    open cur;
    --insert
    insert into t1 values (4,'abc4');
    if not cur%isopen then    
        raise notice '%','test insert: cur%isopen=false';
    end if;
    if cur%found then 
        raise notice '%','test insert: cur%found=true';
    end if;
    if cur%notfound then 
        raise notice '%','test insert: cur%notfound=true';
    end if;    
    raise notice 'test insert: cur%%rowcount=%',cur%rowcount;
    close cur;
end;
/
call sp_testsp_insert();
drop procedure sp_testsp_insert;  
drop table t1;

create table t1(v1 int,v2 varchar2(100));
insert into t1 values (1,'abc1');
insert into t1 values (2,'abc2');
insert into t1 values (3,'abc3');
create or replace procedure sp_testsp_update
as
    v int:=0;
    CURSOR cur IS select v1 from t1; 
begin   
    open cur;
    --update
    update t1 set v1=v1+100 where v1>1000;
    if not cur%isopen then    
        raise notice '%','test update: cur%isopen=false';
    end if;
    if cur%found then 
        raise notice '%','test update: cur%found=true';
    end if;
    if cur%notfound then 
        raise notice '%','test update: cur%notfound=true';
    end if;    
    raise notice 'test update: cur%%rowcount=%',cur%rowcount;
    
    update t1 set v1=v1+100 where v1<1000;
    if not cur%isopen then    
        raise notice '%','test update: cur%isopen=false';
    end if;
    if cur%found then 
        raise notice '%','test update: cur%found=true';
    end if;
    if cur%notfound then 
        raise notice '%','test update: cur%notfound=true';
    end if;    
    raise notice 'test update: cur%%rowcount=%',cur%rowcount;
    close cur;
end;
/
call sp_testsp_update();
drop procedure sp_testsp_update;  
drop table t1;

create table t1(v1 int,v2 varchar2(100));
insert into t1 values (1,'abc1');
insert into t1 values (2,'abc2');
insert into t1 values (3,'abc3');
create or replace procedure sp_testsp_delete
as
    v int:=0;
    CURSOR cur IS select v1 from t1; 
begin   
    open cur;
    --delete
    delete from t1 where v1>1000;
    if not cur%isopen then    
        raise notice '%','test delete: cur%isopen=false';
    end if;
    if cur%found then 
        raise notice '%','test delete: cur%found=true';
    end if;
    if cur%notfound then 
        raise notice '%','test delete: cur%notfound=true';
    end if;    
    raise notice 'test delete: cur%%rowcount=%',cur%rowcount;
    
    delete from t1 where v1<1000;
    if not cur%isopen then    
        raise notice '%','test delete: cur%isopen=false';
    end if;
    if cur%found then 
        raise notice '%','test delete: cur%found=true';
    end if;
    if cur%notfound then 
        raise notice '%','test delete: cur%notfound=true';
    end if;    
    raise notice 'test delete: cur%%rowcount=%',cur%rowcount;
    close cur;
end;
/
call sp_testsp_delete();


--I1.create table
create table fun_refcursor_tab_001(c_int int,c_varchar varchar(20));
--I2.insert data
insert into fun_refcursor_tab_001 values(generate_series(1,1000),generate_series(1,1000)||'abc');
--I3.create function --
create or replace function fun_refcursor_001_1() returns refcursor
as $$
declare
c1 refcursor;
begin
open c1 for select c_int,c_varchar from fun_refcursor_tab_001 order by 1 limit 10;
return c1;
end;$$
language plpgsql;
--I4.create procedure--
create or replace procedure pro_fun_refcursor_001_1(id out int,name out varchar)
as
declare
c1 refcursor;
begin
select fun_refcursor_001_1() into c1;
loop
fetch next from c1 into id,name;
raise notice '% -- %',id,name;
insert into fun_refcursor_tab_001 values(id,name);
exit when c1%notfound;
end loop;
end;
/
--I5.select
select pro_fun_refcursor_001_1();
select count(*) from fun_refcursor_tab_001;
--I6.create function--
create or replace function fun_refcursor_001_2(inputid in fun_refcursor_tab_001.c_int%type)
returns table(out_id int,out_name varchar)
as $$
begin
return query select c_int,c_varchar from fun_refcursor_tab_001 where c_int>inputId order by 1;
end;$$
language plpgsql;

call fun_refcursor_001_2(990);
--I7.create procedure
create or replace procedure pro_refcursor_001(a in int)
as
begin
insert into fun_refcursor_tab_001 values(a,a||'b');
end;
/
--I8.select
select pro_refcursor_001(1024);
select * from fun_refcursor_tab_001 where c_int=1024;

--I9.clean up
drop table fun_refcursor_tab_001;
drop function fun_refcursor_001_1;
drop procedure pro_fun_refcursor_001_1;
drop function fun_refcursor_001_2;
drop procedure pro_refcursor_001;

DROP TABLE t1;
DROP SCHEMA hw_cursor_part2 CASCADE;
\c regression;
drop database IF EXISTS pl_test_cursor_part2;
