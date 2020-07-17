--
-- The test cases for numerical types 
CREATE TABLE MEDTEST(ID INT, NUMVAL NUMERIC, IVAL INT, BVAL BIGINT);
INSERT INTO MEDTEST VALUES(1, 100, 12, 99999), (1, 200, 13, NULL);
INSERT INTO MEDTEST VALUES(2, 100, 7777, NULL), (2, 300, 3333, 11111111), (2, 330, 5555, 11111110);
INSERT INTO MEDTEST VALUES(3, 110, 9, 909), (3, 111, 11, 808), (3, 111, 33, 505), (3, 112, 66, 606), (3, 102, 22, 202);
INSERT INTO MEDTEST VALUES(4, 91, 3, NULL), (4, 90, 3, 888);
INSERT INTO MEDTEST VALUES(5, 'NAN', NULL, 3), (5, 30, NULL, 3);
INSERT INTO MEDTEST VALUES(6, 'NAN', 100, 100), (6, 'NAN', 100, 200);
INSERT INTO MEDTEST VALUES(7, 'NAN', 123, 6666), (7, 'NAN', NULL, NULL), (7, 'NAN', NULL, 3333);
INSERT INTO MEDTEST VALUES(8, 'NAN', 99, 9999), (8, 888, 32, NULL), (8, 889, 123, NULL);

SELECT ID, MEDIAN(NUMVAL), MEDIAN(IVAL), MEDIAN(BVAL) FROM MEDTEST GROUP BY ID ORDER BY 1;

-- the median function is equivalent to percentile_cont(0.5) within group (order by column)
SELECT ID, PERCENTILE_CONT(0.5) WITHIN GROUP (ORDER BY NUMVAL) = MEDIAN(NUMVAL) AS MUST_TRUE1,
PERCENTILE_CONT(0.5) WITHIN GROUP (ORDER BY IVAL) = MEDIAN(IVAL) AS MUST_TRUE2,
PERCENTILE_CONT(0.5) WITHIN GROUP (ORDER BY BVAL) = MEDIAN(BVAL) AS MUST_TRUE3
FROM MEDTEST GROUP BY ID ORDER BY 1;

-- the test case for median with window function
SELECT ID, NUMVAL, MEDIAN(NUMVAL) OVER(PARTITION BY ID) FROM MEDTEST GROUP BY ID, NUMVAL ORDER BY 1, 2;
SELECT ID, IVAL, MEDIAN(IVAL) OVER(PARTITION BY ID) FROM MEDTEST GROUP BY ID, IVAL ORDER BY 1, 2;
SELECT ID, BVAL, MEDIAN(BVAL) OVER(PARTITION BY ID) FROM MEDTEST GROUP BY ID, BVAL ORDER BY 1, 2;

-- the test cases for median(extension)
SELECT ID, MEDIAN(NUMVAL + IVAL) FROM MEDTEST GROUP BY ID ORDER BY 1;
SELECT ID, BVAL + IVAL, MEDIAN(BVAL + IVAL) OVER(PARTITION BY ID) FROM MEDTEST GROUP BY ID, BVAL + IVAL ORDER BY 1, 2;

--
-- The test cases for INTERVAL type
CREATE TABLE MEDTEST2(ID INT, ITVL INTERVAL, TS TIMESTAMP);
INSERT INTO MEDTEST2 VALUES(1, INTERVAL '1 YEAR 2 MONTHS 3 DAYS', NOW());
INSERT INTO MEDTEST2 VALUES(1, INTERVAL '1 YEAR 2 MONTHS 4 DAYS', NOW());
INSERT INTO MEDTEST2 VALUES(2, INTERVAL '11 YEAR 2 MONTHS 4 DAYS', NOW());
INSERT INTO MEDTEST2 VALUES(3, INTERVAL '7 YEAR 1 MONTHS 0 DAYS', NOW());
INSERT INTO MEDTEST2 VALUES(3, INTERVAL '7 YEAR 2 MONTHS 0 DAYS', NOW());
INSERT INTO MEDTEST2 VALUES(3, INTERVAL '7 YEAR 2 MONTHS 1 DAYS', NOW());
INSERT INTO MEDTEST2 VALUES(4, NULL, NULL);
INSERT INTO MEDTEST2 VALUES(5, NULL, NULL);
INSERT INTO MEDTEST2 VALUES(5, INTERVAL '7 YEAR 2 MONTHS 1 DAYS', NULL);

SELECT ID, MEDIAN(ITVL) FROM MEDTEST2 GROUP BY ID;

-- unsupported datatype
SELECT ID, MEDIAN(TS) FROM MEDTEST2 GROUP BY ID;

-- median & percentile_cont(0.5)
SELECT ID, PERCENTILE_CONT(0.5) WITHIN GROUP (ORDER BY ITVL) = MEDIAN(ITVL) AS MUST_TRUE
FROM MEDTEST2 GROUP BY ID ORDER BY 1;

-- the window function
SELECT ID, ITVL, MEDIAN(ITVL) OVER(PARTITION BY ID) FROM MEDTEST2 GROUP BY ID, ITVL ORDER BY 1, 2;
