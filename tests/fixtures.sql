-- Fixture database for redstone tests.
-- Regenerate with `make fixtures`. The resulting test.db is not committed.
--
-- Shapes here are chosen to break naive completion:
--   * a column named with a reserved word           (orders."order")
--   * an identifier that must be quoted             ("total amount")
--   * one column with many distinct values, one with few
--   * a view, which must appear alongside tables
--   * a self-referencing foreign key                (employees.manager_id)
--   * a table whose name is a prefix of another     (order_items / orders)

PRAGMA foreign_keys = ON;

CREATE TABLE departments (
    id       INTEGER PRIMARY KEY,
    name     TEXT NOT NULL UNIQUE,
    building TEXT
);

CREATE TABLE employees (
    id         INTEGER PRIMARY KEY,
    name       TEXT NOT NULL,
    email      TEXT UNIQUE,
    dept_id    INTEGER REFERENCES departments(id),
    manager_id INTEGER REFERENCES employees(id),
    status     TEXT NOT NULL DEFAULT 'active',
    salary     REAL,
    hired_on   TEXT
);

CREATE TABLE orders (
    id        INTEGER PRIMARY KEY,
    "order"   TEXT,
    placed_by INTEGER REFERENCES employees(id),
    status    TEXT NOT NULL,
    placed_on TEXT
);

CREATE TABLE order_items (
    id             INTEGER PRIMARY KEY,
    order_id       INTEGER NOT NULL REFERENCES orders(id),
    sku            TEXT NOT NULL,
    quantity       INTEGER NOT NULL DEFAULT 1,
    "total amount" REAL
);

CREATE VIEW active_employees AS
    SELECT e.id, e.name, e.email, d.name AS department
    FROM employees e
    LEFT JOIN departments d ON d.id = e.dept_id
    WHERE e.status = 'active';

CREATE INDEX idx_employees_dept ON employees(dept_id);
CREATE INDEX idx_order_items_order ON order_items(order_id);

INSERT INTO departments (id, name, building) VALUES
    (1, 'Engineering', 'North'),
    (2, 'Sales',       'South'),
    (3, 'Support',     'North'),
    (4, 'Finance',     'East');

-- status has few distinct values; email has many. Value completion must cope
-- sensibly with both.
INSERT INTO employees (id, name, email, dept_id, manager_id, status, salary, hired_on) VALUES
    (1,  'Ada Lovelace',     'ada@example.com',     1, NULL, 'active',   98000.0, '2019-03-01'),
    (2,  'Grace Hopper',     'grace@example.com',   1, 1,    'active',   94000.0, '2019-07-15'),
    (3,  'Alan Turing',      'alan@example.com',    1, 1,    'active',   96000.0, '2020-01-20'),
    (4,  'Edsger Dijkstra',  'edsger@example.com',  1, 1,    'on_leave', 91000.0, '2020-05-05'),
    (5,  'Barbara Liskov',   'barbara@example.com', 2, NULL, 'active',   88000.0, '2018-11-11'),
    (6,  'Donald Knuth',     'donald@example.com',  2, 5,    'active',   87000.0, '2021-02-28'),
    (7,  'Ken Thompson',     'ken@example.com',     3, NULL, 'active',   85000.0, '2017-06-30'),
    (8,  'Dennis Ritchie',   'dennis@example.com',  3, 7,    'inactive', 85000.0, '2017-06-30'),
    (9,  'Rob Pike',         'rob@example.com',     3, 7,    'active',   83000.0, '2022-09-01'),
    (10, 'Margaret Hamilton','margaret@example.com',4, NULL, 'active',   99000.0, '2016-04-12');

INSERT INTO orders (id, "order", placed_by, status, placed_on) VALUES
    (1, 'ORD-0001', 1,  'shipped',   '2024-01-05'),
    (2, 'ORD-0002', 5,  'pending',   '2024-01-07'),
    (3, 'ORD-0003', 7,  'shipped',   '2024-02-11'),
    (4, 'ORD-0004', 10, 'cancelled', '2024-02-19'),
    (5, 'ORD-0005', 2,  'pending',   '2024-03-02');

INSERT INTO order_items (id, order_id, sku, quantity, "total amount") VALUES
    (1, 1, 'KB-104',  2, 179.98),
    (2, 1, 'MS-002',  1, 39.99),
    (3, 2, 'MON-27',  3, 897.00),
    (4, 3, 'KB-104',  1, 89.99),
    (5, 3, 'DOCK-01', 1, 149.50),
    (6, 4, 'MON-27',  2, 598.00),
    (7, 5, 'CBL-USB', 5, 49.95);
