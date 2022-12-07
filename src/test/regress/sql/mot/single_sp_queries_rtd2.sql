------------------------------------------------------------------------------------
create foreign table table1 (x integer not null, c1 varchar(1020), c2 varchar(1020), c3 varchar(1020), c4 varchar(1020), c5 varchar(1020));
create foreign table table2 (x integer not null, c1 varchar(1020), c2 varchar(1020), c3 varchar(1020), c4 varchar(1020), c5 varchar(1020));

CREATE OR REPLACE PROCEDURE random_text_simple(length INTEGER, out random_text_simple TEXT)
    AS
    DECLARE
        possible_chars TEXT := '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ';
        output TEXT := '';
        i INT4;
        pos INT4;
    BEGIN

        FOR i IN 1..length LOOP
            pos := random_range(1, length(possible_chars));
            output := output || substr(possible_chars, pos, 1);
        END LOOP;

        random_text_simple := output;
    END;
/

CREATE OR REPLACE FUNCTION random_range(INTEGER, INTEGER)
    RETURNS INTEGER
    LANGUAGE SQL
    AS $$
        SELECT ($1 + FLOOR(($2 - $1 + 1) * random() ))::INTEGER;
$$;
	
CREATE OR REPLACE PROCEDURE insert_into_table1(max integer, OUT outable1 integer, OUT err_msg character varying)
AS
DECLARE
    v_max integer := max;
	c1 character varying;
	c2 character varying;
	c3 character varying;
	c4 character varying;
	c5 character varying;
BEGIN
	FOR i IN 1..v_max LOOP
		select random_text_simple(5) into c1;
		select random_text_simple(5) into c2;
		select random_text_simple(5) into c3;
		select random_text_simple(5) into c4;
		select random_text_simple(5) into c5;
		insert into table1 values (i, c1, c2, c3, c4, c5);
    END LOOP;
	select count(*) into outable1 from table1;
EXCEPTION
    WHEN OTHERS THEN
        err_msg := SQLERRM;

END;
/
	
CREATE OR REPLACE PROCEDURE insert_into_table2(max integer, OUT outable1 integer, OUT err_msg character varying)
AS
DECLARE
    v_max integer := max;
	c1 character varying;
	c2 character varying;
	c3 character varying;
	c4 character varying;
	c5 character varying;
BEGIN
	FOR i IN 1..v_max LOOP
		select random_text_simple(5) into c1;
		select random_text_simple(5) into c2;
		select random_text_simple(5) into c3;
		select random_text_simple(5) into c4;
		select random_text_simple(5) into c5;
		insert into table2 values (i, c1, c2, c3, c4, c5);
    END LOOP;
	select count(*) into outable1 from table2;
EXCEPTION
    WHEN OTHERS THEN
        err_msg := SQLERRM;

END;
/

select insert_into_table1(1000);
select insert_into_table2(1000);

CREATE OR REPLACE PROCEDURE insert_into(max integer, OUT outable1 integer, OUT outable2 integer, OUT err_msg character varying)
AS
DECLARE
    v_max integer := max;
BEGIN
    insert into table1 values (generate_series(1,v_max));
	insert into table2 select * from table1 where c1 is null;
	select count(*) into outable1 from table1;
	select count(*) into outable2 from table2;

EXCEPTION
    WHEN OTHERS THEN
        err_msg := SQLERRM;
END;
/

select insert_into(1000);


CREATE OR REPLACE PROCEDURE delete_from(OUT outable1 integer, OUT outable2 integer, OUT err_msg character varying)
AS
DECLARE
	t_record record;
BEGIN
	create index on table1(x);
	create index on table2(x);
	truncate table1;
	truncate table2;
	select insert_into_table1(1000) into t_record;
	select insert_into_table2(1000) into t_record;
	select insert_into(1000) into t_record;
	delete from table1 using table2 where table1.x = table2.x and table1.c1 = table2.c2;
    delete from table2 using table1 where table1.x = table2.x and table1.c1 != table2.c2;
	
	select count (*) into outable1 from table1;
	select count (*) into outable2 from table2;

EXCEPTION
    WHEN OTHERS THEN
        err_msg := SQLERRM;
END;
/

select delete_from();

CREATE OR REPLACE PROCEDURE compare(OUT outable1 integer, OUT err_msg character varying)
AS
DECLARE
	t_record record;
BEGIN
	select count (*) into outable1 from table1,table2 where table1.x = table2.x and table1.c1 like '%%%'  and table2.c1 like '%%%';

EXCEPTION
    WHEN OTHERS THEN
        err_msg := SQLERRM;
END;
/

select compare();

drop foreign table table1;
drop foreign table table2;

