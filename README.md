# PSQL: a piped SQL for DuckDB

PSQL extends [DuckDB](https://duckdb.org)'s SQL with a pipe syntax to provide simple composable queries. It's a lightweight variant of piped languages such as [PRQL](https://prql-lang.org) and [Kusto](https://docs.microsoft.com/azure/data-explorer/kusto/query/samples?pivots=azuredataexplorer), yet leveraging the full power of DuckDB's SQL.

Pipes allow you to compose your SQL queries in a very natural way (example inspired by PRQL):

```sql
from 'https://raw.githubusercontent.com/ywelsch/duckdb-psql/main/example/invoices.csv' |>
where invoice_date >= date '1970-01-16' |>
select
  *, 
  0.8 as transaction_fees,
  total - transaction_fees as income |>
where income > 1 |>
select
  customer_id, 
  avg(total), 
  sum(income) as sum_income, 
  count() as ct
  group by customer_id |>
order by sum_income desc |>
limit 10 |>
as invoices
  join 'https://raw.githubusercontent.com/ywelsch/duckdb-psql/main/example/customers.csv'
    as customers
  on invoices.customer_id = customers.customer_id |>
select
  customer_id,
  last_name || ', ' || first_name as name,
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

Pipelines can be used anywhere a query can appear: in subqueries, CTEs, views, `CREATE TABLE ... AS`, `INSERT INTO ...`, and so on:

```sql
create view invoices as
  from 'https://raw.githubusercontent.com/ywelsch/duckdb-psql/main/example/invoices.csv' |>
  where invoice_date >= date '1970-01-16' |>
  select
    *,
    0.8 as transaction_fees,
    total - transaction_fees as income;

from 'https://raw.githubusercontent.com/ywelsch/duckdb-psql/main/example/customers.csv' |>
where customer_id in (from invoices |> where income > 20 |> select customer_id) |>
select first_name, last_name;
```

Earlier versions of PSQL required piped sub-expressions to be delimited with `(|` and `|)`. This syntax is still accepted, but no longer needed.

## How does it work?

PSQL extends the grammar of DuckDB's PEG parser, so that every query can be followed by pipe stages. A pipe stage is anything that can follow the first table of a `FROM`-first query: a table alias, joins, `SELECT`, `WHERE`, `GROUP BY`, `HAVING`, `WINDOW`, `QUALIFY`, set operations, `ORDER BY`, `LIMIT`, ... It is parsed as if the pipe input was written in that position, so

```sql
A |> B |> C |> D
```
is equivalent to
```sql
FROM (
FROM (
FROM (
  A
)
  B
)
  C
)
  D
```

A `WITH` clause in front of a pipeline is visible in all of its stages.

## Limitations

This is mainly an experiment at simplifying SQL and nowhere as feature-complete as some of the piped language alternatives. Its main advantage is that is has all the power and expressivity of DuckDB's SQL, while gaining some of the benefits of piped languages.

PSQL requires DuckDB 2.0 or later, which made the parser extensible. Earlier versions of PSQL for DuckDB 1.x implemented the syntax as a text substitution on the query, which was unaware of the scoping rules of SQL and did not allow arbitrary nesting of pipelines.

## Running the extension

The PSQL extension is a [DuckDB community extension](https://community-extensions.duckdb.org/extensions/psql.html), and can simply be installed with

```sql
install psql from community;
```

and subsequently loaded with

```sql
load psql;
```

The pipe syntax is a grammar extension, which DuckDB activates per connection with the `active_grammar_extensions` setting. Loading PSQL activates it on every connection:

- the connection that runs `load psql` can use pipes from its next statement on (not in later statements of the same query string, which are already parsed),
- connections opened afterwards have it activated right away,
- connections that were already open activate it at the end of their current or next query. They may be running a query on another thread while PSQL is loaded, so their settings cannot be changed right away.

A connection can opt out with `reset active_grammar_extensions;` (or by setting the list of active grammar extensions without `psql`), and opt in again with `call psql_enable();`, which adds PSQL while keeping other active grammar extensions.

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

To run the tests:
```sh
make test
```
