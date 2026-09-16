-- Fixture for the differential output-parity suite.
--
-- Every row here exists to break a formatter: NULLs and empty strings look
-- alike in some modes and not others, blobs decide between text and hex,
-- embedded newlines decide whether rows get separators, tabs expand to
-- eight-column stops, CJK and combining marks decide column widths, and the
-- control characters decide escaping. Nothing in it is decorative.
CREATE TABLE t (
  id     INTEGER PRIMARY KEY,
  label  TEXT,
  num    REAL,
  raw    BLOB
);

INSERT INTO t VALUES (1, 'plain', 1.5, x'414243');
INSERT INTO t VALUES (2, NULL, NULL, NULL);
INSERT INTO t VALUES (3, '', 0.0, x'');
INSERT INTO t VALUES (4, 'two' || char(10) || 'lines', -2.25, x'00ff10');
INSERT INTO t VALUES (5, 'tab' || char(9) || 'stop', 1e100, NULL);
INSERT INTO t VALUES (6, 'quote''s "and" <tags> & amp', 3.0, NULL);
INSERT INTO t VALUES (7, '日本語テキスト', 42, NULL);
INSERT INTO t VALUES (8, 'e' || char(769) || 'combining', -0.5, NULL);
INSERT INTO t VALUES (9, 'ctrl' || char(1) || char(7) || 'chars', 0.1, NULL);
INSERT INTO t VALUES (10, 'a much longer value than any of the others, to drive the widths', 123456789, NULL);
INSERT INTO t VALUES (11, 'comma,separated"field', 7, NULL);
INSERT INTO t VALUES (12, 'cr' || char(13) || char(10) || 'lf', 8, NULL);

CREATE TABLE empty (a, b);

CREATE TABLE nums (x INTEGER, y REAL);
INSERT INTO nums VALUES (1, 1.0), (22, 2.25), (333, -3.5), (NULL, NULL);
