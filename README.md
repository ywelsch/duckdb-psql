[![Linux](https://github.com/ywelsch/duckdb-psql/actions/workflows/Linux.yml/badge.svg)](https://github.com/ywelsch/duckdb-psql/actions/workflows/Linux.yml) [![MacOS](https://github.com/ywelsch/duckdb-psql/actions/workflows/MacOS.yml/badge.svg)](https://github.com/ywelsch/duckdb-psql/actions/workflows/MacOS.yml) [![Windows](https://github.com/ywelsch/duckdb-psql/actions/workflows/Windows.yml/badge.svg)](https://github.com/ywelsch/duckdb-psql/actions/workflows/Windows.yml)

# PSQL: a piped SQL for DuckDB

PSQL extends [DuckDB](https://duckdb.org)'s SQL with a pipe syntax to provide simple composable queries. It's a lightweight variant of piped languages such as [PRQL](https://prql-lang.org) and [Kusto](https://docs.microsoft.com/azure/data-explorer/kusto/query/samples?pivots=azuredataexplorer), yet leveraging the full power of DuckDB's SQL.

Pipes allow you to compose your SQL queries in a very natural way (example inspired by PRQL):

```sql
from 'https://raw.githubusercontent.com/ywelsch/duckdb-psql/main/example/invoices.csv' |
where invoice_date >= date '1970-01-16' |
select
  *, 
  0.8 as transaction_fees,
  total - transaction_fees as income |
where income > 1 |
select
  customer_id, 
  avg(total), 
  sum(income) as sum_income, 
  count() as ct
  group by customer_id |
order by sum_income desc |
limit 10 |
as invoices |
join 'https://raw.githubusercontent.com/ywelsch/duckdb-psql/main/example/customers.csv'
    as customers
  on invoices.customer_id = customers.customer_id |
select
  customer_id, last_name || ', ' || first_name as name, 
  sum_income,
  version() as db_version;
```

which returns:

```
┌─────────────┬─────────────────────┬────────────┬────────────┐
│ customer_id │        name         │ sum_income │ db_version │
│    int64    │       varchar       │   double   │  varchar   │
├─────────────┼─────────────────────┼────────────┼────────────┤
│           6 │ Holý, Helena        │      43.83 │ v0.7.1     │
│           7 │ Gruber, Astrid      │      36.83 │ v0.7.1     │
│          24 │ Ralston, Frank      │      37.83 │ v0.7.1     │
│          25 │ Stevens, Victor     │      36.83 │ v0.7.1     │
│          26 │ Cunningham, Richard │      41.83 │ v0.7.1     │
│          28 │ Barnett, Julia      │      37.83 │ v0.7.1     │
│          37 │ Zimmermann, Fynn    │      37.83 │ v0.7.1     │
│          45 │ Kovács, Ladislav    │      39.83 │ v0.7.1     │
│          46 │ O'Reilly, Hugh      │      39.83 │ v0.7.1     │
│          57 │ Rojas, Luis         │      40.83 │ v0.7.1     │
├─────────────┴─────────────────────┴────────────┴────────────┤
│ 10 rows                                           4 columns │
└─────────────────────────────────────────────────────────────┘
```

## How does it work?

The underlying engine just does a simple syntactic transformation of the query, rewriting pipes

```sql
A | B | C | D
```
to
```sql
WITH _tmp1 AS (A),
     _tmp2 AS (FROM _tmp1 B)
     _tmp3 AS (FROM _tmp2 C)
FROM _tmp3 D
```

## Limitations

This is mainly an experiment at simplifying SQL and nowhere as feature-complete as some of the piped language alternatives. Its main advantage is that is has all the power and expressivity of DuckDB's SQL, while gaining some of the benefits of piped languages. As it is not implemented using any parsing framework (but just a quick and dirty regex replacement), it does not allow pipes to be used in sub-expressions. Having this would enable further capabilites (e.g. constructing CTEs, views, tables out of PSQL expressions).

## Installing the extension

To install the PSQL extension, DuckDB needs to be launched with the `allow_unsigned_extensions` option set to true.
Depending on the DuckDB usage, this can be configured as follows:

CLI:
```shell
duckdb -unsigned
```

Python:
```python
con = duckdb.connect(':memory:', config={'allow_unsigned_extensions' : 'true'})
```

A custom extension repository then needs to be defined as follows:
```sql
SET custom_extension_repository='welsch.lu/duckdb/psql/latest';
```
Note that the `/latest` path will provide the latest extension version available for the current version of DuckDB.
A given extension version can be selected by using that version as last path element instead.

After running these steps, the extension can then be installed and loaded using the regular INSTALL/LOAD commands in DuckDB:
```sql
FORCE INSTALL psql; # To override current installation with latest
LOAD psql;
```

## Build from source
To build the extension:
```sh
make
```
The main binaries that will be built are:
```sh
./build/release/duckdb
./build/release/test/unittest
./build/release/extension/psql/psql.duckdb_extension
```
- `duckdb` is the binary for the duckdb shell with the extension code automatically loaded.
- `unittest` is the test runner of duckdb. Again, the extension is already linked into the binary.
- `psql.duckdb_extension` is the loadable binary as it would be distributed.

To run the extension code, simply start the shell with `./build/release/duckdb`.
